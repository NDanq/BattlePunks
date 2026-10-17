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

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SkyLightComponent, 2)
    {
        Ar.Field("SkyColor",    C.SkyColor);
        Ar.Field("GroundColor", C.GroundColor);
        Ar.Field("Intensity",   C.Intensity);
        Ar.Flag("Active",       C.bActive);
        if(Ar.GetVersion() >= 2)
        {
            Ar.Field("Environment",          C.Environment);
            Ar.Field("EnvironmentIntensity", C.EnvironmentIntensity);
            Ar.Field("EnvironmentRotation",  C.EnvironmentRotation);
            Ar.Flag("DrawBackground",        C.bDrawBackground);
        }
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(SkyLightComponent)
    {
        Bind.Field("SkyColor",    &SkyLightComponent::SkyColor);
        Bind.Field("GroundColor", &SkyLightComponent::GroundColor);
        Bind.Field("Intensity",   &SkyLightComponent::Intensity);
        Bind.Flag("Active",       &SkyLightComponent::bActive);
        Bind.Field("Environment",          &SkyLightComponent::Environment);
        Bind.Field("EnvironmentIntensity", &SkyLightComponent::EnvironmentIntensity);
        Bind.Field("EnvironmentRotation",  &SkyLightComponent::EnvironmentRotation);
        Bind.Flag("DrawBackground",        &SkyLightComponent::bDrawBackground);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SkyLightComponent)
    {
        Props.TypeCategory("Rendering/Lights");
        Props.Field("SkyColor",    &SkyLightComponent::SkyColor);
        Props.Field("GroundColor", &SkyLightComponent::GroundColor);
        Props.Field("Intensity",   &SkyLightComponent::Intensity);
        Props.Flag("Active",       &SkyLightComponent::bActive);
        Props.Category("Environment");
        Props.Field("Environment", &SkyLightComponent::Environment, GameCore::PF_AssetTexture);
        Props.Field("Environment Intensity", &SkyLightComponent::EnvironmentIntensity);
        Props.Field("Environment Rotation",  &SkyLightComponent::EnvironmentRotation);
        Props.Flag("Draw Background",        &SkyLightComponent::bDrawBackground);
    }
#endif // WITH_EDITOR
}
