#ifndef AICORE_NAVMESHVOLUMECOMPONENT_H
#define AICORE_NAVMESHVOLUMECOMPONENT_H

#include <cstddef>
#include <cstdint>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/NavTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/Math/Vector.h"

namespace AICore
{
    enum : size_t { NAV_PATH_MAX = 128 };   // including the NUL

    /*
     * "Nav Mesh Volume". Where a level's navmesh is and how it is built: a
     * box (HalfExtents about the entity's position, axis-aligned) and the
     * build settings (NavBuildSettings, field for field). One a level; the
     * first found wins.
     *
     * NavMesh names the baked file (.nmnav, a runtime path), written by the
     * editor's Build Navigation beside the level. A running level loads it;
     * when it is missing or stale (its geometry hash no longer matches) and
     * bBuildAtRuntime is on, the level builds its own instead -- a second or
     * so for a small level.
     */
    struct AICORE_API NavMeshVolumeComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 HalfExtents{ 5000.0, 1000.0, 5000.0 };

        float   CellSize        = 15.0f;
        float   CellHeight      = 10.0f;
        float   AgentHeight     = 180.0f;
        float   AgentRadius     = 35.0f;
        float   AgentMaxClimb   = 45.0f;
        float   AgentMaxSlope   = 45.0f;
        int32_t TileSize        = 48;
        float   RegionMinSize   = 8.0f;
        float   RegionMergeSize = 20.0f;
        float   EdgeMaxLength   = 1200.0f;
        float   EdgeMaxError    = 1.3f;
        int32_t LayerMask       = -1;

        char    NavMesh[NAV_PATH_MAX] = {};
        uint8_t bBuildAtRuntime  = 1;
        /* The editor rebuilds tiles under what moves (static geometry,
         * terrain edits) as it moves; a level, when its terrain changes. */
        uint8_t bRebuildOnChange = 1;

        NavBuildSettings GetSettings() const;
        void             SetSettings(const NavBuildSettings& InSettings);

        /* Its box in world space. */
        GameCore::AABB GetWorldBounds() const;
    };
}

#endif //AICORE_NAVMESHVOLUMECOMPONENT_H
