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
    /* "Primitive". A flat, tinted quad -- the first concrete thing this
     * engine can draw. Width/Height are in the same world units as
     * TransformComponent::Scale; the proxy multiplies them together (see
     * PrimitiveProxy.cpp), so a Scale of (2,2,2) on the entity's Transform
     * doubles an already-sized quad rather than fighting it.
     *
     * Material names a material graph's output -- "Glow" is the shader
     * Materials/Glow.fsh and its parameters Materials/Glow.material (see
     * RenderContext::DrawQuadWithMaterial). Empty draws the plain Tint; a
     * material that cannot be loaded does the same, once warned. */
    enum : size_t { PRIMITIVE_MATERIAL_NAME_MAX = 48 };   // including the NUL

    struct RENDERCORE_API PrimitiveComponent
    {
        GAMECORE_COMPONENT_BODY()
        RENDERCORE_PROXY_BODY()

        float             Width  = 1.0f;
        float             Height = 1.0f;
        EngineCore::Color Tint{255, 255, 255};
        uint8_t           bVisible = 1;
        char              Material[PRIMITIVE_MATERIAL_NAME_MAX] = {};
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

    /*
     * PrimitiveComponent's own factory, exported for a caller that wants to
     * register it directly -- Renderer::Initialize (a later build-order
     * step) does exactly this. RENDERCORE_DEFINE_PROXY_COMPONENT's own
     * generated factory function is a plain `static` in RenderComponents.cpp
     * and never leaves that translation unit; this is the one door out.
     */
    RENDERCORE_API CreateProxyFn GetPrimitiveComponentFactory();
}

#endif //RENDERCORE_RENDERCOMPONENTS_H
