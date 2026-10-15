#include "Private/RenderCore/ProceduralSystem.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Public/GameCore/Components/SplineComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/World.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/Components/TerrainComponent.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderCoreLog.h"

namespace RenderCore
{
    namespace
    {
        std::atomic<uint64_t> GRegeneration{ 0 };

        /* FNV-1a, 64 bits: a signature of what a generation read. */
        struct Signature
        {
            uint64_t Value = 0xCBF29CE484222325ull;
            void Bytes(const void* InData, const size_t InSize)
            {
                const auto* Byte = static_cast<const uint8_t*>(InData);
                for(size_t Index = 0; Index < InSize; ++Index)
                {
                    Value ^= Byte[Index];
                    Value *= 0x100000001B3ull;
                }
            }
            template<typename T> void Add(const T& InValue) { Bytes(&InValue, sizeof(T)); }
            void Text(const char* InText) { Bytes(InText, std::strlen(InText) + 1); }
            void Matrix(const EngineCore::Matrix4x4& InMatrix)
            {
                for(int Row = 0; Row < 4; ++Row)
                {
                    for(int Col = 0; Col < 4; ++Col) { Add(float(InMatrix.Data[Row][Col])); }
                }
            }
        };

        void HashSpline(Signature& InOut, const GameCore::SplineComponent& InSpline)
        {
            const int32_t Count = InSpline.GetPointCount();
            InOut.Add(Count);
            for(int32_t Index = 0; Index < Count; ++Index)
            {
                InOut.Add(float(InSpline.Points[Index].GetX()));
                InOut.Add(float(InSpline.Points[Index].GetY()));
                InOut.Add(float(InSpline.Points[Index].GetZ()));
            }
            InOut.Add(InSpline.bClosed);
            InOut.Add(InSpline.Type);
            InOut.Add(InSpline.Width);
        }

        struct StoreKey
        {
            const void* World;
            uint64_t    Entity;
            bool operator<(const StoreKey& InOther) const
            {
                return World != InOther.World ? std::less<const void*>()(World, InOther.World) : Entity < InOther.Entity;
            }
        };

        struct StoreState
        {
            std::mutex                                                   Lock;
            std::map<StoreKey, std::shared_ptr<const ProceduralResult>> Results;
        };

        StoreState& Store()
        {
            static StoreState State;
            return State;
        }

        /* How often (frames) a spawner looks again for the static meshes under it, when nothing else moved. */
        constexpr uint64_t MESH_SCAN_FRAMES = 30;

        /* The world box a spawner's points come from, for finding meshes under it. False: no box. */
        bool SourceBox(const GameCore::Entity& InEntity, const ProceduralSpawnerComponent& InSpawner,
                       const EngineCore::Matrix4x4& InToWorld, float (&OutMin)[3], float (&OutMax)[3])
        {
            for(int Axis = 0; Axis < 3; ++Axis) { OutMin[Axis] = 1e30f; OutMax[Axis] = -1e30f; }
            const auto Grow = [&OutMin, &OutMax](const float InX, const float InY, const float InZ)
            {
                const float P[3] = { InX, InY, InZ };
                for(int Axis = 0; Axis < 3; ++Axis)
                {
                    OutMin[Axis] = std::min(OutMin[Axis], P[Axis]);
                    OutMax[Axis] = std::max(OutMax[Axis], P[Axis]);
                }
            };
            if(InSpawner.Source == PGS_Box)
            {
                const float E[3] = { float(InSpawner.BoxExtent.GetX()), float(InSpawner.BoxExtent.GetY()), float(InSpawner.BoxExtent.GetZ()) };
                for(int Corner = 0; Corner < 8; ++Corner)
                {
                    const float L[3] = { (Corner & 1) ? E[0] : -E[0], (Corner & 2) ? E[1] : -E[1], (Corner & 4) ? E[2] : -E[2] };
                    float W[3];
                    for(int Row = 0; Row < 3; ++Row)
                    {
                        W[Row] = float(InToWorld.Data[Row][0] * L[0] + InToWorld.Data[Row][1] * L[1] + InToWorld.Data[Row][2] * L[2]
                                     + InToWorld.Data[Row][3]);
                    }
                    Grow(W[0], W[1], W[2]);
                }
                return true;
            }
            if(InSpawner.Source == PGS_SplinePath || InSpawner.Source == PGS_SplineArea)
            {
                const GameCore::SplineComponent* Spline = InEntity.GetComponent<GameCore::SplineComponent>();
                if(!Spline) { return false; }
                EngineCore::Vector3 Lo, Hi;
                if(!Spline->GetCurve().Transformed(InToWorld).GetBounds(Lo, Hi)) { return false; }
                const float Side = 0.5f * std::max(Spline->Width, 0.0f) + std::max(InSpawner.Spacing, 1.0f);
                const float Up   = std::max(InSpawner.ProjectRange, 0.0f);
                Grow(float(Lo.GetX()) - Side, float(Lo.GetY()) - Up, float(Lo.GetZ()) - Side);
                Grow(float(Hi.GetX()) + Side, float(Hi.GetY()) + Up, float(Hi.GetZ()) + Side);
                return true;
            }
            return false;
        }

        /* A static mesh's world box, from its file's bounds. */
        bool MeshWorldBox(const MeshData& InMesh, const EngineCore::Matrix4x4& InToWorld, float (&OutMin)[3], float (&OutMax)[3])
        {
            for(int Axis = 0; Axis < 3; ++Axis) { OutMin[Axis] = 1e30f; OutMax[Axis] = -1e30f; }
            for(int Corner = 0; Corner < 8; ++Corner)
            {
                const float L[3] = { (Corner & 1) ? InMesh.BoundsMax[0] : InMesh.BoundsMin[0],
                                     (Corner & 2) ? InMesh.BoundsMax[1] : InMesh.BoundsMin[1],
                                     (Corner & 4) ? InMesh.BoundsMax[2] : InMesh.BoundsMin[2] };
                for(int Row = 0; Row < 3; ++Row)
                {
                    const float W = float(InToWorld.Data[Row][0] * L[0] + InToWorld.Data[Row][1] * L[1] + InToWorld.Data[Row][2] * L[2]
                                        + InToWorld.Data[Row][3]);
                    OutMin[Row] = std::min(OutMin[Row], W);
                    OutMax[Row] = std::max(OutMax[Row], W);
                }
            }
            return true;
        }
    }

    // === The "Procedural" pool ===================================================

    namespace
    {
        /* Never destroyed or unregistered, as the "Textures" pool: a result may
         * be released by a static destructor after RenderCore's own have run.
         * Function-local, so the first allocation finds it registered. */
        struct ProceduralPoolState
        {
            EngineCore::PoolID          Pool   = EngineCore::POOL_Default;
            EngineCore::HeapMemoryPool* Memory = nullptr;
        };

        ProceduralPoolState& GetProceduralPoolState()
        {
            static ProceduralPoolState State = []
            {
                ProceduralPoolState Made;
                Made.Memory = new EngineCore::HeapMemoryPool("Procedural");
                Made.Pool   = EngineCore::RegisterMemoryPool(Made.Memory);
                if(Made.Pool == EngineCore::POOL_Invalid) { Made.Pool = EngineCore::POOL_Default; }
                return Made;
            }();
            return State;
        }

        /* Registered while RenderCore loads -- the registry wants its pools
         * before any thread allocates (CoreMemoryPool.h, THREADING). */
        [[maybe_unused]] const bool GProceduralPoolRegistered = (GetProceduralPoolState(), true);
    }

    EngineCore::PoolID GetProceduralMemoryPool()
    {
        return GetProceduralPoolState().Pool;
    }

    EngineCore::MemoryPoolStats GetProceduralMemoryStats()
    {
        EngineCore::MemoryPoolStats Stats;
        EngineCore::Memory::GetPoolStats(GetProceduralMemoryPool(), Stats);
        return Stats;
    }

    void RegenerateAllProcedural()
    {
        GRegeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    uint64_t GetProceduralRegeneration()
    {
        return GRegeneration.load(std::memory_order_acquire);
    }

    // === ProceduralResultStore ===================================================

    void ProceduralResultStore::Publish(const void* InWorld, const GameCore::EntityID InEntity, std::shared_ptr<const ProceduralResult> InResult)
    {
        StoreState& State = Store();
        std::lock_guard<std::mutex> Guard(State.Lock);
        State.Results[StoreKey{ InWorld, InEntity.Pack() }] = std::move(InResult);
    }

    std::shared_ptr<const ProceduralResult> ProceduralResultStore::Find(const void* InWorld, const GameCore::EntityID InEntity)
    {
        StoreState& State = Store();
        std::lock_guard<std::mutex> Guard(State.Lock);
        const auto Found = State.Results.find(StoreKey{ InWorld, InEntity.Pack() });
        return Found == State.Results.end() ? nullptr : Found->second;
    }

    void ProceduralResultStore::Remove(const void* InWorld, const GameCore::EntityID InEntity)
    {
        StoreState& State = Store();
        std::lock_guard<std::mutex> Guard(State.Lock);
        State.Results.erase(StoreKey{ InWorld, InEntity.Pack() });
    }

    // === ProceduralSystem ========================================================

    struct ProceduralSystem::Spawner
    {
        struct Job
        {
            std::shared_ptr<ProceduralInputs> Inputs;
            std::shared_ptr<ProceduralResult> Result;
            std::atomic<bool>                 bDone{ false };
        };

        struct MeshTarget
        {
            std::string           Path;
            EngineCore::Matrix4x4 ToWorld;
        };

        GameCore::EntityID      Entity;
        uint64_t                Wanted   = 0;   // the signature the last generation started from
        bool                    bStarted = false;
        std::shared_ptr<Job>    Running;
        std::thread             Thread;
        std::vector<MeshTarget> Targets;        // the static meshes under it, as of the last scan
        uint64_t                TargetSignature = 0;
        uint64_t                ScannedFrame    = 0;
        uint64_t                ScannedCheap    = 0;
        bool                    bScanned        = false;
        std::string             LastProblem;
        bool                    bLogged = false;
        bool                    bSeen = false;

        ~Spawner() { Wait(); }

        void Wait()
        {
            if(Thread.joinable()) { Thread.join(); }
        }
    };

    ProceduralSystem::ProceduralSystem() = default;

    ProceduralSystem::~ProceduralSystem()
    {
        Reset();
    }

    void ProceduralSystem::Reset()
    {
        for(auto& Each : Spawners)
        {
            Each.second->Wait();
            ProceduralResultStore::Remove(World, Each.second->Entity);
        }
        Spawners.clear();
        World = nullptr;
    }

    void ProceduralSystem::Update(GameCore::World& InWorld)
    {
        if(World != &InWorld)
        {
            Reset();
            World = &InWorld;
        }
        ++Frame;
        for(auto& Each : Spawners) { Each.second->bSeen = false; }
        const uint64_t MeshGeneration    = MeshAssetCache::GetGeneration();
        const uint64_t TerrainGeneration = GameCore::TerrainAssetCache::GetGeneration();
        const uint64_t Regeneration      = GetProceduralRegeneration();

        InWorld.Each<ProceduralSpawnerComponent>([&](const GameCore::Entity& InEntity, ProceduralSpawnerComponent& InSpawner)
        {
            std::unique_ptr<Spawner>& Slot = Spawners[InEntity.GetID().Pack()];
            if(!Slot)
            {
                Slot = std::make_unique<Spawner>();
                Slot->Entity = InEntity.GetID();
            }
            Spawner& State = *Slot;
            State.bSeen = true;

            /* A finished generation: hand it over. */
            if(State.Running && State.Running->bDone.load(std::memory_order_acquire))
            {
                State.Wait();
                std::shared_ptr<ProceduralResult> Result = State.Running->Result;
                State.Running.reset();
                InSpawner.InstanceCount        = int32_t(Result->InstanceCount);
                InSpawner.GenerateMilliseconds = Result->Milliseconds;
                const char* Name = InEntity.GetName();
                const std::string Label = std::string("Procedural Spawner '") + (Name && Name[0] ? Name : "(unnamed)") + "': ";
                if(!State.bLogged)
                {
                    /* The first generation of each, for a load's numbers; edits after it stay quiet. */
                    State.bLogged = true;
                    char Line[200];
                    const EngineCore::MemoryPoolStats Pool = GetProceduralMemoryStats();
                    std::snprintf(Line, sizeof(Line), "%u instances in %zu cells from %u points, %.1f ms (Procedural pool %.2f MB)",
                                  Result->InstanceCount, Result->Cells.size(), Result->Candidates, double(Result->Milliseconds),
                                  double(Pool.BytesInUse) / (1024.0 * 1024.0));
                    RENDERCORE_LOG(Label + Line)
                }
                const std::string Problem = Result->Problem ? Result->Problem : (Result->bCapped ? "stopped at Max Instances" : "");
                if(Problem != State.LastProblem)
                {
                    State.LastProblem = Problem;
                    if(!Problem.empty()) { RENDERCORE_LOG_WARN(Label + Problem) }
                }
                ProceduralResultStore::Publish(&InWorld, State.Entity, std::move(Result));
            }

            // === What it reads, cheaply ==========================================
            const EngineCore::Matrix4x4 ToWorld = InEntity.GetWorldMatrix();
            const bool bProjects = InSpawner.Source != PGS_MeshSurface && InSpawner.Project != PGP_None;
            const bool bTerrains = bProjects && (InSpawner.Project == PGP_Terrain || InSpawner.Project == PGP_All);
            const bool bMeshes   = bProjects && (InSpawner.Project == PGP_Meshes || InSpawner.Project == PGP_All);

            Signature Cheap;
            {
                const auto* Bytes = reinterpret_cast<const uint8_t*>(&InSpawner);
                const size_t First = offsetof(ProceduralSpawnerComponent, Source);
                const size_t Last  = offsetof(ProceduralSpawnerComponent, LODDistance);
                Cheap.Bytes(Bytes + First, Last - First);
                Cheap.Add(InSpawner.MaxInstances);
            }
            Cheap.Matrix(ToWorld);
            Cheap.Add(MeshGeneration);
            Cheap.Add(Regeneration);
            if(const GameCore::SplineComponent* Spline = InEntity.GetComponent<GameCore::SplineComponent>())
            {
                HashSpline(Cheap, *Spline);
            }
            for(const auto& Name : InSpawner.Exclusions)
            {
                if(Name[0] == '\0') { continue; }
                const GameCore::EntityID Exclusion = InWorld.FindEntityByName(Name);
                if(!Exclusion.IsSet() || !InWorld.IsAlive(Exclusion)) { continue; }
                const GameCore::Entity Other = InWorld.GetEntity(Exclusion);
                if(const GameCore::SplineComponent* Spline = Other.GetComponent<GameCore::SplineComponent>())
                {
                    HashSpline(Cheap, *Spline);
                    Cheap.Matrix(Other.GetWorldMatrix());
                }
            }
            if(InSpawner.Source == PGS_MeshSurface)
            {
                if(const RenderComponent* Render = InEntity.GetComponent<RenderComponent>()) { Cheap.Text(Render->Mesh); }
            }
            if(bTerrains)
            {
                Cheap.Add(TerrainGeneration);
                InWorld.Each<TerrainComponent>([&Cheap](const GameCore::Entity& InTerrainEntity, TerrainComponent& InTerrain)
                {
                    if(!InTerrain.bVisible || InTerrain.Terrain[0] == '\0') { return; }
                    Cheap.Text(InTerrain.Terrain);
                    Cheap.Matrix(InTerrainEntity.GetWorldMatrix());
                    if(const std::shared_ptr<GameCore::TerrainData> Data = GameCore::TerrainAssetCache::FindLoaded(InTerrain.Terrain))
                    {
                        Cheap.Add(Data->GetRevision());
                    }
                });
            }

            /* The static meshes under it: looked for again when anything else moved, or now and then. */
            if(bMeshes && (!State.bScanned || State.ScannedCheap != Cheap.Value || Frame - State.ScannedFrame >= MESH_SCAN_FRAMES))
            {
                State.bScanned     = true;
                State.ScannedCheap = Cheap.Value;
                State.ScannedFrame = Frame;
                State.Targets.clear();
                float Min[3], Max[3];
                if(SourceBox(InEntity, InSpawner, ToWorld, Min, Max))
                {
                    const GameCore::EntityID Self = InEntity.GetID();
                    InWorld.Each<RenderComponent>([&](const GameCore::Entity& InOther, RenderComponent& InRender)
                    {
                        if(InOther.GetID() == Self || !InRender.bVisible || InRender.Mesh[0] == '\0' || !InOther.IsStatic()) { return; }
                        const std::shared_ptr<const MeshData> Mesh = MeshAssetCache::Find(InRender.Mesh);
                        if(!Mesh || Mesh->IsSkinned()) { return; }
                        const EngineCore::Matrix4x4 OtherToWorld = InOther.GetWorldMatrix();
                        float Lo[3], Hi[3];
                        MeshWorldBox(*Mesh, OtherToWorld, Lo, Hi);
                        for(int Axis = 0; Axis < 3; ++Axis)
                        {
                            if(Hi[Axis] < Min[Axis] || Lo[Axis] > Max[Axis]) { return; }
                        }
                        Spawner::MeshTarget Target;
                        Target.Path    = InRender.Mesh;
                        Target.ToWorld = OtherToWorld;
                        State.Targets.push_back(std::move(Target));
                    });
                }
                Signature Targets;
                for(const Spawner::MeshTarget& Each : State.Targets)
                {
                    Targets.Text(Each.Path.c_str());
                    Targets.Matrix(Each.ToWorld);
                }
                State.TargetSignature = Targets.Value;
            }
            if(!bMeshes && !State.Targets.empty())
            {
                State.Targets.clear();
                State.TargetSignature = 0;
            }

            Signature Full = Cheap;
            Full.Add(State.TargetSignature);
            if((State.bStarted && Full.Value == State.Wanted) || State.Running) { return; }

            // === A snapshot, to a worker =========================================
            auto Inputs = std::allocate_shared<ProceduralInputs>(ProceduralAllocator<ProceduralInputs>());
            Inputs->Settings = InSpawner;
            Inputs->ToWorld  = ToWorld;
            if(const GameCore::SplineComponent* Spline = InEntity.GetComponent<GameCore::SplineComponent>())
            {
                Inputs->bHasSpline  = true;
                Inputs->Spline      = Spline->GetCurve();
                Inputs->SplineWidth = Spline->Width;
            }
            for(const auto& Name : InSpawner.Exclusions)
            {
                if(Name[0] == '\0') { continue; }
                const GameCore::EntityID Exclusion = InWorld.FindEntityByName(Name);
                if(!Exclusion.IsSet() || !InWorld.IsAlive(Exclusion)) { continue; }
                const GameCore::Entity Other = InWorld.GetEntity(Exclusion);
                if(const GameCore::SplineComponent* Spline = Other.GetComponent<GameCore::SplineComponent>())
                {
                    ProceduralExclusionInput Out;
                    Out.Curve     = Spline->GetCurve().Transformed(Other.GetWorldMatrix());
                    Out.HalfWidth = 0.5f * std::max(Spline->Width, 0.0f);
                    Inputs->Exclusions.push_back(std::move(Out));
                }
            }
            if(bTerrains)
            {
                InWorld.Each<TerrainComponent>([&Inputs](const GameCore::Entity& InTerrainEntity, TerrainComponent& InTerrain)
                {
                    if(!InTerrain.bVisible || InTerrain.Terrain[0] == '\0') { return; }
                    ProceduralTerrainInput Terrain;
                    Terrain.Data = GameCore::TerrainAssetCache::Find(InTerrain.Terrain);
                    if(!Terrain.Data) { return; }
                    Terrain.ToWorld = InTerrainEntity.GetWorldMatrix();
                    Terrain.ToLocal = EngineCore::Matrix4x4::Inverse(Terrain.ToWorld);
                    Inputs->Terrains.push_back(std::move(Terrain));
                });
            }
            for(const Spawner::MeshTarget& Each : State.Targets)
            {
                ProceduralMeshInput Mesh;
                Mesh.Mesh    = MeshAssetCache::Find(Each.Path);
                Mesh.ToWorld = Each.ToWorld;
                if(Mesh.Mesh) { Inputs->Meshes.push_back(std::move(Mesh)); }
            }
            if(InSpawner.Source == PGS_MeshSurface)
            {
                if(const RenderComponent* Render = InEntity.GetComponent<RenderComponent>())
                {
                    if(Render->Mesh[0] != '\0') { Inputs->Surface.Mesh = MeshAssetCache::Find(Render->Mesh); }
                }
                Inputs->Surface.ToWorld = ToWorld;
            }
            for(uint32_t Entry = 0; Entry < PROCEDURAL_MESHES_MAX; ++Entry)
            {
                const ProceduralMeshEntry& In = InSpawner.Meshes[Entry];
                if(In.Mesh[0] == '\0') { continue; }
                if(const std::shared_ptr<const MeshData> Mesh = MeshAssetCache::Find(In.Mesh))
                {
                    ProceduralEntryInput& Out = Inputs->Entries[Entry];
                    Out.bValid = true;
                    std::memcpy(Out.BoundsMin, Mesh->BoundsMin, sizeof(Out.BoundsMin));
                    std::memcpy(Out.BoundsMax, Mesh->BoundsMax, sizeof(Out.BoundsMax));
                }
            }

            State.Wanted   = Full.Value;
            State.bStarted = true;
            auto Work = std::allocate_shared<Spawner::Job>(ProceduralAllocator<Spawner::Job>());
            Work->Inputs = std::move(Inputs);
            Work->Result = std::allocate_shared<ProceduralResult>(ProceduralAllocator<ProceduralResult>());
            State.Running = Work;
            State.Thread  = std::thread([Work]()
            {
                GenerateProcedural(*Work->Inputs, *Work->Result);
                Work->bDone.store(true, std::memory_order_release);
            });
        });

        /* Spawners gone since last frame: their results go with them. */
        for(auto Each = Spawners.begin(); Each != Spawners.end();)
        {
            if(Each->second->bSeen)
            {
                ++Each;
                continue;
            }
            Each->second->Wait();
            ProceduralResultStore::Remove(World, Each->second->Entity);
            Each = Spawners.erase(Each);
        }
    }
}
