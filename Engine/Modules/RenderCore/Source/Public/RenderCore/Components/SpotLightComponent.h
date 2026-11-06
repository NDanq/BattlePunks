#ifndef RENDERCORE_SPOTLIGHTCOMPONENT_H
#define RENDERCORE_SPOTLIGHTCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/Components/LightShadow.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "Spot Light". A cone from its entity's position along its forward
     * (LightForward): full strength inside InnerConeAngle, fading to nothing
     * at OuterConeAngle (half-angles, degrees) and, by inverse square, at
     * Range. A shadowed one draws one tile of the local shadow atlas. */
    struct RENDERCORE_API SpotLightComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Color   Color{ 255, 255, 255 };
        float               Intensity      = 10.0f;
        float               Range          = 1000.0f;
        float               InnerConeAngle = 20.0f;
        float               OuterConeAngle = 30.0f;
        uint8_t             bActive        = 1;

        /* Shadows (LightShadow.h): Standard or Modulated. */
        uint8_t             bCastShadows     = 1;
        uint8_t             ShadowTechnique  = LST_Standard;
        EngineCore::Color   ShadowTint{ 40, 50, 80 };
        float               ShadowBias       = 0.002f;
        float               ShadowNormalBias = 1.5f;

        /* The bulb's radius, world units: how soft its baked shadows are
         * (Build > Bake Lightmaps), and live, the sphere its highlights and
         * light come from (GLOBAL_ILLUMINATION.md, Phase 3); 0 is a point. */
        float               SourceRadius     = 4.0f;
    };
}

#endif //RENDERCORE_SPOTLIGHTCOMPONENT_H
