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

        /* ECameraProjection. Orthographic is the 2D pixel view (position
         * only); Perspective looks along the Transform's forward (+Z turned
         * by its Rotation, as a light), FovY degrees tall, ZNear clamped to
         * at least 1. */
        uint8_t Projection = 0;
        float   FovY       = 60.0f;
    };

    enum ECameraProjection : uint8_t
    {
        CP_Orthographic = 0,
        CP_Perspective  = 1,
    };
}

#endif //RENDERCORE_CAMERACOMPONENT_H
