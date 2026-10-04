#ifndef RENDERCORE_REFLECTIONPROBECOMPONENT_H
#define RENDERCORE_REFLECTIONPROBECOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/Components/SceneCaptureProbeComponent.h"   // ESceneCaptureFrequency

namespace RenderCore
{
    /*
     * "Reflection Probe": image-based lighting from where its entity stands
     * (RENDERPIPELINE.md, "Image-based lighting"). It captures the scene in
     * six directions (HDR), prefilters that into rough-to-sharp reflections
     * and a diffuse irradiance, and Lit surfaces within its influence take
     * their ambient light and reflections from it instead of the sky.
     *
     *   Radius     the influence: full strength inside 80% of it, fading to
     *              none at it. With a Box Extent, the influence is that box
     *              (half sizes, the entity's axes ignored) and reflections are
     *              box-projected onto it -- right for a room.
     *   Intensity  scales what it gives.
     *   Frequency  Once (the default: when it first lights anything), Interval,
     *              Every Frame or Manual (Capture Now).
     *
     * Up to four light the view at once, the nearest the camera first.
     */
    struct RENDERCORE_API ReflectionProbeComponent
    {
        GAMECORE_COMPONENT_BODY()

        float             Radius     = 1000.0f;
        EngineCore::Vector3 BoxExtent{ 0.0, 0.0, 0.0 };   // half sizes; 0 = a sphere of Radius
        float             Intensity  = 1.0f;
        int32_t           Resolution = 128;               // a cube face's side
        uint8_t           Frequency  = SCF_Once;          // ESceneCaptureFrequency
        float             Interval   = 1.0f;
        float             NearClip   = 5.0f;
        float             FarClip    = 20000.0f;
        uint8_t           bCaptureShadows = 1;
        uint8_t           bEnabled   = 1;
        uint8_t           bCaptureNow = 0;
    };
}

#endif //RENDERCORE_REFLECTIONPROBECOMPONENT_H
