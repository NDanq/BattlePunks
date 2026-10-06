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

    uint8_t ParticleEmitterDesc::InferModules() const
    {
        const auto IsZero = [](const EngineCore::Vector3& InVector)
        {
            return InVector.GetX() == 0.0 && InVector.GetY() == 0.0 && InVector.GetZ() == 0.0;
        };
        uint8_t Out = 0;
        if(Type == PT_Beam)
        {
            return Out;   // a beam uses none of them
        }
        if(Shape != PS_Sphere || EmitterRadius != 0.0f || ShapeSpeed != 0.0f || bShapeSurface) { Out |= PEM_Shape; }
        if(!IsZero(Velocity) || VelocitySpread != 0.0f)                                         { Out |= PEM_Velocity; }
        if(!IsZero(Acceleration) || Drag != 0.0f || Turbulence != 0.0f)                         { Out |= PEM_Forces; }
        if(bRandomRotation || RotationRate != 0.0f)                                              { Out |= PEM_Rotation; }
        return Out;
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
        Out.Shape            = Shape < PS_Count ? Shape : uint8_t(PS_Sphere);
        Out.bShapeSurface    = bShapeSurface != 0;
        Out.BoxSize[0]       = std::max(float(BoxSize.GetX()), 0.0f);
        Out.BoxSize[1]       = std::max(float(BoxSize.GetY()), 0.0f);
        Out.BoxSize[2]       = std::max(float(BoxSize.GetZ()), 0.0f);
        Out.ConeAngle        = std::clamp(ConeAngle, 0.0f, 180.0f);
        Out.RingWidth        = std::max(RingWidth, 0.0f);
        Out.ShapeSpeed       = ShapeSpeed;

        /* A module left out is at rest, whatever its settings hold. */
        if(!HasModule(PEM_Shape))
        {
            Out.Shape         = PS_Sphere;
            Out.EmitterRadius = 0.0f;
            Out.bShapeSurface = false;
            Out.ShapeSpeed    = 0.0f;
        }
        if(!HasModule(PEM_Velocity))
        {
            Out.Velocity[0] = Out.Velocity[1] = Out.Velocity[2] = 0.0f;
            Out.VelocitySpread = 0.0f;
        }
        if(!HasModule(PEM_Forces))
        {
            Out.Acceleration[0] = Out.Acceleration[1] = Out.Acceleration[2] = 0.0f;
            Out.Drag       = 0.0f;
            Out.Turbulence = 0.0f;
        }
        if(!HasModule(PEM_Rotation))
        {
            Out.bRandomRotation = false;
            Out.RotationRate    = 0.0f;
        }
        if(HasModule(PEM_Collision) && Type != PT_Beam)
        {
            Out.CollisionResponse = CollisionResponse < PCR_Count ? CollisionResponse : uint8_t(PCR_Bounce);
            Out.CollisionPlaneY   = CollisionHeight;   // the entity's height is added where it is known (the component)
            Out.Bounce            = std::clamp(Bounce, 0.0f, 1.0f);
            Out.Friction          = std::clamp(Friction, 0.0f, 1.0f);
        }
        if(HasModule(PEM_Flipbook) && (Type == PT_Sprite || Type == PT_Mesh))
        {
            Out.FlipbookColumns = uint32_t(std::clamp<int32_t>(FlipbookColumns, 1, 64));
            Out.FlipbookRows    = uint32_t(std::clamp<int32_t>(FlipbookRows, 1, 64));
            const uint32_t All  = Out.FlipbookColumns * Out.FlipbookRows;
            Out.FlipbookFrames  = FlipbookFrames > 0 ? std::min(uint32_t(FlipbookFrames), All) : All;
            Out.FlipbookRate    = std::max(FlipbookRate, 0.0f);
            Out.FlipbookCycles  = std::max(FlipbookCycles, 0.0f);
            Out.bFlipbookRandomStart = bFlipbookRandomStart != 0;
        }

        /* A gradient or curve, baked as the GPU samples it. */
        if((ColorMode == POL_Keys && ColorKeyCount > 0) || (SizeMode == POL_Keys && SizeKeyCount > 0))
        {
            float Color[4];
            float Size = 0.0f;
            for(uint32_t Index = 0; Index < PARTICLE_LUT_SIZE; ++Index)
            {
                Evaluate(float(Index) / float(PARTICLE_LUT_SIZE - 1), Color, Size);
                for(int Channel = 0; Channel < 4; ++Channel) { Out.ColorLut[Index][Channel] = Color[Channel]; }
                Out.SizeLut[Index] = Size;
            }
            Out.bColorLut = true;
            Out.bSizeLut  = true;
        }
        return Out;
    }

    namespace
    {
        float SmoothStep(const float InEdge0, const float InEdge1, const float InX)
        {
            const float T = std::clamp((InX - InEdge0) / (InEdge1 - InEdge0), 0.0f, 1.0f);
            return T * T * (3.0f - 2.0f * T);
        }
    }

    void ParticleEmitterSettings::BakeStartEnd(float (&OutColor)[PARTICLE_LUT_SIZE][4], float (&OutSize)[PARTICLE_LUT_SIZE]) const
    {
        const float In  = std::clamp(FadeIn, 0.0f, 1.0f);
        const float Out = std::clamp(FadeOut, 0.0f, 1.0f);
        const float Curve = std::clamp(ColorCurve, 0.05f, 20.0f);
        for(uint32_t Index = 0; Index < PARTICLE_LUT_SIZE; ++Index)
        {
            const float Age = float(Index) / float(PARTICLE_LUT_SIZE - 1);
            const float T   = std::pow(Age, Curve);
            for(int Channel = 0; Channel < 4; ++Channel)
            {
                OutColor[Index][Channel] = StartColor[Channel] + (EndColor[Channel] - StartColor[Channel]) * T;
            }
            /* No fade is no fade: an alpha of 1 right to birth and death. */
            const float Rise = In > 0.0f ? SmoothStep(0.0f, In, Age) : 1.0f;
            const float Fall = Out > 0.0f ? 1.0f - SmoothStep(1.0f - Out, 1.0f, Age) : 1.0f;
            OutColor[Index][3] *= Rise * Fall;
            OutSize[Index] = std::max(StartSize + (EndSize - StartSize) * Age, 0.0f);
        }
    }

    void ParticleEmitterDesc::Evaluate(const float InAge, float (&OutColor)[4], float& OutSize) const
    {
        const float Age = std::clamp(InAge, 0.0f, 1.0f);

        /* Start to End, as the settings bake it, at the nearest samples. */
        ParticleEmitterSettings Plain;
        ToFloats(StartColor, Plain.StartColor);
        ToFloats(EndColor, Plain.EndColor);
        Plain.StartSize  = StartSize;
        Plain.EndSize    = EndSize;
        Plain.ColorCurve = ColorCurve;
        Plain.FadeIn     = FadeIn;
        Plain.FadeOut    = FadeOut;
        const float At   = Age * float(PARTICLE_LUT_SIZE - 1);
        const uint32_t Low  = std::min(uint32_t(At), PARTICLE_LUT_SIZE - 1);
        const uint32_t High = std::min(Low + 1, PARTICLE_LUT_SIZE - 1);
        const float Blend = At - float(Low);
        float Colors[PARTICLE_LUT_SIZE][4];
        float Sizes[PARTICLE_LUT_SIZE];
        Plain.BakeStartEnd(Colors, Sizes);
        for(int Channel = 0; Channel < 4; ++Channel)
        {
            OutColor[Channel] = Colors[Low][Channel] + (Colors[High][Channel] - Colors[Low][Channel]) * Blend;
        }
        OutSize = Sizes[Low] + (Sizes[High] - Sizes[Low]) * Blend;

        /* Through the keys, straight between them, held before the first and after the last. */
        const int32_t ColorCount = std::clamp<int32_t>(ColorKeyCount, 0, int32_t(PARTICLE_CURVE_KEYS_MAX));
        if(ColorMode == POL_Keys && ColorCount > 0)
        {
            int32_t After = 0;
            while(After < ColorCount && ColorKeys[After].Time < Age) { ++After; }
            const ParticleColorKey& A = ColorKeys[std::max(After - 1, 0)];
            const ParticleColorKey& B = ColorKeys[std::min(After, ColorCount - 1)];
            const float Span = B.Time - A.Time;
            const float T    = Span > 1e-6f ? std::clamp((Age - A.Time) / Span, 0.0f, 1.0f) : 0.0f;
            float CA[4], CB[4];
            ToFloats(A.Color, CA);
            ToFloats(B.Color, CB);
            for(int Channel = 0; Channel < 4; ++Channel) { OutColor[Channel] = CA[Channel] + (CB[Channel] - CA[Channel]) * T; }
        }
        const int32_t SizeCount = std::clamp<int32_t>(SizeKeyCount, 0, int32_t(PARTICLE_CURVE_KEYS_MAX));
        if(SizeMode == POL_Keys && SizeCount > 0)
        {
            int32_t After = 0;
            while(After < SizeCount && SizeKeys[After].Time < Age) { ++After; }
            const ParticleSizeKey& A = SizeKeys[std::max(After - 1, 0)];
            const ParticleSizeKey& B = SizeKeys[std::min(After, SizeCount - 1)];
            const float Span = B.Time - A.Time;
            const float T    = Span > 1e-6f ? std::clamp((Age - A.Time) / Span, 0.0f, 1.0f) : 0.0f;
            OutSize = std::max(A.Value + (B.Value - A.Value) * T, 0.0f);
        }
    }

    void ParticleEmitterDesc::SeedColorKeys()
    {
        /* Start, End, and where a fade ends or begins. */
        const float Times[4] = { 0.0f, std::clamp(FadeIn, 0.0f, 1.0f), 1.0f - std::clamp(FadeOut, 0.0f, 1.0f), 1.0f };
        ColorKeyCount = 0;
        float Last = -1.0f;
        for(const float Time : Times)
        {
            if(Time <= Last + 1e-4f) { continue; }
            Last = Time;
            float Color[4];
            float Size = 0.0f;
            const uint8_t Mode = ColorMode;
            ColorMode = POL_StartEnd;
            Evaluate(Time, Color, Size);
            ColorMode = Mode;
            ParticleColorKey& Key = ColorKeys[ColorKeyCount++];
            Key.Time  = Time;
            Key.Color = EngineCore::Color(uint8_t(std::lround(Color[0] * 255.0f)), uint8_t(std::lround(Color[1] * 255.0f)),
                                          uint8_t(std::lround(Color[2] * 255.0f)), uint8_t(std::lround(Color[3] * 255.0f)));
        }
    }

    void ParticleEmitterDesc::SeedSizeKeys()
    {
        SizeKeys[0] = ParticleSizeKey{ 0.0f, StartSize };
        SizeKeys[1] = ParticleSizeKey{ 1.0f, EndSize };
        SizeKeyCount = 2;
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
        const float Shape = S.Shape == PS_Box ? 0.5f * Length(S.BoxSize) : S.EmitterRadius + 0.5f * S.RingWidth;
        float Largest = std::max(S.StartSize, S.EndSize);
        if(S.bSizeLut) { for(const float Each : S.SizeLut) { Largest = std::max(Largest, Each); } }
        return Shape + (Length(S.Velocity) + S.VelocitySpread + std::abs(S.ShapeSpeed)) * Life
             + 0.5f * (Length(S.Acceleration) + S.Turbulence) * Life * Life
             + Largest * (S.Billboard == PB_VelocityAligned ? 2.0f : 1.0f)
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
        const char* const SHAPE_NAMES[PS_Count]        = { "Sphere", "Hemisphere", "Box", "Cone", "Ring" };
        const char* const SIZE_MODE_NAMES[POL_Count]   = { "StartEnd", "Curve" };
        const char* const COLOR_MODE_NAMES[POL_Count]  = { "StartEnd", "Gradient" };
        const char* const COLLISION_NAMES[PCR_Count]   = { "None", "Bounce", "Kill" };
    }

    const char* GetParticleModuleName(const uint8_t InModule)
    {
        switch(InModule)
        {
            case PEM_Shape:    return "Shape";
            case PEM_Velocity: return "Velocity";
            case PEM_Forces:   return "Forces";
            case PEM_Rotation: return "Rotation";
            case PEM_Collision: return "Collision";
            case PEM_Flipbook: return "Flipbook";
            default:           return nullptr;
        }
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

        /* "t:v t:v ...", at most PARTICLE_CURVE_KEYS_MAX, each time 0-1; sorted by time. InValue reads one v. */
        template<typename TKey, typename FValue>
        bool ToKeys(const std::string& InText, TKey (&OutKeys)[PARTICLE_CURVE_KEYS_MAX], int32_t& OutCount, FValue&& InValue)
        {
            OutCount = 0;
            size_t Start = 0;
            while(Start < InText.size())
            {
                const size_t Space = InText.find(' ', Start);
                const std::string Token = InText.substr(Start, Space == std::string::npos ? std::string::npos : Space - Start);
                Start = Space == std::string::npos ? InText.size() : Space + 1;
                if(Token.empty()) { continue; }
                const size_t Colon = Token.find(':');
                if(Colon == std::string::npos || OutCount >= int32_t(PARTICLE_CURVE_KEYS_MAX)) { return false; }
                TKey Key;
                if(!ToFloat(Token.substr(0, Colon), Key.Time) || Key.Time < 0.0f || Key.Time > 1.0f
                   || !InValue(Token.substr(Colon + 1), Key))
                {
                    return false;
                }
                OutKeys[OutCount++] = Key;
            }
            std::stable_sort(OutKeys, OutKeys + OutCount, [](const TKey& A, const TKey& B) { return A.Time < B.Time; });
            return true;
        }

        template<typename TKey, typename FValue>
        std::string FormatKeys(const TKey (&InKeys)[PARTICLE_CURVE_KEYS_MAX], const int32_t InCount, FValue&& InValue)
        {
            std::string Out;
            for(int32_t Index = 0; Index < std::clamp<int32_t>(InCount, 0, int32_t(PARTICLE_CURVE_KEYS_MAX)); ++Index)
            {
                Out += (Out.empty() ? "" : " ") + FormatFloat(InKeys[Index].Time) + ":" + InValue(InKeys[Index]);
            }
            return Out;
        }

        /* "Shape Forces": the named modules, in any order; none for an empty list. */
        bool ToModules(const std::string& InText, ParticleEmitterDesc& OutEmitter)
        {
            OutEmitter.Modules = 0;
            size_t Start = 0;
            while(Start < InText.size())
            {
                const size_t Space = InText.find(' ', Start);
                const std::string Name = InText.substr(Start, Space == std::string::npos ? std::string::npos : Space - Start);
                Start = Space == std::string::npos ? InText.size() : Space + 1;
                if(Name.empty()) { continue; }
                bool bKnown = false;
                for(uint8_t Bit = 1; Bit != 0 && Bit <= PEM_Flipbook; Bit = uint8_t(Bit << 1))
                {
                    if(Name == GetParticleModuleName(Bit)) { OutEmitter.Modules |= Bit; bKnown = true; }
                }
                if(!bKnown) { return false; }
            }
            return true;
        }

        std::string FormatModules(const ParticleEmitterDesc& InEmitter)
        {
            std::string Out;
            for(uint8_t Bit = 1; Bit != 0 && Bit <= PEM_Flipbook; Bit = uint8_t(Bit << 1))
            {
                if(InEmitter.HasModule(Bit)) { Out += (Out.empty() ? "" : " ") + std::string(GetParticleModuleName(Bit)); }
            }
            return Out;
        }

        bool ToSizeKeys(const std::string& InText, ParticleEmitterDesc& OutEmitter)
        {
            return ToKeys(InText, OutEmitter.SizeKeys, OutEmitter.SizeKeyCount,
                          [](const std::string& InValue, ParticleSizeKey& OutKey) { return ToFloat(InValue, OutKey.Value); });
        }

        bool ToColorKeys(const std::string& InText, ParticleEmitterDesc& OutEmitter)
        {
            return ToKeys(InText, OutEmitter.ColorKeys, OutEmitter.ColorKeyCount,
                          [](const std::string& InValue, ParticleColorKey& OutKey) { return ToColor(InValue, OutKey.Color); });
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
            { "Modules", &ToModules, &FormatModules },
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
            NM_PARTICLE_CHOICE("Shape",            Shape, SHAPE_NAMES),
            NM_PARTICLE_FLAG  ("ShapeSurface",     bShapeSurface),
            NM_PARTICLE_VECTOR("BoxSize",          BoxSize),
            NM_PARTICLE_FLOAT ("ConeAngle",        ConeAngle),
            NM_PARTICLE_FLOAT ("RingWidth",        RingWidth),
            NM_PARTICLE_FLOAT ("ShapeSpeed",       ShapeSpeed),
            NM_PARTICLE_FLOAT ("SpawnRate",        SpawnRate),
            NM_PARTICLE_INT   ("BurstCount",       BurstCount),
            NM_PARTICLE_FLOAT ("BurstInterval",    BurstInterval),
            NM_PARTICLE_INT   ("BurstCycles",      BurstCycles),
            NM_PARTICLE_FLOAT ("Lifetime",         Lifetime),
            NM_PARTICLE_FLOAT ("LifetimeVariance", LifetimeVariance),
            NM_PARTICLE_FLOAT ("EmitterRadius",    EmitterRadius),
            NM_PARTICLE_INT   ("Seed",             Seed),
            NM_PARTICLE_FLOAT ("CullDistance",     CullDistance),
            NM_PARTICLE_FLAG  ("PauseOffscreen",   bPauseOffscreen),
            NM_PARTICLE_VECTOR("Velocity",         Velocity),
            NM_PARTICLE_FLOAT ("VelocitySpread",   VelocitySpread),
            NM_PARTICLE_VECTOR("Acceleration",     Acceleration),
            NM_PARTICLE_FLOAT ("Drag",             Drag),
            NM_PARTICLE_FLOAT ("Turbulence",       Turbulence),
            NM_PARTICLE_CHOICE("CollisionResponse", CollisionResponse, COLLISION_NAMES),
            NM_PARTICLE_FLOAT ("CollisionHeight",  CollisionHeight),
            NM_PARTICLE_FLOAT ("Bounce",           Bounce),
            NM_PARTICLE_FLOAT ("Friction",         Friction),
            NM_PARTICLE_INT   ("FlipbookColumns",  FlipbookColumns),
            NM_PARTICLE_INT   ("FlipbookRows",     FlipbookRows),
            NM_PARTICLE_INT   ("FlipbookFrames",   FlipbookFrames),
            NM_PARTICLE_FLOAT ("FlipbookRate",     FlipbookRate),
            NM_PARTICLE_FLOAT ("FlipbookCycles",   FlipbookCycles),
            NM_PARTICLE_FLAG  ("FlipbookRandomStart", bFlipbookRandomStart),
            NM_PARTICLE_VECTOR("BeamEnd",          BeamEnd),
            NM_PARTICLE_INT   ("BeamSegments",     BeamSegments),
            NM_PARTICLE_FLOAT ("BeamNoise",        BeamNoise),
            NM_PARTICLE_FLOAT ("BeamNoiseSpeed",   BeamNoiseSpeed),
            NM_PARTICLE_CHOICE("SizeMode",         SizeMode, SIZE_MODE_NAMES),
            NM_PARTICLE_FLOAT ("StartSize",        StartSize),
            NM_PARTICLE_FLOAT ("EndSize",          EndSize),
            { "SizeKeys", &ToSizeKeys, [](const ParticleEmitterDesc& E)
              { return FormatKeys(E.SizeKeys, E.SizeKeyCount, [](const ParticleSizeKey& K) { return FormatFloat(K.Value); }); } },
            NM_PARTICLE_CHOICE("ColorMode",        ColorMode, COLOR_MODE_NAMES),
            NM_PARTICLE_COLOR ("StartColor",       StartColor),
            NM_PARTICLE_COLOR ("EndColor",         EndColor),
            NM_PARTICLE_FLOAT ("ColorCurve",       ColorCurve),
            NM_PARTICLE_FLOAT ("FadeIn",           FadeIn),
            NM_PARTICLE_FLOAT ("FadeOut",          FadeOut),
            { "ColorKeys", &ToColorKeys, [](const ParticleEmitterDesc& E)
              { return FormatKeys(E.ColorKeys, E.ColorKeyCount, [](const ParticleColorKey& K) { return FormatColor(K.Color); }); } },

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
        std::vector<bool> ModulesNamed;   // each emitter's: whether it had a Modules line
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
                ModulesNamed.push_back(false);
                continue;
            }
            if(OutSystem.Emitters.empty())
            {
                return Fail("'" + Key + "' before the first Emitter");
            }
            if(Key == "Modules") { ModulesNamed.back() = true; }
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
        /* Written before modules: the ones its settings use. */
        for(size_t Index = 0; Index < OutSystem.Emitters.size(); ++Index)
        {
            if(!ModulesNamed[Index]) { OutSystem.Emitters[Index].Modules = OutSystem.Emitters[Index].InferModules(); }
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
