#ifndef RENDERCORE_GIVOLUMECOMPONENT_H
#define RENDERCORE_GIVOLUMECOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "GI Volume". Dynamic diffuse GI (GLOBAL_ILLUMINATION.md, Phase 6; DDGI):
     * a grid of irradiance probes filling a box Extent (half sizes) round its
     * entity, Probe Spacing apart -- at most 32 x 8 x 32 of them. A few probes a
     * frame render the scene round them ([Rendering] GIProbesPerFrame), lit by
     * the live lights, the sky and the volume itself, so light bounces again
     * with each round. Inside it, Lit surfaces that are not baked take their
     * diffuse ambient from the eight probes round them, weighted so a probe
     * behind a wall does not light through it, in place of the reflection
     * probes'. Intensity scales that light. One volume lights the scene: the
     * nearest the camera. */
    struct RENDERCORE_API GIVolumeComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 Extent{ 1000.0, 400.0, 1000.0 };   // half sizes, world units
        float               ProbeSpacing = 200.0f;              // world units
        float               Intensity    = 1.0f;
        uint8_t             bEnabled     = 1;
    };
}

#endif //RENDERCORE_GIVOLUMECOMPONENT_H
