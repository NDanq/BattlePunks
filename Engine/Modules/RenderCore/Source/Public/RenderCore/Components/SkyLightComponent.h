#ifndef RENDERCORE_SKYLIGHTCOMPONENT_H
#define RENDERCORE_SKYLIGHTCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "Sky Light". Ambient for Lit materials: SkyColor from above (+Y),
     * GroundColor from below, blended by the normal. The first active one wins.
     *
     * Environment (v2): an HDRI (an equirectangular .nmtexture, RGBE8 from a
     * Radiance .hdr) to light by instead: diffuse and reflections, prefiltered
     * like a reflection probe (RENDERPIPELINE.md, "Image-based lighting"),
     * scaled by EnvironmentIntensity and turned EnvironmentRotation degrees
     * about +Y. DrawBackground shows it behind the scene. Without one, the
     * colours make the sky that surfaces reflect. */
    struct RENDERCORE_API SkyLightComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Color SkyColor{ 120, 130, 150 };
        EngineCore::Color GroundColor{ 60, 55, 50 };
        float             Intensity = 0.6f;
        uint8_t           bActive   = 1;
        char              Environment[128] = {};          // RENDER_MATERIAL_PATH_MAX
        float             EnvironmentIntensity = 1.0f;
        float             EnvironmentRotation  = 0.0f;    // degrees about +Y
        uint8_t           bDrawBackground      = 1;
    };
}

#endif //RENDERCORE_SKYLIGHTCOMPONENT_H
