#include "WorldsLabWalker.h"

#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/LagCompensation.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/NimbleEngine/Ragdoll.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UISystem.h"
#include "Public/UI/UIWidget.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * WorldsLabPvP: Steampunk PvP, a free-for-all deathmatch on the Steampunk
 * street (Content/Levels/SteampunkPvP.nmworld, Tools/SteampunkPvP/) -- NetCore's
 * sample of a server-authoritative shooter. A dedicated server or a listen
 * server, any number of clients:
 *
 *     WorldsLabServer -Game.StartLevel=SteampunkPvP
 *     WorldsLab -Game.StartLevel=SteampunkPvP -connect=127.0.0.1
 *
 * WHO DOES WHAT
 *
 *   moving     the Shooting Range's walker (RangeMove, WorldsLabWalker.h), as an
 *              avatar simulation: predicted on the client that drives it,
 *              stepped on the server from its input, interpolated on the rest
 *   firing     the driving process sends ServerFire(origin, direction) from its
 *              camera, and shows its own muzzle flash and impact at once
 *   hits       the server's alone, by the engine's lag compensation
 *              (LagCompensation.h): the street as it is, each fighter's
 *              LagCompensated capsule where the shooter saw it
 *   damage     the server's: Health, Kills, Deaths and Ammo are replicated
 *              fields of each fighter's PvpFighter component
 *   feedback   Multicast RPCs -- MulticastShot (everyone else's flash, sound
 *              and impact), MulticastKilled (the kill feed, the ragdoll) -- and
 *              ClientHitConfirm to the shooter (the hit marker)
 *   the match  a networked PvpMatch entity the server makes: the clock, the
 *              phase, the winner; first to [WorldsLab] PvpFragLimit kills, or the
 *              most when [WorldsLab] PvpMatchSeconds run out, then a new round
 *
 * GEAR. The fighter prefab is the networked body alone. Its pistol (socketed
 * in the hand) and camera boom are made by each process for each fighter it
 * sees (DressFighter), never sent: a socket and a predicted camera are both
 * this process's to pose, and a replicated Transform would fight them.
 *
 * The HUD (Content/UI/SteampunkPvP/PvpHUD.nmui) is fed from the local
 * fighter's replicated fields, on every process with a player.
 *
 * BOTS. [WorldsLab] PvpBots=N gives the authority N more players, driven by
 * the avatar input filter as a person's devices would be -- so they move by
 * the same prediction path and fire by the same RPC. PvpBot=1 hands this
 * process's own player to it too: two clients with PvpBot=1 against a server
 * play a whole match unattended (the test).
 */

NIMBLE_CONFIG_INT(GPvpFragLimit,     "WorldsLab", "PvpFragLimit",     15,  nullptr);
NIMBLE_CONFIG_INT(GPvpMatchSeconds,  "WorldsLab", "PvpMatchSeconds",  600, nullptr);
NIMBLE_CONFIG_INT(GPvpBots,          "WorldsLab", "PvpBots",          0,   nullptr);   // extra players the authority drives
NIMBLE_CONFIG_BOOL(GPvpBot,          "WorldsLab", "PvpBot",           false, nullptr); // this process's own player too
NIMBLE_CONFIG_BOOL(GPvpLagCompensation, "WorldsLab", "PvpLagCompensation", true, nullptr); // off: shots judged against now (to compare)
NIMBLE_CONFIG_BOOL(GPvpNetStats,     "WorldsLab", "PvpNetStats",      false, nullptr); // a client logs its prediction every 5 s

namespace
{
    constexpr const char* FighterPrefab = "Content/Prefabs/SteampunkPvP/PvpFighter.nmprefab";
    constexpr const char* HudPath       = "Content/UI/SteampunkPvP/PvpHUD.nmui";
    constexpr const char* Sounds        = "Content/Sounds/ShootingRange/";
    constexpr const char* Particles     = "Content/Particles/ShootingRange/";

    constexpr int32_t MaxHealth     = 100;
    constexpr int32_t Magazine      = 12;
    constexpr int32_t BodyDamage    = 25;
    constexpr int32_t HeadDamage    = 60;
    constexpr float   FireDelay     = 0.2f;      // seconds between shots; semi-automatic
    constexpr float   ReloadSeconds = 1.6f;
    constexpr float   RespawnDelay  = 3.0f;
    constexpr float   RoundBreak    = 8.0f;      // the winner's banner, before the next round
    constexpr float   ShotRange     = 8000.0f;
    constexpr int32_t ShotMask      = 1;         // the street's static bodies; fighters are hit by capsule
    constexpr double  HeadFrom      = 150.0;     // a hit this far above the feet is a head shot (the prefab's capsule tops at 182)
    constexpr double  BoomHeight    = 150.0;     // RangeMove's camera pivot over the feet
    constexpr double  MaxOriginSlop = 350.0;     // how far from its pivot a shot may start
    constexpr double  KillKick      = 6000.0;    // the killing round's impulse into the ragdoll
}

// === The fighter =================================================================

/* "PvpFighter": one player's fighter. The server writes it; everyone reads it. */
struct PvpFighterComponent
{
    GAMECORE_COMPONENT_BODY()

    char    Name[24]   = "Fighter";
    int32_t Health     = MaxHealth;
    int32_t Ammo       = Magazine;
    int32_t Kills      = 0;
    int32_t Deaths     = 0;
    uint8_t bDead      = 0;
    uint8_t bReloading = 0;

    // The server's, never sent.
    int32_t Shots   = 0, Hits = 0;   // this round, for the accuracy log
    float Cooldown  = 0.0f;
    float ReloadIn  = 0.0f;
    float RespawnIn = 0.0f;

    // Each process's own, never sent.
    uint8_t bShownDead = 0;    // its ragdoll has been let go here
    float   LocalCooldown = 0.0f;

    /* The driving process's shot: from InOrigin along InDir (normalised here). */
    void ServerFire(const GameCore::RpcContext& InContext, const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir);
    void ServerReload(const GameCore::RpcContext& InContext);

    /* A shot, for everyone but the shooter's own process: InHit 0 nothing, 1 a wall, 2 a fighter. */
    void MulticastShot(const GameCore::RpcContext& InContext, const EngineCore::Vector3& InTo, uint8_t InHit);
    /* This fighter died: InKiller's round, InImpulse at InAt for its ragdoll. */
    void MulticastKilled(const GameCore::RpcContext& InContext, GameCore::EntityID InKiller, bool bInHead,
                         const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt);
    /* To the shooter: its round hit someone. */
    void ClientHitConfirm(const GameCore::RpcContext& InContext, int32_t InDamage, bool bInKill, bool bInHead);
};

GAMECORE_DEFINE_COMPONENT(PvpFighterComponent, "PvpFighter")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(PvpFighterComponent, 1)
{
    Ar.Field("Name", C.Name);
}

GAMECORE_DEFINE_COMPONENT_REPLICATION(PvpFighterComponent)
{
    Rep.Field("Name",      &PvpFighterComponent::Name);
    Rep.Field("Health",    &PvpFighterComponent::Health);
    Rep.Field("Ammo",      &PvpFighterComponent::Ammo);
    Rep.Field("Kills",     &PvpFighterComponent::Kills);
    Rep.Field("Deaths",    &PvpFighterComponent::Deaths);
    Rep.Flag("Dead",       &PvpFighterComponent::bDead);
    Rep.Flag("Reloading",  &PvpFighterComponent::bReloading);
}

GAMECORE_DEFINE_COMPONENT_RPCS(PvpFighterComponent)
{
    /* No faster than the gun (FireDelay), with room for a little jitter; the server drops the rest. */
    Rpc.Server<&PvpFighterComponent::ServerFire>("ServerFire").RateLimit(8);
    Rpc.Server<&PvpFighterComponent::ServerReload>("ServerReload").RateLimit(4);
    Rpc.Multicast<&PvpFighterComponent::MulticastShot>("MulticastShot");
    Rpc.Multicast<&PvpFighterComponent::MulticastKilled>("MulticastKilled");
    Rpc.Client<&PvpFighterComponent::ClientHitConfirm>("ClientHitConfirm");
}

// === The match ===================================================================

/* "PvpMatch": on one networked entity the server makes. */
struct PvpMatchComponent
{
    GAMECORE_COMPONENT_BODY()

    uint8_t Phase       = 0;      // 0 playing, 1 the round is over
    int32_t SecondsLeft = 0;      // of the round, or of the break after it
    char    Winner[24]  = "";

    float Clock = 0.0f;           // the server's, never sent
};

GAMECORE_DEFINE_COMPONENT(PvpMatchComponent, "PvpMatch")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(PvpMatchComponent, 1)
{
    Ar.Field("Phase", C.Phase);
}

GAMECORE_DEFINE_COMPONENT_REPLICATION(PvpMatchComponent)
{
    Rep.Field("Phase",       &PvpMatchComponent::Phase);
    Rep.Field("SecondsLeft", &PvpMatchComponent::SecondsLeft);
    Rep.Field("Winner",      &PvpMatchComponent::Winner);
}

// === What every process keeps ====================================================

namespace
{
    using GameCore::EntityID;
    using EngineCore::Vector3;

    struct Vec { double X, Y, Z; };
    Vec V(const Vector3& In) { return { In.GetX(), In.GetY(), In.GetZ() }; }

    double NowSeconds()
    {
        static const auto Start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
    }

    /* What the HUD shows that is not a replicated field. */
    struct FeedLine { std::string Text; double Until; };
    std::vector<FeedLine> GFeed;
    std::string GStatus;
    double      GStatusUntil = 0.0;
    double      GHitMarkerUntil = 0.0;

    /* Short-lived effects this process made. */
    struct Effect { EntityID Entity; double Until; };
    std::vector<Effect> GEffects;

    bool IsPresentation() { return !NimbleEngine::IsDedicatedServer(); }

    PvpFighterComponent* FighterOf(GameCore::World& InWorld, const EntityID InEntity)
    {
        return InEntity.IsSet() && InWorld.IsAlive(InEntity) ? InWorld.GetEntity(InEntity).GetComponent<PvpFighterComponent>() : nullptr;
    }

    PvpMatchComponent* FindMatch(GameCore::World& InWorld)
    {
        PvpMatchComponent* Found = nullptr;
        InWorld.Each<PvpMatchComponent>([&Found](GameCore::Entity, PvpMatchComponent& InMatch) { Found = &InMatch; });
        return Found;
    }

    Vec FeetOf(GameCore::World& InWorld, const EntityID InEntity)
    {
        const GameCore::TransformComponent* T = InWorld.GetEntity(InEntity).GetComponent<GameCore::TransformComponent>();
        return T ? V(T->Position) : Vec{ 0, 0, 0 };
    }

    void SetStatus(const std::string& InText, const double InSeconds)
    {
        GStatus = InText;
        GStatusUntil = NowSeconds() + InSeconds;
    }

    void SpawnEffect(GameCore::World& InWorld, const char* InSystem, const Vec& InAt, const double InLife)
    {
        if(!IsPresentation()) { return; }
        GameCore::Entity Fx = InWorld.CreateEntity("PvpEffect");
        GameCore::TransformComponent Transform;
        Transform.Position = Vector3(InAt.X, InAt.Y, InAt.Z);
        Fx.AddComponent(Transform);
        RenderCore::ParticleSystemComponent Emitter;
        std::snprintf(Emitter.System, sizeof(Emitter.System), "%s%s.nmparticle", Particles, InSystem);
        Fx.AddComponent(Emitter);
        GEffects.push_back({ Fx.GetID(), NowSeconds() + InLife });
    }

    /* The fighter's muzzle in the world, or its pivot if the pistol is not here yet. */
    Vec MuzzleOf(GameCore::World& InWorld, const EntityID InFighter)
    {
        const EntityID Pistol = WorldsLabWalker::FindChild(InWorld, InFighter, "Pistol");
        const EntityID Muzzle = Pistol.IsSet() ? WorldsLabWalker::FindChild(InWorld, Pistol, "Muzzle") : EntityID();
        if(Muzzle.IsSet()) { return V(InWorld.GetEntity(Muzzle).GetWorldPose().Position); }
        const Vec Feet = FeetOf(InWorld, InFighter);
        return { Feet.X, Feet.Y + 140.0, Feet.Z };
    }

    /* The pistol in the right hand (Muzzle at its end) and the camera boom
     * RangeMove poses, as the Shooting Range's shooter has them -- made here,
     * on this process, the first time it sees InFighter. */
    void DressFighter(GameCore::World& InWorld, const EntityID InFighter)
    {
        if(WorldsLabWalker::FindChild(InWorld, InFighter, "Pistol").IsSet()) { return; }
        const auto Child = [&InWorld](const char* InName, const EntityID InParent, const Vector3& InAt) -> GameCore::Entity
        {
            GameCore::Entity E = InWorld.CreateEntity(InName);
            GameCore::TransformComponent T;
            T.Position = InAt;
            E.AddComponent(T);
            E.SetParent(InParent, false);
            return E;
        };

        GameCore::Entity Pistol = Child("Pistol", InFighter, Vector3(0, 0, 0));
        RenderCore::RenderComponent Mesh;
        std::snprintf(Mesh.Mesh, sizeof(Mesh.Mesh), "%s", "Engine/Content/Characters/CrashDummy/Weapon_Pistol.nmmesh");
        std::snprintf(Mesh.Materials[0], sizeof(Mesh.Materials[0]), "%s", "Content/Materials/ShootingRange/GunMetal.material.nmgraph");
        std::snprintf(Mesh.Materials[1], sizeof(Mesh.Materials[1]), "%s", "Content/Materials/ShootingRange/GunMetal.material.nmgraph");
        Pistol.AddComponent(Mesh);
        RenderCore::AnimSocketComponent Socket;
        std::snprintf(Socket.Bone, sizeof(Socket.Bone), "%s", "DEF-hand.R");
        Socket.PositionOffset = Vector3(-2.110, 7.087, -0.769);
        Socket.RotationOffset = Vector3(-85.592, 13.264, 175.197);
        Pistol.AddComponent(Socket);
        Child("Muzzle", Pistol.GetID(), Vector3(0, 5.5, -18));

        GameCore::Entity Boom = Child("CameraBoom", InFighter, Vector3(0, BoomHeight, 0));
        GameCore::Entity Camera = Child("Camera", Boom.GetID(), Vector3(42, 12, -230));
        RenderCore::CameraComponent Lens;
        Lens.ZNear = 5.0f;
        Lens.ZFar = 20000.0f;
        Lens.bActive = 0;
        Lens.Projection = RenderCore::CP_Perspective;
        Lens.FovY = 62.0f;
        Camera.AddComponent(Lens);
    }

    void AddFeed(const std::string& InText)
    {
        GFeed.push_back({ InText, NowSeconds() + 6.0 });
        if(GFeed.size() > 4) { GFeed.erase(GFeed.begin()); }
    }
}

// === Hits: the server's ==========================================================

namespace
{
    void Respawn(GameCore::World& InWorld, EntityID InFighter);
}

void PvpFighterComponent::ServerFire(const GameCore::RpcContext& InContext, const Vector3& InOrigin, const Vector3& InDir)
{
    GameCore::World& World = *InContext.OwningWorld;
    const EntityID Self = InContext.Entity;
    const PvpMatchComponent* Match = FindMatch(World);
    if(bDead || bReloading || Ammo <= 0 || Cooldown > FireDelay * 0.25f || (Match && Match->Phase != 0)) { return; }

    Vec Dir = V(InDir);
    const double Length = std::sqrt(Dir.X * Dir.X + Dir.Y * Dir.Y + Dir.Z * Dir.Z);
    if(Length < 1e-4) { return; }
    Dir = { Dir.X / Length, Dir.Y / Length, Dir.Z / Length };

    /* The shot starts at the shooter's camera, which is near its pivot -- not anywhere it likes. */
    const Vec Feet = FeetOf(World, Self);
    Vec Origin = V(InOrigin);
    const double OX = Origin.X - Feet.X, OY = Origin.Y - (Feet.Y + BoomHeight), OZ = Origin.Z - Feet.Z;
    if(OX * OX + OY * OY + OZ * OZ > MaxOriginSlop * MaxOriginSlop) { Origin = { Feet.X, Feet.Y + BoomHeight, Feet.Z }; }

    --Ammo;
    Cooldown = FireDelay;

    /* The street as it is, and every other fighter where this shooter saw it (LagCompensation.h). */
    NimbleEngine::LagCompensatedHit Hit;
    const bool bAny = GPvpLagCompensation.GetBool()
        ? NimbleEngine::LagCompensatedRaycast(World, InContext, Vector3(Origin.X, Origin.Y, Origin.Z), Vector3(Dir.X, Dir.Y, Dir.Z),
                                              ShotRange, Hit, ShotMask)
        : NimbleEngine::LagCompensatedRaycastAt(World, 0, Self, Vector3(Origin.X, Origin.Y, Origin.Z), Vector3(Dir.X, Dir.Y, Dir.Z),
                                                ShotRange, Hit, ShotMask);
    ++Shots;
    PvpFighterComponent* Target = bAny && Hit.bCompensated ? FighterOf(World, Hit.Entity) : nullptr;
    if(Target && Target->bDead) { Target = nullptr; }
    const EntityID Victim  = Target ? Hit.Entity : EntityID();
    const double   Nearest = bAny ? double(Hit.Distance) : double(ShotRange);
    const bool     bWall   = bAny && !Hit.bCompensated;
    const Vec End { Origin.X + Dir.X * Nearest, Origin.Y + Dir.Y * Nearest, Origin.Z + Dir.Z * Nearest };
    GAMECORE_RPC(PvpFighterComponent, MulticastShot)(World.GetEntity(Self), Vector3(End.X, End.Y, End.Z),
                                                     uint8_t(Victim.IsSet() ? 2 : (bWall ? 1 : 0)));
    if(!Victim.IsSet()) { return; }

    ++Hits;
    const bool bHead = double(Hit.Height) >= HeadFrom;
    const int32_t Damage = bHead ? HeadDamage : BodyDamage;
    Target->Health = std::max(0, Target->Health - Damage);
    const bool bKill = Target->Health == 0;
    if(bKill)
    {
        Target->bDead     = 1;
        Target->RespawnIn = RespawnDelay;
        ++Target->Deaths;
        /* Its body is a ragdoll now: no capsule standing where it fell. */
        if(NimbleEngine::LagCompensatedComponent* Capsule = World.GetEntity(Victim).GetComponent<NimbleEngine::LagCompensatedComponent>())
        {
            Capsule->bEnabled = 0;
        }
        if(NimbleEngine::AvatarHarnessComponent* Harness = World.GetEntity(Victim).GetComponent<NimbleEngine::AvatarHarnessComponent>())
        {
            Harness->bEnabled = 0;
        }
        /* Pointers into the pool: re-find the shooter after touching the victim. */
        if(PvpFighterComponent* Shooter = FighterOf(World, Self))
        {
            ++Shooter->Kills;
            CORE_LOG(std::string("[SteampunkPvP] ") + Shooter->Name + (bHead ? " head-shot " : " killed ") + Target->Name
                     + " (" + std::to_string(Shooter->Kills) + " kills)")
        }
        GAMECORE_RPC(PvpFighterComponent, MulticastKilled)(World.GetEntity(Victim), Self, bHead,
                                                           Vector3(Dir.X * KillKick, Dir.Y * KillKick, Dir.Z * KillKick),
                                                           Vector3(End.X, End.Y, End.Z));
    }
    GAMECORE_RPC(PvpFighterComponent, ClientHitConfirm)(World.GetEntity(Self), Damage, bKill, bHead);
}

void PvpFighterComponent::ServerReload(const GameCore::RpcContext&)
{
    if(bDead || bReloading || Ammo >= Magazine) { return; }
    bReloading = 1;
    ReloadIn   = ReloadSeconds;
}

// === Feedback: every process =====================================================

void PvpFighterComponent::MulticastShot(const GameCore::RpcContext& InContext, const Vector3& InTo, const uint8_t InHit)
{
    GameCore::World& World = *InContext.OwningWorld;
    const NimbleEngine::AvatarHarnessComponent* Harness =
        World.GetEntity(InContext.Entity).GetComponent<NimbleEngine::AvatarHarnessComponent>();
    if(Harness && Harness->IsLocallyControlled()) { return; }   // the shooter's process showed it as it fired
    if(!IsPresentation()) { return; }

    const Vec Muzzle = MuzzleOf(World, InContext.Entity);
    SpawnEffect(World, "MuzzleFlash", Muzzle, 1.2);
    NimbleEngine::PlaySoundAt(World, (std::string(Sounds) + "Gunshot.nmsound").c_str(), Vector3(Muzzle.X, Muzzle.Y, Muzzle.Z), 1.0f);
    if(InHit != 0)
    {
        SpawnEffect(World, InHit == 2 ? "Sparks" : "Dust", V(InTo), 1.2);
        NimbleEngine::PlaySoundAt(World, (std::string(Sounds) + "Impact.nmsound").c_str(), InTo, 0.6f);
    }
}

void PvpFighterComponent::MulticastKilled(const GameCore::RpcContext& InContext, const EntityID InKiller, const bool bInHead,
                                          const Vector3& InImpulse, const Vector3& InAt)
{
    GameCore::World& World = *InContext.OwningWorld;
    const PvpFighterComponent* Killer = FighterOf(World, InKiller);
    AddFeed(std::string(Killer ? Killer->Name : "?") + (bInHead ? "  [HEADSHOT]  " : "  >  ") + Name);

    if(!IsPresentation()) { return; }
    if(NimbleEngine::RagdollComponent* Ragdoll = World.GetEntity(InContext.Entity).GetComponent<NimbleEngine::RagdollComponent>())
    {
        if(!Ragdoll->IsActive()) { Ragdoll->ActivateWithImpulse(InImpulse, InAt); }
    }
    bShownDead = 1;
    const EntityID Local = NimbleEngine::GetLocalAvatar(World, 0);
    if(InContext.Entity == Local)
    {
        SetStatus(std::string("KILLED BY ") + (Killer ? Killer->Name : "?"), RespawnDelay);
    }
}

void PvpFighterComponent::ClientHitConfirm(const GameCore::RpcContext& InContext, const int32_t, const bool bInKill, const bool bInHead)
{
    if(!IsPresentation()) { return; }
    GameCore::World& World = *InContext.OwningWorld;
    if(InContext.Entity != NimbleEngine::GetLocalAvatar(World, 0)) { return; }   // a bot's
    GHitMarkerUntil = NowSeconds() + 0.25;
    if(bInKill) { SetStatus(bInHead ? "HEADSHOT KILL" : "KILL", 1.5); }
    NimbleEngine::PlaySound2D(World, (std::string(Sounds) + (bInKill ? "SteelHit.nmsound" : "PaperHit.nmsound")).c_str(), 0.7f);
}

// === Respawning, and the round: the server's =====================================

namespace
{
    /* The Player Start farthest from every fighter still standing. */
    EntityID PickStart(GameCore::World& InWorld, const EntityID InFor)
    {
        std::vector<Vec> Standing;
        InWorld.Each<PvpFighterComponent>([&](GameCore::Entity InOther, PvpFighterComponent& InFighter)
        {
            if(InOther.GetID() != InFor && !InFighter.bDead) { Standing.push_back(FeetOf(InWorld, InOther.GetID())); }
        });
        EntityID Best;
        double   BestScore = -1.0;
        InWorld.Each<NimbleEngine::PlayerStartComponent>([&](GameCore::Entity InStart, NimbleEngine::PlayerStartComponent& InComp)
        {
            if(!InComp.bEnabled) { return; }
            const Vec P = FeetOf(InWorld, InStart.GetID());
            double Closest = 1e12;
            for(const Vec& S : Standing)
            {
                const double DX = S.X - P.X, DZ = S.Z - P.Z;
                Closest = std::min(Closest, DX * DX + DZ * DZ);
            }
            if(Closest > BestScore) { BestScore = Closest; Best = InStart.GetID(); }
        });
        return Best;
    }

    void Respawn(GameCore::World& InWorld, const EntityID InFighter)
    {
        const EntityID Start = PickStart(InWorld, InFighter);
        if(Start.IsSet())
        {
            const GameCore::TransformComponent* At = InWorld.GetEntity(Start).GetComponent<GameCore::TransformComponent>();
            if(At) { WorldsLabWalker::PlaceWalker(InWorld, InFighter, At->Position.GetX(), At->Position.GetY(), At->Position.GetZ(), At->Rotation.GetY()); }
        }
        if(PvpFighterComponent* Fighter = FighterOf(InWorld, InFighter))
        {
            Fighter->Health = MaxHealth;
            Fighter->Ammo = Magazine;
            Fighter->bDead = 0;
            Fighter->bReloading = 0;
            Fighter->RespawnIn = 0.0f;
            Fighter->Cooldown = 0.0f;
        }
        if(NimbleEngine::AvatarHarnessComponent* Harness = InWorld.GetEntity(InFighter).GetComponent<NimbleEngine::AvatarHarnessComponent>())
        {
            Harness->bEnabled = 1;
        }
        if(NimbleEngine::LagCompensatedComponent* Capsule = InWorld.GetEntity(InFighter).GetComponent<NimbleEngine::LagCompensatedComponent>())
        {
            Capsule->bEnabled = 1;
        }
    }
}

// === Bots ==========================================================================

namespace
{
    /* The street's loop and the cross street, for a bot with nobody in sight. */
    const Vec GWaypoints[] = {
        { 0, 0, -1545 },    { 1745, 0, -1545 },  { 3490, 0, -1545 }, { 3490, 0, 0 },     { 3490, 0, 1545 },
        { 1745, 0, 1545 },  { 0, 0, 1545 },      { 0, 0, 2900 },     { 0, 0, 1545 },     { -1745, 0, 1545 },
        { -3490, 0, 1545 }, { -3490, 0, 0 },     { -3490, 0, -1545 }, { -1745, 0, -1545 }, { 0, 0, -1545 },
        { 0, 0, -2900 },
    };
    constexpr size_t WaypointCount = sizeof(GWaypoints) / sizeof(GWaypoints[0]);

    struct BotState
    {
        size_t Waypoint = 0;
        double LastX = 0.0, LastZ = 0.0, StuckFor = 0.0, Strafe = 1.0, StrafeFor = 0.0;
        double LastTime = 0.0;
        double NextSightCheck = 0.0;
        bool   bStarted = false;
        bool   bFireHeld = false;
    };
    std::unordered_map<uint64_t, BotState> GBots;

    bool IsBot(GameCore::World& InWorld, const EntityID InAvatar)
    {
        const EntityID Player = NimbleEngine::GetPossessor(InWorld, InAvatar);
        const NimbleEngine::PossessorComponent* Possessor =
            Player.IsSet() ? InWorld.GetEntity(Player).GetComponent<NimbleEngine::PossessorComponent>() : nullptr;
        if(!Possessor) { return false; }
        return Possessor->LocalPlayer > 0 || GPvpBot.GetBool();
    }

    /* Can InFrom see InTo's chest? The street's bodies only. */
    bool LineOfSight(GameCore::World& InWorld, const Vec& InFrom, const Vec& InTo)
    {
        const Vec D { InTo.X - InFrom.X, InTo.Y - InFrom.Y, InTo.Z - InFrom.Z };
        const double L = std::sqrt(D.X * D.X + D.Y * D.Y + D.Z * D.Z);
        if(L < 1.0) { return true; }
        NimbleEngine::PhysicsRaycastHit Hit;
        return !NimbleEngine::PhysicsRaycast(InWorld, Vector3(InFrom.X, InFrom.Y, InFrom.Z), Vector3(D.X / L, D.Y / L, D.Z / L),
                                             float(L), Hit, ShotMask);
    }

    void BotFilter(GameCore::World& InWorld, const EntityID InAvatar, NimbleEngine::InputSnapshot& InOutInput)
    {
        static const int LookX = NimbleEngine::FindInputAxis("Range", "LookX");
        static const int LookY = NimbleEngine::FindInputAxis("Range", "LookY");
        static const int MoveForward = NimbleEngine::FindInputAxis("Range", "MoveForward");
        static const int MoveRight = NimbleEngine::FindInputAxis("Range", "MoveRight");
        const auto Bit = [](const char* InAction) { const int I = NimbleEngine::FindInputAction("Range", InAction); return I >= 0 ? 1u << I : 0u; };

        InOutInput = NimbleEngine::InputSnapshot();
        const RangeMoverComponent* Mover = InWorld.GetEntity(InAvatar).GetComponent<RangeMoverComponent>();
        const PvpFighterComponent* Self = FighterOf(InWorld, InAvatar);
        if(!Mover || !Self || Self->bDead) { return; }

        BotState& Bot = GBots[InAvatar.Pack()];
        const double Now = NowSeconds();
        const double Dt = Bot.bStarted ? std::clamp(Now - Bot.LastTime, 0.0, 0.1) : 0.0;
        Bot.LastTime = Now;
        Bot.bStarted = true;

        const Vec Feet = FeetOf(InWorld, InAvatar);
        const Vec Eye { Mover->CameraX, Mover->CameraY, Mover->CameraZ };
        const Vec Pivot { Feet.X, Feet.Y + BoomHeight, Feet.Z };

        /* The nearest fighter in sight. */
        EntityID Target;
        double   TargetDist = 1e12;
        Vec      Aim{};
        InWorld.Each<PvpFighterComponent>([&](GameCore::Entity InOther, PvpFighterComponent& InFighter)
        {
            if(InOther.GetID() == InAvatar || InFighter.bDead) { return; }
            const Vec F = FeetOf(InWorld, InOther.GetID());
            const Vec Chest { F.X, F.Y + 120.0, F.Z };
            const double DX = Chest.X - Pivot.X, DZ = Chest.Z - Pivot.Z;
            const double D2 = DX * DX + DZ * DZ;
            if(D2 < TargetDist && D2 < 4500.0 * 4500.0 && LineOfSight(InWorld, Pivot, Chest))
            {
                TargetDist = D2;
                Target = InOther.GetID();
                Aim = Chest;
            }
        });

        /* Turns the view toward (yaw, pitch), at most 220 degrees a second. */
        const auto Turn = [&](const double InYaw, const double InPitch)
        {
            const double DYaw = WorldsLabWalker::WrapDegrees(InYaw - double(Mover->CameraYaw));
            const double DPitch = InPitch - double(Mover->Pitch);
            const double Most = std::max(220.0 * std::max(Dt, 1.0 / 240.0), 0.05);
            if(LookX >= 0) { InOutInput.Axes[LookX] = float(std::clamp(DYaw, -Most, Most)); }
            if(LookY >= 0) { InOutInput.Axes[LookY] = float(std::clamp(DPitch, -Most, Most)); }
            return std::fabs(DYaw) + std::fabs(DPitch);
        };

        if(Target.IsSet())
        {
            /* From the camera, through the crosshair. */
            const double DX = Aim.X - Eye.X, DY = Aim.Y - Eye.Y, DZ = Aim.Z - Eye.Z;
            const double Yaw = std::atan2(DX, DZ) * 180.0 / 3.14159265358979323846;
            const double Pitch = -std::atan2(DY, std::sqrt(DX * DX + DZ * DZ)) * 180.0 / 3.14159265358979323846;
            const double Error = Turn(Yaw, Pitch);
            /* Strafe while shooting, switching sides now and then. */
            Bot.StrafeFor -= Dt;
            if(Bot.StrafeFor <= 0.0) { Bot.Strafe = -Bot.Strafe; Bot.StrafeFor = 0.6 + std::fmod(Now * 7.31, 1.0); }
            if(MoveRight >= 0) { InOutInput.Axes[MoveRight] = float(Bot.Strafe * 0.8); }
            /* One press a shot: let go between them. */
            if(Error < 2.5 && !Bot.bFireHeld && Self->Ammo > 0 && !Self->bReloading)
            {
                InOutInput.Pressed |= Bit("Fire");
                InOutInput.Down |= Bit("Fire");
                Bot.bFireHeld = true;
            }
            else
            {
                Bot.bFireHeld = false;
            }
            if(Self->Ammo == 0 && !Self->bReloading) { InOutInput.Pressed |= Bit("Reload"); }
            return;
        }
        Bot.bFireHeld = false;
        if(Self->Ammo < Magazine && !Self->bReloading) { InOutInput.Pressed |= Bit("Reload"); }

        /* Nobody in sight: round the loop, running -- toward a point it can see.
         * After a respawn the last one is often behind a block of houses; the
         * nearest in sight is the way back onto the loop. */
        if(Now >= Bot.NextSightCheck)
        {
            Bot.NextSightCheck = Now + 0.5;
            const auto InSight = [&](const Vec& InPoint) { return LineOfSight(InWorld, Pivot, { InPoint.X, Pivot.Y, InPoint.Z }); };
            if(!InSight(GWaypoints[Bot.Waypoint % WaypointCount]))
            {
                double Best = 1e18;
                for(size_t Index = 0; Index < WaypointCount; ++Index)
                {
                    const double D2 = std::pow(GWaypoints[Index].X - Feet.X, 2.0) + std::pow(GWaypoints[Index].Z - Feet.Z, 2.0);
                    if(D2 < Best && InSight(GWaypoints[Index])) { Best = D2; Bot.Waypoint = Index; }
                }
            }
        }
        const Vec& Goal = GWaypoints[Bot.Waypoint % WaypointCount];
        const double DX = Goal.X - Feet.X, DZ = Goal.Z - Feet.Z;
        const double Distance = std::sqrt(DX * DX + DZ * DZ);
        if(Distance < 150.0) { Bot.Waypoint = (Bot.Waypoint + 1) % WaypointCount; }
        const double Yaw = std::atan2(DX, DZ) * 180.0 / 3.14159265358979323846;
        Turn(Yaw, 6.0);
        const bool bFacing = std::fabs(WorldsLabWalker::WrapDegrees(Yaw - double(Mover->CameraYaw))) < 30.0;
        if(MoveForward >= 0 && bFacing) { InOutInput.Axes[MoveForward] = 1.0f; }
        InOutInput.Down |= Bit("Run");

        /* Stuck on a lamp post or a crate: sidestep, then on to the next point. */
        const double Moved = std::hypot(Feet.X - Bot.LastX, Feet.Z - Bot.LastZ);
        Bot.LastX = Feet.X;
        Bot.LastZ = Feet.Z;
        Bot.StuckFor = (bFacing && Moved < 40.0 * Dt) ? Bot.StuckFor + Dt : 0.0;
        if(Bot.StuckFor > 0.8)
        {
            if(MoveRight >= 0) { InOutInput.Axes[MoveRight] = 1.0f; }
            InOutInput.Pressed |= Bit("Jump");
            if(Bot.StuckFor > 2.0) { Bot.Waypoint = (Bot.Waypoint + 1) % WaypointCount; Bot.StuckFor = 0.0; }
        }
    }

    void PvpInputFilter(GameCore::World& InWorld, const EntityID InAvatar, NimbleEngine::InputSnapshot& InOutInput, void*)
    {
        if(IsBot(InWorld, InAvatar)) { BotFilter(InWorld, InAvatar, InOutInput); return; }
        WorldsLabWalker::ApplyLookSensitivity(InOutInput);
    }
}

// === The mode ======================================================================

namespace
{
    std::string Clock(const int InSeconds)
    {
        char Text[16];
        std::snprintf(Text, sizeof(Text), "%d:%02d", std::max(0, InSeconds) / 60, std::max(0, InSeconds) % 60);
        return Text;
    }
}

class WorldsLabPvPMode : public NimbleEngine::NetGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        GFeed.clear();
        GEffects.clear();
        GBots.clear();
        GStatus.clear();
        if(!NimbleEngine::IsPhysicsAvailable())
        {
            CORE_LOG_WARN("[SteampunkPvP] built without JoltPhysics: fighters walk on Bounds alone, and badly")
        }
        if(NimbleEngine::AvatarWorld* Avatars = InSession.GetAvatars()) { Avatars->SetInputFilter(&PvpInputFilter, nullptr); }
        return NetGameMode::OnLoad(InSession);
    }

    void OnStart() override
    {
        NetGameMode::OnStart();
        GameCore::World& World = Session->GetWorld();
        if(NimbleEngine::HasAuthority())
        {
            /* The match, for every client to see. */
            GameCore::Entity Match = World.CreateEntity("PvpMatch");
            Match.AddComponent<GameCore::NetComponent>();
            PvpMatchComponent State;
            State.SecondsLeft = GPvpMatchSeconds.GetInt();
            Match.AddComponent(State);

            /* Bots: more local players, driven by the input filter. */
            const int Bots = std::clamp(GPvpBots.GetInt(), 0, 3);
            for(int Index = 1; Index <= Bots; ++Index)
            {
                const EntityID Player = NimbleEngine::CreateLocalPlayer(World, uint8_t(Index));
                if(Player.IsSet()) { SpawnFor(Player); }
            }
        }
        if(IsPresentation())
        {
            Session->GetUI().Open(HudPath);
            if(!GPvpBot.GetBool()) { Session->SetMouseCaptured(true); }
        }
        CORE_LOG(std::string("[SteampunkPvP] ") + (NimbleEngine::IsServer() ? "server" : NimbleEngine::IsClient() ? "client" : "standalone")
                 + ": first to " + std::to_string(GPvpFragLimit.GetInt()) + " kills. WASD, Shift runs, Space jumps, "
                 "right mouse aims, left fires, R reloads, Tab scores, Escape frees the mouse")
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        GameCore::World& World = Session->GetWorld();
        const float Dt = float(InFrame.DeltaSeconds);
        if(NimbleEngine::HasAuthority()) { TickServer(World, Dt); }
        TickEveryone(World, Dt);
        if(IsPresentation()) { TickPresentation(World); }
    }

    void OnUnload() override
    {
        if(NimbleEngine::AvatarWorld* Avatars = Session ? Session->GetAvatars() : nullptr) { Avatars->SetInputFilter(nullptr, nullptr); }
        GEffects.clear();
        GBots.clear();
    }

protected:
    const char* GetAvatarPrefab(EntityID) override { return FighterPrefab; }

    void OnAvatarSpawned(const EntityID InPlayer, const EntityID InAvatar, const uint32_t InOrdinal) override
    {
        GameCore::World& World = Session->GetWorld();
        PvpFighterComponent* Fighter = FighterOf(World, InAvatar);
        if(!Fighter) { return; }
        const NimbleEngine::PossessorComponent* Possessor = World.GetEntity(InPlayer).GetComponent<NimbleEngine::PossessorComponent>();
        const bool bBot = Possessor && Possessor->LocalPlayer > 0;
        std::snprintf(Fighter->Name, sizeof(Fighter->Name), "%s %u", bBot ? "Bot" : "Player", unsigned(InOrdinal + 1));
        /* Somewhere nobody is, not the join order's start. */
        Respawn(World, InAvatar);
    }

private:
    void TickServer(GameCore::World& World, const float Dt)
    {
        const double Now = NowSeconds();
        std::vector<EntityID> ToRespawn;
        World.Each<PvpFighterComponent>([&](GameCore::Entity InEntity, PvpFighterComponent& InFighter)
        {
            InFighter.Cooldown = std::max(0.0f, InFighter.Cooldown - Dt);
            if(InFighter.bReloading && (InFighter.ReloadIn -= Dt) <= 0.0f)
            {
                InFighter.bReloading = 0;
                InFighter.Ammo = Magazine;
            }
            if(InFighter.bDead && (InFighter.RespawnIn -= Dt) <= 0.0f) { ToRespawn.push_back(InEntity.GetID()); }

        });

        if(Now >= NextAccuracyAt)
        {
            if(NextAccuracyAt > 0.0) { LogAccuracy(World); }
            NextAccuracyAt = Now + 30.0;
        }

        PvpMatchComponent* Match = FindMatch(World);
        if(Match && Match->Phase == 0)
        {
            for(const EntityID Fighter : ToRespawn) { Respawn(World, Fighter); }
            Match->Clock += Dt;
            Match->SecondsLeft = std::max(0, GPvpMatchSeconds.GetInt() - int(Match->Clock));

            /* First to the limit, or the most kills when the clock runs out. */
            const PvpFighterComponent* Leader = nullptr;
            World.Each<PvpFighterComponent>([&Leader](GameCore::Entity, PvpFighterComponent& InFighter)
            {
                if(!Leader || InFighter.Kills > Leader->Kills) { Leader = &InFighter; }
            });
            if(Leader && (Leader->Kills >= GPvpFragLimit.GetInt() || Match->SecondsLeft == 0))
            {
                Match->Phase = 1;
                Match->Clock = 0.0f;
                std::snprintf(Match->Winner, sizeof(Match->Winner), "%s", Leader->Name);
                CORE_LOG(std::string("[SteampunkPvP] ") + Leader->Name + " wins with " + std::to_string(Leader->Kills) + " kills")
            }
        }
        else if(Match && Match->Phase == 1)
        {
            Match->Clock += Dt;
            Match->SecondsLeft = std::max(0, int(RoundBreak - Match->Clock + 0.999f));
            if(Match->Clock >= RoundBreak)
            {
                LogAccuracy(World);
                /* A new round: scores cleared, everyone somewhere new. */
                std::vector<EntityID> All;
                World.Each<PvpFighterComponent>([&All](GameCore::Entity InEntity, PvpFighterComponent& InFighter)
                {
                    InFighter.Kills = 0;
                    InFighter.Deaths = 0;
                    InFighter.Shots = 0;
                    InFighter.Hits = 0;
                    All.push_back(InEntity.GetID());
                });
                for(const EntityID Fighter : All) { Respawn(World, Fighter); }
                Match->Phase = 0;
                Match->Clock = 0.0f;
                Match->Winner[0] = '\0';
                CORE_LOG("[SteampunkPvP] a new round")
            }
        }
    }

    /* Each fighter's hits of its shots, this round: how lag compensation is doing. */
    static void LogAccuracy(GameCore::World& InWorld)
    {
        std::string Line = std::string("[SteampunkPvP] accuracy") + (GPvpLagCompensation.GetBool() ? "" : " (no lag compensation)") + ":";
        InWorld.Each<PvpFighterComponent>([&Line](GameCore::Entity, PvpFighterComponent& InFighter)
        {
            Line += std::string("  ") + InFighter.Name + " " + std::to_string(InFighter.Hits) + "/" + std::to_string(InFighter.Shots);
        });
        CORE_LOG(Line)
    }

    /* What every process does for every fighter: the anim state for those it
     * does not step, the pose for the dead, the trigger for those it drives. */
    void TickEveryone(GameCore::World& World, const float Dt)
    {
        std::vector<EntityID> Fighters;
        World.Each<PvpFighterComponent>([&Fighters](GameCore::Entity InEntity, PvpFighterComponent&) { Fighters.push_back(InEntity.GetID()); });
        for(const EntityID Id : Fighters)
        {
            GameCore::Entity Fighter = World.GetEntity(Id);
            DressFighter(World, Id);
            PvpFighterComponent* State = Fighter.GetComponent<PvpFighterComponent>();
            NimbleEngine::AvatarHarnessComponent* Harness = Fighter.GetComponent<NimbleEngine::AvatarHarnessComponent>();
            if(!State) { continue; }

            if(RenderCore::AnimationComponent* Animation = Fighter.GetComponent<RenderCore::AnimationComponent>())
            {
                Animation->SetBool("Armed", true);
                /* RangeMove sets the rest where it steps; the others follow what it sent. */
                const bool bStepsHere = NimbleEngine::HasAuthority() || (Harness && Harness->IsLocallyControlled());
                const RangeMoverComponent* Mover = Fighter.GetComponent<RangeMoverComponent>();
                if(!bStepsHere && Mover)
                {
                    Animation->SetFloat("Speed", std::hypot(Mover->VelX, Mover->VelZ));
                    Animation->SetBool("Grounded", Mover->bGrounded != 0 || Mover->AirTime < 0.12f);
                    Animation->SetFloat("AimPitch", -Mover->Pitch);
                }
            }

            /* Dead and alive again: the ragdoll follows the replicated flag (a late joiner hears no Multicast). */
            if(IsPresentation())
            {
                NimbleEngine::RagdollComponent* Ragdoll = Fighter.GetComponent<NimbleEngine::RagdollComponent>();
                if(State->bDead && !State->bShownDead)
                {
                    if(Ragdoll && !Ragdoll->IsActive()) { Ragdoll->Activate(); }
                    State->bShownDead = 1;
                }
                else if(!State->bDead && State->bShownDead)
                {
                    if(Ragdoll) { Ragdoll->Reset(); }
                    State->bShownDead = 0;
                }
            }

            /* The process that drives it pulls the trigger. */
            State->LocalCooldown = std::max(0.0f, State->LocalCooldown - Dt);
            if(!Harness || !Harness->IsLocallyControlled() || State->bDead) { continue; }
            const PvpMatchComponent* Match = FindMatch(World);
            if(Match && Match->Phase != 0) { continue; }
            if(Harness->WasPressed("Reload") && State->Ammo < Magazine && !State->bReloading)
            {
                GAMECORE_RPC(PvpFighterComponent, ServerReload)(Fighter);
                if(Id == NimbleEngine::GetLocalAvatar(World, 0)) { NimbleEngine::PlaySound2D(World, (std::string(Sounds) + "Reload.nmsound").c_str(), 0.8f); }
            }
            if(Harness->WasPressed("Fire") && State->LocalCooldown <= 0.0f && !State->bReloading)
            {
                if(State->Ammo <= 0)
                {
                    if(Id == NimbleEngine::GetLocalAvatar(World, 0)) { NimbleEngine::PlaySound2D(World, (std::string(Sounds) + "DryFire.nmsound").c_str(), 0.9f); }
                    continue;
                }
                State->LocalCooldown = FireDelay;
                Fire(World, Id);
            }
        }

        /* Effects past their life. */
        const double Now = NowSeconds();
        for(size_t I = GEffects.size(); I-- > 0;)
        {
            if(GEffects[I].Until <= Now)
            {
                if(World.IsAlive(GEffects[I].Entity)) { World.DestroyEntity(GEffects[I].Entity); }
                GEffects.erase(GEffects.begin() + std::ptrdiff_t(I));
            }
        }
    }

    /* From the camera, through the crosshair; shown here at once, decided by the server. */
    void Fire(GameCore::World& World, const EntityID InFighter)
    {
        const RangeMoverComponent* Mover = World.GetEntity(InFighter).GetComponent<RangeMoverComponent>();
        if(!Mover) { return; }
        double DX, DY, DZ;
        WorldsLabWalker::Forward(Mover->Pitch, Mover->CameraYaw, DX, DY, DZ);
        /* Start level with the shoulder, not behind it: the camera sits back of the pivot. */
        const Vec Feet = FeetOf(World, InFighter);
        const Vec Cam { Mover->CameraX, Mover->CameraY, Mover->CameraZ };
        const Vec Pivot { Feet.X, Feet.Y + BoomHeight, Feet.Z };
        const double Ahead = std::max(0.0, (Pivot.X - Cam.X) * DX + (Pivot.Y - Cam.Y) * DY + (Pivot.Z - Cam.Z) * DZ);
        const Vec Origin { Cam.X + DX * Ahead, Cam.Y + DY * Ahead, Cam.Z + DZ * Ahead };

        if(IsPresentation())
        {
            const Vec Muzzle = MuzzleOf(World, InFighter);
            SpawnEffect(World, "MuzzleFlash", Muzzle, 1.2);
            if(InFighter == NimbleEngine::GetLocalAvatar(World, 0))
            {
                NimbleEngine::PlaySound2D(World, (std::string(Sounds) + "Gunshot.nmsound").c_str(), 1.0f);
            }
            else
            {
                NimbleEngine::PlaySoundAt(World, (std::string(Sounds) + "Gunshot.nmsound").c_str(), Vector3(Muzzle.X, Muzzle.Y, Muzzle.Z), 1.0f);
            }
            NimbleEngine::PhysicsRaycastHit Hit;
            if(NimbleEngine::PhysicsRaycast(World, Vector3(Origin.X, Origin.Y, Origin.Z), Vector3(DX, DY, DZ), ShotRange, Hit, ShotMask))
            {
                SpawnEffect(World, "Dust", { Hit.Point.GetX() - DX * 3.0, Hit.Point.GetY() - DY * 3.0, Hit.Point.GetZ() - DZ * 3.0 }, 1.2);
            }
        }
        GAMECORE_RPC(PvpFighterComponent, ServerFire)(World.GetEntity(InFighter), Vector3(Origin.X, Origin.Y, Origin.Z), Vector3(DX, DY, DZ));
    }

    void TickPresentation(GameCore::World& World)
    {
        UI::UISystem& UI = Session->GetUI();
        const double Now = NowSeconds();
        const EntityID Local = NimbleEngine::GetLocalAvatar(World, 0);
        const PvpFighterComponent* Me = FighterOf(World, Local);
        const NimbleEngine::AvatarHarnessComponent* Harness =
            Local.IsSet() ? World.GetEntity(Local).GetComponent<NimbleEngine::AvatarHarnessComponent>() : nullptr;

        /* Escape frees the mouse; a click takes it back. */
        if(Harness && !GPvpBot.GetBool())
        {
            if(Harness->WasPressed("Pause"))                                      { Session->SetMouseCaptured(false); }
            else if(!Session->IsMouseCaptured() && Harness->WasPressed("Fire"))   { Session->SetMouseCaptured(true); }
        }

        /* How prediction is doing: corrections, and the furthest one moved the fighter. */
        if(GPvpNetStats.GetBool() && NimbleEngine::IsClient() && Local.IsSet() && Now >= NextStatsAt)
        {
            NextStatsAt = Now + 5.0;
            if(NimbleEngine::AvatarWorld* Avatars = Session->GetAvatars())
            {
                const NimbleEngine::AvatarPredictionStats Stats = Avatars->TakePredictionStats(Local);
                char Line[160];
                std::snprintf(Line, sizeof(Line), "[SteampunkPvP] prediction: %u corrections, %u pending, worst %.2f units",
                              Stats.Corrections, Stats.Pending, Stats.MaxCorrection);
                CORE_LOG(Line)
            }
        }

        const PvpMatchComponent* Match = FindMatch(World);
        Set(UI, "Pvp.Health",     Me ? std::to_string(Me->Health) : "-");
        UI.SetValue("Pvp.HealthFill", UI::UIValue::Number(Me ? double(Me->Health) / MaxHealth : 0.0));
        Set(UI, "Pvp.Ammo",       Me ? std::to_string(Me->Ammo) : "-");
        UI.SetValue("Pvp.AmmoFill", UI::UIValue::Number(Me ? double(Me->Ammo) / Magazine : 0.0));
        Set(UI, "Pvp.Kills",      Me ? std::to_string(Me->Kills) : "0");
        Set(UI, "Pvp.Deaths",     Me ? std::to_string(Me->Deaths) : "0");
        Set(UI, "Pvp.Time",       Match ? Clock(Match->SecondsLeft) : "-");
        Set(UI, "Pvp.Limit",      "FIRST TO " + std::to_string(GPvpFragLimit.GetInt()));
        NimbleEngine::NetStats Link;
        Set(UI, "Pvp.Ping", NimbleEngine::IsClient() && NimbleEngine::GetNetStats(GameCore::NET_CLIENT_NONE, Link)
                                ? std::to_string(Link.RoundTripMs) + " MS" : std::string());

        /* The middle of the screen: the round's end, else what just happened. */
        std::string Banner;
        if(Match && Match->Phase == 1)
        {
            Banner = std::string(Match->Winner) + " WINS  -  NEXT ROUND IN " + std::to_string(Match->SecondsLeft);
        }
        if(NimbleEngine::IsNetReconnecting()) { Banner = "CONNECTION LOST  -  REJOINING"; }
        Set(UI, "Pvp.Banner", Banner);
        std::string Status = Now < GStatusUntil ? GStatus : std::string();
        if(Me && Me->bReloading) { Status = "RELOADING"; }
        if(Me && Me->bDead)
        {
            Status = (Now < GStatusUntil ? GStatus + "  -  " : std::string()) + "RESPAWNING";
        }

        Set(UI, "Pvp.Status", Status);

        /* The kill feed, newest last. */
        GFeed.erase(std::remove_if(GFeed.begin(), GFeed.end(), [Now](const FeedLine& InLine) { return InLine.Until <= Now; }), GFeed.end());
        for(size_t Line = 0; Line < 4; ++Line)
        {
            Set(UI, "Pvp.Feed" + std::to_string(Line), Line < GFeed.size() ? GFeed[Line].Text : std::string());
        }

        /* The scores: every fighter, most kills first; held Tab, or between rounds. */
        struct Row { std::string Name; int32_t Kills, Deaths; bool bMe; };
        std::vector<Row> Rows;
        World.Each<PvpFighterComponent>([&](GameCore::Entity InEntity, PvpFighterComponent& InFighter)
        {
            Rows.push_back({ InFighter.Name, InFighter.Kills, InFighter.Deaths, InEntity.GetID() == Local });
        });
        std::sort(Rows.begin(), Rows.end(), [](const Row& A, const Row& B) { return A.Kills != B.Kills ? A.Kills > B.Kills : A.Deaths < B.Deaths; });
        for(size_t Line = 0; Line < 8; ++Line)
        {
            std::string Text;
            if(Line < Rows.size())
            {
                char Buffer[96];
                std::snprintf(Buffer, sizeof(Buffer), "%s%-16s %3d   %3d", Rows[Line].bMe ? "> " : "  ", Rows[Line].Name.c_str(), Rows[Line].Kills, Rows[Line].Deaths);
                Text = Buffer;
            }
            Set(UI, "Pvp.Score" + std::to_string(Line), Text);
        }
        Set(UI, "Pvp.Leader", Rows.empty() ? std::string() : Rows.front().Name + "  " + std::to_string(Rows.front().Kills));

        if(UI::UIDocumentInstance* Hud = UI.Find(HudPath))
        {
            const bool bScores = (Harness && Harness->IsDown("Scores")) || (Match && Match->Phase == 1);
            SetVisible(Hud, "Scores", bScores);
            SetVisible(Hud, "HitMarker", Now < GHitMarkerUntil);
            SetVisible(Hud, "Crosshair", Me && !Me->bDead);
        }
    }

    double NextStatsAt = 0.0;
    double NextAccuracyAt = 0.0;   // the server's accuracy log

    static void Set(UI::UISystem& InUI, const std::string& InKey, const std::string& InText)
    {
        InUI.SetValue(InKey, UI::UIValue::String(InText));
    }

    static void SetVisible(UI::UIDocumentInstance* InDoc, const char* InWidget, const bool bInVisible)
    {
        if(UI::UIWidget* Widget = InDoc->FindWidget(InWidget))
        {
            Widget->Visibility = bInVisible ? UI::EUIVisibility::HitTestInvisible : UI::EUIVisibility::Hidden;
        }
    }
};

void WorldsLabRegisterPvP()
{
    NimbleEngine::RegisterGameMode<WorldsLabPvPMode>("WorldsLabPvP");
}

void WorldsLabUnregisterPvP()
{
    NimbleEngine::UnregisterGameMode("WorldsLabPvP");
}
