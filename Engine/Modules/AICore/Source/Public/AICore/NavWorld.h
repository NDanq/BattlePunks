#ifndef AICORE_NAVWORLD_H
#define AICORE_NAVWORLD_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/NavMesh.h"
#include "Public/AICore/NavQuery.h"
#include "Public/AICore/NavTypes.h"
#include "Public/AICore/Perception.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }
class dtCrowd;

/*
 * A World's navigation: its navmesh, a query for the game thread, the crowd
 * its Nav Agents walk in, and its Nav Obstacles. Each World's AIWorld owns
 * one; NavWorld::Find reaches it.
 *
 * THE NAVMESH comes from the level's Nav Mesh Volume (the first one): its
 * baked file, else -- bBuildAtRuntime -- built from the World's geometry on
 * the first tick. Build() rebuilds it on demand (the editor's Build
 * Navigation); RebuildTilesIn rebuilds the tiles under a box.
 *
 * AGENTS. The "AI.Nav" system (TP_PrePhysics, after the brains) adds each
 * Nav Agent to the crowd, moves the crowd's copy to where its entity is,
 * steps the crowd -- path corridors and local avoidance -- and writes each
 * agent's DesiredVelocity and Status.
 */
namespace AICore
{
    class AICORE_API NavWorld
    {
    public:
        explicit NavWorld(GameCore::World& InWorld);
        ~NavWorld();
        NavWorld(const NavWorld&)            = delete;
        NavWorld& operator=(const NavWorld&) = delete;

        /* InWorld's (its AIWorld's); null if it has none. */
        static NavWorld* Find(const GameCore::World* InWorld);

        GameCore::World& GetWorld() { return World; }

        // === The navmesh ===================================================

        /* From the volume's settings and the World's geometry, now. False
         * (OutError) with no volume or nothing walkable. */
        bool Build(NavBuildStats* OutStats = nullptr, std::string* OutError = nullptr);
        /* From explicit geometry and settings (tests, tools). */
        bool BuildFrom(const NavInputGeometry& InGeometry, const NavBuildSettings& InSettings,
                       const GameCore::AABB& InBounds = GameCore::AABB(), NavBuildStats* OutStats = nullptr,
                       std::string* OutError = nullptr);
        bool LoadFile(const std::string& InPath, std::string* OutError = nullptr);
        bool SaveFile(const std::string& InDiskPath, std::string* OutError = nullptr) const;

        /* The tiles under InRegion again, from the World's geometry. */
        int32_t RebuildTilesIn(const GameCore::AABB& InRegion);

        /* Whether the baked file's geometry hash still matches the World. */
        bool IsUpToDate() const;

        bool        HasNavMesh() const;
        NavMesh&    GetNavMesh() { return Mesh; }
        const NavMesh& GetNavMesh() const { return Mesh; }
        NavQuery&   GetQuery() { return Query; }
        const NavQuery& GetQuery() const { return Query; }
        /* The level's: walkable, and its Nav Modifiers' area costs. */
        const NavFilter& GetDefaultFilter() const { return Filter; }
        NavFilter&       GetDefaultFilter() { return Filter; }

        /* Bumped by every change of the navmesh (builds, loads, tile
         * rebuilds, obstacles): debug draws redraw on it. */
        uint64_t GetRevision() const;

        // === Agents ========================================================

        /* InAgent (a Nav Agent) walks to InTarget. False if it has no agent
         * component or the target is nowhere near the navmesh. */
        bool MoveTo(GameCore::EntityID InAgent, const EngineCore::Vector3& InTarget);
        void StopMoving(GameCore::EntityID InAgent);

        /* The crowd's corners ahead of InAgent (the debugger's path). */
        size_t GetAgentCorners(GameCore::EntityID InAgent, std::vector<EngineCore::Vector3>& OutCorners) const;

        /* Path requests that failed since the last call (the stats). */
        uint32_t TakeFailedMoveCount();

        /* The system's step: navmesh (first load, obstacles, dirty tiles),
         * then agents. */
        void Tick(float InDeltaSeconds);

        /* Loads or builds the navmesh if it has none yet, as the first Tick
         * does. */
        bool EnsureNavMesh();

        dtCrowd* GetCrowd() const { return Crowd; }

        /* What the game does for agents (AIWorld::SetHost): GroundHeight. */
        void SetHost(const AIHost& InHost) { Host = InHost; }

    private:
        void ResetCrowd();
        void RefreshFilter();
        void UpdateObstacles();
        void UpdateTerrainWatch();
        void UpdateAgents(float InDeltaSeconds);
        void RemoveAgent(uint64_t InKey, int32_t InIndex);

        GameCore::World& World;
        NavMesh   Mesh;
        NavQuery  Query;
        NavFilter Filter;
        dtCrowd*  Crowd = nullptr;
        AIHost    Host;
        bool      bTriedLoad = false;
        uint64_t  LoadedGeometryHash = 0;
        uint64_t  ChangeRevision = 0;
        uint32_t  FailedMoves = 0;
        float     FilterRefresh = 0.0f;
        float     TerrainWatch = 0.0f;
        std::unordered_map<uint64_t, int32_t> AgentIndices;    // entity -> crowd index
        std::unordered_map<uint64_t, uint64_t> TerrainRevisions; // terrain entity -> revision
    };
}

#endif //AICORE_NAVWORLD_H
