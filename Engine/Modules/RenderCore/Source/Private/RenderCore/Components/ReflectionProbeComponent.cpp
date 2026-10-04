#include "Public/RenderCore/Components/ReflectionProbeComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* ReflectionProbeComponent: registration, and what it exposes to saves, Lua
 * and the editor. */

namespace RenderCore
{
    GAMECORE_DEFINE_COMPONENT(ReflectionProbeComponent, "Reflection Probe")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(ReflectionProbeComponent, 1)
    {
        Ar.Field("Radius",         C.Radius);
        Ar.Field("BoxExtent",      C.BoxExtent);
        Ar.Field("Intensity",      C.Intensity);
        Ar.Field("Resolution",     C.Resolution);
        Ar.Field("Frequency",      C.Frequency);
        Ar.Field("Interval",       C.Interval);
        Ar.Field("NearClip",       C.NearClip);
        Ar.Field("FarClip",        C.FarClip);
        Ar.Flag("CaptureShadows",  C.bCaptureShadows);
        Ar.Flag("Enabled",         C.bEnabled);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(ReflectionProbeComponent)
    {
        Bind.Field("Radius",        &ReflectionProbeComponent::Radius);
        Bind.Field("BoxExtent",     &ReflectionProbeComponent::BoxExtent);
        Bind.Field("Intensity",     &ReflectionProbeComponent::Intensity);
        Bind.Field("Resolution",    &ReflectionProbeComponent::Resolution);
        Bind.Field("Frequency",     &ReflectionProbeComponent::Frequency);
        Bind.Field("Interval",      &ReflectionProbeComponent::Interval);
        Bind.Field("NearClip",      &ReflectionProbeComponent::NearClip);
        Bind.Field("FarClip",       &ReflectionProbeComponent::FarClip);
        Bind.Flag("CaptureShadows", &ReflectionProbeComponent::bCaptureShadows);
        Bind.Flag("Enabled",        &ReflectionProbeComponent::bEnabled);
        Bind.Flag("CaptureNow",     &ReflectionProbeComponent::bCaptureNow);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(ReflectionProbeComponent)
    {
        Props.Category("Influence");
        Props.Field("Radius",          &ReflectionProbeComponent::Radius);
        Props.Field("Box Extent",      &ReflectionProbeComponent::BoxExtent);
        Props.Field("Intensity",       &ReflectionProbeComponent::Intensity);
        Props.Category("Capture");
        Props.Field("Resolution",      &ReflectionProbeComponent::Resolution);
        Props.Choice("Frequency",      &ReflectionProbeComponent::Frequency, "Every Frame|Interval|Once|Manual");
        Props.Field("Interval",        &ReflectionProbeComponent::Interval);
        Props.Field("Near Clip",       &ReflectionProbeComponent::NearClip);
        Props.Field("Far Clip",        &ReflectionProbeComponent::FarClip);
        Props.Flag("Capture Shadows",  &ReflectionProbeComponent::bCaptureShadows);
        Props.Flag("Capture Now",      &ReflectionProbeComponent::bCaptureNow);
        Props.Flag("Enabled",          &ReflectionProbeComponent::bEnabled);
    }
#endif // WITH_EDITOR
}
