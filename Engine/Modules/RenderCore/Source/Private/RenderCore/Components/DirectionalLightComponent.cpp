#include "Public/RenderCore/Components/DirectionalLightComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* DirectionalLightComponent: registration, and what it exposes to saves, Lua
 * and the editor. */

namespace RenderCore
{
    /* Read by WorldBridge's per-frame lighting build, like the camera. */
    GAMECORE_DEFINE_COMPONENT(DirectionalLightComponent, "Directional Light")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(DirectionalLightComponent, 1)
    {
        Ar.Field("Direction", C.Direction);
        Ar.Field("Color",     C.Color);
        Ar.Field("Intensity", C.Intensity);
        Ar.Flag("Active",     C.bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(DirectionalLightComponent)
    {
        Bind.Field("Direction", &DirectionalLightComponent::Direction);
        Bind.Field("Color",     &DirectionalLightComponent::Color);
        Bind.Field("Intensity", &DirectionalLightComponent::Intensity);
        Bind.Flag("Active",     &DirectionalLightComponent::bActive);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(DirectionalLightComponent)
    {
        Props.Field("Direction", &DirectionalLightComponent::Direction);
        Props.Field("Color",     &DirectionalLightComponent::Color);
        Props.Field("Intensity", &DirectionalLightComponent::Intensity);
        Props.Flag("Active",     &DirectionalLightComponent::bActive);
    }
#endif // WITH_EDITOR
}
