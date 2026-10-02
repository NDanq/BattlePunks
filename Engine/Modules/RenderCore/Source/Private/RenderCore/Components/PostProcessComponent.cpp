#include "Public/RenderCore/Components/PostProcessComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

#include "Private/RenderCore/Components/MaterialPathUpgrade.h"

/* PostProcessComponent: registration, and what it exposes to saves, Lua and
 * the editor. */

namespace RenderCore
{
    GAMECORE_DEFINE_COMPONENT(PostProcessComponent, "Post Process")

    /* Version 2: the material is a graph path; an older bare name is upgraded. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(PostProcessComponent, 2)
    {
        Ar.Field("Material", C.Material);
        if(Ar.IsLoading() && Ar.GetVersion() < 2) { UpgradeMaterialName(C.Material, sizeof(C.Material)); }
        Ar.Field("Priority", C.Priority);
        Ar.Flag("Enabled",   C.bEnabled);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(PostProcessComponent)
    {
        Bind.Field("Material", &PostProcessComponent::Material);
        Bind.Flag("Enabled",   &PostProcessComponent::bEnabled);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(PostProcessComponent)
    {
        Props.Field("Material", &PostProcessComponent::Material, GameCore::PF_AssetMaterial);
        Props.Field("Priority", &PostProcessComponent::Priority);
        Props.Flag("Enabled",   &PostProcessComponent::bEnabled);
    }
#endif // WITH_EDITOR
}
