#include "Public/AICore/NavWorld.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "DetourCommon.h"
#include "DetourCrowd.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include "Private/AICore/NavInternal.h"
#include "Public/AICore/AIWorld.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/AICore/Components/NavMeshVolumeComponent.h"
#include "Public/AICore/Components/NavModifierComponent.h"
#include "Public/AICore/Components/NavObstacleComponent.h"
#include "Public/AICore/NavGeometry.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/BoundsComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/World.h"

/* NavWorld: a World's navmesh, crowd and obstacles. NavWorld.h has the
 * rules. */
namespace AICore
{
    namespace
    {
        constexpr int   MAX_AGENTS = 256;
        /* Further than this from the crowd's copy, an agent was moved by
         * something else (a respawn, a teleport): it is added again there. */
        constexpr float TELEPORT_DISTANCE = 150.0f;

        NavMeshVolumeComponent* FindVolume(GameCore::World& InWorld, GameCore::EntityID* OutEntity = nullptr)
        {
            NavMeshVolumeComponent* Found = nullptr;
            InWorld.Each<NavMeshVolumeComponent>([&](const GameCore::Entity& InEntity, NavMeshVolumeComponent& InVolume)
            {
                if(!Found)
                {
                    Found = &InVolume;
                    if(OutEntity) { *OutEntity = InEntity.GetID(); }
                }
            });
            return Found;
        }

        void AvoidanceParams(dtCrowd& InCrowd)
        {
            /* RecastDemo's four presets: none (straight steering), low,
             * medium, high -- more samples, better choices, more cost. */
            dtObstacleAvoidanceParams Params = *InCrowd.getObstacleAvoidanceParams(0);
            Params.velBias = 0.5f;
            Params.adaptiveDivs = 5;
            Params.adaptiveRings = 2;
            Params.adaptiveDepth = 1;
            InCrowd.setObstacleAvoidanceParams(0, &Params);
            Params.adaptiveDivs = 5; Params.adaptiveRings = 2; Params.adaptiveDepth = 2;
            InCrowd.setObstacleAvoidanceParams(1, &Params);
            Params.adaptiveDivs = 7; Params.adaptiveRings = 2; Params.adaptiveDepth = 3;
            InCrowd.setObstacleAvoidanceParams(2, &Params);
            Params.adaptiveDivs = 7; Params.adaptiveRings = 3; Params.adaptiveDepth = 3;
            InCrowd.setObstacleAvoidanceParams(3, &Params);
        }

        dtCrowdAgentParams AgentParams(const NavAgentComponent& InAgent)
        {
            dtCrowdAgentParams Params;
            std::memset(&Params, 0, sizeof(Params));
            Params.radius = std::max(InAgent.Radius, 1.0f);
            Params.height = std::max(InAgent.Height, 1.0f);
            Params.maxAcceleration = std::max(InAgent.MaxAcceleration, 1.0f);
            Params.maxSpeed = std::max(InAgent.MaxSpeed, 0.0f);
            Params.collisionQueryRange = Params.radius * 12.0f;
            Params.pathOptimizationRange = Params.radius * 30.0f;
            Params.separationWeight = InAgent.SeparationWeight;
            Params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
            if(InAgent.AvoidanceQuality > 0)
            {
                Params.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
                Params.obstacleAvoidanceType = (unsigned char)std::min<int>(InAgent.AvoidanceQuality, 3);
            }
            if(InAgent.SeparationWeight > 0.0f) { Params.updateFlags |= DT_CROWD_SEPARATION; }
            Params.queryFilterType = 0;
            return Params;
        }
    }

    NavWorld::NavWorld(GameCore::World& InWorld) : World(InWorld) {}

    NavWorld::~NavWorld()
    {
        dtFreeCrowd(Crowd);
    }

    NavWorld* NavWorld::Find(const GameCore::World* InWorld)
    {
        AIWorld* AI = AIWorld::Find(InWorld);
        return AI ? &AI->GetNav() : nullptr;
    }

    // === The navmesh ==========================================================

    bool NavWorld::Build(NavBuildStats* OutStats, std::string* OutError)
    {
        const NavMeshVolumeComponent* Volume = FindVolume(World);
        if(!Volume)
        {
            if(OutError) { *OutError = "the level has no Nav Mesh Volume"; }
            return false;
        }
        const NavBuildSettings Settings = Volume->GetSettings();
        const GameCore::AABB Bounds = Volume->GetWorldBounds();
        NavInputGeometry Geometry;
        GatherNavGeometry(World, Settings, Bounds, Geometry);
        return BuildFrom(Geometry, Settings, Bounds, OutStats, OutError);
    }

    bool NavWorld::BuildFrom(const NavInputGeometry& InGeometry, const NavBuildSettings& InSettings, const GameCore::AABB& InBounds,
                             NavBuildStats* OutStats, std::string* OutError)
    {
        bTriedLoad = true;
        const bool bBuilt = Mesh.Build(InGeometry, InSettings, InBounds, OutStats, OutError);
        LoadedGeometryHash = Mesh.GetGeometryHash();
        if(bBuilt) { Query.Init(Mesh); }
        ResetCrowd();
        RefreshFilter();
        TerrainRevisions.clear();
        UpdateTerrainWatch();
        ++ChangeRevision;
        return bBuilt;
    }

    bool NavWorld::LoadFile(const std::string& InPath, std::string* OutError)
    {
        bTriedLoad = true;
        const bool bLoaded = Mesh.LoadFile(InPath, OutError);
        LoadedGeometryHash = bLoaded ? Mesh.GetGeometryHash() : 0;
        if(bLoaded) { Query.Init(Mesh); }
        ResetCrowd();
        RefreshFilter();
        TerrainRevisions.clear();
        UpdateTerrainWatch();
        ++ChangeRevision;
        return bLoaded;
    }

    bool NavWorld::SaveFile(const std::string& InDiskPath, std::string* OutError) const
    {
        return Mesh.SaveFile(InDiskPath, OutError);
    }

    int32_t NavWorld::RebuildTilesIn(const GameCore::AABB& InRegion)
    {
        if(!Mesh.IsValid() || InRegion.IsEmpty()) { return 0; }
        /* The tiles' borders reach a little past them: gather a tile wider. */
        const float Grow = float(Mesh.GetSettings().TileSize) * Mesh.GetSettings().CellSize;
        GameCore::AABB Gather = InRegion;
        Gather.Min = Gather.Min - EngineCore::Vector3(Grow, 0.0, Grow);
        Gather.Max = Gather.Max + EngineCore::Vector3(Grow, 0.0, Grow);
        NavInputGeometry Geometry;
        GatherNavGeometry(World, Mesh.GetSettings(), Gather, Geometry);
        const int32_t Rebuilt = Mesh.RebuildTiles(Geometry, InRegion);
        if(Rebuilt > 0) { ++ChangeRevision; }
        return Rebuilt;
    }

    bool NavWorld::IsUpToDate() const
    {
        if(!Mesh.IsValid()) { return false; }
        const NavMeshVolumeComponent* Volume = FindVolume(const_cast<GameCore::World&>(World));
        if(!Volume) { return false; }
        const NavBuildSettings Settings = Volume->GetSettings();
        NavInputGeometry Geometry;
        GatherNavGeometry(const_cast<GameCore::World&>(World), Settings, Volume->GetWorldBounds(), Geometry);
        return (Geometry.Hash() ^ Settings.Hash()) == LoadedGeometryHash;
    }

    bool NavWorld::HasNavMesh() const { return Mesh.IsValid(); }

    uint64_t NavWorld::GetRevision() const { return Mesh.GetRevision() + ChangeRevision; }

    bool NavWorld::EnsureNavMesh()
    {
        if(Mesh.IsValid()) { return true; }
        if(bTriedLoad) { return false; }
        bTriedLoad = true;

        NavMeshVolumeComponent* Volume = FindVolume(World);
        if(!Volume) { return false; }

        if(Volume->NavMesh[0] != '\0')
        {
            std::string Error;
            if(LoadFile(Volume->NavMesh, &Error))
            {
                if(!Volume->bBuildAtRuntime || IsUpToDate()) { return true; }
                CORE_LOG_WARN(std::string("[AI] ") + Volume->NavMesh + " is older than the level's geometry: building a fresh navmesh")
            }
            else if(!Volume->bBuildAtRuntime)
            {
                CORE_LOG_WARN(std::string("[AI] the navmesh ") + Volume->NavMesh + " " + Error)
                return false;
            }
        }
        if(!Volume->bBuildAtRuntime) { return false; }

        NavBuildStats Stats;
        std::string Error;
        if(!Build(&Stats, &Error))
        {
            CORE_LOG_WARN("[AI] could not build a navmesh: " + Error)
            return false;
        }
        CORE_LOG("[AI] built a navmesh at runtime: " + std::to_string(Stats.Tiles) + " tiles, " + std::to_string(Stats.Polygons)
                 + " polygons in " + std::to_string(int(Stats.Milliseconds)) + " ms")
        return true;
    }

    void NavWorld::ResetCrowd()
    {
        dtFreeCrowd(Crowd);
        Crowd = nullptr;
        AgentIndices.clear();
        World.Each<NavAgentComponent>([](const GameCore::Entity&, NavAgentComponent& InAgent) { InAgent.CrowdIndex = -1; });
        World.Each<NavObstacleComponent>([](const GameCore::Entity&, NavObstacleComponent& InObstacle) { InObstacle.Handle = 0; });
        if(!Mesh.IsValid()) { return; }

        float MaxRadius = Mesh.GetSettings().AgentRadius;
        World.Each<NavAgentComponent>([&MaxRadius](const GameCore::Entity&, NavAgentComponent& InAgent) { MaxRadius = std::max(MaxRadius, InAgent.Radius); });
        Crowd = dtAllocCrowd();
        if(!Crowd || !Crowd->init(MAX_AGENTS, MaxRadius * 2.0f, Mesh.GetDetourMesh()))
        {
            dtFreeCrowd(Crowd);
            Crowd = nullptr;
            CORE_LOG_ERROR("[AI] could not set up the crowd")
            return;
        }
        AvoidanceParams(*Crowd);
    }

    void NavWorld::RefreshFilter()
    {
        NavFilter Fresh;
        World.Each<NavModifierComponent>([&Fresh](const GameCore::Entity&, NavModifierComponent& InModifier)
        {
            const uint8_t Area = InModifier.GetAreaId();
            if(Area != NAV_AREA_BLOCKED && Area < NAV_AREA_COUNT) { Fresh.AreaCost[Area] = std::max(InModifier.Cost, 0.01f); }
        });
        Filter = Fresh;
        if(Crowd)
        {
            const NavDetail::NavQueryFilter Detour(&Filter);
            *Crowd->getEditableFilter(0) = Detour;
        }
    }

    void NavWorld::UpdateObstacles()
    {
        if(!Mesh.IsValid()) { return; }
        World.Each<NavObstacleComponent>([this](const GameCore::Entity& InEntity, NavObstacleComponent& InObstacle)
        {
            const GameCore::TransformPose Pose = InEntity.GetWorldPose();
            const bool bWant = InObstacle.bEnabled != 0;
            if(InObstacle.Handle != 0)
            {
                const bool bMoved = NavDetail::Distance3D(Pose.Position, InObstacle.PlacedAt) > 5.0f
                                 || std::fabs(float(Pose.Rotation.GetY()) - InObstacle.PlacedYaw) > 2.0f;
                if(!bWant || bMoved)
                {
                    Mesh.RemoveObstacle(InObstacle.Handle);
                    InObstacle.Handle = 0;
                    ++ChangeRevision;
                }
            }
            if(bWant && InObstacle.Handle == 0)
            {
                const EngineCore::Vector3 Half(std::fabs(float(InObstacle.HalfExtents.GetX() * Pose.Scale.GetX())),
                                               std::fabs(float(InObstacle.HalfExtents.GetY() * Pose.Scale.GetY())),
                                               std::fabs(float(InObstacle.HalfExtents.GetZ() * Pose.Scale.GetZ())));
                if(InObstacle.Shape == NOS_Cylinder)
                {
                    const EngineCore::Vector3 Feet(Pose.Position.GetX(), Pose.Position.GetY() - Half.GetY(), Pose.Position.GetZ());
                    InObstacle.Handle = Mesh.AddCylinderObstacle(Feet, float(Half.GetX()), float(Half.GetY()) * 2.0f);
                }
                else
                {
                    InObstacle.Handle = Mesh.AddBoxObstacle(Pose.Position, Half, float(Pose.Rotation.GetY()));
                }
                InObstacle.PlacedAt  = Pose.Position;
                InObstacle.PlacedYaw = float(Pose.Rotation.GetY());
                ++ChangeRevision;
            }
        });
        Mesh.Update();
    }

    void NavWorld::UpdateTerrainWatch()
    {
        const GameCore::TerrainProvider* Terrains = GameCore::GetTerrainProvider();
        if(!Terrains || !Terrains->FindTerrain || !Mesh.IsValid()) { return; }
        const NavMeshVolumeComponent* Volume = FindVolume(World);
        const bool bRebuild = Volume && Volume->bRebuildOnChange;

        std::vector<GameCore::AABB> Dirty;
        World.Each<GameCore::BoundsComponent>([&](const GameCore::Entity& InEntity, GameCore::BoundsComponent& InBounds)
        {
            const std::shared_ptr<GameCore::TerrainData> Terrain = Terrains->FindTerrain(World, InEntity.GetID(), Terrains->UserData);
            if(!Terrain) { return; }
            const uint64_t Revision = Terrain->GetRevision();
            auto Found = TerrainRevisions.find(InEntity.GetID().Pack());
            if(Found == TerrainRevisions.end())
            {
                TerrainRevisions[InEntity.GetID().Pack()] = Revision;
                return;
            }
            if(Found->second != Revision)
            {
                Found->second = Revision;
                if(bRebuild && !InBounds.World.IsEmpty()) { Dirty.push_back(InBounds.World); }
            }
        });
        for(const GameCore::AABB& Box : Dirty) { RebuildTilesIn(Box); }
    }

    // === Agents ===============================================================

    bool NavWorld::MoveTo(const GameCore::EntityID InAgent, const EngineCore::Vector3& InTarget)
    {
        NavAgentComponent* Agent = World.GetEntity(InAgent).GetComponent<NavAgentComponent>();
        if(!Agent) { return false; }
        Agent->Target = InTarget;
        Agent->bHasTarget = 1;
        Agent->Status = NAS_Moving;
        if(!Crowd || Agent->CrowdIndex < 0) { return true; }

        const dtNavMeshQuery* CrowdQuery = Crowd->getNavMeshQuery();
        float Point[3], Nearest[3];
        NavDetail::ToFloat3(InTarget, Point);
        dtPolyRef Ref = 0;
        const float Extents[3] = { 300.0f, 500.0f, 300.0f };
        CrowdQuery->findNearestPoly(Point, Extents, Crowd->getFilter(0), &Ref, Nearest);
        if(!Ref)
        {
            Agent->Status = NAS_Failed;
            Agent->bHasTarget = 0;
            ++FailedMoves;
            return false;
        }
        Crowd->requestMoveTarget(Agent->CrowdIndex, Ref, Nearest);
        return true;
    }

    void NavWorld::StopMoving(const GameCore::EntityID InAgent)
    {
        NavAgentComponent* Agent = World.GetEntity(InAgent).GetComponent<NavAgentComponent>();
        if(!Agent) { return; }
        Agent->bHasTarget = 0;
        Agent->Status = NAS_Idle;
        Agent->DesiredVelocity = EngineCore::Vector3();
        if(Crowd && Agent->CrowdIndex >= 0) { Crowd->resetMoveTarget(Agent->CrowdIndex); }
    }

    size_t NavWorld::GetAgentCorners(const GameCore::EntityID InAgent, std::vector<EngineCore::Vector3>& OutCorners) const
    {
        OutCorners.clear();
        const NavAgentComponent* Agent = World.GetEntity(InAgent).GetComponent<NavAgentComponent>();
        if(!Crowd || !Agent || Agent->CrowdIndex < 0) { return 0; }
        const dtCrowdAgent* Detour = Crowd->getAgent(Agent->CrowdIndex);
        if(!Detour || !Detour->active) { return 0; }
        OutCorners.push_back(NavDetail::FromFloat3(Detour->npos));
        for(int Index = 0; Index < Detour->ncorners; ++Index) { OutCorners.push_back(NavDetail::FromFloat3(&Detour->cornerVerts[Index * 3])); }
        return OutCorners.size();
    }

    uint32_t NavWorld::TakeFailedMoveCount()
    {
        const uint32_t Count = FailedMoves;
        FailedMoves = 0;
        return Count;
    }

    void NavWorld::RemoveAgent(const uint64_t InKey, const int32_t InIndex)
    {
        if(Crowd && InIndex >= 0) { Crowd->removeAgent(InIndex); }
        AgentIndices.erase(InKey);
    }

    void NavWorld::UpdateAgents(const float InDeltaSeconds)
    {
        if(!Crowd) { return; }
        const dtNavMeshQuery* CrowdQuery = Crowd->getNavMeshQuery();
        const float Extents[3] = { 200.0f, 400.0f, 200.0f };

        /* Into the crowd, and the crowd's copies to where their entities are. */
        std::unordered_set<uint64_t> Seen;
        World.Each<NavAgentComponent>([&](const GameCore::Entity& InEntity, NavAgentComponent& InAgent)
        {
            const uint64_t Key = InEntity.GetID().Pack();
            Seen.insert(Key);
            const EngineCore::Vector3 Position = InEntity.GetWorldPose().Position;
            float Point[3];
            NavDetail::ToFloat3(Position, Point);

            if(InAgent.CrowdIndex >= 0)
            {
                dtCrowdAgent* Detour = Crowd->getEditableAgent(InAgent.CrowdIndex);
                if(!Detour || !Detour->active) { InAgent.CrowdIndex = -1; AgentIndices.erase(Key); }
                else if(!InAgent.bDriveTransform)
                {
                    const float DX = Point[0] - Detour->npos[0], DZ = Point[2] - Detour->npos[2];
                    if(DX * DX + DZ * DZ > TELEPORT_DISTANCE * TELEPORT_DISTANCE)
                    {
                        RemoveAgent(Key, InAgent.CrowdIndex);
                        InAgent.CrowdIndex = -1;
                    }
                    else
                    {
                        /* Physics moved it: steer from where it really is. */
                        dtPolyRef Ref = 0;
                        float Nearest[3];
                        CrowdQuery->findNearestPoly(Point, Extents, Crowd->getFilter(0), &Ref, Nearest);
                        if(Ref) { dtVcopy(Detour->npos, Nearest); }
                    }
                }
            }

            if(InAgent.CrowdIndex < 0)
            {
                dtPolyRef Ref = 0;
                float Nearest[3];
                CrowdQuery->findNearestPoly(Point, Extents, Crowd->getFilter(0), &Ref, Nearest);
                if(!Ref)
                {
                    InAgent.Status = InAgent.bHasTarget ? uint8_t(NAS_Failed) : uint8_t(NAS_Idle);
                    InAgent.DesiredVelocity = EngineCore::Vector3();
                    return;
                }
                const dtCrowdAgentParams Params = AgentParams(InAgent);
                const int Index = Crowd->addAgent(Nearest, &Params);
                if(Index < 0) { return; }
                InAgent.CrowdIndex = int16_t(Index);
                AgentIndices[Key] = Index;
                if(InAgent.bHasTarget) { MoveTo(InEntity.GetID(), InAgent.Target); }
            }
            else
            {
                /* Speed and avoidance a tree may have changed. */
                const dtCrowdAgentParams Params = AgentParams(InAgent);
                Crowd->updateAgentParameters(InAgent.CrowdIndex, &Params);
            }
        });

        /* Agents whose entity or component went. */
        for(auto It = AgentIndices.begin(); It != AgentIndices.end();)
        {
            if(Seen.count(It->first)) { ++It; continue; }
            Crowd->removeAgent(It->second);
            It = AgentIndices.erase(It);
        }

        Crowd->update(InDeltaSeconds, nullptr);

        /* Back out: velocities, arrivals, failures. */
        World.Each<NavAgentComponent>([&](const GameCore::Entity& InEntity, NavAgentComponent& InAgent)
        {
            if(InAgent.CrowdIndex < 0) { return; }
            const dtCrowdAgent* Detour = Crowd->getAgent(InAgent.CrowdIndex);
            if(!Detour || !Detour->active) { return; }

            const EngineCore::Vector3 At = NavDetail::FromFloat3(Detour->npos);
            EngineCore::Vector3 Velocity = NavDetail::FromFloat3(Detour->vel);

            if(InAgent.bHasTarget)
            {
                const float Distance = NavDetail::Distance2D(At, InAgent.Target);
                float Remaining = 0.0f;
                EngineCore::Vector3 From = At;
                for(int Index = 0; Index < Detour->ncorners; ++Index)
                {
                    const EngineCore::Vector3 Corner = NavDetail::FromFloat3(&Detour->cornerVerts[Index * 3]);
                    Remaining += NavDetail::Distance2D(From, Corner);
                    From = Corner;
                }
                InAgent.RemainingDistance = std::max(Remaining, Distance * (Detour->ncorners > 0 ? 0.0f : 1.0f));

                if(Detour->targetState == DT_CROWDAGENT_TARGET_FAILED)
                {
                    InAgent.Status = NAS_Failed;
                    InAgent.bHasTarget = 0;
                    ++FailedMoves;
                    Crowd->resetMoveTarget(InAgent.CrowdIndex);
                    Velocity = EngineCore::Vector3();
                }
                else if(Distance <= std::max(InAgent.AcceptanceRadius, 1.0f)
                        || (Detour->targetState == DT_CROWDAGENT_TARGET_VALID && !Detour->partial && Detour->ncorners == 0 && Distance < InAgent.Radius * 4.0f))
                {
                    InAgent.Status = NAS_Arrived;
                    InAgent.bHasTarget = 0;
                    InAgent.RemainingDistance = 0.0f;
                    Crowd->resetMoveTarget(InAgent.CrowdIndex);
                    Velocity = EngineCore::Vector3();
                }
                else
                {
                    InAgent.Status = NAS_Moving;
                }
            }
            else if(InAgent.Status == NAS_Moving)
            {
                InAgent.Status = NAS_Idle;
            }

            /* Idle, the crowd's separation still nudges: ignore a drift. */
            if(!InAgent.bHasTarget && Velocity.GetLength() < 20.0f) { Velocity = EngineCore::Vector3(); }
            InAgent.DesiredVelocity = Velocity;

            if(InAgent.bDriveTransform)
            {
                GameCore::TransformPose Pose = InEntity.GetWorldPose();
                Pose.Position = At;
                float Ground = 0.0f;
                if(Host.GroundHeight && Host.GroundHeight(World, EngineCore::Vector3(At.GetX(), At.GetY() + InAgent.Height * 0.5f, At.GetZ()), Ground, Host.UserData))
                {
                    Pose.Position.SetY(Ground);
                }
                const double Speed2 = double(Velocity.GetX()) * Velocity.GetX() + double(Velocity.GetZ()) * Velocity.GetZ();
                if(Speed2 > 100.0)
                {
                    Pose.Rotation.SetY(float(std::atan2(double(Velocity.GetX()), double(Velocity.GetZ())) * 180.0 / 3.14159265358979323846));
                }
                InEntity.SetWorldPose(Pose);
            }
        });
    }

    void NavWorld::Tick(const float InDeltaSeconds)
    {
        EnsureNavMesh();
        if(!Mesh.IsValid()) { return; }

        FilterRefresh -= InDeltaSeconds;
        if(FilterRefresh <= 0.0f)
        {
            FilterRefresh = 1.0f;
            RefreshFilter();
        }
        TerrainWatch -= InDeltaSeconds;
        if(TerrainWatch <= 0.0f)
        {
            TerrainWatch = 1.0f;
            UpdateTerrainWatch();
        }
        UpdateObstacles();
        UpdateAgents(std::max(InDeltaSeconds, 0.0f));
    }
}
