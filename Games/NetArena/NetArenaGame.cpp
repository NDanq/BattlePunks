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
#include <vector>

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
 *   - Each process reads its own keyboard and sends each frame's move, in
 *     the numbered Server RPC ServerMove. On the server the call simply runs;
 *     the server's Transform is the truth, replicated to everyone.
 *   - The server turns the Spinner (NetArena.Spin).
 *
 * Smooth, and answering at once (NetFacade.h, "CLIENT-SIDE PREDICTION"): a
 * client shows other capsules and the Spinner a moment behind, blended
 * between the server's updates. Its own capsule is predicted -- Pawn.nmprefab
 * marks its Net component Predicted -- so the client moves it the moment a
 * key is down (ApplyMove, the same code the server runs), and when the
 * server's state comes back replays the moves the server had not reached yet
 * (OnCorrection). The 5-second report logs the worst correction: 0 when
 * client and server agree.
 *
 * Run it as:
 *
 *   NetArenaServer -port=7777                  + two of: NetArena -connect=127.0.0.1:7777
 *   NetArena -listen                           + one of: NetArena -connect=127.0.0.1:7777
 *   the editor: open Content/Levels/NetArena.nmworld; Edit > Editor Settings >
 *               Play > Network mode = Listen Server; Play (F5); then
 *               Play > New Client Window (Shift+F6)
 *
 *   NetArenaPawnComponent     a capsule: its colour, and ServerMove.
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

    /* One frame of its player's input, numbered InSequence, on the server:
     * the same ApplyMove the player's own process has already run
     * (prediction), then acknowledged in NetComponent::InputAck so that
     * process knows which of its moves this state includes. Old or repeated
     * numbers are ignored; the input is clamped -- a client says where to go,
     * never how far. */
    void ServerMove(int32_t InSequence, const EngineCore::Vector3& InDirection, float InSeconds);
};

GAMECORE_DEFINE_COMPONENT(NetArenaPawnComponent, "NetArenaPawn")

/* Saved in the prefab, and what a client gets when the capsule appears there. */
GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetArenaPawnComponent, 1)
{
    Ar.Field("ColorIndex", C.ColorIndex);
}

GAMECORE_DEFINE_COMPONENT_RPCS(NetArenaPawnComponent)
{
    Rpc.Server<&NetArenaPawnComponent::ServerMove>("ServerMove");
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

// === Movement ================================================================

namespace
{
    /* The longest frame a move may claim, so a client cannot cover more
     * ground than the time it has had. */
    constexpr float MaxMoveSeconds = 0.1f;

    /*
     * One frame of movement, InDirection (X right, Z forward, each -1..1) for
     * InSeconds, kept on the plane. THE ONE MOVE: the server's RPC, the
     * owner's prediction and its replay after a correction all call this, so
     * they agree to the bit -- prediction only works if they do.
     */
    void ApplyMove(GameCore::TransformComponent& InOut, const EngineCore::Vector3& InDirection, const float InSeconds)
    {
        double X = std::clamp(double(InDirection.GetX()), -1.0, 1.0);
        double Z = std::clamp(double(InDirection.GetZ()), -1.0, 1.0);
        const double Length = std::sqrt(X * X + Z * Z);
        if(Length < 0.01) { return; }
        if(Length > 1.0) { X /= Length; Z /= Length; }   // diagonals no faster

        const double Step = PawnSpeed * double(std::clamp(InSeconds, 0.0f, MaxMoveSeconds));
        const double NewX = std::clamp(double(InOut.Position.GetX()) + X * Step, -ArenaHalfSize, ArenaHalfSize);
        const double NewZ = std::clamp(double(InOut.Position.GetZ()) + Z * Step, -ArenaHalfSize, ArenaHalfSize);
        InOut.Position = EngineCore::Vector3(NewX, InOut.Position.GetY(), NewZ);

        /* Face where it is going. */
        InOut.Rotation = EngineCore::Vector3(0.0, std::atan2(X, Z) * 57.29577951308232, 0.0);
    }
}

void NetArenaPawnComponent::ServerMove(const int32_t InSequence, const EngineCore::Vector3& InDirection, const float InSeconds)
{
    GameCore::NetComponent* Net = Owner.GetSibling<GameCore::NetComponent>();
    if(Net && InSequence <= Net->InputAck) { return; }   // already applied

    if(GameCore::TransformComponent* Transform = Owner.GetSibling<GameCore::TransformComponent>())
    {
        ApplyMove(*Transform, InDirection, InSeconds);
    }
    if(Net) { Net->InputAck = InSequence; }
}

// === Systems (server only) ===================================================

namespace
{
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
        NimbleEngine::SetNetCorrectionHandler(&OnCorrection, this);
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
        SendInput(World, float(InFrame.DeltaSeconds));

        if(ElapsedSeconds - LastReport >= ReportInterval)
        {
            LastReport = ElapsedSeconds;
            Report(World);
        }
    }

    void OnUnload() override
    {
        NimbleEngine::SetNetCorrectionHandler(nullptr, nullptr);
        Session = nullptr;
    }

private:
    /* One move this client has made that the server has not acknowledged yet. */
    struct PendingMove
    {
        int32_t             Sequence;
        EngineCore::Vector3 Direction;
        float               Seconds;
    };

    /*
     * This process's keys to the capsule it owns, every frame they move it: a
     * numbered ServerMove. A client whose capsule is predicted (the prefab's
     * Net component says so) moves it at once with the same ApplyMove and
     * keeps the move until the server acknowledges it (OnCorrection). On the
     * server, or standalone, the RPC simply runs here.
     */
    void SendInput(GameCore::World& InWorld, const float InSeconds)
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

        if(InSeconds <= 0.0f || (Wanted.GetX() == 0.0f && Wanted.GetZ() == 0.0f)) { return; }

        InWorld.Each<NetArenaPawnComponent>([&](GameCore::Entity InPawn, NetArenaPawnComponent&)
        {
            const GameCore::NetComponent* Net = InPawn.GetComponent<GameCore::NetComponent>();
            if(!Net || !NimbleEngine::IsLocallyOwned(*Net)) { return; }

            const int32_t Sequence = ++NextSequence;
            const float   Seconds  = std::min(InSeconds, MaxMoveSeconds);
            if(NimbleEngine::IsClient() && Net->bPredicted)
            {
                if(GameCore::TransformComponent* Transform = InPawn.GetComponent<GameCore::TransformComponent>())
                {
                    ApplyMove(*Transform, Wanted, Seconds);
                    Pending.push_back({ Sequence, Wanted, Seconds });
                    if(Pending.size() > 256) { Pending.erase(Pending.begin()); }   // the server has stopped answering
                    LastShown  = Transform->Position;
                    bHaveShown = true;
                }
            }
            GAMECORE_RPC(NetArenaPawnComponent, ServerMove)(InPawn, Sequence, Wanted, Seconds);
        });
    }

    /*
     * The server's state for this client's capsule arrived: its Transform is
     * now the server's, as of move InInputAck. Forget the moves up to it and
     * replay the rest -- the same code from the same place, so the capsule
     * ends where it was shown unless the server disagreed. How far it jumped
     * is the correction, reported.
     */
    static void OnCorrection(GameCore::World& InWorld, const GameCore::EntityID InEntity, const int32_t InInputAck,
                             void* InUserData)
    {
        NetArenaGameMode& Self = *static_cast<NetArenaGameMode*>(InUserData);
        GameCore::TransformComponent* Transform = InWorld.GetEntity(InEntity).GetComponent<GameCore::TransformComponent>();
        if(!Transform) { return; }

        Self.Pending.erase(std::remove_if(Self.Pending.begin(), Self.Pending.end(),
                                          [&](const PendingMove& InMove) { return InMove.Sequence <= InInputAck; }),
                           Self.Pending.end());
        for(const PendingMove& Move : Self.Pending)
        {
            ApplyMove(*Transform, Move.Direction, Move.Seconds);
        }

        if(Self.bHaveShown)
        {
            const double DX = double(Transform->Position.GetX()) - double(Self.LastShown.GetX());
            const double DZ = double(Transform->Position.GetZ()) - double(Self.LastShown.GetZ());
            Self.MaxCorrection = std::max(Self.MaxCorrection, std::sqrt(DX * DX + DZ * DZ));
        }
        Self.LastShown  = Transform->Position;
        Self.bHaveShown = true;
        ++Self.Corrections;
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
        if(Corrections > 0)
        {
            /* Prediction's evidence: how many server states replayed, and the
             * furthest one moved the capsule from where it was shown. */
            Line += " predicted: " + std::to_string(Corrections) + " states, " + std::to_string(Pending.size())
                  + " pending, worst correction " + Fixed0(MaxCorrection) + "u";
            Corrections   = 0;
            MaxCorrection = 0.0;
        }
        CORE_LOG(Line)
    }

    NimbleEngine::GameSession* Session = nullptr;
    GameCore::EntityID         Controller;

    int                      NextColor    = 0;
    int32_t                  NextSequence = 0;
    std::vector<PendingMove> Pending;
    EngineCore::Vector3      LastShown;
    bool                     bHaveShown    = false;
    int                      Corrections   = 0;
    double                   MaxCorrection = 0.0;
    double                   ElapsedSeconds = 0.0;
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

        /* By its source path, in every build: the Editor reads the text it
         * edits, a Debug or RelWithDebInfo Client or Server cooks it the first
         * time, Shipping reads the copy cooked ahead (WorldPackage.h). */
        if(!GameCore::LoadContentWorld(LevelFile, World))
        {
            CORE_LOG_ERROR(std::string("[NetArena] could not load ") + LevelFile)
            return false;
        }

        /* This process's keys: the level's Controller (an Input component). */
        const GameCore::EntityID Controller = World.FindEntityByName("Controller");
        if(!Controller.IsSet())
        {
            CORE_LOG_WARN("[NetArena] the level has no entity named Controller; nobody can steer")
        }

        World.RegisterSystem("NetArena.Spin", GameCore::TP_PrePhysics, &SpinSystem);

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
