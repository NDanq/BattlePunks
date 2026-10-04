#ifndef GAMECORE_MOBILITY_H
#define GAMECORE_MOBILITY_H

#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    /*
     * An entity's mobility: whether it may move or change at run time.
     *
     *   Dynamic  the default: refitted, extracted and ticked every frame.
     *   Static   level geometry that never moves in play. Its bounds are
     *            not refitted once in the spatial index, two static
     *            entities are never tested for overlap, a prefab instance
     *            whose root is static gets no phase ticks (events still
     *            arrive, as EventsOnly), and the renderer keeps its draws in
     *            a cached static list instead of rebuilding them each frame
     *            (RenderCore's RENDERPIPELINE.md, "Static draw lists").
     *            With baked lighting on (WorldSettings.h), a static light or
     *            Render component is what Build > Bake Lightmaps bakes, and
     *            the renderer then draws it from the bake ("Baked lighting").
     *
     * Tools may still move a static entity (the editor does): the renderer
     * notices a changed transform or component and rebuilds that entity's
     * cached draws, and World::RefreshEntityBounds refits it.
     */
    enum class EMobility : uint8_t
    {
        Dynamic = 0,
        Static  = 1,
    };

    /* "Dynamic" / "Static". */
    GAMECORE_API const char* MobilityName(EMobility InMobility);

    /* MobilityName's inverse, case-insensitive. False, OutMobility untouched,
     * for anything else. */
    GAMECORE_API bool ParseMobility(const char* InText, EMobility& OutMobility);
}

#endif //GAMECORE_MOBILITY_H
