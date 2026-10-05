#include "Public/RenderCore/ParticleSystemAsset.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>
#if WITH_EDITOR
#include <filesystem>
#include <fstream>
#endif

#include "Public/Core/CoreFileProvider.h"
#include "Public/RenderCore/RenderCoreLog.h"

/* ParticleSystemAsset.h: the emitter's settings, .nmparticle text, and the cache. */

namespace RenderCore
{
    // === ParticleEmitterDesc ==================================================

    namespace
    {
        void ToFloats(const EngineCore::Vector3& InVector, float (&OutVector)[3])
        {
            OutVector[0] = float(InVector.GetX());
            OutVector[1] = float(InVector.GetY());
            OutVector[2] = float(InVector.GetZ());
        }

        void ToFloats(const EngineCore::Color InColor, float (&OutColor)[4])
        {
            OutColor[0] = InColor.Rf();
            OutColor[1] = InColor.Gf();
            OutColor[2] = InColor.Bf();
            OutColor[3] = InColor.Af();
        }

        float Length(const float (&InVector)[3])
        {
            return std::sqrt(InVector[0] * InVector[0] + InVector[1] * InVector[1] + InVector[2] * InVector[2]);
        }
    }

    ParticleEmitterSettings ParticleEmitterDesc::ToSettings() const
    {
        ParticleEmitterSettings Out;
        Out.Type             = Type < PT_Count ? Type : uint8_t(PT_Sprite);
        Out.MaxParticles     = uint32_t(std::clamp<int32_t>(MaxParticles, 1, int32_t(PARTICLE_MAX_PER_SYSTEM)));
        Out.SpawnMode        = SpawnMode < PSM_Count ? SpawnMode : uint8_t(PSM_Continuous);
        Out.SpawnRate        = std::max(SpawnRate, 0.0f);
        Out.BurstCount       = uint32_t(std::clamp<int32_t>(BurstCount, 0, int32_t(PARTICLE_MAX_PER_SYSTEM)));
        Out.BurstInterval    = std::max(BurstInterval, 0.0f);
        Out.BurstCycles      = uint32_t(std::max<int32_t>(BurstCycles, 0));
        Out.Lifetime         = std::max(Lifetime, 0.001f);
        Out.LifetimeVariance = std::clamp(LifetimeVariance, 0.0f, 1.0f);
        Out.EmitterRadius    = std::max(EmitterRadius, 0.0f);
        ToFloats(Velocity, Out.Velocity);
        Out.VelocitySpread   = std::max(VelocitySpread, 0.0f);
        ToFloats(Acceleration, Out.Acceleration);
        Out.Drag             = std::max(Drag, 0.0f);
        Out.Turbulence       = std::max(Turbulence, 0.0f);
        Out.StartSize        = std::max(StartSize, 0.0f);
        Out.EndSize          = std::max(EndSize, 0.0f);
        ToFloats(StartColor, Out.StartColor);
        ToFloats(EndColor, Out.EndColor);
        Out.ColorCurve       = std::clamp(ColorCurve, 0.05f, 20.0f);
        Out.FadeIn           = std::clamp(FadeIn, 0.0f, 1.0f);
        Out.FadeOut          = std::clamp(FadeOut, 0.0f, 1.0f);
        Out.Billboard        = Billboard < PB_Count ? Billboard : uint8_t(PB_FaceCamera);
        Out.VelocityStretch  = std::max(VelocityStretch, 0.0f);
        Out.MeshOrientation  = MeshOrientation < PMO_Count ? MeshOrientation : uint8_t(PMO_Fixed);
        Out.bRandomRotation  = bRandomRotation != 0;
        Out.RotationRate     = RotationRate;
        ToFloats(BeamEnd, Out.BeamEnd);
        Out.BeamSegments     = uint32_t(std::clamp<int32_t>(BeamSegments, 1, int32_t(PARTICLE_BEAM_SEGMENTS_MAX)));
        Out.BeamNoise        = std::max(BeamNoise, 0.0f);
        Out.BeamNoiseSpeed   = std::max(BeamNoiseSpeed, 0.0f);
        Out.Seed             = uint32_t(Seed);
        return Out;
    }

    float ParticleEmitterDesc::GetSteadyStateCount() const
    {
        const ParticleEmitterSettings S = ToSettings();
        if(S.Type == PT_Beam)
        {
            return float(S.BeamSegments + 1);
        }
        const float MeanLife = S.Lifetime * (1.0f - 0.5f * S.LifetimeVariance);
        if(S.SpawnMode == PSM_Burst)
        {
            /* Bursts overlap when their particles outlive the interval. */
            const float Overlap = S.BurstInterval > 0.0f ? std::max(1.0f, std::ceil(MeanLife / S.BurstInterval)) : 1.0f;
            const float Cycles  = S.BurstInterval > 0.0f && S.BurstCycles > 0 ? std::min(Overlap, float(S.BurstCycles)) : Overlap;
            return std::min(float(S.BurstCount) * Cycles, float(S.MaxParticles));
        }
        return std::min(S.SpawnRate * MeanLife, float(S.MaxParticles));
    }

    float ParticleEmitterDesc::GetReach() const
    {
        const ParticleEmitterSettings S = ToSettings();
        if(S.Type == PT_Beam)
        {
            return Length(S.BeamEnd) + S.BeamNoise + std::max(S.StartSize, S.EndSize);
        }
        const float Life = S.Lifetime;
        return S.EmitterRadius + (Length(S.Velocity) + S.VelocitySpread) * Life
             + 0.5f * (Length(S.Acceleration) + S.Turbulence) * Life * Life
             + std::max(S.StartSize, S.EndSize) * (S.Billboard == PB_VelocityAligned ? 2.0f : 1.0f)
             + (S.Billboard == PB_VelocityAligned ? (Length(S.Velocity) + S.VelocitySpread) * S.VelocityStretch : 0.0f);
    }

    const char* ParticleEmitterDesc::GetMeshPath() const
    {
        if(Mesh[0] != '\0') { return Mesh; }
        return Type == PT_Mesh ? PARTICLE_DEFAULT_SHAPE_MESH : PARTICLE_DEFAULT_MESH;
    }

    namespace
    {
        const char* const TYPE_NAMES[PT_Count]         = { "Sprite", "Mesh", "Ribbon", "Beam" };
        const char* const BILLBOARD_NAMES[PB_Count]    = { "FaceCamera", "Vertical", "VelocityAligned", "Fixed" };
        const char* const ORIENTATION_NAMES[PMO_Count] = { "Fixed", "RandomSpin", "AlignToVelocity" };
        const char* const SPAWN_NAMES[PSM_Count]       = { "Continuous", "Burst" };
    }

    const char* GetParticleTypeName(const uint8_t InType)
    {
        return InType < PT_Count ? TYPE_NAMES[InType] : "Sprite";
    }

    // === Text =================================================================

    namespace
    {
        constexpr const char* HEADER_KEY  = "NimbleParticleSystem";
        constexpr const char* EMITTER_KEY = "Emitter";

        std::string Trim(const std::string& InText)
        {
            const size_t First = InText.find_first_not_of(" \t\r");
            if(First == std::string::npos) { return std::string(); }
            const size_t Last = InText.find_last_not_of(" \t\r");
            return InText.substr(First, Last - First + 1);
        }

        bool ToFloat(const std::string& InText, float& OutValue)
        {
            char* End = nullptr;
            const double Value = std::strtod(InText.c_str(), &End);
            if(InText.empty() || End != InText.c_str() + InText.size() || !std::isfinite(Value))
            {
                return false;
            }
            OutValue = float(Value);
            return true;
        }

        bool ToInt(const std::string& InText, int32_t& OutValue)
        {
            const char* End = InText.c_str() + InText.size();
            const std::from_chars_result Result = std::from_chars(InText.c_str(), End, OutValue);
            return !InText.empty() && Result.ec == std::errc() && Result.ptr == End;
        }

        /* "a,b,c" into exactly InCount parts. */
        bool Split(const std::string& InText, std::vector<std::string>& OutParts, const size_t InCount)
        {
            OutParts.clear();
            size_t Start = 0;
            while(true)
            {
                const size_t Comma = InText.find(',', Start);
                OutParts.push_back(Trim(InText.substr(Start, Comma == std::string::npos ? std::string::npos : Comma - Start)));
                if(Comma == std::string::npos) { break; }
                Start = Comma + 1;
            }
            return OutParts.size() == InCount;
        }

        bool ToVector(const std::string& InText, EngineCore::Vector3& OutValue)
        {
            std::vector<std::string> Parts;
            float X = 0.0f, Y = 0.0f, Z = 0.0f;
            if(!Split(InText, Parts, 3) || !ToFloat(Parts[0], X) || !ToFloat(Parts[1], Y) || !ToFloat(Parts[2], Z))
            {
                return false;
            }
            OutValue = EngineCore::Vector3(X, Y, Z);
            return true;
        }

        bool ToColor(const std::string& InText, EngineCore::Color& OutValue)
        {
            std::vector<std::string> Parts;
            int32_t Channels[4] = {};
            if(!Split(InText, Parts, 4)) { return false; }
            for(int Index = 0; Index < 4; ++Index)
            {
                if(!ToInt(Parts[Index], Channels[Index]) || Channels[Index] < 0 || Channels[Index] > 255) { return false; }
            }
            OutValue = EngineCore::Color(uint8_t(Channels[0]), uint8_t(Channels[1]), uint8_t(Channels[2]), uint8_t(Channels[3]));
            return true;
        }

        bool ToFlag(const std::string& InText, uint8_t& OutValue)
        {
            if(InText == "true")  { OutValue = 1; return true; }
            if(InText == "false") { OutValue = 0; return true; }
            return false;
        }

        /* A choice by its name, one of InCount. */
        bool ToChoice(const std::string& InText, const char* const* InNames, const size_t InCount, uint8_t& OutValue)
        {
            for(size_t Index = 0; Index < InCount; ++Index)
            {
                if(InText == InNames[Index])
                {
                    OutValue = uint8_t(Index);
                    return true;
                }
            }
            return false;
        }

        template<size_t N>
        bool ToFixed(const std::string& InText, char (&OutValue)[N])
        {
            if(InText.size() >= N) { return false; }
            std::memset(OutValue, 0, N);
            std::memcpy(OutValue, InText.data(), InText.size());
            return true;
        }

        std::string FormatFloat(const float InValue)
        {
            char Buffer[32];
            const std::to_chars_result Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), InValue);
            return std::string(Buffer, Result.ptr);
        }

        std::string FormatVector(const EngineCore::Vector3& InValue)
        {
            return FormatFloat(float(InValue.GetX())) + "," + FormatFloat(float(InValue.GetY())) + "," + FormatFloat(float(InValue.GetZ()));
        }

        std::string FormatColor(const EngineCore::Color InValue)
        {
            return std::to_string(InValue.R) + "," + std::to_string(InValue.G) + "," + std::to_string(InValue.B) + ","
                 + std::to_string(InValue.A);
        }

        /*
         * Every setting, by its key, in the order written: how to read it and
         * how to write it. One table, so the reader and the writer cannot drift.
         * A key with no writer is only read: an older file's.
         */
        struct EmitterKey
        {
            const char* Key;
            bool        (*Read)(const std::string& InValue, ParticleEmitterDesc& OutEmitter);
            std::string (*Write)(const ParticleEmitterDesc& InEmitter);
        };

#define NM_PARTICLE_FLOAT(Name, Member)                                                                         \
        { Name, [](const std::string& V, ParticleEmitterDesc& E) { return ToFloat(V, E.Member); },               \
                [](const ParticleEmitterDesc& E) { return FormatFloat(E.Member); } }
#define NM_PARTICLE_INT(Name, Member)                                                                           \
        { Name, [](const std::string& V, ParticleEmitterDesc& E) { return ToInt(V, E.Member); },                 \
                [](const ParticleEmitterDesc& E) { return std::to_string(E.Member); } }
#define NM_PARTICLE_VECTOR(Name, Member)                                                                        \
        { Name, [](const std::string& V, ParticleEmitterDesc& E) { return ToVector(V, E.Member); },              \
                [](const ParticleEmitterDesc& E) { return FormatVector(E.Member); } }
#define NM_PARTICLE_COLOR(Name, Member)                                                                         \
        { Name, [](const std::string& V, ParticleEmitterDesc& E) { return ToColor(V, E.Member); },               \
                [](const ParticleEmitterDesc& E) { return FormatColor(E.Member); } }
#define NM_PARTICLE_FLAG(Name, Member)                                                                          \
        { Name, [](const std::string& V, ParticleEmitterDesc& E) { return ToFlag(V, E.Member); },                \
                [](const ParticleEmitterDesc& E) { return std::string(E.Member ? "true" : "false"); } }
#define NM_PARTICLE_CHOICE(Name, Member, Names)                                                                 \
        { Name, [](const std::string& V, ParticleEmitterDesc& E) { return ToChoice(V, Names, std::size(Names), E.Member); }, \
                [](const ParticleEmitterDesc& E) { return std::string(E.Member < std::size(Names) ? Names[E.Member] : Names[0]); } }
#define NM_PARTICLE_STRING(Name, Member)                                                                        \
        { Name, [](const std::string& V, ParticleEmitterDesc& E) { return ToFixed(V, E.Member); },               \
                [](const ParticleEmitterDesc& E) { return std::string(E.Member); } }

        const EmitterKey EMITTER_KEYS[] = {
            NM_PARTICLE_FLAG  ("Enabled",          bEnabled),
            NM_PARTICLE_CHOICE("Type",             Type, TYPE_NAMES),
            NM_PARTICLE_STRING("Mesh",             Mesh),
            NM_PARTICLE_STRING("Material",         Material),
            NM_PARTICLE_VECTOR("Offset",           Offset),
            NM_PARTICLE_CHOICE("Billboard",        Billboard, BILLBOARD_NAMES),
            NM_PARTICLE_FLOAT ("VelocityStretch",  VelocityStretch),
            NM_PARTICLE_CHOICE("MeshOrientation",  MeshOrientation, ORIENTATION_NAMES),
            NM_PARTICLE_FLAG  ("RandomRotation",   bRandomRotation),
            NM_PARTICLE_FLOAT ("RotationRate",     RotationRate),
            NM_PARTICLE_CHOICE("SpawnMode",        SpawnMode, SPAWN_NAMES),
            NM_PARTICLE_INT   ("MaxParticles",     MaxParticles),
            NM_PARTICLE_FLOAT ("SpawnRate",        SpawnRate),
            NM_PARTICLE_INT   ("BurstCount",       BurstCount),
            NM_PARTICLE_FLOAT ("BurstInterval",    BurstInterval),
            NM_PARTICLE_INT   ("BurstCycles",      BurstCycles),
            NM_PARTICLE_FLOAT ("Lifetime",         Lifetime),
            NM_PARTICLE_FLOAT ("LifetimeVariance", LifetimeVariance),
            NM_PARTICLE_FLOAT ("EmitterRadius",    EmitterRadius),
            NM_PARTICLE_INT   ("Seed",             Seed),
            NM_PARTICLE_VECTOR("Velocity",         Velocity),
            NM_PARTICLE_FLOAT ("VelocitySpread",   VelocitySpread),
            NM_PARTICLE_VECTOR("Acceleration",     Acceleration),
            NM_PARTICLE_FLOAT ("Drag",             Drag),
            NM_PARTICLE_FLOAT ("Turbulence",       Turbulence),
            NM_PARTICLE_VECTOR("BeamEnd",          BeamEnd),
            NM_PARTICLE_INT   ("BeamSegments",     BeamSegments),
            NM_PARTICLE_FLOAT ("BeamNoise",        BeamNoise),
            NM_PARTICLE_FLOAT ("BeamNoiseSpeed",   BeamNoiseSpeed),
            NM_PARTICLE_FLOAT ("StartSize",        StartSize),
            NM_PARTICLE_FLOAT ("EndSize",          EndSize),
            NM_PARTICLE_COLOR ("StartColor",       StartColor),
            NM_PARTICLE_COLOR ("EndColor",         EndColor),
            NM_PARTICLE_FLOAT ("ColorCurve",       ColorCurve),
            NM_PARTICLE_FLOAT ("FadeIn",           FadeIn),
            NM_PARTICLE_FLOAT ("FadeOut",          FadeOut),

            /* Before billboards: true is Face Camera, false Fixed. */
            { "FaceCamera", [](const std::string& V, ParticleEmitterDesc& E)
              {
                  uint8_t bFace = 1;
                  if(!ToFlag(V, bFace)) { return false; }
                  E.Billboard = bFace ? PB_FaceCamera : PB_Fixed;
                  return true;
              }, nullptr },
        };

#undef NM_PARTICLE_FLOAT
#undef NM_PARTICLE_INT
#undef NM_PARTICLE_VECTOR
#undef NM_PARTICLE_COLOR
#undef NM_PARTICLE_FLAG
#undef NM_PARTICLE_STRING
#undef NM_PARTICLE_CHOICE
    }

    bool ParseParticleSystem(const std::string& InText, ParticleSystemDesc& OutSystem, std::string& OutError)
    {
        OutSystem = ParticleSystemDesc();
        OutError.clear();

        bool   bHeader = false;
        size_t LineNumber = 0;
        size_t Start = 0;
        while(Start <= InText.size())
        {
            const size_t End = InText.find('\n', Start);
            const std::string Line = Trim(InText.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
            Start = End == std::string::npos ? InText.size() + 1 : End + 1;
            ++LineNumber;
            if(Line.empty() || Line[0] == '#')
            {
                continue;
            }

            const size_t Space = Line.find_first_of(" \t");
            const std::string Key   = Line.substr(0, Space);
            const std::string Value = Space == std::string::npos ? std::string() : Trim(Line.substr(Space));
            const auto Fail = [&](const std::string& InWhy)
            {
                OutError = "line " + std::to_string(LineNumber) + ": " + InWhy;
                OutSystem = ParticleSystemDesc();
                return false;
            };

            if(!bHeader)
            {
                int32_t Format = 0;
                if(Key != HEADER_KEY || !ToInt(Value, Format))
                {
                    return Fail("not a particle system (no NimbleParticleSystem line)");
                }
                if(Format < 1 || uint32_t(Format) > PARTICLE_SYSTEM_FORMAT)
                {
                    return Fail("format " + Value + " is newer than this engine reads");
                }
                bHeader = true;
                continue;
            }
            if(Key == EMITTER_KEY)
            {
                if(OutSystem.Emitters.size() >= PARTICLE_SYSTEM_EMITTERS_MAX)
                {
                    return Fail("more than " + std::to_string(PARTICLE_SYSTEM_EMITTERS_MAX) + " emitters");
                }
                ParticleEmitterDesc Emitter;
                if(!ToFixed(Value, Emitter.Name))
                {
                    return Fail("emitter name too long");
                }
                OutSystem.Emitters.push_back(Emitter);
                continue;
            }
            if(OutSystem.Emitters.empty())
            {
                return Fail("'" + Key + "' before the first Emitter");
            }
            for(const EmitterKey& Each : EMITTER_KEYS)
            {
                if(Key == Each.Key)
                {
                    if(!Each.Read(Value, OutSystem.Emitters.back()))
                    {
                        return Fail("cannot read " + Key + " '" + Value + "'");
                    }
                    break;
                }
            }
        }
        if(!bHeader)
        {
            OutError = "empty: not a particle system";
            return false;
        }
        return true;
    }

    std::string WriteParticleSystem(const ParticleSystemDesc& InSystem)
    {
        std::string Out = std::string(HEADER_KEY) + " " + std::to_string(PARTICLE_SYSTEM_FORMAT) + "\n";
        for(const ParticleEmitterDesc& Emitter : InSystem.Emitters)
        {
            Out += "\n" + std::string(EMITTER_KEY) + " " + Emitter.Name + "\n";
            for(const EmitterKey& Each : EMITTER_KEYS)
            {
                if(!Each.Write) { continue; }
                const std::string Value = Each.Write(Emitter);
                Out += "    " + std::string(Each.Key) + (Value.empty() ? std::string() : " " + Value) + "\n";
            }
        }
        return Out;
    }

    bool LoadParticleSystemFile(const std::string& InPath, ParticleSystemDesc& OutSystem)
    {
        std::string Text, Error;
        EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
        if(!Provider || !Provider->ReadAllText(InPath, Text))
        {
            RENDERCORE_LOG_WARN("particle system '" + InPath + "' cannot be read")
            return false;
        }
        if(!ParseParticleSystem(Text, OutSystem, Error))
        {
            RENDERCORE_LOG_WARN("particle system '" + InPath + "': " + Error)
            return false;
        }
        return true;
    }

#if WITH_EDITOR
    bool SaveParticleSystemFile(const std::string& InFilePath, const ParticleSystemDesc& InSystem)
    {
        const std::string Temporary = InFilePath + ".tmp";
        {
            std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
            if(!File)
            {
                return false;
            }
            const std::string Text = WriteParticleSystem(InSystem);
            File.write(Text.data(), std::streamsize(Text.size()));
            if(!File.good())
            {
                return false;
            }
        }
        std::error_code Error;
        std::filesystem::rename(Temporary, InFilePath, Error);
        if(Error)
        {
            std::filesystem::remove(Temporary, Error);
            return false;
        }
        return true;
    }
#endif

    std::shared_ptr<const ParticleSystemDesc> GetDefaultParticleSystem()
    {
        static const std::shared_ptr<const ParticleSystemDesc> Default = []()
        {
            auto System = std::make_shared<ParticleSystemDesc>();
            System->Emitters.emplace_back();
            return std::shared_ptr<const ParticleSystemDesc>(std::move(System));
        }();
        return Default;
    }

    // === ParticleSystemCache ==================================================

    namespace
    {
        struct ParticleCacheState
        {
            std::mutex                                                                  Lock;
            std::unordered_map<std::string, std::shared_ptr<const ParticleSystemDesc>> Systems;   // null: failed
        };

        ParticleCacheState& GetParticleCache()
        {
            static ParticleCacheState State;
            return State;
        }
    }

    std::shared_ptr<const ParticleSystemDesc> ParticleSystemCache::Find(const std::string& InPath)
    {
        if(InPath.empty())
        {
            return nullptr;
        }
        ParticleCacheState& Cache = GetParticleCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Found = Cache.Systems.find(InPath);
            if(Found != Cache.Systems.end())
            {
                return Found->second;
            }
        }

        /* Loaded outside the lock; if two threads race, the first stored wins.
         * A failure is remembered too, so it warns once. */
        std::shared_ptr<const ParticleSystemDesc> Loaded;
        auto System = std::make_shared<ParticleSystemDesc>();
        if(LoadParticleSystemFile(InPath, *System))
        {
            Loaded = std::move(System);
        }
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Systems.emplace(InPath, Loaded).first->second;
    }

    void ParticleSystemCache::Store(const std::string& InPath, std::shared_ptr<const ParticleSystemDesc> InSystem)
    {
        if(InPath.empty())
        {
            return;
        }
        ParticleCacheState& Cache = GetParticleCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Systems[InPath] = std::move(InSystem);
    }

    void ParticleSystemCache::Invalidate(const std::string& InPath)
    {
        ParticleCacheState& Cache = GetParticleCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        if(InPath.empty())
        {
            Cache.Systems.clear();
            return;
        }
        Cache.Systems.erase(InPath);
    }
}
