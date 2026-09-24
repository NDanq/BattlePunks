#include "Public/GameCore/ComponentSerializer.h"

#include "Public/GameCore/Components.h"

/*
 * The built-in components' serializers, in one file so "what persists" reads
 * as a list. What each one leaves out is per-run state -- see DESIGN.md. None
 * touches C.Owner.
 */

namespace GameCore
{
    /* Every field is real data. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(TransformComponent, 1)
    {
        SerializeVector3(Ar, C.Position);
        SerializeVector3(Ar, C.Rotation);
        SerializeVector3(Ar, C.Scale);
    }

    /* Local and bStatic only. World and ProxyID are a cache of the spatial
     * index; the loader's RefreshSpatialIndex rebuilds both. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(BoundsComponent, 1)
    {
        SerializeAABB(Ar, C.Local);
        Ar << C.bStatic;
    }
}
