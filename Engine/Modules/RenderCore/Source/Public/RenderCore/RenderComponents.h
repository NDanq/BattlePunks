#ifndef RENDERCORE_RENDERCOMPONENTS_H
#define RENDERCORE_RENDERCOMPONENTS_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderProxy.h"

/*
 * RenderCore's own GameCore components, declared the same way a game
 * declares its own -- GAMECORE_COMPONENT_BODY() first, RENDERCORE_PROXY_BODY()
 * second, GAMECORE_DEFINE_COMPONENT()/RENDERCORE_DEFINE_PROXY_COMPONENT() once
 * in RenderComponents.cpp. Nothing in World or ComponentPool is special-cased
 * for either of these; a game's own render component follows the identical
 * two-macro shape.
 */
namespace RenderCore
{
    /* "Render". Draws Mesh (a runtime-relative .nmmesh path) if set and
     * loadable, else a Width x Height tinted quad; both scaled by the
     * Transform.
     *
     * Material names a material graph's output ("Glow" = Materials/Glow.fsh +
     * Glow.material) and overrides every mesh section's material. Empty, or
     * unloadable (warned once), draws Tint or the sections' own materials. */
    enum : size_t { RENDER_MATERIAL_NAME_MAX = 48 };   // including the NUL
    enum : size_t { RENDER_MESH_PATH_MAX     = 128 };  // including the NUL

    struct RENDERCORE_API RenderComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()

        float             Width  = 1.0f;
        float             Height = 1.0f;
        EngineCore::Color Tint{255, 255, 255};
        uint8_t           bVisible = 1;
        char              Material[RENDER_MATERIAL_NAME_MAX] = {};
        char              Mesh[RENDER_MESH_PATH_MAX] = {};
    };

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

    /* RenderComponent's proxy factory, for Renderer::Initialize to register. */
    RENDERCORE_API CreateProxyFn GetRenderComponentFactory();
}

#endif //RENDERCORE_RENDERCOMPONENTS_H
