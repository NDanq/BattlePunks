#ifndef RENDERCORE_DIRECTIONALLIGHTCOMPONENT_H
#define RENDERCORE_DIRECTIONALLIGHTCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/Components/LightShadow.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "Directional Light". The scene's sun: the first active one WorldBridge
     * finds lights every Lit material (RenderLighting.h). It travels along
     * its entity's forward (+Z turned by the Transform's Rotation;
     * LightForward), so a light with no Transform shines along +Z; -Y is
     * down. Without one the scene keeps the default light -- from above, as
     * Mesh.fsh always was. Its position only places the editor's gizmo. */
    struct RENDERCORE_API DirectionalLightComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Color   Color{ 255, 255, 255 };
        float               Intensity = 3.0f;
        uint8_t             bActive   = 1;

        /* Shadows (LightShadow.h). */
        uint8_t             bCastShadows       = 1;
        uint8_t             ShadowTechnique    = LST_Cascaded;
        EngineCore::Color   ShadowTint{ 40, 50, 80 };
        float               ShadowBias         = 0.0015f;   // depth, x 256 texels of its cascade (Shadows.glsl), more where the light grazes
        float               ShadowNormalBias   = 2.0f;      // world units, along the receiver's normal

        /* How far from the eye the shadow reaches, and Cascaded's split:
         * CascadeCount (1-4) slices blended uniform (0) to logarithmic (1). */
        float               ShadowDistance     = 4000.0f;
        int32_t             CascadeCount       = 4;
        float               CascadeSplitLambda = 0.75f;

        /* Baked lighting only (Build > Bake Lightmaps): the sun's angular
         * radius, degrees -- how soft its baked shadows are; 0 is sharp. */
        float               SourceAngle        = 0.75f;
    };
}

#endif //RENDERCORE_DIRECTIONALLIGHTCOMPONENT_H
