#ifndef RENDERCORE_POSTPROCESSCOMPONENT_H
#define RENDERCORE_POSTPROCESSCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/Components/RenderComponent.h"   // RENDER_MATERIAL_PATH_MAX

namespace RenderCore
{
    /*
     * "Post Process". A post process stack (a *.pp.nmgraph's runtime path;
     * PostProcessStack.h) applied while the camera is in this entity's
     * volume: its sibling Bounds component's World box. Full strength
     * inside, fading to nothing over BlendDistance outside (0: a hard edge).
     * With no Bounds it applies everywhere.
     *
     * Every volume the camera is in blends, Priority low first, so a higher
     * one's settings win where both have the effect (BlendPostProcess). Stack
     * may also name a single PostProcess material graph, run after the
     * effects -- what this component held before stacks.
     */
    struct RENDERCORE_API PostProcessComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    Stack[RENDER_MATERIAL_PATH_MAX] = {};
        int32_t Priority = 0;
        uint8_t bEnabled = 1;
        float   BlendDistance = 0.0f;
    };
}

#endif //RENDERCORE_POSTPROCESSCOMPONENT_H
