#ifndef NETCORE_NETTYPES_H
#define NETCORE_NETTYPES_H

#include <cstdint>

#include "Public/GameCore/ComponentRpc.h"   // GameCore::NetClientID
#include "Public/NetCore/NetCoreAPI.h"

namespace NetCore
{
    /* What this process is in a session. */
    enum class ENetRole : uint8_t
    {
        Standalone,   // no networking: RPCs run locally, nothing replicates
        Server,       // authority: owns networked entities, sends them to clients
        Client,       // connected to a server; networked entities are the server's
    };

    NETCORE_API const char* GetNetRoleName(ENetRole InRole);

    enum : uint16_t { NET_DEFAULT_PORT = 7777 };

    struct NetConfig
    {
        /* A client's server: a host name or an IPv4 address. */
        char     ServerAddress[128] = "127.0.0.1";
        /* A server listens here (0: any free port, see NetSession::GetPort);
         * a client connects here. */
        uint16_t Port       = NET_DEFAULT_PORT;
        uint32_t MaxClients = 16;
        /* The longest a peer may go unanswered -- a connection attempt, or a
         * peer that stopped acknowledging -- before it is given up. Generous,
         * so a long frame (a client compiling its pipelines) is survived. */
        uint32_t TimeoutMs  = 30000;

        /* A client shows other entities this far in the past, blending between
         * the server's updates (NetComponent.h). About three of the server's
         * sends: 100ms at a 30Hz server. 0 snaps to each update. */
        uint32_t InterpolationDelayMs = 100;
    };
}

#endif //NETCORE_NETTYPES_H
