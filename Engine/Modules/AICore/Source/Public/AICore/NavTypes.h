#ifndef AICORE_NAVTYPES_H
#define AICORE_NAVTYPES_H

#include <cstddef>
#include <cstdint>

#include "Public/AICore/AICoreAPI.h"

/*
 * What a navmesh is built with and marked by. World units throughout
 * (centimetres in the shipped content), +Y up, as Recast expects.
 */
namespace AICore
{
    /* Area ids, one byte a polygon. 0 is not walkable (Recast's RC_NULL_AREA);
     * 63 is plain ground (RC_WALKABLE_AREA). A Nav Modifier marks the rest. */
    enum : uint8_t
    {
        NAV_AREA_BLOCKED     = 0,
        NAV_AREA_HIGH_COST   = 1,    // mud, water, fire: walkable, avoided
        NAV_AREA_CUSTOM_MIN  = 2,    // a game's own areas, 2..62
        NAV_AREA_CUSTOM_MAX  = 62,
        NAV_AREA_GROUND      = 63,
        NAV_AREA_COUNT       = 64,
    };

    /* Polygon flags a query filter includes or excludes. */
    enum : uint16_t
    {
        NAV_FLAG_WALK     = 1 << 0,
        NAV_FLAG_DISABLED = 1 << 4,
        NAV_FLAG_ALL      = 0xFFFF,
    };

    /* The default cost of a High Cost area, a multiplier on distance. */
    constexpr float NAV_HIGH_COST_DEFAULT = 5.0f;

    /*
     * How a navmesh is voxelised and simplified, and for which agent. Stored
     * per level on its Nav Mesh Volume. The defaults suit a person-sized
     * agent in centimetres: 35 cm wide, 1.8 m tall, stepping 45 cm, on
     * slopes up to 45 degrees, in 15 cm voxels and 7.2 m tiles.
     *
     * Trivially copyable: it lives inside a component.
     */
    struct NavBuildSettings
    {
        float   CellSize        = 15.0f;    // voxel width (X/Z)
        float   CellHeight      = 10.0f;    // voxel height (Y)
        float   AgentHeight     = 180.0f;
        float   AgentRadius     = 35.0f;
        float   AgentMaxClimb   = 45.0f;    // the highest step it walks up
        float   AgentMaxSlope   = 45.0f;    // degrees
        int32_t TileSize        = 48;       // voxels a tile side
        float   RegionMinSize   = 8.0f;     // voxels a side: smaller islands are dropped
        float   RegionMergeSize = 20.0f;
        float   EdgeMaxLength   = 1200.0f;  // world units; 0 for no limit
        float   EdgeMaxError    = 1.3f;     // voxels a contour may stray
        int32_t LayerMask       = -1;       // the Rigid Body layers that are geometry

        /* Every field, for the .nmnav header's staleness check. */
        AICORE_API uint64_t Hash() const;
    };

    /* What a build did. */
    struct NavBuildStats
    {
        int32_t TilesX       = 0;
        int32_t TilesZ       = 0;
        int32_t Tiles        = 0;   // navmesh tiles made (a tile may hold several layers)
        int32_t Polygons     = 0;
        int32_t Triangles    = 0;   // input triangles
        double  Milliseconds = 0.0;
        size_t  Bytes        = 0;   // the saved form
    };

    /* How a path query ended. */
    enum class ENavPathStatus : uint8_t
    {
        None = 0,
        Complete,      // reaches the end point
        Partial,       // the end is unreachable: as near as it gets
        Failed,        // no start or end polygon, or no navmesh
    };

    /*
     * Which polygons a query walks and what each area costs. Every path and
     * crowd query takes one; NavWorld keeps the level's default (its Nav
     * Modifiers' costs).
     */
    struct NavFilter
    {
        uint16_t IncludeFlags = NAV_FLAG_WALK;
        uint16_t ExcludeFlags = NAV_FLAG_DISABLED;
        float    AreaCost[NAV_AREA_COUNT];

        NavFilter()
        {
            for(size_t Index = 0; Index < NAV_AREA_COUNT; ++Index) { AreaCost[Index] = 1.0f; }
            AreaCost[NAV_AREA_HIGH_COST] = NAV_HIGH_COST_DEFAULT;
        }
    };
}

#endif //AICORE_NAVTYPES_H
