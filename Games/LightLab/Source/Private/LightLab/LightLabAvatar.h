#ifndef LIGHTLAB_LIGHTLABAVATAR_H
#define LIGHTLAB_LIGHTLABAVATAR_H

#include <cstddef>

#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

/*
 * LightLab in multiplayer: every connected player is a glowing orb where their
 * camera is, so each sees the others fly the yard.
 *
 *     LightLabServer -port=7777
 *     LightLab -connect=127.0.0.1:7777      (as many as you like)
 *
 * The server owns the orbs. When a client joins, the engine makes its player
 * entity (NetComponent + PlayerState, IGameMode::OnClientConnected) and
 * LightLab adds the orb to it. Each client flies its own camera locally and
 * tells the server where it is ten times a second -- the Server RPC
 * ServerSetPose -- and the server's Transform replicates the orb to everyone.
 * A client hides its own orb: it would sit on its camera.
 *
 * Standalone (or in the editor) none of this runs.
 */

/* "LightLabAvatar": the orb. No data of its own: it is the entity's
 * Transform that moves; this is what the RPC is addressed to. */
struct LightLabAvatarComponent
{
    GAMECORE_COMPONENT_BODY()

    uint8_t bReserved = 0;   // a component needs a field; nothing reads it

    /* Server: the owning client's camera moved. */
    void ServerSetPose(const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InRotation);
};

namespace LightLabAvatar
{
    /* Server: InPlayer (a client's, from OnClientConnected) gets its orb. */
    void Add(GameCore::World& InWorld, GameCore::EntityID InPlayer);

    /* Client: send InView's pose to this client's orb, and keep that orb
     * hidden here. Call every frame; it sends ten times a second. */
    void Update(GameCore::World& InWorld, GameCore::EntityID InView, double InDeltaSeconds);

    /* Orbs that are not this process's own: every player, on a server. */
    size_t CountRemote(GameCore::World& InWorld);
}

#endif //LIGHTLAB_LIGHTLABAVATAR_H
