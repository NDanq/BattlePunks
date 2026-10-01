#ifndef GAMECORE_WORLDSERIALIZER_H
#define GAMECORE_WORLDSERIALIZER_H

#include <cstdint>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    class World;

    /*
     * Every live entity (name, script path) and every serializable component,
     * both directions. Components are keyed by type NAME, each in a sized
     * chunk so unknown types are skipped. EntityIDs held outside the World are
     * stale after a load.
     *
     * Not persisted: systems, bindings, the spatial index (rebuilt), render
     * proxies and Lua instance state (scripts restart). See DESIGN.md.
     *
     * Game thread only; refused while ticking. A failed load leaves the World
     * partly filled; use a discardable World.
     */
    GAMECORE_API bool SerializeWorld(EngineCore::Archive& Ar, World& InOutWorld);
}

#endif //GAMECORE_WORLDSERIALIZER_H
