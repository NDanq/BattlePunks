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

    /* Version 2: SourceRadius (baked lighting). */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SpotLightComponent, 2)
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
        if(Ar.GetVersion() >= 2)
        {
            Ar.Field("SourceRadius", C.SourceRadius);
        }
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
        Bind.Field("SourceRadius",     &SpotLightComponent::SourceRadius);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SpotLightComponent)
    {
        Props.TypeCategory("Rendering/Lights");
        Props.Category("Light");
        Props.Field("Color",            &SpotLightComponent::Color);
        Props.Field("Intensity",        &SpotLightComponent::Intensity);
        Props.Range(0.0f, 1.0e9f);
        Props.Tooltip("Brightness at the light; falls off by inverse square");
        Props.Field("Range",            &SpotLightComponent::Range);
        Props.Range(0.0f, 1.0e9f);
        Props.Tooltip("World units: no light reaches past this");
        Props.Field("Inner Cone Angle", &SpotLightComponent::InnerConeAngle);
        Props.Range(0.0f, 90.0f);
        Props.Tooltip("Half-angle in degrees: full strength inside it");
        Props.Field("Outer Cone Angle", &SpotLightComponent::OuterConeAngle);
        Props.Range(0.0f, 90.0f);
        Props.Tooltip("Half-angle in degrees: no light outside it");
        Props.Flag("Active",            &SpotLightComponent::bActive);

        Props.Category("Shadows");
        Props.Flag("Cast Shadows",    &SpotLightComponent::bCastShadows);
        Props.Choice("Technique",     &SpotLightComponent::ShadowTechnique, "Standard|Modulated");
        Props.Field("Modulated Tint", &SpotLightComponent::ShadowTint);
        Props.Field("Depth Bias",     &SpotLightComponent::ShadowBias);
        Props.Range(0.0f, 1.0e9f);
        Props.Field("Normal Bias",    &SpotLightComponent::ShadowNormalBias);
        Props.Range(0.0f, 1.0e9f);

        Props.Category("Baked Lighting");
        Props.Field("Source Radius", &SpotLightComponent::SourceRadius);
        Props.Range(0.0f, 1.0e9f);
        Props.Tooltip("World units: the sphere the light comes from -- softer shadows when larger");
    }
#endif
}
