#include "Public/GameCore/Components/BoundsComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* BoundsComponent: registration, and what it exposes to saves, Lua and the
 * editor. The refit pass that maintains it is World's (SpatialIndex.h). */

namespace GameCore
{
    GAMECORE_DEFINE_COMPONENT(BoundsComponent, "Bounds")

    /* Local and bStatic only. World and ProxyID are a cache of the spatial
     * index; the loader's RefreshSpatialIndex rebuilds both. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(BoundsComponent, 1)
    {
        Ar.Field("Local", C.Local);
        Ar.Flag("Static", C.bStatic);
    }

    /* GetLocal/SetLocal (six numbers), GetWorld (read-only -- the refit owns
     * it), GetStatic/SetStatic (boolean). ProxyID stays hidden. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(BoundsComponent)
    {
        Bind.Field("Local", &BoundsComponent::Local);
        Bind.ReadOnly("World", &BoundsComponent::World);
        Bind.Flag("Static", &BoundsComponent::bStatic);
    }

#if WITH_EDITOR
    /* World is the refit's output, so read-only. ProxyID stays hidden. */
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(BoundsComponent)
    {
        Props.Field("Local", &BoundsComponent::Local);
        Props.Field("World", &BoundsComponent::World, PF_ReadOnly);
        Props.Flag("Static", &BoundsComponent::bStatic);
    }
#endif // WITH_EDITOR
}
