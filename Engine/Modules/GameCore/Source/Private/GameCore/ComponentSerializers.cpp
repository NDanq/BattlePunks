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

    /* Length-prefixed, and re-clamped on load to this build's buffer. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(NameComponent, 1)
    {
        Ar.SerializeFixedString(C.Name, sizeof(C.Name));
    }

    /* Local and bStatic only. World and ProxyID are a cache of the spatial
     * index; the loader's RefreshSpatialIndex rebuilds both. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(BoundsComponent, 1)
    {
        SerializeAABB(Ar, C.Local);
        Ar << C.bStatic;
    }

    /* Not InstanceRef (a luaL_ref into one lua_State) or bStarted/bErrored.
     * SerializeWorldWithScripts re-instantiates from ClassName, so the Lua
     * instance's own table state does not survive a save. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(ScriptComponent, 1)
    {
        Ar.SerializeFixedString(C.ClassName, sizeof(C.ClassName));
        Ar << C.Phase;
        Ar << C.bCullOutsideActiveRegion;
    }
}
