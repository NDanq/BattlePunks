#include "Public/RenderCore/Components/PointLightComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* PointLightComponent: registration, and what it exposes to saves, Lua and
 * the editor. */

namespace RenderCore
{
    /* Read by WorldBridge's per-frame lighting build. */
    GAMECORE_DEFINE_COMPONENT(PointLightComponent, "Point Light")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(PointLightComponent, 1)
    {
        Ar.Field("Color",            C.Color);
        Ar.Field("Intensity",        C.Intensity);
        Ar.Field("Radius",           C.Radius);
        Ar.Flag("Active",            C.bActive);
        Ar.Flag("CastShadows",       C.bCastShadows);
        Ar.Field("ShadowTechnique",  C.ShadowTechnique);
        Ar.Field("ShadowTint",       C.ShadowTint);
        Ar.Field("ShadowBias",       C.ShadowBias);
        Ar.Field("ShadowNormalBias", C.ShadowNormalBias);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(PointLightComponent)
    {
        Bind.Field("Color",            &PointLightComponent::Color);
        Bind.Field("Intensity",        &PointLightComponent::Intensity);
        Bind.Field("Radius",           &PointLightComponent::Radius);
        Bind.Flag("Active",            &PointLightComponent::bActive);
        Bind.Flag("CastShadows",       &PointLightComponent::bCastShadows);
        Bind.Field("ShadowTechnique",  &PointLightComponent::ShadowTechnique);
        Bind.Field("ShadowTint",       &PointLightComponent::ShadowTint);
        Bind.Field("ShadowBias",       &PointLightComponent::ShadowBias);
        Bind.Field("ShadowNormalBias", &PointLightComponent::ShadowNormalBias);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(PointLightComponent)
    {
        Props.Category("Light");
        Props.Field("Color",     &PointLightComponent::Color);
        Props.Field("Intensity", &PointLightComponent::Intensity);
        Props.Field("Radius",    &PointLightComponent::Radius);
        Props.Flag("Active",     &PointLightComponent::bActive);

        Props.Category("Shadows");
        Props.Flag("Cast Shadows",    &PointLightComponent::bCastShadows);
        Props.Choice("Technique",     &PointLightComponent::ShadowTechnique, "Standard|Modulated");
        Props.Field("Modulated Tint", &PointLightComponent::ShadowTint);
        Props.Field("Depth Bias",     &PointLightComponent::ShadowBias);
        Props.Field("Normal Bias",    &PointLightComponent::ShadowNormalBias);
    }
#endif
}
