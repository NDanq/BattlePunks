#ifndef NIMBLEENGINE_NETFACADE_H
#define NIMBLEENGINE_NETFACADE_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/ComponentRpc.h"   // GameCore::NetClientID

namespace GameCore { struct NetComponent; }

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

    /* A client's id, once its server has welcomed it; NET_CLIENT_NONE otherwise. */
    NE_API GameCore::NetClientID GetLocalClientId();

    /* Is this process the one that controls InNet's entity? A client owns
     * what has its id; a server or standalone process owns what no client
     * does. */
    NE_API bool IsLocallyOwned(const GameCore::NetComponent& InNet);

    /* A client: connected and welcomed. A server: listening. */
    NE_API bool IsNetConnected();
}

#endif //NIMBLEENGINE_NETFACADE_H
