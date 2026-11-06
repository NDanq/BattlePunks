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

    /* Version 2: Direction became the Transform's forward, and the shadow
     * fields arrived. A version 1 save's Direction is read and dropped.
     * Version 3: SourceAngle (baked lighting). */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(DirectionalLightComponent, 3)
    {
        if(Ar.GetVersion() < 2)
        {
            EngineCore::Vector3 Direction;
            Ar.Field("Direction", Direction);
        }
        Ar.Field("Color",     C.Color);
        Ar.Field("Intensity", C.Intensity);
        Ar.Flag("Active",     C.bActive);
        if(Ar.GetVersion() >= 2)
        {
            Ar.Flag("CastShadows",         C.bCastShadows);
            Ar.Field("ShadowTechnique",    C.ShadowTechnique);
            Ar.Field("ShadowTint",         C.ShadowTint);
            Ar.Field("ShadowBias",         C.ShadowBias);
            Ar.Field("ShadowNormalBias",   C.ShadowNormalBias);
            Ar.Field("ShadowDistance",     C.ShadowDistance);
            Ar.Field("CascadeCount",       C.CascadeCount);
            Ar.Field("CascadeSplitLambda", C.CascadeSplitLambda);
        }
        if(Ar.GetVersion() >= 3)
        {
            Ar.Field("SourceAngle", C.SourceAngle);
        }
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(DirectionalLightComponent)
    {
        Bind.Field("Color",              &DirectionalLightComponent::Color);
        Bind.Field("Intensity",          &DirectionalLightComponent::Intensity);
        Bind.Flag("Active",              &DirectionalLightComponent::bActive);
        Bind.Flag("CastShadows",         &DirectionalLightComponent::bCastShadows);
        Bind.Field("ShadowTechnique",    &DirectionalLightComponent::ShadowTechnique);
        Bind.Field("ShadowTint",         &DirectionalLightComponent::ShadowTint);
        Bind.Field("ShadowBias",         &DirectionalLightComponent::ShadowBias);
        Bind.Field("ShadowNormalBias",   &DirectionalLightComponent::ShadowNormalBias);
        Bind.Field("ShadowDistance",     &DirectionalLightComponent::ShadowDistance);
        Bind.Field("CascadeCount",       &DirectionalLightComponent::CascadeCount);
        Bind.Field("CascadeSplitLambda", &DirectionalLightComponent::CascadeSplitLambda);
        Bind.Field("SourceAngle",        &DirectionalLightComponent::SourceAngle);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(DirectionalLightComponent)
    {
        Props.TypeCategory("Rendering/Lights");
        Props.Category("Light");
        Props.Field("Color",     &DirectionalLightComponent::Color);
        Props.Field("Intensity", &DirectionalLightComponent::Intensity);
        Props.Range(0.0f, 1.0e9f);
        Props.Flag("Active",     &DirectionalLightComponent::bActive);

        Props.Category("Shadows");
        Props.Flag("Cast Shadows",          &DirectionalLightComponent::bCastShadows);
        Props.Choice("Technique",           &DirectionalLightComponent::ShadowTechnique, "Standard|Modulated|Cascaded");
        Props.Field("Modulated Tint",       &DirectionalLightComponent::ShadowTint);
        Props.Field("Depth Bias",           &DirectionalLightComponent::ShadowBias);
        Props.Field("Normal Bias",          &DirectionalLightComponent::ShadowNormalBias);
        Props.Field("Shadow Distance",      &DirectionalLightComponent::ShadowDistance);
        Props.Range(0.0f, 1.0e9f);
        Props.Tooltip("World units from the camera that cast shadows");
        Props.Field("Cascade Count",        &DirectionalLightComponent::CascadeCount);
        Props.Range(1.0f, 4.0f);
        Props.Tooltip("Cascaded: how many slices the shadow distance is cut into");
        Props.Field("Cascade Split Lambda", &DirectionalLightComponent::CascadeSplitLambda);
        Props.Range(0.0f, 1.0f);
        Props.Tooltip("Cascaded: 0 slices evenly, 1 logarithmically (finer near the camera)");

        Props.Category("Baked Lighting");
        Props.Field("Source Angle", &DirectionalLightComponent::SourceAngle);
        Props.Range(0.0f, 90.0f);
        Props.Tooltip("Degrees across the sun's disc -- softer baked shadows when larger");
    }
#endif
}
