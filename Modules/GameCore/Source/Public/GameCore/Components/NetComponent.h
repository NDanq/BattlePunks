#ifndef GAMECORE_NETCOMPONENT_H
#define GAMECORE_NETCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    /*
     * "Net": marks an entity as networked. A server gives it a NetId and
     * replicates it to every client -- its serialized components when it
     * appears (or, for an entity the level already has, matched by GUID), then
     * its components' replicated fields (ComponentReplication.h) as they
     * change -- and RPCs (ComponentRpc.h) address it by that id.
     *
     * Here, not in NetCore, so a level or prefab that carries one still loads
     * in a build with NetCore compiled out; it is inert there.
     *
     * NetId and OwnerClient are the server's to set; a level saves neither.
     */
    struct GAMECORE_API NetComponent
    {
        GAMECORE_COMPONENT_BODY()

        int32_t NetId       = 0;                 // 0 until the server assigns one
        int32_t OwnerClient = NET_CLIENT_NONE;   // the client that owns it (Client RPCs go there); NONE: the server
        uint8_t bReplicate  = 1;                 // off: the entity stays local to each process

        bool HasNetId() const { return NetId != 0; }
    };
}

#endif //GAMECORE_NETCOMPONENT_H
