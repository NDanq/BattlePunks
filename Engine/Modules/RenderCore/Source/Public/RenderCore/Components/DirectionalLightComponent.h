#ifndef RENDERCORE_DIRECTIONALLIGHTCOMPONENT_H
#define RENDERCORE_DIRECTIONALLIGHTCOMPONENT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "Directional Light". The scene's sun: the first active one WorldBridge
     * finds lights every Lit material (RenderLighting.h). Direction is the
     * way light travels, world space; +Y is down. Without one the scene keeps
     * the default light -- from above, as Mesh.fsh always was. */
    struct RENDERCORE_API DirectionalLightComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 Direction{ 0.35, 1.0, 0.55 };
        EngineCore::Color   Color{ 255, 255, 255 };
        float               Intensity = 3.0f;
        uint8_t             bActive   = 1;
    };
}

#endif //RENDERCORE_DIRECTIONALLIGHTCOMPONENT_H
