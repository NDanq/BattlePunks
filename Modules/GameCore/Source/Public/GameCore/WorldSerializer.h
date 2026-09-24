#ifndef GAMECORE_WORLDSERIALIZER_H
#define GAMECORE_WORLDSERIALIZER_H

#include <cstdint>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    class ScriptEngine;
    class World;

    /*
     * Every live entity and every component with a registered serializer, both
     * directions. Components are keyed by type NAME (ids are assignment-order),
     * each instance in its own sized chunk so unknown types are skipped rather
     * than failing the load. Entity references go through IEntityRemapper, so
     * any EntityID held outside the World is stale after a load.
     *
     * Not persisted: systems, delegate/event bindings, the spatial index
     * (rebuilt here), render proxies, and Lua instance state. See DESIGN.md
     * for the format and the reasons.
     *
     * Game thread only, and refused while the World is ticking -- deferred
     * destroys would be half-applied. A failed load leaves the World partially
     * populated; load into a World you can discard.
     */
    GAMECORE_API bool SerializeWorld(EngineCore::Archive& Ar, World& InOutWorld);

    /*
     * SerializeWorld, then -- on load only -- re-instantiates each restored
     * ScriptComponent from its ClassName, after every component is in place so
     * a script's OnStart sees its siblings. An unregistered class is skipped
     * with a warning; the entity keeps its other components.
     */
    GAMECORE_API bool SerializeWorldWithScripts(EngineCore::Archive& Ar, World& InOutWorld,
                                                ScriptEngine& InScripts);
}

#endif //GAMECORE_WORLDSERIALIZER_H
