#include "Public/RenderCore/Components/SpotLightComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* SpotLightComponent: registration, and what it exposes to saves, Lua and the
 * editor. */

namespace RenderCore
{
    /* Read by WorldBridge's per-frame lighting build. */
    GAMECORE_DEFINE_COMPONENT(SpotLightComponent, "Spot Light")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SpotLightComponent, 1)
    {
        Ar.Field("Color",            C.Color);
        Ar.Field("Intensity",        C.Intensity);
        Ar.Field("Range",            C.Range);
        Ar.Field("InnerConeAngle",   C.InnerConeAngle);
        Ar.Field("OuterConeAngle",   C.OuterConeAngle);
        Ar.Flag("Active",            C.bActive);
        Ar.Flag("CastShadows",       C.bCastShadows);
        Ar.Field("ShadowTechnique",  C.ShadowTechnique);
        Ar.Field("ShadowTint",       C.ShadowTint);
        Ar.Field("ShadowBias",       C.ShadowBias);
        Ar.Field("ShadowNormalBias", C.ShadowNormalBias);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(SpotLightComponent)
    {
        Bind.Field("Color",            &SpotLightComponent::Color);
        Bind.Field("Intensity",        &SpotLightComponent::Intensity);
        Bind.Field("Range",            &SpotLightComponent::Range);
        Bind.Field("InnerConeAngle",   &SpotLightComponent::InnerConeAngle);
        Bind.Field("OuterConeAngle",   &SpotLightComponent::OuterConeAngle);
        Bind.Flag("Active",            &SpotLightComponent::bActive);
        Bind.Flag("CastShadows",       &SpotLightComponent::bCastShadows);
        Bind.Field("ShadowTechnique",  &SpotLightComponent::ShadowTechnique);
        Bind.Field("ShadowTint",       &SpotLightComponent::ShadowTint);
        Bind.Field("ShadowBias",       &SpotLightComponent::ShadowBias);
        Bind.Field("ShadowNormalBias", &SpotLightComponent::ShadowNormalBias);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SpotLightComponent)
    {
        Props.Category("Light");
        Props.Field("Color",            &SpotLightComponent::Color);
        Props.Field("Intensity",        &SpotLightComponent::Intensity);
        Props.Field("Range",            &SpotLightComponent::Range);
        Props.Field("Inner Cone Angle", &SpotLightComponent::InnerConeAngle);
        Props.Field("Outer Cone Angle", &SpotLightComponent::OuterConeAngle);
        Props.Flag("Active",            &SpotLightComponent::bActive);

        Props.Category("Shadows");
        Props.Flag("Cast Shadows",    &SpotLightComponent::bCastShadows);
        Props.Choice("Technique",     &SpotLightComponent::ShadowTechnique, "Standard|Modulated");
        Props.Field("Modulated Tint", &SpotLightComponent::ShadowTint);
        Props.Field("Depth Bias",     &SpotLightComponent::ShadowBias);
        Props.Field("Normal Bias",    &SpotLightComponent::ShadowNormalBias);
    }
#endif
}
