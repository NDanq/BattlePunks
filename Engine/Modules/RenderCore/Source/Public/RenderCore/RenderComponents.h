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
    /* "Render". What an entity draws: a Mesh if it names one, else a flat,
     * tinted quad. Width/Height size the quad, in the same world units as
     * TransformComponent::Scale; the proxy multiplies them together (see
     * RenderComponentProxy.cpp), so a Scale of (2,2,2) on the entity's
     * Transform doubles an already-sized quad rather than fighting it. A mesh
     * ignores Width/Height -- it has its own size, scaled by the Transform.
     *
     * Mesh is a runtime-relative path to a .nmmesh ("Content/Props/Crate
     * .nmmesh", see MeshAsset.h); empty draws the quad, and a mesh that cannot
     * be loaded draws the quad too, once warned.
     *
     * Material names a material graph's output -- "Glow" is the shader
     * Materials/Glow.fsh and its parameters Materials/Glow.material (see
     * RenderContext::DrawQuadWithMaterial). On a mesh it replaces every
     * section's own material. Empty draws the plain Tint (lit, on a mesh, or
     * a section's imported material if that one loads); a material that
     * cannot be loaded does the same, once warned. */
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

    /*
     * RenderComponent's own factory, exported for a caller that wants to
     * register it directly -- Renderer::Initialize (a later build-order
     * step) does exactly this. RENDERCORE_DEFINE_PROXY_COMPONENT's own
     * generated factory function is a plain `static` in RenderComponents.cpp
     * and never leaves that translation unit; this is the one door out.
     */
    RENDERCORE_API CreateProxyFn GetRenderComponentFactory();
}

#endif //RENDERCORE_RENDERCOMPONENTS_H
