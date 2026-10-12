#ifndef NETCORE_NETSESSION_H
#define NETCORE_NETSESSION_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/EntityID.h"
#include "Public/NetCore/NetCoreAPI.h"
#include "Public/NetCore/NetTypes.h"

namespace GameCore { class World; }

/*
 * ONE PROCESS'S PART IN A GAME SESSION: a server and its clients, over ENet.
 *
 * SERVER-AUTHORITATIVE. The server gives every entity with a NetComponent
 * (bReplicate on) a NetId and sends it to each client: its serialized
 * components when it first appears -- an entity the client's level already
 * has is matched by EntityGuid, any other is created -- then each replicated
 * field (GameCore/ComponentReplication.h) whenever it changes, and its
 * destruction. Clients never send state; they send Server RPCs.
 *
 * RPCS. Started, the session installs itself as GameCore's RPC sink
 * (ComponentRpc.h) and routes each call by its target and this role:
 *
 *                 on the server                  on a client
 *     Server      runs                           sent to the server
 *     Client      sent to the owning client      runs
 *                 (runs if the server owns it)
 *     Multicast   runs, and sent to every client runs
 *
 * A Server RPC is accepted only from the client that owns the entity
 * (NetComponent::OwnerClient).
 *
 * WIRE. A reliable, ordered ENet channel carries spawns, destroys and RPCs in
 * the order they were made. Replicated state goes unreliably on its own,
 * each batch against what that client has acknowledged, so a lost packet
 * delays nothing behind it (NetProtocol.h, README.md "Unreliable state").
 * Every packet is an EngineCore::Archive (little-endian); one that does not
 * parse disconnects its sender. A client whose code differs -- protocol
 * version, or the replication and RPC tables (NetSchema) -- is refused.
 *
 * DRIVING IT, once a frame on the game thread (the host does this):
 *
 *     Session.PollIncoming();    // before the World ticks: connections, state, RPCs
 *     ...tick...
 *     Session.FlushOutgoing();   // after: the server diffs and sends
 *
 * and BindWorld/UnbindWorld around each level.
 */
namespace NetCore
{
    class NETCORE_API NetSession
    {
    public:
        NetSession();
        ~NetSession();

        NetSession(const NetSession&)            = delete;
        NetSession& operator=(const NetSession&) = delete;

        /* Server: listens on InConfig.Port. Client: starts connecting to
         * InConfig.ServerAddress:Port (IsConnected() once the server welcomes
         * it). Standalone: nothing to start. Installs the RPC sink. False,
         * logged, if the socket could not be opened or the address resolved. */
        bool Start(ENetRole InRole, const NetConfig& InConfig);

        /* Disconnects everyone, closes the socket, restores GameCore's local
         * RPC sink. Idempotent; the destructor calls it. */
        void Stop();

        ENetRole GetRole() const;
        bool     IsServer() const { return GetRole() == ENetRole::Server; }
        bool     IsClient() const { return GetRole() == ENetRole::Client; }

        /* A server is "connected" while it listens; a client once welcomed. */
        bool IsConnected() const;

        /* A client's id on its server, once welcomed; NET_CLIENT_NONE otherwise. */
        GameCore::NetClientID GetLocalClientId() const;

        /* The server's clock, in ms: a server's own (what its batches are
         * stamped with, and an RpcContext's ViewTimeMs is on), a client's
         * estimate of it. 0 for a client before its first batch. */
        uint32_t GetTimeMs() const;

        /* A client trying to rejoin a server it lost (NetConfig::ReconnectGraceMs). */
        bool IsReconnecting() const;

        /* The token the server gave this client: what rejoins as it, after a
         * restart too (NetConfig::ReconnectToken). 0 before a welcome. */
        uint64_t GetReconnectToken() const;

        /* The port a server is listening on -- what a Port of 0 became. */
        uint16_t GetPort() const;

        /* A server's welcomed clients. */
        size_t GetClientCount() const;

        /* The connection's round trip, loss and traffic: a client's to its
         * server, once connected; a server's to InClient, once welcomed.
         * False otherwise. */
        bool GetServerStats(NetPeerStats& OutStats) const;
        bool GetClientStats(GameCore::NetClientID InClient, NetPeerStats& OutStats) const;

        /* The World networked entities live in, and the level it was loaded
         * from (sent in the handshake, compared, and only warned about). A
         * server spawns everything it has to its clients; a client binds what
         * it receives. Must outlive the binding. */
        void BindWorld(GameCore::World& InWorld, const char* InLevelName);
        void UnbindWorld();
        GameCore::World* GetBoundWorld() const;

        /* Connections, handshakes, spawns, state and RPCs that arrived. Before
         * the World ticks. */
        void PollIncoming();

        /* The server: new and destroyed networked entities, and every
         * replicated field that changed since the last call. Then everything
         * queued goes out. After the World ticks. */
        void FlushOutgoing();

        /* InEntity's NetId; 0 if it has none. */
        int32_t           GetNetId(GameCore::EntityID InEntity) const;
        /* The bound World's entity with InNetId; unset if none. */
        GameCore::EntityID FindEntityByNetId(int32_t InNetId) const;

        /* A server's clients coming and going (after the welcome, and after
         * the disconnect). The host makes the client's player here. */
        using ClientEventFn = void (*)(GameCore::NetClientID InClient, void* InUserData);
        void SetClientEvents(ClientEventFn InConnected, ClientEventFn InDisconnected, void* InUserData);

        /* A server's client whose connection was lost (InAway: its place is
         * kept, everything it owns stays) and came back in time (InReturned:
         * same id, its entities sent again). One that does not come back
         * within NetConfig::ReconnectGraceMs gets InDisconnected then. Same
         * user data as SetClientEvents. */
        void SetClientAwayEvents(ClientEventFn InAway, ClientEventFn InReturned);

        /*
         * A client's prediction hook. For each entity this client owns and
         * has marked predicted (NetComponent::bPredicted), whose server state
         * arrived in this PollIncoming: called once, after that state was
         * written -- the Transform is the server's, as of the input
         * NetComponent::InputAck says it had applied. The game replays its
         * moves after that ack here. Every other networked Transform is
         * interpolated instead (NetConfig::InterpolationDelayMs).
         */
        using CorrectionFn = void (*)(GameCore::World& InWorld, GameCore::EntityID InEntity, int32_t InInputAck,
                                      void* InUserData);
        void SetCorrectionHandler(CorrectionFn InFn, void* InUserData);

        /* The session that is started now, if any: what NetFacade reports. */
        static NetSession* GetActive();

        struct Impl;

    private:
        Impl* P;
    };

    /* The replication and RPC tables' hash, as the handshake compares it, for
     * the components and RPCs registered now (a loaded game module's too).
     * Two builds whose hashes differ can't play together: what Collaboration's
     * content worker publishes per commit (Modules/Editor/COLLABORATION.md). */
    NETCORE_API uint64_t ComputeSchemaHash();
}

#endif //NETCORE_NETSESSION_H
