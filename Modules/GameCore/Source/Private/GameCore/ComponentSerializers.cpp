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
        Ar.Field("Position", C.Position);
        Ar.Field("Rotation", C.Rotation);
        Ar.Field("Scale",    C.Scale);
    }

    /* Local and bStatic only. World and ProxyID are a cache of the spatial
     * index; the loader's RefreshSpatialIndex rebuilds both. */
    /* The authored fields; overlaps are this run's, and begin again after a load. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(CollisionComponent, 1)
    {
        Ar.Flag("Enabled", C.bEnabled);
        Ar.Field("Source", C.Source);
        Ar.Field("Layer",  C.Layer);
        Ar.Field("Mask",   C.Mask);
    }

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(BoundsComponent, 1)
    {
        Ar.Field("Local", C.Local);
        Ar.Flag("Static", C.bStatic);
    }
}
