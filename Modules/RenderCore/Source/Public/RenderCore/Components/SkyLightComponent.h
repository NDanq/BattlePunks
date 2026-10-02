#ifndef RENDERCORE_SKYLIGHTCOMPONENT_H
#define RENDERCORE_SKYLIGHTCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "Sky Light". Ambient for Lit materials: SkyColor from above (-Y),
     * GroundColor from below, blended by the normal. The first active one wins. */
    struct RENDERCORE_API SkyLightComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Color SkyColor{ 120, 130, 150 };
        EngineCore::Color GroundColor{ 60, 55, 50 };
        float             Intensity = 0.6f;
        uint8_t           bActive   = 1;
    };
}

#endif //RENDERCORE_SKYLIGHTCOMPONENT_H
