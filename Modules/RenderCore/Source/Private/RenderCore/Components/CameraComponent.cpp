#include "Public/RenderCore/Components/CameraComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* CameraComponent: registration, and what it exposes to saves, Lua and the
 * editor. */

namespace RenderCore
{
    /* No RENDERCORE_PROXY_BODY() -- a CameraComponent drives WorldBridge's
     * view build directly (a later build-order step reads it, it does not
     * mirror it onto a proxy), so GAMECORE_DEFINE_COMPONENT alone is all
     * this one needs. */
    GAMECORE_DEFINE_COMPONENT(CameraComponent, "Camera")

    /* Every field is real data -- a CameraComponent holds no per-run state at
     * all (its position comes from the sibling Transform, which has its own
     * serializer). */
    /* Version 2: Projection and FovY (a version 1 save stays orthographic). */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(CameraComponent, 2)
    {
        Ar.Field("ZNear", C.ZNear);
        Ar.Field("ZFar",  C.ZFar);
        Ar.Flag("Active", C.bActive);
        if(Ar.GetVersion() >= 2)
        {
            Ar.Field("Projection", C.Projection);
            Ar.Field("FovY",       C.FovY);
        }
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(CameraComponent)
    {
        Bind.Field("ZNear", &CameraComponent::ZNear);
        Bind.Field("ZFar",  &CameraComponent::ZFar);
        Bind.Flag("Active", &CameraComponent::bActive);
        Bind.Field("Projection", &CameraComponent::Projection);
        Bind.Field("FovY",       &CameraComponent::FovY);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(CameraComponent)
    {
        Props.TypeCategory("Rendering");
        Props.Field("ZNear", &CameraComponent::ZNear);
        Props.Field("ZFar",  &CameraComponent::ZFar);
        Props.Flag("Active", &CameraComponent::bActive);
        Props.Choice("Projection", &CameraComponent::Projection, "Orthographic|Perspective");
        Props.Field("FovY",        &CameraComponent::FovY);
        Props.Range(1.0f, 179.0f);
        Props.Tooltip("Vertical field of view, in degrees (Perspective)");
    }
#endif // WITH_EDITOR
}
