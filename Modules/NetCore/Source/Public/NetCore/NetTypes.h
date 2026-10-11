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

        /* Testing: this percentage of the unreliable state packets this end
         * sends (a server's batches, a client's acks) is dropped, as a lossy
         * network would. 0 in a real game. */
        uint32_t SimulatedLossPercent = 0;

        /* RPC LIMITS, a server's on each client (README.md, "RPC limits").
         * RpcPerSecond Server RPCs a second in all, in bursts of up to two
         * seconds' worth; past that they are dropped, and a client that has
         * more than RpcFloodPerSecond dropped in one second is disconnected
         * (0: never). An RPC's own RateLimit applies on top. One whose
         * arguments are longer than MaxRpcBytes is malformed. Generous by
         * default: an avatar sends its input once a frame. */
        /* RECONNECTING (README.md, "Reconnecting"). A server keeps the place
         * of a client whose connection was lost -- not one that quit -- this
         * long: its player, what it owns, its id. A client that lost its
         * server tries to rejoin for as long, with the token the server gave
         * it. 0 turns it off: a lost client is gone at once. */
        uint32_t ReconnectGraceMs = 30000;
        /* A client's token from an earlier connection (GetReconnectToken),
         * to rejoin as that client after a restart; 0 joins as a new one. */
        uint64_t ReconnectToken   = 0;

        /* THE BANDWIDTH BUDGET, a server's for each client's state, in
         * kilobytes a second (0: none). What does not fit in a tick waits for
         * the next, the client's own entities first, then by the Net
         * component's Priority, the time waited and nearness to the client's
         * own avatar (README.md, "Bandwidth budget"). Spawns, RPCs and the
         * editor link are not counted. */
        uint32_t ClientBandwidthKBps = 256;

        uint32_t RpcPerSecond      = 1000;
        uint32_t RpcFloodPerSecond = 1000;
        uint32_t MaxRpcBytes       = 1024;
    };

    /* One connection, as seen from this end: what NetSession::GetServerStats
     * (a client) and GetClientStats (a server) report. */
    struct NetPeerStats
    {
        uint32_t RoundTripMs         = 0;      // ENet's smoothed round trip
        uint32_t RoundTripVarianceMs = 0;
        float    PacketLossPercent   = 0.0f;   // ENet's estimate, from reliable resends
        uint64_t BytesSent     = 0, BytesReceived   = 0;   // payload, since connected
        uint64_t PacketsSent   = 0, PacketsReceived = 0;
        float    SendBytesPerSecond    = 0.0f;   // over the last whole second
        float    ReceiveBytesPerSecond = 0.0f;
        uint32_t StateDeferred         = 0;      // a server's: entities its budget held back last tick
    };
}

#endif //NETCORE_NETTYPES_H
