#ifndef AICORE_NAVMESH_H
#define AICORE_NAVMESH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/NavGeometry.h"
#include "Public/AICore/NavTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Vector.h"

class dtNavMesh;
class dtTileCache;

/*
 * A navmesh: Recast's tiled build, kept as DetourTileCache layers so that
 * tiles rebuild cheaply and obstacles (doors, crates) cut it at runtime.
 *
 * THE PIPELINE, per tile (Recast's Sample_TempObstacles): rasterize the
 * tile's triangles into a heightfield; drop low obstacles, ledges and low
 * ceilings; erode by the agent radius; mark the area boxes; split into
 * height layers; compress each layer into the tile cache. The tile cache
 * then builds each layer's Detour polygons -- at bake, at load, and whenever
 * an obstacle near it changes.
 *
 * .nmnav -- "NMNV", a version, then chunks (CoreArchive):
 *   HEAD v1   the settings, their hash, the geometry hash, the bounds, the
 *             tile grid
 *   LAYR v1   one tile cache layer: its compressed bytes, as built
 * A chunk a newer build adds is stepped over.
 *
 * Game thread only.
 */
namespace AICore
{
    class AICORE_API NavMesh
    {
    public:
        NavMesh();
        ~NavMesh();
        NavMesh(const NavMesh&)            = delete;
        NavMesh& operator=(const NavMesh&) = delete;

        /* Every tile of InBounds (empty: the geometry's bounds) from
         * InGeometry. Replaces what it held. False, with OutError, if
         * nothing walkable came of it. */
        bool Build(const NavInputGeometry& InGeometry, const NavBuildSettings& InSettings,
                   const GameCore::AABB& InBounds = GameCore::AABB(), NavBuildStats* OutStats = nullptr,
                   std::string* OutError = nullptr);

        /* The tiles overlapping InRegion again, from InGeometry (which must
         * cover them; GatherNavGeometry for InRegion grown by a tile does).
         * Obstacles are kept. How many tiles were rebuilt. */
        int32_t RebuildTiles(const NavInputGeometry& InGeometry, const GameCore::AABB& InRegion);

        void Clear();
        bool IsValid() const;

        // === Files ==========================================================
        std::vector<uint8_t> Save() const;
        bool Load(const std::vector<uint8_t>& InBytes, std::string* OutError = nullptr);

        /* Through the file provider / to disk (a .tmp and a rename). */
        bool LoadFile(const std::string& InPath, std::string* OutError = nullptr);
        bool SaveFile(const std::string& InDiskPath, std::string* OutError = nullptr) const;

        // === Obstacles ======================================================
        // Cut the navmesh where they stand; the tiles they touch rebuild
        // over the next Update calls. 0 when the cache is full.

        uint32_t AddBoxObstacle(const EngineCore::Vector3& InCenter, const EngineCore::Vector3& InHalfExtents, float InYawDegrees);
        uint32_t AddCylinderObstacle(const EngineCore::Vector3& InFeet, float InRadius, float InHeight);
        void     RemoveObstacle(uint32_t InObstacle);
        size_t   GetObstacleCount() const;

        /* Rebuilds what obstacles changed. True when nothing is pending. */
        bool Update();

        // === What it is =====================================================
        const NavBuildSettings& GetSettings() const;
        GameCore::AABB          GetBounds() const;
        uint64_t                GetGeometryHash() const;
        NavBuildStats           GetStats() const;

        /* Bumped by every build, load, tile rebuild and obstacle update: when
         * a debug draw or a cached path is stale. */
        uint64_t GetRevision() const;

        /* Detour's, for NavQuery, the crowd and debug drawing. */
        dtNavMesh*   GetDetourMesh() const;
        dtTileCache* GetTileCache() const;

    private:
        struct Impl;
        Impl* P = nullptr;
    };
}

#endif //AICORE_NAVMESH_H
