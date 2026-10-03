#include "Public/GameCore/Components/CollisionComponent.h"

#include <cstring>

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/World.h"

/* CollisionComponent: registration, its methods, and what it exposes to
 * saves, Lua and the editor. The overlap pass itself is Collision.cpp. */

namespace GameCore
{
    GAMECORE_DEFINE_COMPONENT(CollisionComponent, "Collision")

    bool CollisionComponent::IsOverlapping(const EntityID InOther) const
    {
        return Owner.OwningWorld && Owner.OwningWorld->AreOverlapping(Owner.Entity, InOther);
    }

    const char* CollisionComponent::GetSourceName() const
    {
        return Source == CS_Mesh ? "Mesh" : "Bounds";
    }

    void CollisionComponent::SetSourceName(const char* InName)
    {
        if(!InName) { return; }
        if(std::strcmp(InName, "Mesh") == 0)   { Source = CS_Mesh; }
        if(std::strcmp(InName, "Bounds") == 0) { Source = CS_Bounds; }
    }

    /* The authored fields; overlaps are this run's, and begin again after a load. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(CollisionComponent, 1)
    {
        Ar.Flag("Enabled", C.bEnabled);
        Ar.Field("Source", C.Source);
        Ar.Field("Layer",  C.Layer);
        Ar.Field("Mask",   C.Mask);
    }

    /* GetEnabled/SetEnabled, GetLayer/SetLayer, GetMask/SetMask,
     * GetOverlapCount (read-only), GetSource/SetSource ("Bounds", "Mesh"),
     * IsOverlapping(other). Scripts react through OnOverlapBegin(self, other)
     * and OnOverlapEnd(self, other) on their class table. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(CollisionComponent)
    {
        Bind.Flag("Enabled", &CollisionComponent::bEnabled);
        Bind.Field("Layer", &CollisionComponent::Layer);
        Bind.Field("Mask",  &CollisionComponent::Mask);
        Bind.ReadOnly("OverlapCount", &CollisionComponent::OverlapCount);

        Bind.NativePure<&CollisionComponent::GetSourceName>("GetSource", "-> Source");
        Bind.Native<&CollisionComponent::SetSourceName>("SetSource", "Source");
        Bind.NativePure<&CollisionComponent::IsOverlapping>("IsOverlapping", "Other -> Overlapping");
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(CollisionComponent)
    {
        Props.Flag("Enabled", &CollisionComponent::bEnabled);
        Props.Category("Shape");
        Props.Choice("Source", &CollisionComponent::Source, "Bounds component|Render component mesh");   // ECollisionSource
        Props.Category("Filtering");
        Props.Field("Layer",  &CollisionComponent::Layer);
        Props.Field("Mask",   &CollisionComponent::Mask);
    }
#endif // WITH_EDITOR
}
