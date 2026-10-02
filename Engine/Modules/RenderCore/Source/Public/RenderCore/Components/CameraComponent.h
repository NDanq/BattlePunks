#ifndef RENDERCORE_CAMERACOMPONENT_H
#define RENDERCORE_CAMERACOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    /* "Camera". Position comes from the sibling TransformComponent (identity
     * origin if there is none) -- a CameraComponent carries no position of
     * its own, the same "read the sibling, do not duplicate it" rule
     * BoundsComponent's own refit pass follows for the same reason. The
     * first entity with an active CameraComponent that WorldBridge's
     * per-frame view build finds wins; see DESIGN.md's "View" section. */
    struct RENDERCORE_API CameraComponent
    {
        GAMECORE_COMPONENT_BODY()

        float   ZNear    = -1.0f;
        float   ZFar     = 1000.0f;
        uint8_t bActive  = 1;
    };
}

#endif //RENDERCORE_CAMERACOMPONENT_H
