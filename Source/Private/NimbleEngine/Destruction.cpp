#include "Public/NimbleEngine/Destruction.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/ScriptLibrary.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/MeshFracture.h"

#if WITH_JOLTPHYSICS
#include "Public/JoltPhysics/PhysicsWorld.h"
#endif

/* Destruction.h has the contract. */

NIMBLE_CONFIG_INT(GDestructionMaxDebris, "Destruction", "MaxDebris", 256, nullptr);              // fallen pieces alive at once
NIMBLE_CONFIG_FLOAT(GDestructionCosmeticVolume, "Destruction", "CosmeticVolume", 1000.0f, nullptr); // cubic units: smaller pieces are cosmetic
NIMBLE_CONFIG_INT(GDestructionDebrisLayer, "Destruction", "DebrisLayer", 1, nullptr);            // a cosmetic piece's Layer (bits)
NIMBLE_CONFIG_INT(GDestructionDebrisMask, "Destruction", "DebrisMask", -1, nullptr);             // and what it collides with (bits)
NIMBLE_CONFIG_INT(GDestructionMaxCachedCuts, "Destruction", "MaxCachedCuts", 16, nullptr);       // runtime fractures kept for reuse with no piece alive

namespace NimbleEngine
{
    using EngineCore::Vector3;
    using GameCore::TransformMath::EulerToRotation;
    using GameCore::TransformMath::Transposed;

    namespace
    {
        // === Small math =========================================================

        Vector3 V(const double X, const double Y, const double Z) { return Vector3(X, Y, Z); }
        Vector3 Add(const Vector3& A, const Vector3& B)   { return V(A.GetX() + B.GetX(), A.GetY() + B.GetY(), A.GetZ() + B.GetZ()); }
        Vector3 Sub(const Vector3& A, const Vector3& B)   { return V(A.GetX() - B.GetX(), A.GetY() - B.GetY(), A.GetZ() - B.GetZ()); }
        Vector3 Scale(const Vector3& A, const double S)   { return V(A.GetX() * S, A.GetY() * S, A.GetZ() * S); }
        Vector3 Mul(const Vector3& A, const Vector3& B)   { return V(A.GetX() * B.GetX(), A.GetY() * B.GetY(), A.GetZ() * B.GetZ()); }
        double  Dot(const Vector3& A, const Vector3& B)   { return A.GetX() * B.GetX() + A.GetY() * B.GetY() + A.GetZ() * B.GetZ(); }
        double  Len(const Vector3& A)                     { return std::sqrt(Dot(A, A)); }
        Vector3 Cross(const Vector3& A, const Vector3& B)
        {
            return V(A.GetY() * B.GetZ() - A.GetZ() * B.GetY(), A.GetZ() * B.GetX() - A.GetX() * B.GetZ(), A.GetX() * B.GetY() - A.GetY() * B.GetX());
        }
        double SafeDiv(const double A, const double B) { return std::fabs(B) > 1e-12 ? A / B : 0.0; }

        bool SameName(const char* A, const char* B)
        {
            for(; *A && *B; ++A, ++B)
            {
                if(std::tolower(static_cast<unsigned char>(*A)) != std::tolower(static_cast<unsigned char>(*B))) { return false; }
            }
            return *A == *B;
        }

        /* A world point into the entity's mesh space, and back. */
        Vector3 ToLocal(const GameCore::TransformPose& InPose, const Vector3& InWorld)
        {
            const Vector3 Unrotated = Transposed(EulerToRotation(InPose.Rotation)) * Sub(InWorld, InPose.Position);
            return V(SafeDiv(Unrotated.GetX(), InPose.Scale.GetX()), SafeDiv(Unrotated.GetY(), InPose.Scale.GetY()),
                     SafeDiv(Unrotated.GetZ(), InPose.Scale.GetZ()));
        }
        Vector3 ToWorld(const GameCore::TransformPose& InPose, const Vector3& InLocal)
        {
            return Add(InPose.Position, EulerToRotation(InPose.Rotation) * Mul(InPose.Scale, InLocal));
        }

        uint32_t HashBytes(const void* InData, const size_t InSize, uint32_t InHash = 2166136261u)
        {
            const unsigned char* Bytes = static_cast<const unsigned char*>(InData);
            for(size_t I = 0; I < InSize; ++I) { InHash = (InHash ^ Bytes[I]) * 16777619u; }
            return InHash;
        }

        float UnitsPerMeter()
        {
            const EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Physics", "UnitsPerMeter");
            return Var && Var->GetFloat() > 0.0f ? Var->GetFloat() : 100.0f;
        }

        // === Materials ==========================================================

        DestructionMaterial Preset(const char* InName, const uint8_t InPattern, const int32_t InPieces, const float InStrength,
                                   const float InDensity, const float InFriction, const float InLifetime, const float InScatter)
        {
            DestructionMaterial M;
            M.Name           = InName;
            M.Pattern        = InPattern;
            M.Pieces         = InPieces;
            M.Strength       = InStrength;
            M.Health         = InStrength * 3.0f;
            M.Density        = InDensity;
            M.Friction       = InFriction;
            M.DebrisLifetime = InLifetime;
            M.Scatter        = InScatter;
            return M;
        }

        const std::vector<DestructionMaterial>& BuiltInMaterials()
        {
            static const std::vector<DestructionMaterial> Materials = []()
            {
                std::vector<DestructionMaterial> Out;
                Out.push_back(Preset("Concrete", RenderCore::FP_Uniform,   14, 15000.0f, 2400.0f, 0.8f,  8.0f, 200.0f));
                Out.push_back(Preset("Wood",     RenderCore::FP_Splinter,  10,  6000.0f,  600.0f, 0.6f, 10.0f, 250.0f));
                Out.push_back(Preset("Glass",    RenderCore::FP_Radial,    24,  1500.0f, 2500.0f, 0.3f,  4.0f, 350.0f));
                Out.push_back(Preset("Stone",    RenderCore::FP_Clustered, 12, 20000.0f, 2600.0f, 0.8f, 12.0f, 150.0f));
                Out.push_back(Preset("Metal",    RenderCore::FP_Uniform,    6, 40000.0f, 7800.0f, 0.5f, 15.0f, 100.0f));
                Out[2].Restitution = 0.2f;
                Out[2].BreakRadius = 0.5f;
                Out[1].BreakRadius = 0.3f;
                return Out;
            }();
            return Materials;
        }

        uint8_t ParsePattern(const char* InText, const uint8_t InFallback)
        {
            static const char* const Names[] = { "Uniform", "Clustered", "Splinter", "Radial" };
            for(uint8_t I = 0; I < 4; ++I)
            {
                if(SameName(InText, Names[I])) { return I; }
            }
            return InFallback;
        }

        /* Every [Destruction_<Name>] entry over the built-ins. */
        struct ConfigMaterials
        {
            std::vector<DestructionMaterial> All;

            DestructionMaterial& Named(const std::string& InName)
            {
                for(DestructionMaterial& Each : All)
                {
                    if(SameName(Each.Name.c_str(), InName.c_str())) { return Each; }
                }
                DestructionMaterial Added = BuiltInMaterials().front();   // a new one starts as Concrete
                Added.Name = InName;
                All.push_back(Added);
                return All.back();
            }

            static void Visit(const char* InSection, const char* InKey, const char* InValue, void* InUserData)
            {
                ConfigMaterials& Self = *static_cast<ConfigMaterials*>(InUserData);
                const std::string Section = InSection;
                const size_t      Dot     = Section.find('_');
                if(Dot == std::string::npos || Dot + 1 >= Section.size()) { return; }
                DestructionMaterial& M = Self.Named(Section.substr(Dot + 1));
                const std::string Key = InKey;
                const float Number = float(std::atof(InValue));
                if(Key == "Pattern")               { M.Pattern = ParsePattern(InValue, M.Pattern); }
                else if(Key == "Pieces")           { M.Pieces = std::max(1, std::atoi(InValue)); }
                else if(Key == "GrainStretch")     { M.GrainStretch = std::max(1.0f, Number); }
                else if(Key == "Strength")         { M.Strength = Number; }
                else if(Key == "Health")           { M.Health = Number; }
                else if(Key == "Density")          { M.Density = Number; }
                else if(Key == "Friction")         { M.Friction = Number; }
                else if(Key == "Restitution")      { M.Restitution = Number; }
                else if(Key == "Scatter")          { M.Scatter = Number; }
                else if(Key == "BreakRadius")      { M.BreakRadius = Number; }
                else if(Key == "DebrisLifetime")   { M.DebrisLifetime = Number; }
                else if(Key == "InteriorMaterial") { M.InteriorMaterial = InValue; }
                else if(Key == "BreakParticles")   { M.BreakParticles = InValue; }
                else if(Key == "BreakSound")       { M.BreakSound = InValue; }
                else { CORE_LOG_WARN(std::string("[Destruction] unknown key ") + InKey + " in [" + InSection + "]") }
            }

            ConfigMaterials()
            {
                All = BuiltInMaterials();
                EngineCore::ConfigForEachEntry("Destruction_", &Visit, this);
            }
        };

        /* The component's own numbers over its material's. */
        float StrengthOf(const DestructibleComponent& InC, const DestructionMaterial& InM) { return InC.Strength > 0.0f ? InC.Strength : InM.Strength; }
        float HealthOf(const DestructibleComponent& InC, const DestructionMaterial& InM)   { return InC.Health > 0.0f ? InC.Health : InM.Health; }

        DestructionMaterial MaterialOf(const DestructibleComponent& InC)
        {
            DestructionMaterial M;
            if(!FindDestructionMaterial(InC.Material, M)) { FindDestructionMaterial("Concrete", M); }
            return M;
        }
    }

    bool FindDestructionMaterial(const char* InName, DestructionMaterial& OutMaterial)
    {
        if(!InName) { return false; }
        const ConfigMaterials Materials;
        for(const DestructionMaterial& Each : Materials.All)
        {
            if(SameName(Each.Name.c_str(), InName))
            {
                OutMaterial = Each;
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> GetDestructionMaterialNames()
    {
        std::vector<std::string> Out;
        for(const DestructionMaterial& Each : ConfigMaterials().All) { Out.push_back(Each.Name); }
        return Out;
    }

    // === The component ============================================================

    void DestructibleComponent::Break(const Vector3& InPoint, const Vector3& InImpulse)
    {
        if(!bEnabled || (bBroken && !bStructural)) { return; }
        const GameCore::Entity Self = Owner.GetEntity();
        if(!Self.IsValid()) { return; }
        /* A client waits for its server's break of anything networked. */
        if(IsClient() && Self.HasComponent<GameCore::NetComponent>()) { return; }

        uint32_t Chosen = uint32_t(Seed);
        if(Chosen == 0)
        {
            const GameCore::EntityGuid Guid = Self.GetGuid();
            Chosen = HashBytes(&Guid, sizeof(Guid)) ^ (BreakCount * 2654435761u);
            Chosen = Chosen != 0 ? Chosen : 1u;
        }
        GAMECORE_RPC(DestructibleComponent, MulticastBreak)(Self, InPoint, InImpulse, Chosen);
    }

    void DestructibleComponent::ApplyDamage(const Vector3& InPoint, const Vector3& InImpulse)
    {
        if(!bEnabled || (bBroken && !bStructural) || bPendingBreak) { return; }
        const DestructionMaterial M = MaterialOf(*this);
        const float Amount   = float(Len(InImpulse));
        const float Strength = StrengthOf(*this, M);
        if(bBroken)
        {
            /* What a structure has left breaks to half its strength. */
            if(Amount >= 0.5f * Strength) { Break(InPoint, InImpulse); }
            return;
        }
        if(Amount >= Strength) { Break(InPoint, InImpulse); return; }
        Damage += Amount;
        if(Damage >= HealthOf(*this, M)) { Break(InPoint, InImpulse); }
    }

    void DestructibleComponent::MulticastBreak(const Vector3& InPoint, const Vector3& InImpulse, const uint32_t InSeed)
    {
        bPendingBreak  = 1;
        PendingPoint   = InPoint;
        PendingImpulse = InImpulse;
        PendingSeed    = InSeed;
    }

    // === The system ===============================================================

    namespace
    {
        /* One fracture of one mesh: the pieces' meshes (in memory or files)
         * and what is known of each. Shared by every break that cut the
         * same mesh the same way. */
        struct CutSet
        {
            std::string              Key;
            bool                     bInMemory = true;
            std::vector<std::string> Paths;
            std::vector<Vector3>     Centers;    // mesh space
            std::vector<float>       Volumes;    // mesh units^3
            std::vector<Vector3>     BoundsMin;  // each piece's own, about its centre
            std::vector<Vector3>     BoundsMax;
            std::vector<std::vector<uint32_t>> Neighbours;
            float    TotalVolume = 0.0f;
            uint32_t Users       = 0;            // pieces alive
            uint64_t LastUsed    = 0;
        };

        /* A piece made by a break: a standing part of a structure, a falling
         * piece of debris, or the break's particle burst. */
        struct PieceRecord
        {
            GameCore::EntityID      Owner;
            std::shared_ptr<CutSet> Cut;
            uint32_t                Index    = 0;
            bool                    bDebris  = false;   // falling, ageing
            bool                    bEffect  = false;   // a particle burst, not a piece
            float                   Age      = 0.0f;
            float                   Lifetime = 8.0f;
            float                   Asleep   = 0.0f;
            float                   Shrink   = -1.0f;   // >= 0 while shrinking away
            Vector3                 BaseScale;
            uint64_t                Order    = 0;       // when it fell: the oldest go first
        };

        /* A Structural destructible after its first break. */
        struct Structure
        {
            std::shared_ptr<CutSet>         Cut;
            std::vector<GameCore::EntityID> Pieces;     // by piece; unset once fallen
            std::vector<uint8_t>            Anchor;     // rests on the base
            GameCore::TransformPose         Pose;
            float                           Size = 1.0f;
        };
    }

    struct DestructionWorld::Impl
    {
        explicit Impl(GameCore::World& InWorld) : World(InWorld) {}

        GameCore::World& World;
        int Attachments = 0;

        std::map<std::string, std::shared_ptr<CutSet>> Cuts;
        std::unordered_map<uint64_t, PieceRecord>       Pieces;       // by entity
        std::unordered_map<uint64_t, Structure>         Structures;   // by owner
        uint64_t Clock      = 0;
        uint64_t FellOrder  = 0;
        uint32_t CutCounter = 0;
        float    LastFractureMs = 0.0f;

        // --- Cuts ----------------------------------------------------------------

        std::shared_ptr<CutSet> CutFromResult(RenderCore::FractureResult& InResult, const std::string& InKey)
        {
            auto Set = std::make_shared<CutSet>();
            Set->Key = InKey;
            ++CutCounter;
            char Prefix[64];
            std::snprintf(Prefix, sizeof(Prefix), "@fracture/%p/%u/", static_cast<void*>(&World), CutCounter);
            const size_t Count = InResult.Pieces.size();
            Set->Neighbours.resize(Count);
            for(size_t I = 0; I < Count; ++I)
            {
                RenderCore::FracturePiece& Piece = InResult.Pieces[I];
                Set->Paths.push_back(Prefix + std::to_string(I));
                Set->Centers.push_back(V(Piece.Center[0], Piece.Center[1], Piece.Center[2]));
                Set->Volumes.push_back(Piece.Volume);
                Set->BoundsMin.push_back(V(Piece.Mesh.BoundsMin[0], Piece.Mesh.BoundsMin[1], Piece.Mesh.BoundsMin[2]));
                Set->BoundsMax.push_back(V(Piece.Mesh.BoundsMax[0], Piece.Mesh.BoundsMax[1], Piece.Mesh.BoundsMax[2]));
                Set->TotalVolume += Piece.Volume;
                RenderCore::MeshAssetCache::Store(Set->Paths.back(), std::make_shared<const RenderCore::MeshData>(std::move(Piece.Mesh)));
            }
            for(const auto& Pair : InResult.Neighbours)
            {
                Set->Neighbours[Pair.first].push_back(Pair.second);
                Set->Neighbours[Pair.second].push_back(Pair.first);
            }
            return Set;
        }

        /* A baked .nmfrac: "NimbleFracture 1", then per piece
         * "Piece <path> <cx> <cy> <cz> <volume>", then "Neighbour <a> <b>". */
        std::shared_ptr<CutSet> LoadBaked(const std::string& InPath)
        {
            const std::string Key = "B:" + InPath;
            if(const auto Found = Cuts.find(Key); Found != Cuts.end()) { return Found->second; }

            /* The provider first, then the platform layer, as LoadArchiveFromFile does. */
            std::string Text;
            bool bRead = false;
            if(EngineCore::IFileProvider* Files = EngineCore::GetFileProvider(); Files && Files->Exists(InPath))
            {
                bRead = Files->ReadAllText(InPath, Text);
            }
            if(!bRead)
            {
                std::vector<uint8_t> Bytes;
                bRead = EngineCore::appFileReadAll(InPath, Bytes);
                Text.assign(Bytes.begin(), Bytes.end());
            }
            if(!bRead)
            {
                CORE_LOG_WARN("[Destruction] cannot read the fracture " + InPath)
                return nullptr;
            }
            auto Set = std::make_shared<CutSet>();
            Set->Key       = Key;
            Set->bInMemory = false;
            std::istringstream Lines(Text);
            std::string Word;
            while(Lines >> Word)
            {
                if(Word == "Piece")
                {
                    std::string Path;
                    double X = 0, Y = 0, Z = 0;
                    float  Volume = 0.0f;
                    Lines >> Path >> X >> Y >> Z >> Volume;
                    Set->Paths.push_back(Path);
                    Set->Centers.push_back(V(X, Y, Z));
                    Set->Volumes.push_back(Volume);
                    Set->TotalVolume += Volume;
                    Vector3 Min, Max;
                    if(const std::shared_ptr<const RenderCore::MeshData> Mesh = RenderCore::MeshAssetCache::Find(Path))
                    {
                        Min = V(Mesh->BoundsMin[0], Mesh->BoundsMin[1], Mesh->BoundsMin[2]);
                        Max = V(Mesh->BoundsMax[0], Mesh->BoundsMax[1], Mesh->BoundsMax[2]);
                    }
                    Set->BoundsMin.push_back(Min);
                    Set->BoundsMax.push_back(Max);
                }
                else if(Word == "Neighbour")
                {
                    uint32_t A = 0, B = 0;
                    Lines >> A >> B;
                    Set->Neighbours.resize(Set->Paths.size());
                    if(A < Set->Paths.size() && B < Set->Paths.size())
                    {
                        Set->Neighbours[A].push_back(B);
                        Set->Neighbours[B].push_back(A);
                    }
                }
                else
                {
                    std::string Rest;
                    std::getline(Lines, Rest);   // the header, or anything newer
                }
            }
            Set->Neighbours.resize(Set->Paths.size());
            if(Set->Paths.empty())
            {
                CORE_LOG_WARN("[Destruction] the fracture " + InPath + " has no pieces")
                return nullptr;
            }
            Cuts[Key] = Set;
            return Set;
        }

        /* InEntity's mesh cut as its component and material say, around
         * InLocalHit; reused when an earlier break cut it the same way. */
        std::shared_ptr<CutSet> RuntimeCut(const char* InMesh, const DestructibleComponent& InC, const DestructionMaterial& InM,
                                           const uint32_t InSeed, const Vector3& InLocalHit, const std::string& InInterior)
        {
            RenderCore::FractureSettings Settings;
            Settings.Seed             = InC.Seed != 0 ? uint32_t(InC.Seed) : InSeed;
            Settings.PieceCount       = uint32_t(std::clamp(InC.Pieces > 0 ? InC.Pieces : InM.Pieces, 1, 64));
            Settings.Pattern          = InM.Pattern;
            Settings.GrainStretch     = InM.GrainStretch;
            Settings.InteriorMaterial = InInterior;
            const bool bAroundHit = InM.Pattern == RenderCore::FP_Clustered || InM.Pattern == RenderCore::FP_Radial;
            if(bAroundHit)
            {
                Settings.bHasImpact     = true;
                Settings.ImpactPoint[0] = float(InLocalHit.GetX());
                Settings.ImpactPoint[1] = float(InLocalHit.GetY());
                Settings.ImpactPoint[2] = float(InLocalHit.GetZ());
            }

            /* The hit, rounded to a tenth of a unit, only matters to the
             * patterns that use it. */
            char Key[512];
            std::snprintf(Key, sizeof(Key), "R:%s|%s|%u|%u|%u|%.2f|%s", InMesh, InM.Name.c_str(), Settings.Seed,
                          Settings.PieceCount, unsigned(Settings.Pattern), double(Settings.GrainStretch), InInterior.c_str());
            std::string FullKey = Key;
            if(bAroundHit)
            {
                std::snprintf(Key, sizeof(Key), "|%.1f,%.1f,%.1f", InLocalHit.GetX(), InLocalHit.GetY(), InLocalHit.GetZ());
                FullKey += Key;
            }
            if(const auto Found = Cuts.find(FullKey); Found != Cuts.end()) { return Found->second; }

            const std::shared_ptr<const RenderCore::MeshData> Mesh = RenderCore::MeshAssetCache::Find(InMesh);
            if(!Mesh) { return nullptr; }
            /* The cuts' UVs run 0-1 across the mesh, whatever its units:
             * the engine's unit Box scaled to a wall included. */
            const float Extent = std::max({ Mesh->BoundsMax[0] - Mesh->BoundsMin[0], Mesh->BoundsMax[1] - Mesh->BoundsMin[1],
                                            Mesh->BoundsMax[2] - Mesh->BoundsMin[2], 1e-3f });
            Settings.InteriorUVScale = 1.0f / Extent;
            RenderCore::FractureResult Result;
            const auto Start = std::chrono::steady_clock::now();
            const bool bCut = RenderCore::FractureMesh(*Mesh, Settings, Result);
            LastFractureMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - Start).count();
            if(!bCut || Result.Pieces.empty()) { return nullptr; }
            if(Result.OpenCaps > 0)
            {
                CORE_LOG_WARN(std::string("[Destruction] ") + InMesh + " is not closed: " + std::to_string(Result.OpenCaps) + " cuts left open")
            }
            std::shared_ptr<CutSet> Set = CutFromResult(Result, FullKey);
            Cuts[FullKey] = Set;
            return Set;
        }

        void ReleaseCut(const std::shared_ptr<CutSet>& InCut)
        {
            if(!InCut || InCut->Users == 0) { return; }
            --InCut->Users;
            InCut->LastUsed = Clock;
            TrimCuts();
        }

        /* Unused runtime cuts beyond [Destruction] MaxCachedCuts go, oldest first. */
        void TrimCuts()
        {
            const size_t Keep = size_t(std::max(0, GDestructionMaxCachedCuts.GetInt()));
            for(;;)
            {
                size_t Unused = 0;
                auto   Oldest = Cuts.end();
                for(auto It = Cuts.begin(); It != Cuts.end(); ++It)
                {
                    if(It->second->Users != 0 || !It->second->bInMemory) { continue; }
                    ++Unused;
                    if(Oldest == Cuts.end() || It->second->LastUsed < Oldest->second->LastUsed) { Oldest = It; }
                }
                if(Unused <= Keep || Oldest == Cuts.end()) { return; }
                FreeCut(*Oldest->second);
                Cuts.erase(Oldest);
            }
        }

        void FreeCut(const CutSet& InCut)
        {
            if(!InCut.bInMemory) { return; }
#if WITH_JOLTPHYSICS
            JoltPhysics::PhysicsWorld* Physics = JoltPhysics::PhysicsWorld::Find(World);
#endif
            for(const std::string& Path : InCut.Paths)
            {
                RenderCore::MeshAssetCache::Remove(Path);
                ForgetCollisionMesh(Path.c_str());
#if WITH_JOLTPHYSICS
                if(Physics) { Physics->ForgetMeshShapes(Path.c_str()); }
#endif
            }
        }

        // --- Pieces --------------------------------------------------------------

        GameCore::Entity SpawnPiece(const GameCore::Entity& InSource, const std::shared_ptr<CutSet>& InCut, const uint32_t InIndex,
                                    const GameCore::TransformPose& InPose, const RenderCore::RenderComponent& InRender,
                                    const GameCore::RigidBodyComponent* InBody, const DestructionMaterial& InM,
                                    const float InTotalMass, const bool bInStanding)
        {
            char Name[160];
            std::snprintf(Name, sizeof(Name), "%s.Piece%02u", InSource.GetName(), InIndex);
            GameCore::Entity Piece = World.CreateEntity(Name);
            Piece.AddComponent<GameCore::TransformComponent>();
            GameCore::TransformPose Pose = InPose;
            Pose.Position = ToWorld(InPose, InCut->Centers[InIndex]);
            Piece.SetWorldPose(Pose);

            RenderCore::RenderComponent Render;
            std::memcpy(Render.Materials, InRender.Materials, sizeof(Render.Materials));
            std::memcpy(Render.Parameters, InRender.Parameters, sizeof(Render.Parameters));
            std::strncpy(Render.Mesh, InCut->Paths[InIndex].c_str(), sizeof(Render.Mesh) - 1);
            Render.bCastShadows     = InRender.bCastShadows;
            Render.CustomPassGroups = InRender.CustomPassGroups;
            Piece.AddComponent(Render);

            const double WorldVolume = double(InCut->Volumes[InIndex]) * std::fabs(InPose.Scale.GetX() * InPose.Scale.GetY() * InPose.Scale.GetZ());
            GameCore::RigidBodyComponent Body;
            Body.Motion         = bInStanding ? uint8_t(GameCore::RBM_Static) : uint8_t(GameCore::RBM_Dynamic);
            Body.Shape          = GameCore::RBS_ConvexHull;
            Body.ShapeSource    = GameCore::RBSS_Mesh;
            Body.Mass           = std::max(0.05f, InTotalMass * InCut->Volumes[InIndex] / std::max(InCut->TotalVolume, 1e-6f));
            Body.Friction       = InM.Friction;
            Body.Restitution    = InM.Restitution;
            Body.AngularDamping = 0.2f;
            if(InBody)
            {
                Body.Layer = InBody->Layer;
                Body.Mask  = InBody->Mask;
            }
            if(!bInStanding && WorldVolume < double(GDestructionCosmeticVolume.GetFloat()))
            {
                Body.Layer = GDestructionDebrisLayer.GetInt();
                Body.Mask  = GDestructionDebrisMask.GetInt();
            }
            Piece.AddComponent(Body);

            PieceRecord Record;
            Record.Owner     = InSource.GetID();
            Record.Cut       = InCut;
            Record.Index     = InIndex;
            Record.Lifetime  = InM.DebrisLifetime;
            Record.BaseScale = Pose.Scale;
            ++InCut->Users;
            Pieces[Piece.GetID().Pack()] = Record;
            return Piece;
        }

        /* A piece starts falling: thrown from the hit, with the whole's motion. */
        void Fall(const GameCore::EntityID InPiece, const Vector3& InVelocity, const Vector3& InSpin, const float InLifetime)
        {
            GameCore::Entity Piece = World.GetEntity(InPiece);
            auto Found = Pieces.find(InPiece.Pack());
            if(!Piece.IsAlive() || Found == Pieces.end()) { return; }
            PieceRecord& Record = Found->second;
            Record.bDebris  = true;
            Record.Lifetime = InLifetime;
            Record.Order    = ++FellOrder;
            if(GameCore::RigidBodyComponent* Body = Piece.GetComponent<GameCore::RigidBodyComponent>())
            {
                Body->Motion = GameCore::RBM_Dynamic;
                Body->InitialLinearVelocity  = InVelocity;
                Body->InitialAngularVelocity = InSpin;
                Body->SetLinearVelocity(InVelocity);
                Body->SetAngularVelocity(InSpin);
            }
        }

        void DestroyPiece(const uint64_t InKey)
        {
            const auto Found = Pieces.find(InKey);
            if(Found == Pieces.end()) { return; }
            const GameCore::EntityID Entity = GameCore::EntityID::Unpack(InKey);
            std::shared_ptr<CutSet> Cut = Found->second.Cut;
            Pieces.erase(Found);
            if(World.IsAlive(Entity)) { World.DestroyEntity(Entity); }
            ReleaseCut(Cut);
        }

        // --- Breaking ------------------------------------------------------------

        void DoBreak(GameCore::Entity InEntity, DestructibleComponent& InC, const Vector3& InPoint, const Vector3& InImpulse, const uint32_t InSeed)
        {
            const DestructionMaterial M = MaterialOf(InC);
            const float Lifetime = InC.DebrisLifetime > 0.0f ? InC.DebrisLifetime : M.DebrisLifetime;
            auto* Render = InEntity.GetComponent<RenderCore::RenderComponent>();
            auto* Body   = InEntity.GetComponent<GameCore::RigidBodyComponent>();
            const uint64_t Key = InEntity.GetID().Pack();
            auto Standing = Structures.find(Key);

            if(Standing == Structures.end())
            {
                /* The first break: cut it, and replace it with its pieces. */
                if(InC.bBroken) { return; }   // already broken, but nothing of it stands
                InC.bBroken = 1;
                if(!Render || Render->Mesh[0] == '\0')
                {
                    CORE_LOG_WARN(std::string("[Destruction] ") + InEntity.GetName() + " has no Render mesh to break")
                    return;
                }
                const GameCore::TransformPose Pose = InEntity.GetWorldPose();
                const Vector3 LocalHit = ToLocal(Pose, InPoint);

                std::string Interior = M.InteriorMaterial;
                if(Interior.empty()) { Interior = Render->GetMaterial(0); }
                if(Interior.empty())
                {
                    if(const std::shared_ptr<const RenderCore::MeshData> Mesh = RenderCore::MeshAssetCache::Find(Render->Mesh))
                    {
                        if(!Mesh->Sections.empty()) { Interior = Mesh->Sections[0].Material; }
                    }
                }
                std::shared_ptr<CutSet> Cut = InC.Mode == DMODE_Baked && InC.Fracture[0] != '\0'
                                            ? LoadBaked(InC.Fracture)
                                            : RuntimeCut(Render->Mesh, InC, M, InSeed, LocalHit, Interior);
                if(!Cut)
                {
                    CORE_LOG_WARN(std::string("[Destruction] ") + InEntity.GetName() + "'s mesh " + Render->Mesh + " could not be cut")
                    return;
                }
                Cut->LastUsed = ++Clock;

                /* Copies: spawning the pieces adds Render and Rigid Body
                 * components, which can move these pools. */
                const RenderCore::RenderComponent SourceRender = *Render;
                GameCore::RigidBodyComponent      SourceBody;
                const bool    bHasBody = Body != nullptr;
                if(bHasBody) { SourceBody = *Body; }
                const bool    bMoving  = bHasBody && Body->Motion == GameCore::RBM_Dynamic;
                const Vector3 Linear   = bMoving ? Body->LinearVelocity : V(0.0, 0.0, 0.0);
                const Vector3 Angular  = bMoving ? Body->AngularVelocity : V(0.0, 0.0, 0.0);

                const bool bStructural = InC.bStructural && (!Body || Body->Motion == GameCore::RBM_Static);
                const double ScaleVolume = std::fabs(Pose.Scale.GetX() * Pose.Scale.GetY() * Pose.Scale.GetZ());
                const float  UPM = UnitsPerMeter();
                const float  TotalMass = Body && Body->Mass > 0.0f
                                       ? Body->Mass
                                       : float(double(M.Density) * Cut->TotalVolume * ScaleVolume / (double(UPM) * UPM * UPM));

                Structure Built;
                Built.Cut  = Cut;
                Built.Pose = Pose;
                Vector3 Min = V(1e30, 1e30, 1e30), Max = V(-1e30, -1e30, -1e30);
                for(size_t I = 0; I < Cut->Paths.size(); ++I)
                {
                    const Vector3 Lo = Add(Cut->Centers[I], Cut->BoundsMin[I]), Hi = Add(Cut->Centers[I], Cut->BoundsMax[I]);
                    Min = V(std::min(Min.GetX(), Lo.GetX()), std::min(Min.GetY(), Lo.GetY()), std::min(Min.GetZ(), Lo.GetZ()));
                    Max = V(std::max(Max.GetX(), Hi.GetX()), std::max(Max.GetY(), Hi.GetY()), std::max(Max.GetZ(), Hi.GetZ()));
                }
                Built.Size = float(Len(Mul(Sub(Max, Min), Pose.Scale)));
                const double BaseTolerance = std::max(1e-3, (Max.GetY() - Min.GetY()) * 0.02);
                for(size_t I = 0; I < Cut->Paths.size(); ++I)
                {
                    const GameCore::Entity Piece = SpawnPiece(InEntity, Cut, uint32_t(I), Pose, SourceRender, bHasBody ? &SourceBody : nullptr,
                                                              M, TotalMass, bStructural);
                    Built.Pieces.push_back(Piece.GetID());
                    const double Bottom = Cut->Centers[I].GetY() + Cut->BoundsMin[I].GetY();
                    Built.Anchor.push_back(InC.bAnchored && Bottom - Min.GetY() <= BaseTolerance ? 1 : 0);
                }

                /* Hide the whole; its pieces take its place. */
                Render = InEntity.GetComponent<RenderCore::RenderComponent>();
                Body   = InEntity.GetComponent<GameCore::RigidBodyComponent>();
                if(Render) { Render->bVisible = 0; }
                if(Body)   { Body->bEnabled = 0; }
                if(!bStructural)
                {
                    /* Everything falls. */
                    for(size_t I = 0; I < Built.Pieces.size(); ++I)
                    {
                        Fall(Built.Pieces[I], ThrowVelocity(Built, uint32_t(I), InPoint, InImpulse, Linear, Angular, M, TotalMass, true),
                             Spin(Angular, uint32_t(I), InSeed), Lifetime);
                    }
                    Effects(InPoint, M, InSeed);
                    Told(InEntity);
                    return;
                }
                Standing = Structures.emplace(Key, std::move(Built)).first;
            }

            /* A structure: what is near the hit falls, then what that cut off. */
            Structure& S = Standing->second;
            const float  Radius = std::max(1.0f, M.BreakRadius * S.Size);
            const double UPM = UnitsPerMeter();
            const float  PieceMass = float(double(M.Density) * S.Cut->TotalVolume / std::max<size_t>(1, S.Pieces.size())
                                         * std::fabs(S.Pose.Scale.GetX() * S.Pose.Scale.GetY() * S.Pose.Scale.GetZ()) / (UPM * UPM * UPM));
            int32_t Nearest = -1;
            double  NearestDistance = 1e30;
            std::vector<uint32_t> Falling;
            for(size_t I = 0; I < S.Pieces.size(); ++I)
            {
                if(!S.Pieces[I].IsSet()) { continue; }
                const double Distance = Len(Sub(ToWorld(S.Pose, S.Cut->Centers[I]), InPoint));
                if(Distance < NearestDistance) { NearestDistance = Distance; Nearest = int32_t(I); }
                if(Distance <= Radius) { Falling.push_back(uint32_t(I)); }
            }
            if(Falling.empty() && Nearest >= 0) { Falling.push_back(uint32_t(Nearest)); }
            for(const uint32_t I : Falling)
            {
                Fall(S.Pieces[I], ThrowVelocity(S, I, InPoint, InImpulse, Vector3(), Vector3(), M, PieceMass * float(S.Pieces.size()), true),
                     Spin(Vector3(), I, InSeed), Lifetime);
                S.Pieces[I] = GameCore::EntityID();
            }

            /* Anything no longer joined to an anchor through standing pieces. */
            std::vector<uint8_t> Held(S.Pieces.size(), 0);
            std::vector<uint32_t> Open;
            for(size_t I = 0; I < S.Pieces.size(); ++I)
            {
                if(S.Pieces[I].IsSet() && S.Anchor[I]) { Held[I] = 1; Open.push_back(uint32_t(I)); }
            }
            while(!Open.empty())
            {
                const uint32_t At = Open.back();
                Open.pop_back();
                for(const uint32_t Next : S.Cut->Neighbours[At])
                {
                    if(!Held[Next] && S.Pieces[Next].IsSet()) { Held[Next] = 1; Open.push_back(Next); }
                }
            }
            for(size_t I = 0; I < S.Pieces.size(); ++I)
            {
                if(!S.Pieces[I].IsSet() || Held[I]) { continue; }
                Fall(S.Pieces[I], Vector3(), Spin(Vector3(), uint32_t(I), InSeed), Lifetime);
                S.Pieces[I] = GameCore::EntityID();
            }

            Effects(InPoint, M, InSeed);
            Told(InEntity);
        }

        Vector3 ThrowVelocity(const Structure& InS, const uint32_t InIndex, const Vector3& InPoint, const Vector3& InImpulse,
                              const Vector3& InLinear, const Vector3& InAngular, const DestructionMaterial& InM,
                              const float InMass, const bool bInScatter) const
        {
            const Vector3 Centre   = ToWorld(InS.Pose, InS.Cut->Centers[InIndex]);
            const Vector3 FromBody = Sub(Centre, InS.Pose.Position);
            const double  ToRad    = 3.14159265358979323846 / 180.0;
            Vector3 Out = Add(InLinear, Cross(Scale(InAngular, ToRad), FromBody));
            if(!bInScatter) { return Out; }
            const Vector3 Away    = Sub(Centre, InPoint);
            const double  Dist    = Len(Away);
            const double  Falloff = std::max(0.0, 1.0 - Dist / std::max(1.0, double(InS.Size)));
            if(Dist > 1e-6) { Out = Add(Out, Scale(Away, double(InM.Scatter) * Falloff / Dist)); }
            if(InMass > 0.0f) { Out = Add(Out, Scale(InImpulse, Falloff / double(InMass))); }
            return Out;
        }

        static Vector3 Spin(const Vector3& InAngular, const uint32_t InIndex, const uint32_t InSeed)
        {
            /* A little tumble of its own, the same on every machine. */
            uint32_t H = HashBytes(&InIndex, sizeof(InIndex), InSeed);
            auto Next = [&]() { H = H * 1664525u + 1013904223u; return double(H >> 8) / 16777216.0 * 2.0 - 1.0; };
            const double X = Next() * 90.0, Y = Next() * 90.0, Z = Next() * 90.0;
            return Add(InAngular, V(X, Y, Z));
        }

        void Effects(const Vector3& InPoint, const DestructionMaterial& InM, const uint32_t InSeed)
        {
            if(!InM.BreakParticles.empty())
            {
                GameCore::Entity Burst = World.CreateEntity("DestructionBurst");
                Burst.AddComponent<GameCore::TransformComponent>();
                GameCore::TransformPose Pose;
                Pose.Position = InPoint;
                Burst.SetWorldPose(Pose);
                RenderCore::ParticleSystemComponent Particles;
                std::strncpy(Particles.System, InM.BreakParticles.c_str(), sizeof(Particles.System) - 1);
                Particles.Seed = int32_t(InSeed & 0x7fffffff);
                Burst.AddComponent(Particles);
                PieceRecord Record;
                Record.bEffect  = true;
                Record.Lifetime = 4.0f;
                Pieces[Burst.GetID().Pack()] = Record;
            }
            if(!InM.BreakSound.empty()) { PlaySoundAt(World, InM.BreakSound.c_str(), InPoint); }
        }

        void Told(const GameCore::Entity& InEntity)
        {
            GameCore::EventArgs Args;
            Args.Args[0] = GameCore::EventArg::Make(InEntity.GetID());
            Args.Count   = 1;
            World.GetEvents().Emit("DestructibleBroken", Args);
            World.CallScriptMethod(InEntity.GetID(), "OnBroken", InEntity.GetID());
        }

        // --- The frame -----------------------------------------------------------

        /* The Destructible a piece came from, or the entity itself. */
        GameCore::EntityID OwnerOf(const GameCore::EntityID InEntity) const
        {
            const auto Found = InEntity.IsSet() ? Pieces.find(InEntity.Pack()) : Pieces.end();
            return Found != Pieces.end() ? Found->second.Owner : InEntity;
        }

        /* An impact on InEntity by InOther: its own Destructible, or the
         * structure it is part of. A structure's falling pieces landing on
         * what still stands of it break nothing more: the break that
         * dropped them already chose what falls. */
        void Hit(const GameCore::EntityID InEntity, const GameCore::EntityID InOther, const Vector3& InPoint, const Vector3& InImpulse)
        {
            if(!InEntity.IsSet() || !World.IsAlive(InEntity)) { return; }
            if(InOther.IsSet() && OwnerOf(InOther) == OwnerOf(InEntity)) { return; }
            GameCore::Entity Target = World.GetEntity(InEntity);
            if(auto* C = Target.GetComponent<DestructibleComponent>())
            {
                C->ApplyDamage(InPoint, InImpulse);
                return;
            }
            const auto Found = Pieces.find(InEntity.Pack());
            if(Found == Pieces.end() || Found->second.bDebris || !Structures.count(Found->second.Owner.Pack())) { return; }
            GameCore::Entity Owner = World.GetEntity(Found->second.Owner);
            if(auto* C = Owner.IsAlive() ? Owner.GetComponent<DestructibleComponent>() : nullptr)
            {
                C->ApplyDamage(InPoint, InImpulse);
            }
        }

        void Update(const float InDeltaSeconds)
        {
            ++Clock;
#if WITH_JOLTPHYSICS
            if(JoltPhysics::PhysicsWorld* Physics = JoltPhysics::PhysicsWorld::Find(World))
            {
                /* Copied: a break spawns bodies, but the list is the last step's. */
                const std::vector<JoltPhysics::PhysicsImpact> Impacts = Physics->GetImpacts();
                for(const JoltPhysics::PhysicsImpact& Each : Impacts)
                {
                    const Vector3 OnB = Scale(Each.Normal, Each.Impulse);
                    Hit(Each.A, Each.B, Each.Point, Scale(OnB, -1.0));
                    Hit(Each.B, Each.A, Each.Point, OnB);
                }
            }
#endif

            /* Breaks asked for since the last frame, everywhere. A late
             * joiner hides what its server broke before it came. */
            struct Pending { GameCore::EntityID Entity; Vector3 Point, Impulse; uint32_t Seed; };
            std::vector<Pending> Breaks;
            World.Each<DestructibleComponent>([&](GameCore::Entity InEntity, DestructibleComponent& C)
            {
                if(C.bPendingBreak)
                {
                    Breaks.push_back({ InEntity.GetID(), C.PendingPoint, C.PendingImpulse, C.PendingSeed });
                    C.bPendingBreak = 0;
                }
                else if(C.bBroken && !Structures.count(InEntity.GetID().Pack()))
                {
                    if(auto* Render = InEntity.GetComponent<RenderCore::RenderComponent>(); Render && Render->bVisible) { Render->bVisible = 0; }
                    if(auto* Body = InEntity.GetComponent<GameCore::RigidBodyComponent>(); Body && Body->bEnabled) { Body->bEnabled = 0; }
                }
            });
            for(const Pending& Each : Breaks)
            {
                GameCore::Entity Entity = World.GetEntity(Each.Entity);
                if(DestructibleComponent* C = Entity.IsAlive() ? Entity.GetComponent<DestructibleComponent>() : nullptr)
                {
                    DoBreak(Entity, *C, Each.Point, Each.Impulse, Each.Seed);
                    ++C->BreakCount;
                }
            }

            AgeDebris(InDeltaSeconds);

            /* A structure whose owner went takes its pieces with it. */
            for(auto It = Structures.begin(); It != Structures.end();)
            {
                if(World.IsAlive(GameCore::EntityID::Unpack(It->first))) { ++It; continue; }
                for(const GameCore::EntityID& Piece : It->second.Pieces)
                {
                    if(Piece.IsSet()) { DestroyPiece(Piece.Pack()); }
                }
                It = Structures.erase(It);
            }
        }

        void AgeDebris(const float InDeltaSeconds)
        {
            std::vector<uint64_t> Gone;
            std::vector<std::pair<uint64_t, uint64_t>> Live;   // (order, key) of falling pieces not yet shrinking
            for(auto& Entry : Pieces)
            {
                PieceRecord& Record = Entry.second;
                const GameCore::EntityID Entity = GameCore::EntityID::Unpack(Entry.first);
                if(!World.IsAlive(Entity)) { Gone.push_back(Entry.first); continue; }
                if(!Record.bDebris && !Record.bEffect) { continue; }
                Record.Age += InDeltaSeconds;
                if(Record.bEffect)
                {
                    if(Record.Age >= Record.Lifetime) { Gone.push_back(Entry.first); }
                    continue;
                }
                GameCore::Entity Piece = World.GetEntity(Entity);
                auto* Body = Piece.GetComponent<GameCore::RigidBodyComponent>();
                Record.Asleep = Body && Body->bEnabled && !Body->IsAwake() ? Record.Asleep + InDeltaSeconds : 0.0f;
                if(Record.Shrink < 0.0f)
                {
                    if(Record.Age >= Record.Lifetime || (Record.Asleep >= 2.0f && Record.Age >= 0.5f * Record.Lifetime))
                    {
                        StartShrink(Piece, Record);
                    }
                    else
                    {
                        Live.emplace_back(Record.Order, Entry.first);
                    }
                    continue;
                }
                Record.Shrink += InDeltaSeconds / 0.5f;
                if(Record.Shrink >= 1.0f) { Gone.push_back(Entry.first); continue; }
                if(auto* Transform = Piece.GetComponent<GameCore::TransformComponent>())
                {
                    Transform->Scale = Scale(Record.BaseScale, 1.0 - Record.Shrink);
                }
            }

            /* Over the cap: the oldest start going. */
            const size_t Max = size_t(std::max(0, GDestructionMaxDebris.GetInt()));
            if(Live.size() > Max)
            {
                std::sort(Live.begin(), Live.end());
                for(size_t I = 0; I + Max < Live.size(); ++I)
                {
                    auto Found = Pieces.find(Live[I].second);
                    if(Found != Pieces.end()) { StartShrink(World.GetEntity(GameCore::EntityID::Unpack(Live[I].second)), Found->second); }
                }
            }
            for(const uint64_t Key : Gone) { DestroyPiece(Key); }
        }

        static void StartShrink(GameCore::Entity InPiece, PieceRecord& InOutRecord)
        {
            InOutRecord.Shrink = 0.0f;
            /* It stops where it lies: a body rebuilt at every scale would jitter. */
            if(auto* Body = InPiece.GetComponent<GameCore::RigidBodyComponent>()) { Body->bEnabled = 0; }
        }

        void DestroyAll()
        {
            std::vector<uint64_t> Keys;
            for(const auto& Entry : Pieces) { Keys.push_back(Entry.first); }
            for(const uint64_t Key : Keys) { DestroyPiece(Key); }
            Structures.clear();
            for(const auto& Entry : Cuts) { FreeCut(*Entry.second); }
            Cuts.clear();
        }

        static void UpdateSystem(GameCore::World&, const float InDeltaSeconds, void* InUserData)
        {
            static_cast<Impl*>(InUserData)->Update(InDeltaSeconds);
        }
    };

    namespace
    {
        std::unordered_map<const GameCore::World*, DestructionWorld*>& Systems()
        {
            static std::unordered_map<const GameCore::World*, DestructionWorld*> Map;
            return Map;
        }
    }

    DestructionWorld::DestructionWorld(GameCore::World& InWorld) : P(new Impl(InWorld)) {}

    DestructionWorld::~DestructionWorld()
    {
        delete P;
    }

    DestructionWorld* DestructionWorld::Attach(GameCore::World& InWorld)
    {
        DestructionWorld*& System = Systems()[&InWorld];
        if(!System)
        {
            System = new DestructionWorld(InWorld);
            InWorld.RegisterSystem("Engine.Destruction", GameCore::TP_PostPhysics, &Impl::UpdateSystem, System->P);
        }
        ++System->P->Attachments;
        return System;
    }

    void DestructionWorld::Detach(GameCore::World& InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(&InWorld);
        if(Found == Map.end()) { return; }
        DestructionWorld* System = Found->second;
        if(--System->P->Attachments > 0) { return; }
        InWorld.UnregisterSystem(&Impl::UpdateSystem, System->P);
        System->P->DestroyAll();
        Map.erase(Found);
        delete System;
    }

    DestructionWorld* DestructionWorld::Find(const GameCore::World* InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(InWorld);
        return Found != Map.end() ? Found->second : nullptr;
    }

    void DestructionWorld::Update(const float InDeltaSeconds)
    {
        P->Update(InDeltaSeconds);
    }

    void DestructionWorld::GetStats(DestructionStats& OutStats) const
    {
        OutStats = DestructionStats();
        P->World.Each<DestructibleComponent>([&](GameCore::Entity, DestructibleComponent& C)
        {
            ++OutStats.Destructibles;
            OutStats.Broken += C.bBroken ? 1 : 0;
        });
        for(const auto& Entry : P->Pieces)
        {
            if(Entry.second.bEffect) { continue; }
            if(Entry.second.bDebris) { ++OutStats.Debris; }
            else                     { ++OutStats.Remnants; }
        }
        for(const auto& Entry : P->Cuts) { OutStats.CachedCuts += Entry.second->bInMemory ? 1 : 0; }
        OutStats.LastFractureMs = P->LastFractureMs;
    }

    int32_t RadialDamage(GameCore::World& InWorld, const Vector3& InCentre, const float InRadius, const float InImpulse)
    {
        if(InRadius <= 0.0f) { return 0; }
        struct Target { GameCore::EntityID Entity; Vector3 Push; Vector3 At; };
        std::vector<Target> Damaged, Pushed;
        InWorld.Each<DestructibleComponent>([&](GameCore::Entity InEntity, DestructibleComponent&)
        {
            const Vector3 At = InEntity.GetWorldPose().Position;
            const Vector3 Away = Sub(At, InCentre);
            const double  Dist = Len(Away);
            if(Dist > InRadius) { return; }
            const double Amount = double(InImpulse) * (1.0 - Dist / InRadius);
            const Vector3 Dir = Dist > 1e-6 ? Scale(Away, 1.0 / Dist) : V(0.0, 1.0, 0.0);
            /* The hit lands on the near side, so a runtime cut breaks finest there. */
            Damaged.push_back({ InEntity.GetID(), Scale(Dir, Amount), Add(InCentre, Scale(Away, 0.5)) });
        });
        InWorld.Each<GameCore::RigidBodyComponent>([&](GameCore::Entity InEntity, GameCore::RigidBodyComponent& Body)
        {
            if(!Body.bEnabled || Body.Motion != GameCore::RBM_Dynamic) { return; }
            const Vector3 Away = Sub(InEntity.GetWorldPose().Position, InCentre);
            const double  Dist = Len(Away);
            if(Dist > InRadius || Dist < 1e-6) { return; }
            Pushed.push_back({ InEntity.GetID(), Scale(Away, double(InImpulse) * (1.0 - Dist / InRadius) / Dist * 0.2), Vector3() });
        });
        for(const Target& Each : Pushed)
        {
            if(auto* Body = InWorld.GetEntity(Each.Entity).GetComponent<GameCore::RigidBodyComponent>()) { Body->AddImpulse(Each.Push); }
        }
        for(const Target& Each : Damaged)
        {
            if(auto* C = InWorld.GetEntity(Each.Entity).GetComponent<DestructibleComponent>()) { C->ApplyDamage(Each.At, Each.Push); }
        }
        return int32_t(Damaged.size());
    }

#if WITH_EDITOR
    bool BakeDestructible(GameCore::World& InWorld, const GameCore::EntityID InEntity, const std::string& InPath,
                          const std::string& InRuntimePath, std::string& OutError)
    {
        GameCore::Entity Entity = InWorld.GetEntity(InEntity);
        auto* C      = Entity.IsAlive() ? Entity.GetComponent<DestructibleComponent>() : nullptr;
        auto* Render = Entity.IsAlive() ? Entity.GetComponent<RenderCore::RenderComponent>() : nullptr;
        if(!C)                             { OutError = "it has no Destructible component"; return false; }
        if(!Render || !Render->Mesh[0])    { OutError = "it has no Render mesh"; return false; }
        const std::shared_ptr<const RenderCore::MeshData> Mesh = RenderCore::MeshAssetCache::Find(Render->Mesh);
        if(!Mesh)                          { OutError = std::string("cannot load ") + Render->Mesh; return false; }

        const DestructionMaterial M = MaterialOf(*C);
        RenderCore::FractureSettings Settings;
        Settings.Seed       = C->Seed != 0 ? uint32_t(C->Seed) : 1u;
        Settings.PieceCount = uint32_t(std::clamp(C->Pieces > 0 ? C->Pieces : M.Pieces, 1, 64));
        Settings.Pattern    = M.Pattern;
        Settings.GrainStretch = M.GrainStretch;
        Settings.InteriorMaterial = !M.InteriorMaterial.empty() ? M.InteriorMaterial : std::string(Render->GetMaterial(0));
        RenderCore::FractureResult Result;
        if(!RenderCore::FractureMesh(*Mesh, Settings, Result) || Result.Pieces.empty()) { OutError = "the mesh could not be cut"; return false; }

        /* Foo.nmfrac beside Foo.fracture/Piece_NN.nmmesh. */
        const std::string Stem        = InPath.substr(0, InPath.rfind('.'));
        const std::string RuntimeStem = InRuntimePath.substr(0, InRuntimePath.rfind('.'));
        std::ostringstream Text;
        Text << "NimbleFracture 1\n";
        Text << "Source " << Render->Mesh << " " << M.Name << " " << Settings.Seed << "\n";
        for(size_t I = 0; I < Result.Pieces.size(); ++I)
        {
            char Leaf[32];
            std::snprintf(Leaf, sizeof(Leaf), "/Piece_%02u.nmmesh", unsigned(I));
            RenderCore::FracturePiece& Piece = Result.Pieces[I];
            if(!RenderCore::SaveMeshFile(Stem + ".fracture" + Leaf, Piece.Mesh)) { OutError = "cannot write " + Stem + ".fracture" + Leaf; return false; }
            RenderCore::MeshAssetCache::Invalidate(RuntimeStem + ".fracture" + Leaf);
            Text << "Piece " << RuntimeStem << ".fracture" << Leaf << " " << Piece.Center[0] << " " << Piece.Center[1] << " "
                 << Piece.Center[2] << " " << Piece.Volume << "\n";
        }
        for(const auto& Pair : Result.Neighbours) { Text << "Neighbour " << Pair.first << " " << Pair.second << "\n"; }
        std::ofstream File(InPath, std::ios::binary | std::ios::trunc);
        if(!File) { OutError = "cannot write " + InPath; return false; }
        File << Text.str();

        C->Mode = DMODE_Baked;
        std::strncpy(C->Fracture, InRuntimePath.c_str(), sizeof(C->Fracture) - 1);
        C->Fracture[sizeof(C->Fracture) - 1] = '\0';
        return true;
    }
#endif
}

// === Registration =============================================================

using NimbleEngine::DestructibleComponent;

GAMECORE_DEFINE_COMPONENT(DestructibleComponent, "Destructible")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(DestructibleComponent, 1)
{
    Ar.Flag("Enabled",          C.bEnabled);
    Ar.Field("Mode",            C.Mode);
    Ar.Flag("Structural",       C.bStructural);
    Ar.Flag("Anchored",         C.bAnchored);
    Ar.Field("Material",        C.Material);
    Ar.Field("Fracture",        C.Fracture);
    Ar.Field("Pieces",          C.Pieces);
    Ar.Field("Seed",            C.Seed);
    Ar.Field("Strength",        C.Strength);
    Ar.Field("Health",          C.Health);
    Ar.Field("DebrisLifetime",  C.DebrisLifetime);
}

/* A client that joins after a break sees it broken. */
GAMECORE_DEFINE_COMPONENT_REPLICATION(DestructibleComponent)
{
    Rep.Field("Damage",     &DestructibleComponent::Damage);
    Rep.Flag("Broken",      &DestructibleComponent::bBroken);
}

GAMECORE_DEFINE_COMPONENT_RPCS(DestructibleComponent)
{
    Rpc.Multicast<&DestructibleComponent::MulticastBreak>("MulticastBreak");
}

/* destructible:Break(point, impulse), :ApplyDamage(point, impulse), :IsBroken(). */
GAMECORE_DEFINE_COMPONENT_SCRIPT(DestructibleComponent)
{
    Bind.Flag("Enabled",       &DestructibleComponent::bEnabled);
    Bind.Field("Material",     &DestructibleComponent::Material);
    Bind.Field("Strength",     &DestructibleComponent::Strength);
    Bind.Field("Health",       &DestructibleComponent::Health);
    Bind.ReadOnly("Damage",    &DestructibleComponent::Damage);
    Bind.ReadOnly("Broken",    &DestructibleComponent::bBroken);

    Bind.NativePure<&DestructibleComponent::IsBroken>("IsBroken", "-> Broken");
    Bind.Method("Break", [](DestructibleComponent& C, GameCore::ScriptCall& Call)
    {
        const EngineCore::Vector3 Point   = Call.GetVector3(1);
        const EngineCore::Vector3 Impulse = Call.GetVector3(4);
        if(Call.HasError()) { return; }
        C.Break(Point, Impulse);
    }, "Point:Vector3, Impulse:Vector3");
    Bind.Method("ApplyDamage", [](DestructibleComponent& C, GameCore::ScriptCall& Call)
    {
        const EngineCore::Vector3 Point   = Call.GetVector3(1);
        const EngineCore::Vector3 Impulse = Call.GetVector3(4);
        if(Call.HasError()) { return; }
        C.ApplyDamage(Point, Impulse);
    }, "Point:Vector3, Impulse:Vector3");
}

namespace
{
    int32_t ScriptRadialDamage(GameCore::World& InWorld, const EngineCore::Vector3& InCentre, const float InRadius, const float InImpulse)
    {
        return NimbleEngine::RadialDamage(InWorld, InCentre, InRadius, InImpulse);
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Destruction)
{
    Lib.Native<&ScriptRadialDamage>("RadialDamage", "Centre, Radius, Impulse -> Reached")
       .Tooltip("Damage every Destructible near a point and push the bodies there: an explosion");
    Lib.Event("DestructibleBroken", "Entity:Entity").Tooltip("A Destructible broke (a structure, each time more of it falls)");
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(DestructibleComponent)
{
    Props.TypeCategory("Physics");
    Props.Flag("Enabled", &DestructibleComponent::bEnabled);
    Props.Category("Fracture");
    Props.Field("Material", &DestructibleComponent::Material);
    Props.Choice("Mode", &DestructibleComponent::Mode, "Runtime|Baked");   // EDestructibleMode
    Props.Field("Fracture", &DestructibleComponent::Fracture);
    Props.Field("Pieces", &DestructibleComponent::Pieces);
    Props.Field("Seed", &DestructibleComponent::Seed);
    Props.Category("Damage");
    Props.Field("Strength", &DestructibleComponent::Strength);
    Props.Field("Health", &DestructibleComponent::Health);
    Props.Category("Structure");
    Props.Flag("Structural", &DestructibleComponent::bStructural);
    Props.Flag("Anchored", &DestructibleComponent::bAnchored);
    Props.Category("Debris");
    Props.Field("Lifetime", &DestructibleComponent::DebrisLifetime);
    Props.Category("State");
    Props.Field("Damage", &DestructibleComponent::Damage, GameCore::PF_ReadOnly);
    Props.Flag("Broken", &DestructibleComponent::bBroken, GameCore::PF_ReadOnly);
}
#endif
