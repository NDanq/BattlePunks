#ifndef RENDERCORE_POINTLIGHTCOMPONENT_H
#define RENDERCORE_POINTLIGHTCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/Components/LightShadow.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "Point Light". Shines every way from its entity's position, fading by
     * inverse square to nothing at Radius. WorldBridge sends the nearest
     * RENDER_LOCAL_LIGHT_MAX point and spot lights a frame; a shadowed one
     * draws six cube faces into the local shadow atlas. */
    struct RENDERCORE_API PointLightComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Color   Color{ 255, 255, 255 };
        float               Intensity = 8.0f;
        float               Radius    = 600.0f;
        uint8_t             bActive   = 1;

        /* Shadows (LightShadow.h): Standard or Modulated. */
        uint8_t             bCastShadows     = 0;
        uint8_t             ShadowTechnique  = LST_Standard;
        EngineCore::Color   ShadowTint{ 40, 50, 80 };
        float               ShadowBias       = 0.004f;
        float               ShadowNormalBias = 2.0f;

        /* Baked lighting only (Build > Bake Lightmaps): the bulb's radius,
         * world units -- how soft its baked shadows are; 0 is a point. */
        float               SourceRadius     = 4.0f;   // live too: a sphere light this big (GLOBAL_ILLUMINATION.md)
    };
}

#endif //RENDERCORE_POINTLIGHTCOMPONENT_H
