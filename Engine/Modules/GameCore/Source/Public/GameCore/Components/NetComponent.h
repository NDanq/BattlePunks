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
     * NetId, OwnerClient and InputAck are the server's to set; a level saves
     * none of them.
     *
     * MOVEMENT. On a client, a networked entity's Transform is shown about
     * [Net] InterpolationDelayMs in the past, blended between the server's
     * updates, so it moves smoothly whatever the server's send rate -- except
     * one this client owns and has marked bPredicted: the game moves that one
     * itself, at once, and reconciles with the server (NetFacade.h,
     * SetNetCorrectionHandler; Engine/Modules/NetCore/GUIDE.md).
     */
    struct GAMECORE_API NetComponent
    {
        GAMECORE_COMPONENT_BODY()

        int32_t NetId       = 0;                 // 0 until the server assigns one
        int32_t OwnerClient = NET_CLIENT_NONE;   // the client that owns it (Client RPCs go there); NONE: the server
        uint8_t bReplicate  = 1;                 // off: the entity stays local to each process
        uint8_t bPredicted  = 0;                 // its owner moves it ahead of the server: never interpolated there

        /* The server's: the last input (by the game's own sequence numbers) it
         * applied to this entity. Replicated, so the owner knows which of its
         * predicted moves the server's state already includes. */
        int32_t InputAck    = 0;

        bool HasNetId() const { return NetId != 0; }
    };
}

#endif //GAMECORE_NETCOMPONENT_H
