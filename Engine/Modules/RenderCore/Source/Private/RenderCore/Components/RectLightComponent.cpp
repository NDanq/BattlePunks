#include "Public/RenderCore/Components/RectLightComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* RectLightComponent: registration, and what it exposes to saves, Lua and the
 * editor. */

namespace RenderCore
{
    /* Read by WorldBridge's per-frame lighting build. */
    GAMECORE_DEFINE_COMPONENT(RectLightComponent, "Rect Light")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(RectLightComponent, 1)
    {
        Ar.Field("Color",     C.Color);
        Ar.Field("Intensity", C.Intensity);
        Ar.Field("Width",     C.Width);
        Ar.Field("Height",    C.Height);
        Ar.Field("Range",     C.Range);
        Ar.Flag("Active",     C.bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(RectLightComponent)
    {
        Bind.Field("Color",     &RectLightComponent::Color);
        Bind.Field("Intensity", &RectLightComponent::Intensity);
        Bind.Field("Width",     &RectLightComponent::Width);
        Bind.Field("Height",    &RectLightComponent::Height);
        Bind.Field("Range",     &RectLightComponent::Range);
        Bind.Flag("Active",     &RectLightComponent::bActive);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(RectLightComponent)
    {
        Props.TypeCategory("Rendering/Lights");
        Props.Category("Light");
        Props.Field("Color",     &RectLightComponent::Color);
        Props.Field("Intensity", &RectLightComponent::Intensity);
        Props.Range(0.0f, 1.0e9f);
        Props.Field("Width",     &RectLightComponent::Width);
        Props.Range(0.0f, 1.0e9f);
        Props.Field("Height",    &RectLightComponent::Height);
        Props.Range(0.0f, 1.0e9f);
        Props.Field("Range",     &RectLightComponent::Range);
        Props.Range(0.0f, 1.0e9f);
        Props.Tooltip("World units: no light reaches past this");
        Props.Flag("Active",     &RectLightComponent::bActive);
    }
#endif
}
