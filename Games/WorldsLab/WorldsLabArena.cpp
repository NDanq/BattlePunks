#include "WorldsLabWalker.h"

#include "Public/AICore/AIWorld.h"
#include "Public/AICore/BehaviorTree.h"
#include "Public/AICore/Components/AIBrainComponent.h"
#include "Public/AICore/Components/NavAgentComponent.h"
#include "Public/AICore/Components/NavObstacleComponent.h"
#include "Public/AICore/Components/PerceptionComponent.h"
#include "Public/NimbleEngine/AIFacade.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/Ragdoll.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * WorldsLabArena: the Toon Arena's mode (Content/Levels/ToonArena.nmworld,
 * Tools/Toony/make_toon_arena.py) -- AICore's demo. Three Red crash dummies
 * against three Blue, every one an AI:
 *
 *   body      the engine's CrashDummyDemo prefab, its demo script taken out,
 *             in a team's Toony paint, the pistol in its hand
 *   moving    a Nav Agent with Drive Transform: the crowd walks it across the
 *             navmesh and turns it (Mesh Yaw 180: the dummy faces -Z), the
 *             ground under it found by physics (the engine's AIHost)
 *   seeing    Perception (team, sight cone, hearing) and an AI Stimulus
 *   deciding  an AI Brain running Content/AI/ArenaBot.nmbt: a utility selector
 *             over Engage, TakeCover, Reload, Investigate and Patrol
 *   fighting  this file's tree nodes: AimAndFire (turn, then hit-scan shots
 *             through physics, with spread), Reload, PickPatrolPoint, and the
 *             ArenaStatus service (Health, Ammo, UnderFire onto the blackboard).
 *             A gunshot is a noise (AI::ReportNoise) every bot near hears
 *   dying     its ragdoll takes the killing shot; four seconds later it is back
 *             at its base, the farthest spawn point from its enemies
 *
 * The doors across the north-west open and shut every 12 s: Nav Obstacles,
 * so the navmesh closes behind them and bots re-plan round.
 *
 * VIEW. The camera circles the yard; Tab (NextView) follows each bot in turn.
 * [WorldsLab] ArenaPlayer=1 adds you, as the Shooting Range's shooter, to Red.
 * Your pistol (its Lua) ragdolls a bot, which the mode counts as your kill.
 *
 * AUTOPLAY. [WorldsLab] ArenaAutoplay=<seconds> (or -WorldsLab.ArenaAutoplay=120)
 * runs that long, logs the match ([ToonArena] lines: kills, deaths, shots and
 * hits, failed path requests, stuck agents, distance walked, and each branch's
 * share of the bots' time), and quits.
 */

NIMBLE_CONFIG_FLOAT(GArenaAutoplay, "WorldsLab", "ArenaAutoplay", 0.0f, nullptr);
NIMBLE_CONFIG_INT(GArenaBots,       "WorldsLab", "ArenaBots",     3,    nullptr);   // a team
NIMBLE_CONFIG_BOOL(GArenaPlayer,    "WorldsLab", "ArenaPlayer",   false, nullptr);

NIMBLE_DEFINE_INPUT_MAP(Arena)
{
    Map.Action("NextView", "Tab, Gamepad.RightShoulder");
    Map.Action("Pause",    "Escape, Gamepad.Start");
}

using GameCore::EntityID;
using EngineCore::Vector3;

namespace
{
    constexpr const char* DummyPrefab   = "Engine/Content/Characters/CrashDummy/CrashDummyDemo.nmprefab";
    constexpr const char* ShooterPrefab = "Content/Prefabs/ShootingRange/RangeShooter.nmprefab";
    constexpr const char* BotTree       = "Content/AI/ArenaBot.nmbt";
    constexpr const char* Sounds        = "Content/Sounds/ShootingRange/";
    constexpr const char* Particles     = "Content/Particles/ShootingRange/";

    constexpr uint8_t TeamRed = 1, TeamBlue = 2;
    constexpr int32_t MaxHealth    = 100;
    constexpr int32_t Magazine     = 10;
    constexpr int32_t BodyDamage   = 20;
    constexpr int32_t HeadDamage   = 55;
    constexpr float   FireDelay    = 0.32f;
    constexpr float   ReloadTime   = 1.6f;
    constexpr float   RespawnDelay = 4.0f;
    constexpr float   ShotRange    = 6000.0f;
    constexpr float   AimTolerance = 9.0f;     // degrees off the target it fires at
    constexpr float   UnderFireFor = 2.5f;     // seconds a hit keeps UnderFire set
    constexpr float   DoorPeriod   = 12.0f;
    constexpr double  KillKick     = 5000.0;
    constexpr double  Pi           = 3.14159265358979323846;

    /* The demo's hand socket (CrashDummyDemo.lua's HAND). */
    const Vector3 HandOffset(-2.110, 7.087, -0.769);
    const Vector3 HandRotation(-85.592, 13.264, 175.197);

    double WrapDegrees(double InDegrees)
    {
        while(InDegrees > 180.0)  { InDegrees -= 360.0; }
        while(InDegrees < -180.0) { InDegrees += 360.0; }
        return InDegrees;
    }

    double Hypot2D(const Vector3& InA, const Vector3& InB)
    {
        const double DX = double(InA.GetX()) - InB.GetX(), DZ = double(InA.GetZ()) - InB.GetZ();
        return std::sqrt(DX * DX + DZ * DZ);
    }

    const char* TeamName(const uint8_t InTeam) { return InTeam == TeamRed ? "Red" : "Blue"; }
}

// === The fighter =================================================================

/* "ArenaFighter": one bot's (or the player's) fight. The mode writes it. */
struct ArenaFighterComponent
{
    GAMECORE_COMPONENT_BODY()

    char    Name[24]  = "Bot";
    uint8_t Team      = TeamRed;
    uint8_t bDead     = 0;
    uint8_t bPlayer   = 0;
    uint8_t bReloading = 0;
    int32_t Health    = MaxHealth;
    int32_t Ammo      = Magazine;
    int32_t Kills     = 0;
    int32_t Deaths    = 0;
    int32_t Shots     = 0;
    int32_t Hits      = 0;
    float   Cooldown  = 0.0f;
    float   ReloadIn  = 0.0f;
    float   RespawnIn = 0.0f;
    float   HurtFor   = 0.0f;     // UnderFire while above 0
};

GAMECORE_DEFINE_COMPONENT(ArenaFighterComponent, "ArenaFighter")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(ArenaFighterComponent, 1)
{
    Ar.Field("Name", C.Name);
    Ar.Field("Team", C.Team);
}

// === What every system and tree node uses ======================================

namespace
{
    struct BotTrack
    {
        Vector3 LastPosition;
        double  Walked   = 0.0;
        double  StuckFor = 0.0;
        bool    bStuck   = false;
        Vector3 LastTarget;
        bool    bHadTarget = false;
        double  LastRise = 0.0;     // last frame's change in height
        int     Frames   = 0;       // since it (re)spawned
    };

    ArenaFighterComponent* FighterOf(GameCore::World& InWorld, const EntityID InEntity)
    {
        return InWorld.IsAlive(InEntity) ? InWorld.GetEntity(InEntity).GetComponent<ArenaFighterComponent>() : nullptr;
    }

    EntityID RootOf(GameCore::World& InWorld, EntityID InEntity)
    {
        for(int Depth = 0; Depth < 32 && InWorld.IsAlive(InEntity); ++Depth)
        {
            const EntityID Parent = InWorld.GetEntity(InEntity).GetParent();
            if(!Parent.IsSet()) { break; }
            InEntity = Parent;
        }
        return InEntity;
    }

    Vector3 FeetOf(GameCore::World& InWorld, const EntityID InEntity)
    {
        return InWorld.GetEntity(InEntity).GetWorldPose().Position;
    }

    Vector3 ChestOf(GameCore::World& InWorld, const EntityID InEntity)
    {
        const Vector3 Feet = FeetOf(InWorld, InEntity);
        return Vector3(Feet.GetX(), Feet.GetY() + 120.0, Feet.GetZ());
    }

    /* The nearest physics hit from InFrom toward InTo that is not InSelf's
     * (its own hit boxes are stepped past). False: nothing in the way. */
    bool ShotRay(GameCore::World& InWorld, const EntityID InSelf, const Vector3& InFrom, const Vector3& InDirection, const float InLength,
                 NimbleEngine::PhysicsRaycastHit& OutHit)
    {
        Vector3 From = InFrom;
        float   Left = InLength;
        for(int Step = 0; Step < 8 && Left > 1.0f; ++Step)
        {
            if(!NimbleEngine::PhysicsRaycast(InWorld, From, InDirection, Left, OutHit)) { return false; }
            if(RootOf(InWorld, OutHit.Entity) != InSelf) { OutHit.Distance += InLength - Left; return true; }
            const float Past = OutHit.Distance + std::max(2.0f, 12.0f - OutHit.Distance);   // out of its own capsules
            From = From + InDirection * Past;
            Left -= Past;
        }
        return false;
    }

    // === Dying and coming back ===================================================

    void SetAlive(GameCore::World& InWorld, const EntityID InBot, const bool bInAlive)
    {
        GameCore::Entity Bot = InWorld.GetEntity(InBot);
        if(AICore::AIStimulusComponent* Stimulus = Bot.GetComponent<AICore::AIStimulusComponent>()) { Stimulus->bEnabled = bInAlive ? 1 : 0; }
        if(AICore::PerceptionComponent* Sight = Bot.GetComponent<AICore::PerceptionComponent>())   { Sight->bEnabled = bInAlive ? 1 : 0; }
        AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
        if(!AI) { return; }
        if(bInAlive)
        {
            AI->StartBrain(InBot);
        }
        else
        {
            AI->StopBrain(InBot);
            AI->GetNav().StopMoving(InBot);
            if(AICore::NavAgentComponent* Agent = Bot.GetComponent<AICore::NavAgentComponent>()) { Agent->bHasFacing = 0; }
        }
    }
}

// === The mode =====================================================================

/*
 * The match lives here, not in globals: the mode is made as the level loads
 * and destroyed as it unloads, so each load starts fresh. The tree nodes and
 * anything else with only a World reach it with FindGameMode.
 */
class WorldsLabArenaMode : public NimbleEngine::IGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        GameCore::World& World = InSession.GetWorld();

        /* The engine's crash dummy, without its demo timeline: the brain drives it. */
        bDummyLoaded = GameCore::LoadPrefabFile(DummyPrefab, Dummy);
        if(!bDummyLoaded)
        {
            CORE_LOG_ERROR(std::string("[ToonArena] cannot load ") + DummyPrefab)
            return true;
        }
        Dummy.SetScriptPath("");
        Dummy.SetTickMode(GameCore::EPrefabTickMode::Off);

        /* The level's markers and doors, by name. */
        World.ForEachEntity([](const EntityID InEntity, void* InSelf)
        {
            WorldsLabArenaMode& Self = *static_cast<WorldsLabArenaMode*>(InSelf);
            GameCore::World& W = Self.GetWorld();
            const std::string Name = W.GetEntity(InEntity).GetName();
            const Vector3 At = W.GetEntity(InEntity).GetWorldPose().Position;
            if(Name.rfind("Patrol_", 0) == 0)          { Self.Patrol.push_back(At); }
            else if(Name.rfind("Spawn_Red_", 0) == 0)  { Self.TeamSpawns[TeamRed].push_back(At); }
            else if(Name.rfind("Spawn_Blue_", 0) == 0) { Self.TeamSpawns[TeamBlue].push_back(At); }
            else if(Name.rfind("Door_", 0) == 0)       { Self.Doors.push_back(InEntity); Self.DoorHome.push_back(At); }
        }, this);
        std::sort(Patrol.begin(), Patrol.end(), [](const Vector3& A, const Vector3& B) { return A.GetX() < B.GetX(); });

        const int PerTeam = std::clamp(GArenaBots.GetInt(), 1, 8);
        for(int Index = 0; Index < PerTeam; ++Index)
        {
            for(const uint8_t Team : { TeamRed, TeamBlue })
            {
                const EntityID Bot = SpawnBot(World, Team, Index);
                if(Bot.IsSet()) { Bots.push_back(Bot); }
            }
        }

        /* The camera, and the keys that move it. */
        GameCore::Entity Camera = World.CreateEntity("ArenaCamera");
        Camera.AddComponent<GameCore::TransformComponent>();
        RenderCore::CameraComponent Lens;
        Lens.Projection = RenderCore::CP_Perspective;
        Lens.ZNear = 5.0f;
        Lens.FovY = 58.0f;
        Lens.ZFar = 30000.0f;
        Camera.AddComponent(Lens);
        CameraEntity = Camera.GetID();
        GameCore::Entity Input = World.CreateEntity("ArenaInput");
        NimbleEngine::InputComponent Keys;
        std::snprintf(Keys.Map, sizeof(Keys.Map), "%s", "Arena");
        Input.AddComponent(Keys);
        InputEntity = Input.GetID();

        /* Member functions; the session takes them back after OnUnload. */
        AddSystemBeforeScripts<&WorldsLabArenaMode::PreTick>("WorldsLab.ArenaPre", GameCore::TP_PrePhysics);
        AddSystem<&WorldsLabArenaMode::PostTick>("WorldsLab.ArenaPost", GameCore::TP_PostPhysics);
        CORE_LOG("[ToonArena] " + std::to_string(Bots.size()) + " bots, " + std::to_string(Patrol.size()) + " patrol points, "
                 + std::to_string(Doors.size()) + " doors")
        return true;
    }

    void OnStart() override
    {
        GameCore::World& World = GetWorld();
        if(GArenaPlayer.GetBool()) { SpawnPlayer(World); }
        else { GetSession().SetViewTarget(CameraEntity); }
        CORE_LOG(std::string("[ToonArena] Tab follows each bot in turn") + (GArenaAutoplay.GetFloat() > 0.0f
                 ? "; autoplay for " + std::to_string(int(GArenaAutoplay.GetFloat())) + " s" : std::string()))
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        GameCore::World& World = GetWorld();
        if(const NimbleEngine::InputComponent* Keys = World.GetEntity(InputEntity).GetComponent<NimbleEngine::InputComponent>())
        {
            if(Keys->WasPressed("NextView") && !Player.IsSet())
            {
                FollowIndex = FollowIndex + 1 >= int(Bots.size()) ? -1 : FollowIndex + 1;
            }
        }
        if(!Player.IsSet()) { MoveCamera(World, InFrame.DeltaSeconds); }

        if(Clock >= NextReport)
        {
            NextReport += 30.0;
            Report(World, false);
        }
        const float Autoplay = GArenaAutoplay.GetFloat();
        if(Autoplay > 0.0f && Clock >= double(Autoplay) && !bReported)
        {
            bReported = true;
            Report(World, true);
            GetSession().RequestQuit();
        }
    }

    /* Nothing to reset or unbind: the members go with the mode, the systems
     * with the session's take-back. */
    void OnUnload() override
    {
        if(!bReported && Clock > 5.0) { Report(GetWorld(), true); }
    }

    // === For the tree nodes (FindGameMode<WorldsLabArenaMode>) ===============

    /* InShooter's shot at InTarget, if its gun is ready. */
    void Fire(GameCore::World& InWorld, EntityID InShooter, EntityID InTarget);

    /* The Patrol_N markers, west to east. */
    const std::vector<Vector3>& GetPatrol() const { return Patrol; }

private:
    struct Effect { EntityID Entity; double Until; };

    void SpawnEffect(GameCore::World& InWorld, const char* InSystem, const Vector3& InAt, double InLife);
    void Kill(GameCore::World& InWorld, EntityID InVictim, EntityID InKiller, const Vector3& InImpulse, const Vector3& InAt, bool bInHead);
    Vector3 PickSpawn(GameCore::World& InWorld, uint8_t InTeam);
    void Respawn(GameCore::World& InWorld, EntityID InBot);
    void PreTick(GameCore::World& InWorld, float InDeltaSeconds);
    void PostTick(GameCore::World& InWorld, float InDeltaSeconds);
    EntityID SpawnBot(GameCore::World& InWorld, uint8_t InTeam, int InIndex);
    void SpawnPlayer(GameCore::World& InWorld);
    void Report(GameCore::World& InWorld, bool bInFinal);

    void MoveCamera(GameCore::World& InWorld, const double InDeltaSeconds)
    {
        GameCore::TransformComponent* View = InWorld.GetEntity(CameraEntity).GetComponent<GameCore::TransformComponent>();
        if(!View) { return; }
        Vector3 Eye, Look;
        if(FollowIndex >= 0 && FollowIndex < int(Bots.size()))
        {
            /* Behind and above the bot, looking where it looks. */
            const EntityID Bot = Bots[size_t(FollowIndex)];
            const Vector3 Feet = FeetOf(InWorld, Bot);
            const double Yaw = double(AICore::GetFacingYaw(InWorld.GetEntity(Bot))) * Pi / 180.0;
            Eye  = Vector3(Feet.GetX() - std::sin(Yaw) * 340.0, Feet.GetY() + 210.0, Feet.GetZ() - std::cos(Yaw) * 340.0);
            Look = Vector3(Feet.GetX() + std::sin(Yaw) * 250.0, Feet.GetY() + 130.0, Feet.GetZ() + std::cos(Yaw) * 250.0);
        }
        else
        {
            /* Round the yard, close in over the fighting: the camera looks at
             * the bots' middle and circles it. */
            Orbit += InDeltaSeconds * 6.0 * Pi / 180.0;
            Vector3 Middle;
            int Alive = 0;
            for(const EntityID Bot : Bots)
            {
                const ArenaFighterComponent* Fighter = FighterOf(InWorld, Bot);
                if(!Fighter || Fighter->bDead) { continue; }
                Middle = Middle + FeetOf(InWorld, Bot);
                ++Alive;
            }
            Middle = Alive ? Middle / float(Alive) : Vector3();
            Focus = Focus + (Middle - Focus) * float(std::min(1.0, InDeltaSeconds * 0.8));
            Eye  = Vector3(Focus.GetX() + std::sin(Orbit) * 2300.0, 1400.0, Focus.GetZ() + std::cos(Orbit) * 2300.0);
            Look = Vector3(Focus.GetX(), 60.0, Focus.GetZ());
        }
        /* Eased, so a cut is a short swoop. */
        const double Ease = std::min(1.0, InDeltaSeconds * 4.0);
        const Vector3 From = View->Position;
        View->Position = From + (Eye - From) * float(Ease);
        const Vector3 Dir = Look - View->Position;
        const double Flat = std::sqrt(double(Dir.GetX()) * Dir.GetX() + double(Dir.GetZ()) * Dir.GetZ());
        const double Yaw = std::atan2(double(Dir.GetX()), double(Dir.GetZ())) * 180.0 / Pi;
        const double Pitch = std::atan2(-double(Dir.GetY()), std::max(Flat, 1.0)) * 180.0 / Pi;
        View->Rotation = Vector3(Pitch, Yaw, 0.0);
    }

    // === The match: this level's, made and gone with the mode ===============

    GameCore::PrefabAsset      Dummy;          // CrashDummyDemo, its script removed
    bool                       bDummyLoaded = false;
    std::vector<EntityID>      Bots;
    EntityID                   Player;
    EntityID                   CameraEntity;
    EntityID                   InputEntity;
    std::vector<Vector3>       Patrol;
    std::vector<Vector3>       TeamSpawns[3];  // by team
    std::vector<EntityID>      Doors;
    std::vector<Vector3>       DoorHome;
    bool                       bDoorsShut = true;
    double  DoorClock = 0.0;
    double  Clock = 0.0;
    double  NextReport = 30.0;
    int     FollowIndex = -1;                  // -1 orbits; else follows Bots[FollowIndex]
    int32_t TeamKills[3] = {};
    int32_t PlayerKills = 0;
    uint32_t FailedMoves = 0;
    uint32_t StuckEvents = 0;
    uint32_t BobFrames = 0;     // height jumped and came straight back: a flicker
    uint32_t BotFrames = 0;
    uint32_t MoveRequests = 0;
    double   PathLengthSum = 0.0;
    uint32_t ShotSeed = 0x1234567u;            // the shots' spread: the same each load
    std::unordered_map<uint64_t, BotTrack> Tracks;
    std::vector<Effect> Effects;

    double  Orbit = 0.0;
    Vector3 Focus;
    bool    bReported = false;
};

void WorldsLabArenaMode::SpawnEffect(GameCore::World& InWorld, const char* InSystem, const Vector3& InAt, const double InLife)
{
    GameCore::Entity Fx = InWorld.CreateEntity("ArenaEffect");
    GameCore::TransformComponent Transform;
    Transform.Position = InAt;
    Fx.AddComponent(Transform);
    RenderCore::ParticleSystemComponent Emitter;
    std::snprintf(Emitter.System, sizeof(Emitter.System), "%s%s.nmparticle", Particles, InSystem);
    Fx.AddComponent(Emitter);
    Effects.push_back({ Fx.GetID(), Clock + InLife });
}

void WorldsLabArenaMode::Kill(GameCore::World& InWorld, const EntityID InVictim, const EntityID InKiller, const Vector3& InImpulse, const Vector3& InAt,
          const bool bInHead)
{
    ArenaFighterComponent* Victim = FighterOf(InWorld, InVictim);
    if(!Victim || Victim->bDead) { return; }
    Victim->bDead = 1;
    Victim->RespawnIn = RespawnDelay;
    ++Victim->Deaths;
    const std::string VictimName = Victim->Name;
    const uint8_t     VictimTeam = Victim->Team;
    if(!Victim->bPlayer) { SetAlive(InWorld, InVictim, false); }
    if(NimbleEngine::RagdollComponent* Ragdoll = InWorld.GetEntity(InVictim).GetComponent<NimbleEngine::RagdollComponent>())
    {
        if(!Ragdoll->IsActive()) { Ragdoll->ActivateWithImpulse(InImpulse, InAt); }
    }
    std::string KillerName = "?";
    if(ArenaFighterComponent* Killer = FighterOf(InWorld, InKiller))
    {
        ++Killer->Kills;
        ++TeamKills[Killer->Team];
        if(Killer->bPlayer) { ++PlayerKills; }
        KillerName = Killer->Name;
    }
    CORE_LOG(std::string("[ToonArena] ") + KillerName + (bInHead ? " head-shot " : " downed ") + VictimName + " ("
             + TeamName(VictimTeam) + ")")
}

Vector3 WorldsLabArenaMode::PickSpawn(GameCore::World& InWorld, const uint8_t InTeam)
{
    const std::vector<Vector3>& Spawns = TeamSpawns[InTeam];
    if(Spawns.empty()) { return Vector3(); }
    double Best = -1.0;
    Vector3 Chosen = Spawns.front();
    for(const Vector3& Spawn : Spawns)
    {
        double Nearest = 1e12;
        InWorld.Each<ArenaFighterComponent>([&](const GameCore::Entity& InOther, ArenaFighterComponent& InFighter)
        {
            if(InFighter.Team == InTeam || InFighter.bDead) { return; }
            Nearest = std::min(Nearest, Hypot2D(InOther.GetWorldPose().Position, Spawn));
        });
        if(Nearest > Best) { Best = Nearest; Chosen = Spawn; }
    }
    return Chosen;
}

void WorldsLabArenaMode::Respawn(GameCore::World& InWorld, const EntityID InBot)
{
    ArenaFighterComponent* Fighter = FighterOf(InWorld, InBot);
    if(!Fighter) { return; }
    if(NimbleEngine::RagdollComponent* Ragdoll = InWorld.GetEntity(InBot).GetComponent<NimbleEngine::RagdollComponent>()) { Ragdoll->Reset(); }
    const Vector3 At = PickSpawn(InWorld, Fighter->Team);
    if(Fighter->bPlayer)
    {
        WorldsLabWalker::PlaceWalker(InWorld, InBot, At.GetX(), At.GetY(), At.GetZ(), Fighter->Team == TeamRed ? 45.0 : -135.0);
    }
    else
    {
        GameCore::TransformPose Pose = InWorld.GetEntity(InBot).GetWorldPose();
        Pose.Position = At;
        Pose.Rotation = Vector3(0.0, (Fighter->Team == TeamRed ? 45.0 : -135.0) + 180.0, 0.0);
        InWorld.GetEntity(InBot).SetWorldPose(Pose);
    }
    Fighter = FighterOf(InWorld, InBot);
    Fighter->Health = MaxHealth;
    Fighter->Ammo = Magazine;
    Fighter->bDead = 0;
    Fighter->bReloading = 0;
    Fighter->RespawnIn = 0.0f;
    Fighter->HurtFor = 0.0f;
    Tracks[InBot.Pack()] = BotTrack{ At };
    if(!Fighter->bPlayer) { SetAlive(InWorld, InBot, true); }
}

// === Shooting ================================================================

void WorldsLabArenaMode::Fire(GameCore::World& InWorld, const EntityID InShooter, const EntityID InTarget)
{
    ArenaFighterComponent* Self = FighterOf(InWorld, InShooter);
    if(!Self || Self->bDead || Self->Ammo <= 0 || Self->bReloading || Self->Cooldown > 0.0f) { return; }
    Self->Cooldown = FireDelay;
    --Self->Ammo;
    ++Self->Shots;
    const uint8_t Team = Self->Team;

    const GameCore::TransformPose Pose = InWorld.GetEntity(InShooter).GetWorldPose();
    const double Yaw = double(AICore::GetFacingYaw(InWorld.GetEntity(InShooter))) * Pi / 180.0;
    const Vector3 Muzzle(Pose.Position.GetX() + std::sin(Yaw) * 45.0, Pose.Position.GetY() + 135.0, Pose.Position.GetZ() + std::cos(Yaw) * 45.0);

    /* At the chest, with spread: wider when it moves, tighter close in. */
    const Vector3 Aim = ChestOf(InWorld, InTarget);
    Vector3 Direction = Aim - Muzzle;
    const double Distance = std::max(double(Direction.GetLength()), 1.0);
    Direction = Direction / float(Distance);
    const AICore::NavAgentComponent* Agent = InWorld.GetEntity(InShooter).GetComponent<AICore::NavAgentComponent>();
    const double Moving = Agent ? std::min(1.0, double(Agent->DesiredVelocity.GetLength()) / 300.0) : 0.0;
    const double Spread = (1.4 + 2.2 * Moving) * Pi / 180.0;
    const auto Rand = [this]() { ShotSeed = ShotSeed * 1664525u + 1013904223u; return double(ShotSeed >> 8) / double(1 << 24) * 2.0 - 1.0; };
    const double SA = Rand() * Spread, SB = Rand() * Spread;
    Direction = Vector3(Direction.GetX() + SA * Direction.GetZ(), Direction.GetY() + SB, Direction.GetZ() - SA * Direction.GetX());
    Direction = Direction / float(std::max(double(Direction.GetLength()), 1e-6));

    NimbleEngine::PhysicsRaycastHit Hit;
    const bool bHit = ShotRay(InWorld, InShooter, Muzzle, Direction, ShotRange, Hit);
    const Vector3 End = bHit ? Hit.Point : Muzzle + Direction * ShotRange;

    SpawnEffect(InWorld, "MuzzleFlash", Muzzle, 1.0);
    NimbleEngine::PlaySoundAt(InWorld, (std::string(Sounds) + "Gunshot.nmsound").c_str(), Muzzle, 0.9f);
    NimbleEngine::ReportAINoise(InWorld, Muzzle, 1.0f, InShooter);
    if(!bHit) { return; }

    const EntityID Victim = RootOf(InWorld, Hit.Entity);
    ArenaFighterComponent* Target = FighterOf(InWorld, Victim);
    SpawnEffect(InWorld, Target ? "Sparks" : "Dust", End, 1.0);
    NimbleEngine::PlaySoundAt(InWorld, (std::string(Sounds) + "Impact.nmsound").c_str(), End, 0.5f);
    if(!Target || Target->bDead || Target->Team == Team) { return; }

    if(ArenaFighterComponent* Shooter = FighterOf(InWorld, InShooter)) { ++Shooter->Hits; }
    bool bHead = false;
    if(const NimbleEngine::RagdollComponent* Ragdoll = InWorld.GetEntity(Victim).GetComponent<NimbleEngine::RagdollComponent>())
    {
        bHead = Ragdoll->GetPartAt(End) == "Head";
    }
    Target = FighterOf(InWorld, Victim);
    Target->Health = std::max(0, Target->Health - (bHead ? HeadDamage : BodyDamage));
    Target->HurtFor = UnderFireFor;
    /* It knows who shot it. */
    NimbleEngine::SetAIKeyEntity(InWorld, Victim, "TargetActor", InShooter);
    if(Target->Health == 0)
    {
        Kill(InWorld, Victim, InShooter, Direction * float(KillKick), End, bHead);
    }
}

// === The systems =============================================================

/* TP_PrePhysics, before the brains: timers, deaths the player's Lua caused, the doors. */
void WorldsLabArenaMode::PreTick(GameCore::World& InWorld, const float InDeltaSeconds)
{
    Clock += double(InDeltaSeconds);
    std::vector<EntityID> Respawning;
    InWorld.Each<ArenaFighterComponent>([&](const GameCore::Entity& InEntity, ArenaFighterComponent& InFighter)
    {
        InFighter.Cooldown = std::max(0.0f, InFighter.Cooldown - InDeltaSeconds);
        InFighter.HurtFor  = std::max(0.0f, InFighter.HurtFor - InDeltaSeconds);
        if(InFighter.bReloading)
        {
            InFighter.ReloadIn -= InDeltaSeconds;
            if(InFighter.ReloadIn <= 0.0f) { InFighter.bReloading = 0; InFighter.Ammo = Magazine; }
        }
        if(InFighter.bDead)
        {
            InFighter.RespawnIn -= InDeltaSeconds;
            if(InFighter.RespawnIn <= 0.0f) { Respawning.push_back(InEntity.GetID()); }
        }
    });
    /* A bot the player's pistol ragdolled (RangeShooter.lua): the player's kill. */
    for(const EntityID Bot : Bots)
    {
        const ArenaFighterComponent* Fighter = FighterOf(InWorld, Bot);
        const NimbleEngine::RagdollComponent* Ragdoll = InWorld.GetEntity(Bot).GetComponent<NimbleEngine::RagdollComponent>();
        if(Fighter && !Fighter->bDead && Ragdoll && Ragdoll->IsActive())
        {
            Kill(InWorld, Bot, Player, Vector3(), FeetOf(InWorld, Bot), false);
        }
    }
    for(const EntityID Id : Respawning) { Respawn(InWorld, Id); }

    DoorClock += double(InDeltaSeconds);
    if(DoorClock >= DoorPeriod)
    {
        DoorClock = 0.0;
        bDoorsShut = !bDoorsShut;
        for(size_t Index = 0; Index < Doors.size(); ++Index)
        {
            GameCore::Entity Door = InWorld.GetEntity(Doors[Index]);
            if(GameCore::TransformComponent* T = Door.GetComponent<GameCore::TransformComponent>())
            {
                const Vector3 Home = DoorHome[Index];
                T->Position = bDoorsShut ? Home : Vector3(Home.GetX(), Home.GetY() + 330.0, Home.GetZ());
            }
            if(AICore::NavObstacleComponent* Obstacle = Door.GetComponent<AICore::NavObstacleComponent>())
            {
                Obstacle->bEnabled = bDoorsShut ? 1 : 0;
            }
        }
    }
}

/* TP_PostPhysics: the anim state from how each bot moved, and the stats. */
void WorldsLabArenaMode::PostTick(GameCore::World& InWorld, const float InDeltaSeconds)
{
    const double Dt = std::max(double(InDeltaSeconds), 1e-4);
    for(const EntityID Bot : Bots)
    {
        GameCore::Entity Entity = InWorld.GetEntity(Bot);
        const ArenaFighterComponent* Fighter = Entity.GetComponent<ArenaFighterComponent>();
        AICore::NavAgentComponent* Agent = Entity.GetComponent<AICore::NavAgentComponent>();
        if(!Fighter || !Agent) { continue; }
        BotTrack& Track = Tracks[Bot.Pack()];
        const Vector3 Here = Entity.GetWorldPose().Position;
        const double Moved = Hypot2D(Here, Track.LastPosition);
        const double Rise = double(Here.GetY()) - double(Track.LastPosition.GetY());
        if(!Fighter->bDead) { ++BotFrames; }
        if(!Fighter->bDead && ++Track.Frames > 3 && std::fabs(Rise) > 5.0 && std::fabs(Track.LastRise) > 5.0 && (Rise > 0.0) != (Track.LastRise > 0.0))
        {
            ++BobFrames;
        }
        Track.LastRise = Rise;
        Track.LastPosition = Here;
        if(Fighter->bDead) { Track.StuckFor = 0.0; continue; }
        Track.Walked += Moved;

        if(RenderCore::AnimationComponent* Animation = Entity.GetComponent<RenderCore::AnimationComponent>())
        {
            Animation->SetFloat("Speed", float(Moved / Dt));
            Animation->SetBool("Grounded", true);
            Animation->SetBool("Armed", true);
            float Pitch = 0.0f;
            if(Agent->bHasFacing)
            {
                const double DY = double(Agent->FaceTarget.GetY()) - (Here.GetY() + 135.0);
                Pitch = float(std::clamp(std::atan2(DY, std::max(Hypot2D(Agent->FaceTarget, Here), 1.0)) * 180.0 / Pi, -40.0, 55.0));
            }
            Animation->SetFloat("AimPitch", Pitch);
        }

        /* Stuck: told to move, and has not, for 3 s. */
        if(Agent->bHasTarget && Agent->Status == AICore::NAS_Moving && Moved < 15.0 * Dt)
        {
            Track.StuckFor += Dt;
            if(Track.StuckFor > 3.0 && !Track.bStuck)
            {
                Track.bStuck = true;
                ++StuckEvents;
                CORE_LOG_WARN(std::string("[ToonArena] ") + Fighter->Name + " is stuck at " + std::to_string(int(Here.GetX())) + ", "
                              + std::to_string(int(Here.GetZ())))
            }
        }
        else
        {
            Track.StuckFor = 0.0;
            Track.bStuck = false;
        }
        /* A new move: its path length, from the crowd's first corners. */
        if(Agent->bHasTarget && (!Track.bHadTarget || Hypot2D(Agent->Target, Track.LastTarget) > 50.0) && Agent->RemainingDistance > 0.0f)
        {
            ++MoveRequests;
            PathLengthSum += double(Agent->RemainingDistance);
            Track.LastTarget = Agent->Target;
            Track.bHadTarget = true;
        }
        if(!Agent->bHasTarget) { Track.bHadTarget = false; }
    }

    for(size_t Index = 0; Index < Effects.size();)
    {
        if(Clock >= Effects[Index].Until)
        {
            InWorld.DestroyEntity(Effects[Index].Entity);
            Effects[Index] = Effects.back();
            Effects.pop_back();
        }
        else { ++Index; }
    }
}

// === Spawning ================================================================

EntityID WorldsLabArenaMode::SpawnBot(GameCore::World& InWorld, const uint8_t InTeam, const int InIndex)
{
    const Vector3 At = PickSpawn(InWorld, InTeam);
    GameCore::TransformPose Pose;
    Pose.Position = At;
    Pose.Rotation = Vector3(0.0, (InTeam == TeamRed ? 45.0 : -135.0) + 180.0, 0.0);
    char Name[24];
    std::snprintf(Name, sizeof(Name), "%s%d", TeamName(InTeam), InIndex + 1);
    GameCore::PrefabInstance Instance = InWorld.SpawnPrefab(Dummy, Pose, Name);
    if(!Instance.IsValid()) { return EntityID(); }
    GameCore::Entity Root = Instance.GetRoot();
    const EntityID Id = Root.GetID();

    /* The team's paint, and the pistol in the hand. */
    if(RenderCore::RenderComponent* Render = Root.GetComponent<RenderCore::RenderComponent>())
    {
        for(int Slot = 0; Slot < 6; ++Slot)
        {
            std::snprintf(Render->Materials[Slot], sizeof(Render->Materials[Slot]), "%s",
                          InTeam == TeamRed ? "Content/Materials/Toony/ToonDummyRed.material.nmgraph"
                                            : "Content/Materials/Toony/ToonDummyBlue.material.nmgraph");
        }
    }
    const EntityID Pistol = WorldsLabWalker::FindChild(InWorld, Id, "Pistol");
    if(Pistol.IsSet())
    {
        GameCore::Entity Gun = InWorld.GetEntity(Pistol);
        if(RenderCore::AnimSocketComponent* Socket = Gun.GetComponent<RenderCore::AnimSocketComponent>())
        {
            std::snprintf(Socket->Bone, sizeof(Socket->Bone), "%s", "DEF-hand.R");
            Socket->PositionOffset = HandOffset;
            Socket->RotationOffset = HandRotation;
        }
        if(RenderCore::RenderComponent* Render = Gun.GetComponent<RenderCore::RenderComponent>())
        {
            std::snprintf(Render->Materials[0], sizeof(Render->Materials[0]), "%s", "Content/Materials/Toony/ToonGun.material.nmgraph");
            std::snprintf(Render->Materials[1], sizeof(Render->Materials[1]), "%s", "Content/Materials/Toony/ToonGun.material.nmgraph");
        }
    }

    AICore::NavAgentComponent Agent;
    Agent.Radius = 35.0f;
    Agent.Height = 180.0f;
    Agent.MaxSpeed = 360.0f;
    Agent.MaxAcceleration = 1800.0f;
    Agent.bDriveTransform = 1;
    Agent.MeshYaw = 180.0f;
    Agent.TurnRate = 420.0f;
    Root.AddComponent(Agent);

    AICore::PerceptionComponent Sight;
    Sight.Team = InTeam;
    Sight.SightRange = 3800.0f;
    Sight.FovHalfAngle = 75.0f;
    Sight.EyeHeight = 160.0f;
    Sight.HearingRange = 2600.0f;
    Sight.SightInterval = 0.2f;
    Root.AddComponent(Sight);

    AICore::AIStimulusComponent Stimulus;
    Stimulus.Team = InTeam;
    Stimulus.TargetHeight = 120.0f;
    Root.AddComponent(Stimulus);

    NimbleEngine::RagdollComponent Ragdoll;
    Ragdoll.Mass = 70.0f;
    Ragdoll.AngularDamping = 0.5f;
    Root.AddComponent(Ragdoll);

    ArenaFighterComponent Fighter;
    std::snprintf(Fighter.Name, sizeof(Fighter.Name), "%s", Name);
    Fighter.Team = InTeam;
    Root.AddComponent(Fighter);

    AICore::AIBrainComponent Brain;
    std::snprintf(Brain.Tree, sizeof(Brain.Tree), "%s", BotTree);
    Brain.TickInterval = 0.05f;
    Brain.bAutoStart = 1;
    Root.AddComponent(Brain);

    Tracks[Id.Pack()] = BotTrack{ At };
    return Id;
}

void WorldsLabArenaMode::SpawnPlayer(GameCore::World& InWorld)
{
    const Vector3 At = PickSpawn(InWorld, TeamRed);
    GameCore::TransformPose Pose;
    Pose.Position = At;
    GameCore::PrefabInstance Instance = InWorld.SpawnPrefab(ShooterPrefab, Pose, "Player");
    if(!Instance.IsValid()) { CORE_LOG_WARN("[ToonArena] no RangeShooter prefab: playing without you") return; }
    const EntityID Id = Instance.GetRoot().GetID();
    WorldsLabWalker::PlaceWalker(InWorld, Id, At.GetX(), At.GetY(), At.GetZ(), 45.0);
    AICore::AIStimulusComponent Stimulus;
    Stimulus.Team = TeamRed;
    InWorld.GetEntity(Id).AddComponent(Stimulus);
    ArenaFighterComponent Fighter;
    std::snprintf(Fighter.Name, sizeof(Fighter.Name), "%s", "You");
    Fighter.Team = TeamRed;
    Fighter.bPlayer = 1;
    InWorld.GetEntity(Id).AddComponent(Fighter);
    const EntityID Local = NimbleEngine::CreateLocalPlayer(InWorld, 0);
    NimbleEngine::Possess(InWorld, Local, Id);
    Player = Id;
    GetSession().SetMouseCaptured(true);
}

// === The report ==============================================================

void WorldsLabArenaMode::Report(GameCore::World& InWorld, const bool bInFinal)
{
    AICore::AIWorld* AI = AICore::AIWorld::Find(&InWorld);
    if(AI) { FailedMoves += AI->GetNav().TakeFailedMoveCount(); }
    char Line[256];
    std::snprintf(Line, sizeof(Line), "[ToonArena] %.0f s: Red %d kills, Blue %d kills%s", Clock, TeamKills[TeamRed],
                  TeamKills[TeamBlue], PlayerKills ? (" (yours " + std::to_string(PlayerKills) + ")").c_str() : "");
    CORE_LOG(Line)
    if(!bInFinal) { return; }

    int32_t Shots = 0, Hits = 0, Deaths[3] = {};
    double Walked = 0.0;
    InWorld.Each<ArenaFighterComponent>([&](const GameCore::Entity& InEntity, ArenaFighterComponent& InFighter)
    {
        Shots += InFighter.Shots;
        Hits += InFighter.Hits;
        Deaths[InFighter.Team] += InFighter.Deaths;
        const auto Found = Tracks.find(InEntity.GetID().Pack());
        if(Found != Tracks.end()) { Walked += Found->second.Walked; }
    });
    std::snprintf(Line, sizeof(Line), "[ToonArena] deaths: Red %d, Blue %d; shots %d, hits %d (%.0f%%)", Deaths[TeamRed], Deaths[TeamBlue],
                  Shots, Hits, Shots ? 100.0 * Hits / Shots : 0.0);
    CORE_LOG(Line)
    std::snprintf(Line, sizeof(Line), "[ToonArena] navigation: %u moves, average path %.1f m, %.0f m walked, %u failed path requests, "
                  "%u stuck agents, %u bobbing frames of %u", MoveRequests, MoveRequests ? PathLengthSum / MoveRequests / 100.0 : 0.0,
                  Walked / 100.0, FailedMoves, StuckEvents, BobFrames, BotFrames);
    CORE_LOG(Line)

    /* Each branch's share of the bots' time: the root's children. */
    if(!AI) { return; }
    std::vector<std::string> Names;
    std::vector<double> Seconds;
    double Total = 0.0;
    AI->ForEachBrain([&](const EntityID, AICore::BTInstance& InBrain)
    {
        const AICore::BehaviorTreeAsset& Tree = InBrain.GetAsset();
        if(Tree.Nodes.empty()) { return; }
        const std::vector<uint32_t>& Children = Tree.Nodes[0].Children;
        if(Names.empty()) { for(const uint32_t Child : Children) { Names.push_back(Tree.Nodes[Child].GetDisplayName()); Seconds.push_back(0.0); } }
        for(size_t Index = 0; Index < Children.size() && Index < Seconds.size(); ++Index)
        {
            Seconds[Index] += InBrain.GetNodeActiveSeconds(Children[Index]);
            Total += InBrain.GetNodeActiveSeconds(Children[Index]);
        }
    });
    std::string Shares = "[ToonArena] branches:";
    bool bAll = true;
    for(size_t Index = 0; Index < Names.size(); ++Index)
    {
        char Part[64];
        std::snprintf(Part, sizeof(Part), " %s %.0f%%", Names[Index].c_str(), Total > 0.0 ? 100.0 * Seconds[Index] / Total : 0.0);
        Shares += Part;
        if(Seconds[Index] <= 0.0) { bAll = false; }
    }
    CORE_LOG(Shares)
    CORE_LOG(std::string("[ToonArena] ") + (TeamKills[TeamRed] > 0 && TeamKills[TeamBlue] > 0 && StuckEvents == 0 && BobFrames * 200 < std::max(BotFrames, 1u) && bAll
             ? "PASS: kills on both teams, no stuck or bobbing agents, every branch ran"
             : "CHECK: expected kills on both teams, no stuck or bobbing agents, and every branch to run"))
}

// === Tree nodes ===================================================================

/* Turns to the target (its Nav Agent's facing) and fires while it sees it and
 * is lined up. Runs until the target is gone; fails after 2.5 s unseen. */
class AimAndFire : public AICore::BTTask
{
public:
    AICore::EBTStatus OnEnter(AICore::BTContext& InContext) const override
    {
        InContext.Memory.Time = InContext.Time;   // last seen
        return AICore::EBTStatus::Running;
    }

    AICore::EBTStatus OnTick(AICore::BTContext& InContext) const override
    {
        const int32_t Key = InContext.GetKey("Key");
        const EntityID Target = Key >= 0 ? InContext.Board.GetValue(Key).EntityValue : EntityID();
        const ArenaFighterComponent* Foe = FighterOf(InContext.World, Target);
        if(!Foe || Foe->bDead) { return AICore::EBTStatus::Failure; }
        AICore::NavAgentComponent* Agent = InContext.World.GetEntity(InContext.Self).GetComponent<AICore::NavAgentComponent>();
        if(!Agent) { return AICore::EBTStatus::Failure; }

        const Vector3 At = ChestOf(InContext.World, Target);
        Agent->FaceTarget = At;
        Agent->bHasFacing = 1;
        const bool bVisible = InContext.Board.GetBool("TargetVisible");
        if(bVisible) { InContext.Memory.Time = InContext.Time; }
        else if(InContext.Time - InContext.Memory.Time > 2.5) { return AICore::EBTStatus::Failure; }

        const Vector3 Here = FeetOf(InContext.World, InContext.Self);
        const double Want = std::atan2(double(At.GetX()) - Here.GetX(), double(At.GetZ()) - Here.GetZ()) * 180.0 / Pi;
        const double Error = std::fabs(WrapDegrees(Want - double(AICore::GetFacingYaw(InContext.World.GetEntity(InContext.Self)))));
        if(bVisible && Error < AimTolerance)
        {
            if(WorldsLabArenaMode* Arena = NimbleEngine::FindGameMode<WorldsLabArenaMode>(InContext.World))
            {
                Arena->Fire(InContext.World, InContext.Self, Target);
            }
        }
        return AICore::EBTStatus::Running;
    }

    void OnExit(AICore::BTContext& InContext, const AICore::EBTStatus) const override
    {
        if(AICore::NavAgentComponent* Agent = InContext.World.GetEntity(InContext.Self).GetComponent<AICore::NavAgentComponent>())
        {
            Agent->bHasFacing = 0;
        }
    }
};
AICORE_DEFINE_BT_TASK(AimAndFire, "Aim and Fire")

/* A fresh magazine: Running for the reload's length. Success at once when full. */
class Reload : public AICore::BTTask
{
public:
    AICore::EBTStatus OnEnter(AICore::BTContext& InContext) const override
    {
        ArenaFighterComponent* Self = FighterOf(InContext.World, InContext.Self);
        if(!Self || Self->bDead) { return AICore::EBTStatus::Failure; }
        if(Self->Ammo >= Magazine) { return AICore::EBTStatus::Success; }
        Self->bReloading = 1;
        Self->ReloadIn = ReloadTime;
        NimbleEngine::PlaySoundAt(InContext.World, (std::string(Sounds) + "Reload.nmsound").c_str(), FeetOf(InContext.World, InContext.Self), 0.6f);
        return AICore::EBTStatus::Running;
    }

    AICore::EBTStatus OnTick(AICore::BTContext& InContext) const override
    {
        const ArenaFighterComponent* Self = FighterOf(InContext.World, InContext.Self);
        if(!Self || Self->bDead) { return AICore::EBTStatus::Failure; }
        return Self->bReloading ? AICore::EBTStatus::Running : AICore::EBTStatus::Success;
    }
};
AICORE_DEFINE_BT_TASK(Reload, "Reload")

/* One of the arena's Patrol_N markers, into Out: a far one, more often than not. */
class PickPatrolPoint : public AICore::BTTask
{
public:
    AICore::EBTStatus OnEnter(AICore::BTContext& InContext) const override
    {
        const int32_t Out = InContext.GetKey("Out");
        const WorldsLabArenaMode* Arena = NimbleEngine::FindGameMode<WorldsLabArenaMode>(InContext.World);
        if(Out < 0 || !Arena || Arena->GetPatrol().empty()) { return AICore::EBTStatus::Failure; }
        const std::vector<Vector3>& Patrol = Arena->GetPatrol();
        const Vector3 Here = FeetOf(InContext.World, InContext.Self);
        size_t Pick = size_t(InContext.Random() * float(Patrol.size())) % Patrol.size();
        for(int Try = 0; Try < 4 && Hypot2D(Patrol[Pick], Here) < 1200.0; ++Try)
        {
            Pick = size_t(InContext.Random() * float(Patrol.size())) % Patrol.size();
        }
        InContext.Board.SetVector(Out, Patrol[Pick]);
        return AICore::EBTStatus::Success;
    }
};
AICORE_DEFINE_BT_TASK(PickPatrolPoint, "Pick Patrol Point")

/* The fighter onto the blackboard: Health and Ammo (0..1), UnderFire. */
class ArenaStatus : public AICore::BTService
{
public:
    void OnTick(AICore::BTContext& InContext) const override
    {
        const ArenaFighterComponent* Self = FighterOf(InContext.World, InContext.Self);
        if(!Self) { return; }
        InContext.Board.SetFloat("Health", float(Self->Health) / float(MaxHealth));
        InContext.Board.SetFloat("Ammo", Self->bReloading ? 0.0f : float(Self->Ammo) / float(Magazine));
        InContext.Board.SetBool("UnderFire", Self->HurtFor > 0.0f);
    }
};
AICORE_DEFINE_BT_SERVICE(ArenaStatus)

NIMBLE_REGISTER_GAME_MODE(WorldsLabArenaMode, "WorldsLabArena")
