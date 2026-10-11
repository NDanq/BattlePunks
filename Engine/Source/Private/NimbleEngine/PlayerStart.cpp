#include "Public/NimbleEngine/PlayerStart.h"

#include <algorithm>
#include <string>
#include <vector>

#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/NetFacade.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"

/* PlayerStart.h: spawn points, spawning an avatar, AutoPossess, NetGameMode. */

/* The avatar a player gets when neither the mode nor the start names one. */
NIMBLE_CONFIG_STRING(GAvatarDefaultPrefab, "Avatar", "DefaultPrefab", "", nullptr);
/* Local players NetGameMode gives avatars on a listen server or standalone. */
NIMBLE_CONFIG_INT(GAvatarLocalPlayers, "Avatar", "LocalPlayers", 1, nullptr);

namespace NimbleEngine
{
GameCore::EntityID FindPlayerStart(GameCore::World& InWorld, const uint32_t InOrdinal)
{
    std::vector<GameCore::EntityID> Enabled;
    GameCore::EntityID Kept;
    InWorld.Each<PlayerStartComponent>([&](GameCore::Entity InEntity, PlayerStartComponent& InStart)
    {
        if(!InStart.bEnabled) { return; }
        if(InStart.PlayerSlot != 0 && uint32_t(InStart.PlayerSlot) == InOrdinal + 1 && !Kept.IsSet()) { Kept = InEntity.GetID(); }
        if(InStart.PlayerSlot == 0) { Enabled.push_back(InEntity.GetID()); }
    });
    if(Kept.IsSet()) { return Kept; }
    if(!Enabled.empty()) { return Enabled[InOrdinal % Enabled.size()]; }

    /* No start for any: a slotted one, rather than nothing. */
    GameCore::EntityID Any;
    InWorld.Each<PlayerStartComponent>([&](GameCore::Entity InEntity, PlayerStartComponent& InStart)
    {
        if(!Any.IsSet() && InStart.bEnabled) { Any = InEntity.GetID(); }
    });
    return Any;
}

GameCore::EntityID SpawnAvatarFor(GameCore::World& InWorld, const GameCore::EntityID InPlayer, const char* InPrefab,
                                  const GameCore::EntityID InStart)
{
    if(!HasAuthority() || !InWorld.IsAlive(InPlayer)) { return GameCore::EntityID(); }

    GameCore::TransformPose At;
    const PlayerStartComponent* Start = nullptr;
    if(InWorld.IsAlive(InStart))
    {
        const GameCore::Entity StartEntity = InWorld.GetEntity(InStart);
        Start = StartEntity.GetComponent<PlayerStartComponent>();
        const GameCore::TransformPose Pose = StartEntity.GetWorldPose();
        At.Position = Pose.Position;
        At.Rotation = Pose.Rotation;
    }

    std::string Prefab = InPrefab && InPrefab[0] ? InPrefab : "";
    if(Prefab.empty() && Start) { Prefab = Start->AvatarPrefab; }
    if(Prefab.empty())          { Prefab = GAvatarDefaultPrefab.GetString(); }
    if(Prefab.empty())
    {
        CORE_LOG_ERROR("[Avatar] no avatar prefab for '" + std::string(InWorld.GetEntity(InPlayer).GetName())
                       + "': name one on its Player Start, in the mode, or as [Avatar] DefaultPrefab")
        return GameCore::EntityID();
    }

    const std::string Name = std::string(InWorld.GetEntity(InPlayer).GetName()) + " Avatar";
    const GameCore::Entity Avatar = InWorld.SpawnPrefab(Prefab.c_str(), At, Name.c_str()).GetRoot();
    if(!Avatar.IsValid() || !Avatar.IsAlive())
    {
        CORE_LOG_ERROR("[Avatar] could not spawn '" + Prefab + "'")
        return GameCore::EntityID();
    }
    Avatar.SetName(Name.c_str());   // SpawnPrefab's name is the instance's

    /* Owned by the player's client before it is first sent, and kept by it
     * when let go (RestOwner): its avatar leaves with it. */
    const GameCore::NetComponent* PlayerNet = InWorld.GetEntity(InPlayer).GetComponent<GameCore::NetComponent>();
    if(GameCore::NetComponent* Net = Avatar.GetComponent<GameCore::NetComponent>())
    {
        Net->OwnerClient = PlayerNet ? PlayerNet->OwnerClient : GameCore::NET_CLIENT_NONE;
    }

    if(!Possess(InWorld, InPlayer, Avatar.GetID()))
    {
        CORE_LOG_ERROR("[Avatar] '" + Prefab + "' spawned, but could not be possessed (has it an Avatar Harness?)")
    }
    return Avatar.GetID();
}

void ApplyAutoPossess(GameCore::World& InWorld)
{
    if(!HasAuthority() || IsDedicatedServer()) { return; }

    struct Claim { GameCore::EntityID Avatar; uint8_t LocalPlayer; };
    std::vector<Claim> Claims;
    InWorld.Each<AvatarHarnessComponent>([&](GameCore::Entity InEntity, AvatarHarnessComponent& InHarness)
    {
        if(InHarness.AutoPossess >= 1 && InHarness.AutoPossess <= INPUT_PLAYERS_MAX && !InHarness.IsPossessed())
        {
            Claims.push_back({ InEntity.GetID(), uint8_t(InHarness.AutoPossess - 1) });
        }
    });
    for(const Claim& Each : Claims)
    {
        const GameCore::EntityID Player = CreateLocalPlayer(InWorld, Each.LocalPlayer);
        if(GetAvatar(InWorld, Player).IsSet()) { continue; }   // two placed for one player: the first
        Possess(InWorld, Player, Each.Avatar);
    }
}

// === NetGameMode ===========================================================

bool NetGameMode::OnLoad(GameSession& InSession)
{
    Session     = &InSession;
    NextOrdinal = 0;
    return true;
}

void NetGameMode::OnStart()
{
    if(!Session || !HasAuthority() || IsDedicatedServer()) { return; }
    GameCore::World& World = Session->GetWorld();
    const int Count = std::max(1, std::min(int(INPUT_PLAYERS_MAX), GAvatarLocalPlayers.GetInt()));
    for(int Local = 0; Local < Count; ++Local)
    {
        const GameCore::EntityID Player = CreateLocalPlayer(World, uint8_t(Local));
        if(GetAvatar(World, Player).IsSet()) { ++NextOrdinal; continue; }   // placed for it (AutoPossess)
        SpawnFor(Player);
    }
}

void NetGameMode::OnClientConnected(const int32_t InClient, const GameCore::EntityID InPlayer)
{
    (void)InClient;
    if(Session && InPlayer.IsSet()) { SpawnFor(InPlayer); }
}

GameCore::EntityID NetGameMode::SpawnFor(const GameCore::EntityID InPlayer)
{
    GameCore::World& World = Session->GetWorld();
    const uint32_t Ordinal = NextOrdinal++;
    const GameCore::EntityID Avatar = SpawnAvatarFor(World, InPlayer, GetAvatarPrefab(InPlayer), FindPlayerStart(World, Ordinal));
    if(Avatar.IsSet()) { OnAvatarSpawned(InPlayer, Avatar, Ordinal); }
    return Avatar;
}
}

// === Registration =============================================================

using NimbleEngine::PlayerStartComponent;

GAMECORE_DEFINE_COMPONENT(PlayerStartComponent, "PlayerStart")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(PlayerStartComponent, 1)
{
    Ar.Field("AvatarPrefab", C.AvatarPrefab);
    Ar.Field("PlayerSlot",   C.PlayerSlot);
    Ar.Flag("Enabled",       C.bEnabled);
}

GAMECORE_DEFINE_COMPONENT_SCRIPT(PlayerStartComponent)
{
    Bind.Field("AvatarPrefab", &PlayerStartComponent::AvatarPrefab);
    Bind.Field("PlayerSlot",   &PlayerStartComponent::PlayerSlot);
    Bind.Flag("Enabled",       &PlayerStartComponent::bEnabled);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(PlayerStartComponent)
{
    Props.Field("AvatarPrefab", &PlayerStartComponent::AvatarPrefab, GameCore::PF_AssetPrefab);
    Props.Choice("PlayerSlot",  &PlayerStartComponent::PlayerSlot, "Any|Player 1|Player 2|Player 3|Player 4");
    Props.Flag("Enabled",       &PlayerStartComponent::bEnabled);
}
#endif
