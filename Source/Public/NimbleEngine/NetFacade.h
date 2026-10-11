#ifndef NIMBLEENGINE_NETFACADE_H
#define NIMBLEENGINE_NETFACADE_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/ComponentRpc.h"   // GameCore::NetClientID

namespace GameCore { struct NetComponent; class World; }

/*
 * What a game asks about networking, without NetCore. Always compiled: with
 * NetCore compiled out (NIMBLE_WITH_NETCORE=OFF) or simply not in a session,
 * every answer is Standalone's, so game code needs no #if.
 *
 * SERVER-AUTHORITATIVE GAMES check HasAuthority() before anything that
 * creates, destroys or changes networked state -- spawning a pickup, scoring,
 * moving an AI. On a client the server does those, and they arrive by
 * replication. Standalone has authority over everything.
 *
 *     if(NimbleEngine::HasAuthority()) { SpawnWave(); }
 *     if(NimbleEngine::IsLocallyOwned(*Pawn.GetComponent<GameCore::NetComponent>()))
 *     {
 *         GAMECORE_RPC(ShipComponent, ServerFire)(Pawn, Aim);   // input -> the server
 *     }
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    enum class ENetMode : uint8_t
    {
        Standalone,   // not networked
        Server,       // the authority, with clients (a dedicated server, or a listen server)
        Client,       // connected to a server
    };

    /* False when NetCore is compiled out. */
    NE_API bool IsNetworkingAvailable();

    NE_API ENetMode GetNetMode();
    inline bool IsServer()     { return GetNetMode() == ENetMode::Server; }
    inline bool IsClient()     { return GetNetMode() == ENetMode::Client; }
    inline bool HasAuthority() { return GetNetMode() != ENetMode::Client; }

    /* A server with no local player: the headless <Game>Server. A listen
     * server (-listen, or the editor's Play as Listen Server) is not one, so
     * a mode gives its own user a player when this is false and it has
     * authority. */
    NE_API bool IsDedicatedServer();

    /* A client's id, once its server has welcomed it; NET_CLIENT_NONE otherwise. */
    NE_API GameCore::NetClientID GetLocalClientId();

    /* Is this process the one that controls InNet's entity? A client owns
     * what has its id; a server or standalone process owns what no client
     * does. */
    NE_API bool IsLocallyOwned(const GameCore::NetComponent& InNet);

    /* A client: connected and welcomed. A server: listening. */
    NE_API bool IsNetConnected();

    /* The server's clock in ms: a server's own, a client's estimate of it;
     * 0 when not networked. RpcContext::ViewTimeMs is on it (LagCompensation.h). */
    NE_API uint32_t GetNetTimeMs();

    /* A client that lost its server and is trying to rejoin ([Net] ReconnectGraceMs). */
    NE_API bool IsNetReconnecting();

    /* One connection, as this end sees it. */
    struct NetStats
    {
        uint32_t RoundTripMs         = 0;      // smoothed
        uint32_t RoundTripVarianceMs = 0;
        float    PacketLossPercent   = 0.0f;   // estimated from reliable resends
        uint64_t BytesSent     = 0, BytesReceived   = 0;   // payload, since connected
        uint64_t PacketsSent   = 0, PacketsReceived = 0;
        float    SendBytesPerSecond    = 0.0f;   // over the last whole second
        float    ReceiveBytesPerSecond = 0.0f;
    };

    /* A client: its connection to the server (InClient ignored). A server:
     * its connection to InClient -- an RpcContext's SenderClient, say, to
     * allow for a shooter's latency. False when there is no such
     * connection, or no networking. */
    NE_API bool GetNetStats(GameCore::NetClientID InClient, NetStats& OutStats);

    /*
     * CLIENT-SIDE PREDICTION. A client shows networked entities a moment in
     * the past, blended between the server's updates ([Net]
     * InterpolationDelayMs) -- smooth, but a player's own pawn would answer
     * its keys a round trip late. So a game predicts that one:
     *
     *   1. Mark it predicted (NetComponent::bPredicted, "Predicted" in the
     *      editor). Its owner then sees the server's state for it as-is,
     *      never interpolated.
     *   2. On the owner, each input gets a sequence number; the game applies
     *      it at once AND sends it in a Server RPC, keeping it until the
     *      server has caught up.
     *   3. On the server, the RPC applies the same input with the same code,
     *      then sets NetComponent::InputAck to its number (replicated).
     *   4. When the server's state arrives, InHandler runs once per predicted
     *      entity, after that state is written: drop the inputs up to InAck,
     *      replay the rest on top. Agreeing code gives the same place, so
     *      nothing visibly moves; a disagreement is corrected.
     *
     * One handler, nullptr clears it; a mode sets it in OnLoad and clears it
     * in OnUnload. An avatar whose harness names a Simulation needs none of
     * this -- the engine does all four steps (Avatar.h, AVATARS.md) and the
     * handler hears only of other entities.
     */
    using NetCorrectionFn = void (*)(GameCore::World& InWorld, GameCore::EntityID InEntity, int32_t InInputAck,
                                     void* InUserData);
    NE_API void SetNetCorrectionHandler(NetCorrectionFn InHandler, void* InUserData);
}

#endif //NIMBLEENGINE_NETFACADE_H
