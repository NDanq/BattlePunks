#include "Public/RenderCore/RenderComponents.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

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

    /*
     * THE SERIALIZERS, here beside the definitions rather than in GameCore --
     * the module that owns a component type owns its serializer, the same rule
     * that puts these two types' GAMECORE_DEFINE_COMPONENT calls here rather
     * than in GameCore::Components.cpp.
     *
     * That placement is also what makes a build WITHOUT RenderCore able to load
     * a save that contains these components: nothing registers them, so
     * WorldSerializer skips both chunks by size and warns. See
     * WorldSerializer.h's format note on why every payload is sized.
     *
     * C.Proxy IS NOT SAVED, and cannot be. A RenderProxyHandle names a live
     * render-thread object that RenderCore's WorldBridge creates by extracting
     * this component every frame -- the same "it is a rebuildable cache, not
     * data" reason BoundsComponent::ProxyID is skipped. A restored
     * PrimitiveComponent gets a fresh proxy on the first frame after the load,
     * from the values below, with nothing here having to arrange it.
     */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(PrimitiveComponent, 1)
    {
        Ar.Field("Width",  C.Width);
        Ar.Field("Height", C.Height);
        Ar.Field("Tint",   C.Tint);
        Ar.Flag("Visible", C.bVisible);
    }

    /* Every field is real data -- a CameraComponent holds no per-run state at
     * all (its position comes from the sibling Transform, which has its own
     * serializer). */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(CameraComponent, 1)
    {
        Ar.Field("ZNear", C.ZNear);
        Ar.Field("ZFar",  C.ZFar);
        Ar.Flag("Active", C.bActive);
    }

    /*
     * WHAT LUA MAY TOUCH. Written through GameCore's typed binding API -- this
     * module never includes lua.h (ComponentScript.h says why). A change made
     * from Lua reaches the screen the same way a C++ one does: WorldBridge
     * re-extracts the component into its proxy every frame. C.Proxy is not
     * listed, so scripts cannot see the render handle.
     */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(PrimitiveComponent)
    {
        Bind.Field("Width",  &PrimitiveComponent::Width);
        Bind.Field("Height", &PrimitiveComponent::Height);
        Bind.Field("Tint",   &PrimitiveComponent::Tint);     // r, g, b[, a]
        Bind.Flag("Visible", &PrimitiveComponent::bVisible);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(CameraComponent)
    {
        Bind.Field("ZNear", &CameraComponent::ZNear);
        Bind.Field("ZFar",  &CameraComponent::ZFar);
        Bind.Flag("Active", &CameraComponent::bActive);
    }

#if WITH_EDITOR
    /* WHAT THE EDITOR SHOWS -- the same fields as the Lua list above, kept as
     * a separate list on purpose (ComponentProperties.h). C.Proxy is not
     * listed. Editor targets only. */
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(PrimitiveComponent)
    {
        Props.Field("Width",  &PrimitiveComponent::Width);
        Props.Field("Height", &PrimitiveComponent::Height);
        Props.Field("Tint",   &PrimitiveComponent::Tint);
        Props.Flag("Visible", &PrimitiveComponent::bVisible);
    }

    GAMECORE_DEFINE_COMPONENT_PROPERTIES(CameraComponent)
    {
        Props.Field("ZNear", &CameraComponent::ZNear);
        Props.Field("ZFar",  &CameraComponent::ZFar);
        Props.Flag("Active", &CameraComponent::bActive);
    }
#endif // WITH_EDITOR
}
