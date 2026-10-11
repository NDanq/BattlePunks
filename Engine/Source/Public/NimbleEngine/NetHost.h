#ifndef NIMBLEENGINE_NETHOST_H
#define NIMBLEENGINE_NETHOST_H

#include <cstdint>
#include <string>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/NimbleEngine/NetFacade.h"

namespace NimbleEngine
{
    class GameSession;

    /* What a NetHost starts as. */
    struct NetHostDesc
    {
        ENetMode    Mode          = ENetMode::Standalone;
        std::string ServerAddress = "127.0.0.1";   // a client's server
        uint16_t    Port          = 7777;          // a server's, or the one a client connects to
        uint32_t    MaxClients    = 16;
        uint32_t    TimeoutMs     = 30000;
        /* The host makes each joining client a player (IGameMode::OnClientConnected). */
        bool        bSpawnPlayerPerClient = true;
        /* No local player: a headless server (NetFacade's IsDedicatedServer). */
        bool        bDedicated = false;
        /* A client shows other entities this far in the past ([Net] InterpolationDelayMs). */
        uint32_t    InterpolationDelayMs = 100;
        /* Testing: this share of state packets is dropped ([Net] SimulatedLossPercent). */
        uint32_t    SimulatedLossPercent = 0;
        /* A server's limits on each client's RPCs ([Net] RpcPerSecond, RpcFloodPerSecond, MaxRpcBytes; 0: none). */
        /* A lost client's place is kept this long, and a client tries to rejoin as long ([Net] ReconnectGraceMs; 0: off). */
        uint32_t    ReconnectGraceMs  = 30000;
        /* A server's budget for each client's state, KB a second ([Net] ClientBandwidthKBps; 0: none). */
        uint32_t    ClientBandwidthKBps = 256;
        uint32_t    RpcPerSecond      = 1000;
        uint32_t    RpcFloodPerSecond = 1000;
        uint32_t    MaxRpcBytes       = 1024;

        /*
         * The game's [Net] config. InMode, unless it is Standalone: then
         * [Net] Listen makes a listen server and a [Net] ServerAddress a
         * client (what -listen and -connect set).
         */
        NE_API static NetHostDesc FromConfig(ENetMode InMode = ENetMode::Standalone);
    };

    /* [Net] ServerTickRate: a headless server's frame rate. */
    NE_API uint32_t GetConfiguredServerTickRate();

    /*
     * A GameSession's network session: what the game launcher (Application)
     * and the editor's Play-in-Editor both drive, so a game networks the same
     * way in either. NetCore is reached only from the .cpp; without it
     * (NIMBLE_WITH_NETCORE=OFF) Start warns and every call does nothing.
     *
     * Each frame, around the session's:
     *
     *     Net.BeginFrame();               // before GameSession::ApplyPendingLevel
     *     Session.ApplyPendingLevel();
     *     Net.Poll();                     // the new World bound; what arrived
     *     Session.Tick(Frame);
     *     Net.Flush();                    // what changed goes out
     *
     * A server makes a player entity for each client that joins -- a
     * NetComponent the client owns and a PlayerStateComponent -- then calls
     * IGameMode::OnClientConnected; when it leaves, OnClientDisconnected,
     * then everything the client owned is destroyed.
     *
     * Game thread only.
     */
    class NE_API NetHost
    {
    public:
        NetHost();
        ~NetHost();   // Stop()s

        NetHost(const NetHost&)            = delete;
        NetHost& operator=(const NetHost&) = delete;

        /* Before InSession loads its level, so the level and mode know their
         * role (NetFacade.h); the World is bound by the first Poll after the
         * load. Standalone does nothing and succeeds. False, logged, if
         * the socket could not be opened: the session runs standalone. */
        bool Start(GameSession& InSession, const NetHostDesc& InDesc);

        /* Clients told, the World let go. Idempotent. */
        void Stop();

        void BeginFrame();
        void Poll();
        void Flush();

        bool     IsActive() const;
        ENetMode GetMode() const;
        /* A server's listening port (what a Port of 0 became). */
        uint16_t GetPort() const;

        struct Impl;

    private:
        Impl* P;
    };
}

#endif //NIMBLEENGINE_NETHOST_H
