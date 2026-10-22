#include "Public/RenderCore/Components/GIVolumeComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* GIVolumeComponent: registration, and what it exposes to saves, Lua and the
 * editor. */

namespace RenderCore
{
    /* Read by WorldBridge's capture list (BuildGIVolume). */
    GAMECORE_DEFINE_COMPONENT(GIVolumeComponent, "GI Volume")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(GIVolumeComponent, 1)
    {
        Ar.Field("Extent",       C.Extent);
        Ar.Field("ProbeSpacing", C.ProbeSpacing);
        Ar.Field("Intensity",    C.Intensity);
        Ar.Flag("Enabled",       C.bEnabled);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(GIVolumeComponent)
    {
        Bind.Field("Extent",       &GIVolumeComponent::Extent);
        Bind.Field("ProbeSpacing", &GIVolumeComponent::ProbeSpacing);
        Bind.Field("Intensity",    &GIVolumeComponent::Intensity);
        Bind.Flag("Enabled",       &GIVolumeComponent::bEnabled);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(GIVolumeComponent)
    {
        Props.TypeCategory("Rendering/Lights");
        Props.Category("GI Volume");
        Props.Field("Extent",        &GIVolumeComponent::Extent);
        Props.Field("Probe Spacing", &GIVolumeComponent::ProbeSpacing);
        Props.Field("Intensity",     &GIVolumeComponent::Intensity);
        Props.Flag("Enabled",        &GIVolumeComponent::bEnabled);
    }
#endif
}
