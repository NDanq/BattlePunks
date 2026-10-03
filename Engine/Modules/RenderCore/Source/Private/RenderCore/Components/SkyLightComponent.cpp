#include "Public/RenderCore/Components/SkyLightComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* SkyLightComponent: registration, and what it exposes to saves, Lua and the
 * editor. */

namespace RenderCore
{
    /* Read by WorldBridge's per-frame lighting build, like the camera. */
    GAMECORE_DEFINE_COMPONENT(SkyLightComponent, "Sky Light")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SkyLightComponent, 1)
    {
        Ar.Field("SkyColor",    C.SkyColor);
        Ar.Field("GroundColor", C.GroundColor);
        Ar.Field("Intensity",   C.Intensity);
        Ar.Flag("Active",       C.bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(SkyLightComponent)
    {
        Bind.Field("SkyColor",    &SkyLightComponent::SkyColor);
        Bind.Field("GroundColor", &SkyLightComponent::GroundColor);
        Bind.Field("Intensity",   &SkyLightComponent::Intensity);
        Bind.Flag("Active",       &SkyLightComponent::bActive);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SkyLightComponent)
    {
        Props.Field("SkyColor",    &SkyLightComponent::SkyColor);
        Props.Field("GroundColor", &SkyLightComponent::GroundColor);
        Props.Field("Intensity",   &SkyLightComponent::Intensity);
        Props.Flag("Active",       &SkyLightComponent::bActive);
    }
#endif // WITH_EDITOR
}
