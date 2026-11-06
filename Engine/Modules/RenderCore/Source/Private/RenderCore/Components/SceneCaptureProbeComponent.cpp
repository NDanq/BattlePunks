#include "Public/RenderCore/Components/SceneCaptureProbeComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* SceneCaptureProbeComponent: registration, and what it exposes to saves,
 * Lua and the editor. */

namespace RenderCore
{
    GAMECORE_DEFINE_COMPONENT(SceneCaptureProbeComponent, "Scene Capture Probe")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SceneCaptureProbeComponent, 2)
    {
        Ar.Field("Target",     C.Target);
        Ar.Field("Mode",       C.Mode);
        Ar.Field("Resolution", C.Resolution);
        Ar.Field("Frequency",  C.Frequency);
        Ar.Field("Interval",   C.Interval);
        Ar.Field("NearClip",   C.NearClip);
        Ar.Field("FarClip",    C.FarClip);
        Ar.Field("ExtentX",    C.ExtentX);
        Ar.Field("ExtentZ",    C.ExtentZ);
        Ar.Flag("HideOwner",   C.bHideOwner);
        Ar.Flag("Enabled",     C.bEnabled);
        if(Ar.GetVersion() >= 2)
        {
            Ar.Flag("CaptureShadows", C.bCaptureShadows);
        }
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(SceneCaptureProbeComponent)
    {
        Bind.Field("Target",     &SceneCaptureProbeComponent::Target);
        Bind.Field("Mode",       &SceneCaptureProbeComponent::Mode);
        Bind.Field("Resolution", &SceneCaptureProbeComponent::Resolution);
        Bind.Field("Frequency",  &SceneCaptureProbeComponent::Frequency);
        Bind.Field("Interval",   &SceneCaptureProbeComponent::Interval);
        Bind.Field("NearClip",   &SceneCaptureProbeComponent::NearClip);
        Bind.Field("FarClip",    &SceneCaptureProbeComponent::FarClip);
        Bind.Field("ExtentX",    &SceneCaptureProbeComponent::ExtentX);
        Bind.Field("ExtentZ",    &SceneCaptureProbeComponent::ExtentZ);
        Bind.Flag("HideOwner",   &SceneCaptureProbeComponent::bHideOwner);
        Bind.Flag("Enabled",     &SceneCaptureProbeComponent::bEnabled);
        Bind.Flag("CaptureNow",  &SceneCaptureProbeComponent::bCaptureNow);
        Bind.Flag("CaptureShadows", &SceneCaptureProbeComponent::bCaptureShadows);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SceneCaptureProbeComponent)
    {
        Props.TypeCategory("Rendering");
        Props.Category("Capture");
        Props.Field("Target",      &SceneCaptureProbeComponent::Target, GameCore::PF_AssetRenderTarget);
        Props.Choice("Mode",       &SceneCaptureProbeComponent::Mode, "Planar Mirror|Omnidirectional");
        Props.Field("Resolution",  &SceneCaptureProbeComponent::Resolution);
        Props.Choice("Frequency",  &SceneCaptureProbeComponent::Frequency, "Every Frame|Interval|Once|Manual");
        Props.Field("Interval",    &SceneCaptureProbeComponent::Interval);
        Props.Flag("Capture Now",  &SceneCaptureProbeComponent::bCaptureNow);
        Props.Flag("Capture Shadows", &SceneCaptureProbeComponent::bCaptureShadows);
        Props.Category("Extent");
        Props.Field("Near Clip",   &SceneCaptureProbeComponent::NearClip);
        Props.Field("Far Clip",    &SceneCaptureProbeComponent::FarClip);
        Props.Field("Extent X",    &SceneCaptureProbeComponent::ExtentX);
        Props.Field("Extent Z",    &SceneCaptureProbeComponent::ExtentZ);
        Props.Flag("Hide Owner",   &SceneCaptureProbeComponent::bHideOwner);
        Props.Flag("Enabled",      &SceneCaptureProbeComponent::bEnabled);
    }
#endif // WITH_EDITOR
}
