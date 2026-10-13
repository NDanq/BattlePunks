#ifndef AICORE_NAVDEBUGDRAW_H
#define AICORE_NAVDEBUGDRAW_H

#include <cstdint>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/Math/Vector.h"

/*
 * Navmeshes and paths as lines, for an editor overlay or an in-game debug
 * view. Recast's own DebugUtils draws them (duDebugDrawNavMesh and friends)
 * through an adapter inside AICore, so callers never include Recast: they
 * get a line sink.
 *
 * Colours are 0xAARRGGBB.
 */
namespace AICore
{
    class NavMesh;

    struct NavDebugLine
    {
        EngineCore::Vector3 From;
        EngineCore::Vector3 To;
        uint32_t Color = 0xFFFFFFFF;
    };

    /* What to draw. */
    enum ENavDebugFlags : uint32_t
    {
        NDF_PolygonEdges = 1 << 0,   // every polygon's outline (teal; areas in their colours)
        NDF_Boundaries   = 1 << 1,   // the outer edges, darker and thicker
        NDF_TileBounds   = 1 << 2,   // each tile's box
        NDF_Obstacles    = 1 << 3,   // the tile cache's obstacles
        NDF_Default      = NDF_PolygonEdges | NDF_Boundaries,
    };

    /* The colour an area draws in: ground teal, High Cost amber, a custom
     * area its own hue. */
    AICORE_API uint32_t GetNavAreaColor(uint8_t InArea);

    /* InMesh as lines, appended to OutLines. Lifted InLift above the
     * surface, so they show over the ground they sit on. */
    AICORE_API void DrawNavMesh(const NavMesh& InMesh, std::vector<NavDebugLine>& OutLines, uint32_t InFlags = NDF_Default,
                                float InLift = 8.0f);

    /* A path: its segments, and a small cross at each corner. */
    AICORE_API void DrawNavPath(const std::vector<EngineCore::Vector3>& InPoints, std::vector<NavDebugLine>& OutLines,
                                uint32_t InColor = 0xFFFFD040, float InLift = 12.0f);
}

#endif //AICORE_NAVDEBUGDRAW_H
