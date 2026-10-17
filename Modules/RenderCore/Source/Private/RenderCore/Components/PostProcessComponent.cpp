#include "Public/RenderCore/Components/PostProcessComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* PostProcessComponent: registration, and what it exposes to saves, Lua and
 * the editor. */

namespace RenderCore
{
    GAMECORE_DEFINE_COMPONENT(PostProcessComponent, "Post Process")

    /* Version 2: the stack (a version 1 save's Material, a material graph
     * path, reads into it -- a stack of that one material) and BlendDistance.
     * Version 3: Weight. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(PostProcessComponent, 3)
    {
        Ar.Field(Ar.GetVersion() >= 2 ? "Stack" : "Material", C.Stack);
        Ar.Field("Priority", C.Priority);
        Ar.Flag("Enabled",   C.bEnabled);
        if(Ar.GetVersion() >= 2)
        {
            Ar.Field("BlendDistance", C.BlendDistance);
        }
        if(Ar.GetVersion() >= 3)
        {
            Ar.Field("Weight", C.Weight);
        }
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(PostProcessComponent)
    {
        Bind.Field("Stack",         &PostProcessComponent::Stack);
        Bind.Field("Priority",      &PostProcessComponent::Priority);
        Bind.Field("BlendDistance", &PostProcessComponent::BlendDistance);
        Bind.Field("Weight",        &PostProcessComponent::Weight);
        Bind.Flag("Enabled",        &PostProcessComponent::bEnabled);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(PostProcessComponent)
    {
        Props.TypeCategory("Rendering");
        Props.Field("Stack",          &PostProcessComponent::Stack, GameCore::PF_AssetPostProcess);
        Props.Field("Priority",       &PostProcessComponent::Priority);
        Props.Field("Blend Distance", &PostProcessComponent::BlendDistance);
        Props.Field("Weight",         &PostProcessComponent::Weight);
        Props.Flag("Enabled",         &PostProcessComponent::bEnabled);
    }
#endif // WITH_EDITOR
}
