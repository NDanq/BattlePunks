#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentSerializer.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/RenderCore/RenderComponents.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

/*
 * NetArenaGame: networking's test game. Capsules on a flat plane, one per
 * player, each steered by its own player with WASD (or the left stick):
 * the first player's is red, the second's blue, then green and yellow.
 *
 * AUTHORED, NOT BUILT IN CODE. Everything placed is content the editor opens
 * and edits; this file only gives it behaviour:
 *
 *   Content/Levels/NetArena.nmworld   the plane, the sun and sky, the camera,
 *                                     the Controller (this process's keys),
 *                                     four spawn points (NetArenaSpawn: which
 *                                     colour starts there), and the Spinner --
 *                                     a bar with a Net component, so the level
 *                                     itself has something networked in it.
 *   Content/Prefabs/Pawn.nmprefab     a player's capsule: Transform, Render
 *                                     (the capsule mesh, the PawnColor
 *                                     material), Net and NetArenaPawn. The
 *                                     server spawns one per player.
 *
 * What a level entity with a Net component means: every process loads the
 * world, so every process has the Spinner; the server gives its copy a
 * NetId and the clients' copies are matched to it by GUID -- one entity in
 * the session, moved by the server (NetArena.Spin), seen turning by all.
 *
 * What a prefab with a Net component means: spawned on the server, it is
 * sent whole to every client (and to any that joins later); the server sets
 * who owns it (NetComponent::OwnerClient) and its colour after spawning.
 *
 * Server-authoritative, as NetCore is (Engine/Modules/NetCore/GUIDE.md):
 *
 *   - The server spawns the capsules: one for each client that joins
 *     (OnClientConnected), and one for a listen server's or standalone
 *     game's own user (OnStart).
 *   - Each process reads its own keyboard and sends where its capsule should
 *     go -- the Server RPC ServerSetMove, only when that changes. On the
 *     server the call simply runs.
 *   - The server moves every capsule (NetArena.MovePawns) and turns the
 *     Spinner (NetArena.Spin); Transform replication shows it to everyone.
 *
 * Run it as:
 *
 *   NetArenaServer -port=7777                  + two of: NetArena -connect=127.0.0.1:7777
 *   NetArena -listen                           + one of: NetArena -connect=127.0.0.1:7777
 *   the editor: open Content/Levels/NetArena.nmworld; Edit > Editor Settings >
 *               Play > Network mode = Listen Server; Play (F5); then
 *               Play > New Client Window (Shift+F6)
 *
 *   NetArenaPawnComponent     a capsule: its colour and the move it was asked for.
 *   NetArenaSpawnComponent    a spawn point, authored in the level.
 *   NetArenaSpinnerComponent  turns its entity, on the server.
 *   NetArenaLevel             loads the world; registers the systems; owns the mode.
 *   NetArenaGameMode          sends this process's input, spawns capsules.
 */

namespace
{
    const char* const LevelFile = "Content/Levels/NetArena.nmworld";
    const char* const PawnPrefab = "Content/Prefabs/Pawn.nmprefab";

    /* The arena: a square of this half-size around the origin. */
    constexpr double ArenaHalfSize  = 900.0;
    /* World units a second. */
    constexpr double PawnSpeed      = 450.0;
    /* Seconds between the mode's log reports. */
    constexpr double ReportInterval = 5.0;

    struct PawnColor
    {
        const char* Name;
        float R, G, B;
    };
    constexpr PawnColor PawnColors[] = {
        { "red",    0.90f, 0.08f, 0.08f },
        { "blue",   0.08f, 0.25f, 0.95f },
        { "green",  0.10f, 0.80f, 0.20f },
        { "yellow", 0.95f, 0.85f, 0.10f },
    };
    constexpr int PawnColorCount = int(sizeof(PawnColors) / sizeof(PawnColors[0]));

    std::string Fixed0(const double InValue)
    {
        char Buffer[32];
        std::snprintf(Buffer, sizeof(Buffer), "%.0f", std::round(InValue) + 0.0);   // + 0.0 turns -0 into 0
        return Buffer;
    }
}

/* A bot at the keys, for unattended tests: each process steers its capsule
 * round a square of its own, a new side each second. -NetArena.AutoMove=1. */
NIMBLE_CONFIG_BOOL(GNetArenaAutoMove, "NetArena", "AutoMove", false, nullptr);

// === Input ===================================================================

/* The local player's keys, read through the level's Controller entity. Each
 * process reads its own; the capsule they steer is whichever one it owns. */
NIMBLE_DEFINE_INPUT_MAP(Default)
{
    Map.Axis("MoveForward", "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Axis("MoveRight",   "D, Right, -A, -Left, Gamepad.LeftX");
}

// === Components ==============================================================

/* "NetArenaPawn": a player's capsule (Content/Prefabs/Pawn.nmprefab). Its
 * place is its Transform, which the server moves and replicates. */
struct NetArenaPawnComponent
{
    GAMECORE_COMPONENT_BODY()

    /* Which of PawnColors. The server sets it, and the Render colour, when
     * it spawns the capsule; both reach clients with it. */
    uint8_t ColorIndex = 0;

    /* The server's: where its player last asked to go, each axis -1..1 in X
     * (right) and Z (forward). Never sent to clients. */
    EngineCore::Vector3 MoveInput;

    /* A player's input, on the server. Clamped: a client says where to go,
     * never how fast. */
    void ServerSetMove(const EngineCore::Vector3& InDirection)
    {
        const double X = std::clamp(double(InDirection.GetX()), -1.0, 1.0);
        const double Z = std::clamp(double(InDirection.GetZ()), -1.0, 1.0);
        MoveInput = EngineCore::Vector3(X, 0.0, Z);
    }
};

GAMECORE_DEFINE_COMPONENT(NetArenaPawnComponent, "NetArenaPawn")

/* Saved in the prefab, and what a client gets when the capsule appears there. */
GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetArenaPawnComponent, 1)
{
    Ar.Field("ColorIndex", C.ColorIndex);
}

GAMECORE_DEFINE_COMPONENT_RPCS(NetArenaPawnComponent)
{
    Rpc.Server<&NetArenaPawnComponent::ServerSetMove>("ServerSetMove");
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(NetArenaPawnComponent)
{
    Props.Choice("Color", &NetArenaPawnComponent::ColorIndex, "Red|Blue|Green|Yellow");
}
#endif

/* "NetArenaSpawn": where a colour's capsule starts. Placed in the level, an
 * entity with a Transform and this; nothing else reads it but the mode. */
struct NetArenaSpawnComponent
{
    GAMECORE_COMPONENT_BODY()

    uint8_t ColorIndex = 0;
};

GAMECORE_DEFINE_COMPONENT(NetArenaSpawnComponent, "NetArenaSpawn")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetArenaSpawnComponent, 1)
{
    Ar.Field("ColorIndex", C.ColorIndex);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(NetArenaSpawnComponent)
{
    Props.Choice("Color", &NetArenaSpawnComponent::ColorIndex, "Red|Blue|Green|Yellow");
}
#endif

/* "NetArenaSpinner": turns its entity about Y. Only the server turns it; on
 * an entity with a Net component, clients see the server's Transform. */
struct NetArenaSpinnerComponent
{
    GAMECORE_COMPONENT_BODY()

    float DegreesPerSecond = 45.0f;
};

GAMECORE_DEFINE_COMPONENT(NetArenaSpinnerComponent, "NetArenaSpinner")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetArenaSpinnerComponent, 1)
{
    Ar.Field("DegreesPerSecond", C.DegreesPerSecond);
}

/* Replicated too: a server that changes the speed changes it everywhere. */
GAMECORE_DEFINE_COMPONENT_REPLICATION(NetArenaSpinnerComponent)
{
    Rep.Field("DegreesPerSecond", &NetArenaSpinnerComponent::DegreesPerSecond);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(NetArenaSpinnerComponent)
{
    Props.Field("Degrees per second", &NetArenaSpinnerComponent::DegreesPerSecond);
}
#endif

// === Systems (server only) ===================================================

namespace
{
    /* PrePhysics: every capsule by what its player asked, kept on the plane. */
    void MovePawnsSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* /*InUserData*/)
    {
        if(!NimbleEngine::HasAuthority()) { return; }

        InWorld.Each<NetArenaPawnComponent>([&](GameCore::Entity InPawn, NetArenaPawnComponent& InState)
        {
            GameCore::TransformComponent* Transform = InPawn.GetComponent<GameCore::TransformComponent>();
            if(!Transform) { return; }

            EngineCore::Vector3 Direction = InState.MoveInput;
            const double Length = std::sqrt(double(Direction.GetX()) * Direction.GetX() + double(Direction.GetZ()) * Direction.GetZ());
            if(Length < 0.01) { return; }
            if(Length > 1.0) { Direction = Direction * (1.0 / Length); }   // diagonals no faster

            const double Step = PawnSpeed * double(InDeltaSeconds);
            const double X = std::clamp(double(Transform->Position.GetX()) + double(Direction.GetX()) * Step, -ArenaHalfSize, ArenaHalfSize);
            const double Z = std::clamp(double(Transform->Position.GetZ()) + double(Direction.GetZ()) * Step, -ArenaHalfSize, ArenaHalfSize);
            Transform->Position = EngineCore::Vector3(X, Transform->Position.GetY(), Z);

            /* Face where it is going. */
            Transform->Rotation = EngineCore::Vector3(0.0, std::atan2(double(Direction.GetX()), double(Direction.GetZ())) * 57.29577951308232, 0.0);
        });
    }

    /* PrePhysics: every spinner turns. */
    void SpinSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* /*InUserData*/)
    {
        if(!NimbleEngine::HasAuthority()) { return; }

        InWorld.Each<NetArenaSpinnerComponent>([&](GameCore::Entity InSpinner, NetArenaSpinnerComponent& InSpin)
        {
            if(GameCore::TransformComponent* Transform = InSpinner.GetComponent<GameCore::TransformComponent>())
            {
                const double Yaw = std::fmod(double(Transform->Rotation.GetY()) + double(InSpin.DegreesPerSecond) * InDeltaSeconds, 360.0);
                Transform->Rotation.SetY(float(Yaw));
            }
        });
    }

    /* Server (or standalone): a capsule from the prefab, at InColor's spawn
     * point, owned by InOwner (NET_CLIENT_NONE: the server's own user). */
    GameCore::EntityID SpawnPawn(GameCore::World& InWorld, const GameCore::NetClientID InOwner, const int InColor,
                                 const char* InName)
    {
        const int Color = InColor % PawnColorCount;

        /* The authored spawn point for this colour; the middle without one. */
        GameCore::TransformPose At;
        InWorld.Each<NetArenaSpawnComponent>([&](GameCore::Entity InSpawn, NetArenaSpawnComponent& InPoint)
        {
            if(InPoint.ColorIndex != Color) { return; }
            if(const GameCore::TransformComponent* Transform = InSpawn.GetComponent<GameCore::TransformComponent>())
            {
                At.Position = Transform->Position;
                At.Rotation = Transform->Rotation;
            }
        });

        const GameCore::PrefabInstance Instance = InWorld.SpawnPrefab(PawnPrefab, At, InName);
        const GameCore::Entity Pawn = Instance.GetRoot();
        if(!Pawn.IsValid())
        {
            CORE_LOG_ERROR(std::string("[NetArena] could not spawn ") + PawnPrefab)
            return GameCore::EntityID();
        }

        /* Who it belongs to, and its colour: set before the end of the frame,
         * when the server first sends it, so clients get it this way. */
        if(GameCore::NetComponent* Net = Pawn.GetComponent<GameCore::NetComponent>()) { Net->OwnerClient = InOwner; }
        if(NetArenaPawnComponent* State = Pawn.GetComponent<NetArenaPawnComponent>()) { State->ColorIndex = uint8_t(Color); }
        if(RenderCore::RenderComponent* Render = Pawn.GetComponent<RenderCore::RenderComponent>())
        {
            const PawnColor& Tint = PawnColors[Color];
            Render->SetMaterialParameter(0, "BaseColor", Tint.R, Tint.G, Tint.B, 0.0f);
        }

        CORE_LOG(std::string("[NetArena] ") + PawnColors[Color].Name + " capsule for " + InName)
        return Pawn.GetID();
    }
}

// === Mode ===================================================================

class NetArenaGameMode : public NimbleEngine::IGameMode
{
public:
    explicit NetArenaGameMode(const GameCore::EntityID InController) : Controller(InController) {}

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;
        return true;
    }

    void OnStart() override
    {
        /* A server with a player of its own (a listen server), or no network
         * at all: this process's user gets the first capsule. A dedicated
         * server has no user; a client waits for the server's. */
        if(NimbleEngine::HasAuthority() && !NimbleEngine::IsDedicatedServer())
        {
            SpawnPawn(Session->GetWorld(), GameCore::NET_CLIENT_NONE, NextColor++, "Host");
        }

        CORE_LOG(std::string("[NetArena] started as ")
                 + (NimbleEngine::IsDedicatedServer() ? "a dedicated server"
                    : NimbleEngine::IsServer()        ? "a listen server"
                    : NimbleEngine::IsClient()        ? "a client"
                                                      : "standalone")
                 + " -- WASD moves your capsule")
    }

    /* Server: a capsule for the client, owned by it. The engine's player
     * entity for it (InPlayer, a PlayerState) stays as it is; everything the
     * client owns -- the capsule too -- goes when it leaves. */
    void OnClientConnected(const int32_t InClient, const GameCore::EntityID /*InPlayer*/) override
    {
        SpawnPawn(Session->GetWorld(), InClient, NextColor++, ("Pawn " + std::to_string(InClient)).c_str());
    }

    void OnClientDisconnected(const int32_t InClient, const GameCore::EntityID /*InPlayer*/) override
    {
        CORE_LOG("[NetArena] client " + std::to_string(InClient) + " left; its capsule goes with it")
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        GameCore::World& World = Session->GetWorld();
        ElapsedSeconds += InFrame.DeltaSeconds;
        SendInput(World);

        if(ElapsedSeconds - LastReport >= ReportInterval)
        {
            LastReport = ElapsedSeconds;
            Report(World);
        }
    }

    void OnUnload() override
    {
        Session = nullptr;
    }

private:
    /* This process's keys to the capsule it owns -- only when they change,
     * as the RPC is reliable and the server keeps the last one. */
    void SendInput(GameCore::World& InWorld)
    {
        const NimbleEngine::InputComponent* Input = InWorld.GetEntity(Controller).GetComponent<NimbleEngine::InputComponent>();
        if(!Input) { return; }
        EngineCore::Vector3 Wanted(double(Input->GetAxis("MoveRight")), 0.0, double(Input->GetAxis("MoveForward")));
        if(GNetArenaAutoMove.GetBool())
        {
            /* Right, forward, left, back; each process a side apart, so two
             * bots go different ways. */
            static const double Sides[4][2] = { { 1.0, 0.0 }, { 0.0, 1.0 }, { -1.0, 0.0 }, { 0.0, -1.0 } };
            const int Side = (int(ElapsedSeconds) + int(NimbleEngine::GetLocalClientId() + 1) * 2) & 3;
            Wanted = EngineCore::Vector3(Sides[Side][0], 0.0, Sides[Side][1]);
        }

        InWorld.Each<NetArenaPawnComponent>([&](GameCore::Entity InPawn, NetArenaPawnComponent&)
        {
            const GameCore::NetComponent* Net = InPawn.GetComponent<GameCore::NetComponent>();
            if(!Net || !NimbleEngine::IsLocallyOwned(*Net)) { return; }

            /* A capsule this process has not steered yet (a client's arrives
             * after it joins) is told once, whatever the keys are. */
            if(InPawn.GetID() == SteeredPawn && Wanted.GetX() == LastSent.GetX() && Wanted.GetZ() == LastSent.GetZ()) { return; }
            if(GAMECORE_RPC(NetArenaPawnComponent, ServerSetMove)(InPawn, Wanted))
            {
                SteeredPawn = InPawn.GetID();
                LastSent    = Wanted;
            }
        });
    }

    /* Where each capsule is, and the spinner's angle, as this process sees
     * them: the test's evidence. */
    void Report(GameCore::World& InWorld)
    {
        std::string Line = "[NetArena]";
        InWorld.Each<NetArenaPawnComponent>([&](GameCore::Entity InPawn, NetArenaPawnComponent& InState)
        {
            const GameCore::TransformComponent* Transform = InPawn.GetComponent<GameCore::TransformComponent>();
            const GameCore::NetComponent*       Net       = InPawn.GetComponent<GameCore::NetComponent>();
            if(!Transform) { return; }
            Line += std::string(" ") + PawnColors[InState.ColorIndex % PawnColorCount].Name
                  + (Net && NimbleEngine::IsLocallyOwned(*Net) ? "(you)" : "")
                  + "=(" + Fixed0(Transform->Position.GetX()) + "," + Fixed0(Transform->Position.GetZ()) + ")";
        });
        InWorld.Each<NetArenaSpinnerComponent>([&](GameCore::Entity InSpinner, NetArenaSpinnerComponent&)
        {
            if(const GameCore::TransformComponent* Transform = InSpinner.GetComponent<GameCore::TransformComponent>())
            {
                Line += " spinner=" + Fixed0(Transform->Rotation.GetY()) + "deg";
            }
        });
        CORE_LOG(Line)
    }

    NimbleEngine::GameSession* Session = nullptr;
    GameCore::EntityID         Controller;

    int                 NextColor = 0;
    GameCore::EntityID  SteeredPawn;
    EngineCore::Vector3 LastSent;
    double              ElapsedSeconds = 0.0;
    double              LastReport     = 0.0;
};

// === Level ==================================================================

/* Loads Content/Levels/NetArena.nmworld -- every process the same file --
 * and gives it behaviour: the server-only systems and the mode. */
class NetArenaLevel : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override      { return "NetArena"; }
    /* The editor plays this level while that world is open. */
    const char* GetWorldFile() const override { return LevelFile; }

    bool OnLoad(NimbleEngine::GameSession& /*InSession*/) override
    {
        GameCore::World& World = GetWorld();

        /* An Editor build reads the text world it edits; a Client or Server
         * build its cooked twin in Content/Levels/Cooked/ (cook it in the
         * editor's console: cook Content/Levels/NetArena.nmworld). */
        std::string Path = LevelFile;
#if !WITH_EDITOR
        const size_t Slash = Path.find_last_of('/');
        Path = Path.substr(0, Slash + 1) + "Cooked/" + Path.substr(Slash + 1);
#endif
        if(!GameCore::LoadWorldFile(Path.c_str(), World))
        {
            CORE_LOG_ERROR("[NetArena] could not load " + Path)
            return false;
        }

        /* This process's keys: the level's Controller (an Input component). */
        const GameCore::EntityID Controller = World.FindEntityByName("Controller");
        if(!Controller.IsSet())
        {
            CORE_LOG_WARN("[NetArena] the level has no entity named Controller; nobody can steer")
        }

        World.RegisterSystem("NetArena.MovePawns", GameCore::TP_PrePhysics, &MovePawnsSystem);
        World.RegisterSystem("NetArena.Spin",      GameCore::TP_PrePhysics, &SpinSystem);

        Mode.emplace(Controller);
        return true;
    }

    NimbleEngine::IGameMode* GetGameMode() override { return Mode ? &*Mode : nullptr; }

private:
    std::optional<NetArenaGameMode> Mode;
};

// === Module =================================================================

/* Owns the level, so it lives exactly as long as its code. */
class NetArenaGameModule : public NimbleEngine::IGameModule
{
public:
    const char* GetName() const override          { return "NetArenaGame"; }
    const char* GetVersionString() const override { return "0.1"; }

    bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
    {
        NimbleEngine::RegisterLevel(Level);
        return true;
    }

    void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
    {
        /* The registry holds a bare pointer, and this image is about to go. */
        NimbleEngine::UnregisterLevel(Level);
    }

    NimbleEngine::ILevel* GetStartLevel() override { return &Level; }

private:
    NetArenaLevel Level;
};

NIMBLE_IMPLEMENT_GAME_MODULE(NetArenaGameModule, "NetArenaGame")
