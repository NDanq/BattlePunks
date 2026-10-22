#ifndef RENDERCORE_RECTLIGHTCOMPONENT_H
#define RENDERCORE_RECTLIGHTCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "Rect Light". A glowing rectangle, Width x Height world units, centred on
     * its entity and facing along its forward (LightForward): its right is the
     * entity's +X, its up +Y. Lit by linearly transformed cosines
     * (GLOBAL_ILLUMINATION.md, Phase 3): soft-edged highlights the rectangle's
     * shape, and light that falls off with its solid angle, windowed to nothing
     * at Range. Intensity is its surface's brightness, so a larger one lights
     * more. Only its front lights; it casts no shadow yet. */
    struct RENDERCORE_API RectLightComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Color   Color{ 255, 255, 255 };
        float               Intensity = 4.0f;
        float               Width     = 200.0f;
        float               Height    = 100.0f;
        float               Range     = 1500.0f;
        uint8_t             bActive   = 1;
    };
}

#endif //RENDERCORE_RECTLIGHTCOMPONENT_H
