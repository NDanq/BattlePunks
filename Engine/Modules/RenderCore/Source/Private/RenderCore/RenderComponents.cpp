#include "Public/RenderCore/RenderComponents.h"

#include "Private/RenderCore/PrimitiveProxy.h"

/*
 * The two built-in components' registration. Nothing else to define --
 * unlike GameCore::Components.cpp's TransformComponent::GetLocalMatrix,
 * neither PrimitiveComponent nor CameraComponent has an out-of-line method
 * of its own; RENDERCORE_API is only for StaticType() itself, the same
 * reason GameCore::DESIGN.md gives for annotating even its plain-looking
 * components.
 */

namespace RenderCore
{
    RENDERCORE_DEFINE_PROXY_COMPONENT(PrimitiveComponent, "Primitive", PrimitiveProxy)

    /* No RENDERCORE_PROXY_BODY() -- a CameraComponent drives WorldBridge's
     * view build directly (a later build-order step reads it, it does not
     * mirror it onto a proxy), so GAMECORE_DEFINE_COMPONENT alone is all
     * this one needs. */
    GAMECORE_DEFINE_COMPONENT(CameraComponent, "Camera")

    CreateProxyFn GetPrimitiveComponentFactory()
    {
        return &PrimitiveComponent_CreateProxy;
    }
}
