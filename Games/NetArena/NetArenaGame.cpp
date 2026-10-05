#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
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
 * Server-authoritative, as NetCore is (Engine/Modules/NetCore/README.md):
 *
 *   - The server owns the capsules. A client that joins gets one on the
 *     player entity the engine made for it (OnClientConnected); a listen
 *     server or a standalone game gives its own user one (OnStart).
 *   - Each process reads its own keyboard and sends where its capsule should
 *     go -- the Server RPC ServerSetMove, only when that changes. On the
 *     server the call simply runs.
 *   - The server moves every capsule (NetArena.MovePawns) and Transform
 *     replication shows it to everyone.
 *
 * Run it as:
 *
 *   NetArenaServer -port=7777                  + two of: NetArena -connect=127.0.0.1:7777
 *   NetArena -listen                           + one of: NetArena -connect=127.0.0.1:7777
 *   the editor: Edit > Editor Settings > Play > Network mode = Listen Server,
 *               Play (F5), then Play > New Client Window (Shift+F6)
 *
 *   NetArenaPawnComponent  a capsule: its colour and the move it was asked for.
 *   NetArenaLevel          the plane, the lights, the camera, the local
 *                          controller; owns the mode.
 *   NetArenaGameMode       sends this process's input, makes capsules.
 */

namespace
{
    const char* const CapsuleMesh   = "Engine/Content/Meshes/Capsule.nmmesh";
    const char* const PlaneMesh     = "Engine/Content/Meshes/Plane.nmmesh";
    const char* const PawnMaterial  = "Content/Materials/PawnColor.material.nmgraph";

    /* The arena: a square of this half-size around the origin, on y = 0. */
    constexpr double ArenaHalfSize  = 900.0;
    /* World units a second. */
    constexpr double PawnSpeed      = 450.0;
    /* The capsule is 1 wide and 2 tall (generate_primitive_meshes.py). */
    constexpr double PawnScale      = 70.0;
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

/* The local player's keys. Each process reads its own; the capsule they
 * steer is whichever one this process owns. */
NIMBLE_DEFINE_INPUT_MAP(Default)
{
    Map.Axis("MoveForward", "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Axis("MoveRight",   "D, Right, -A, -Left, Gamepad.LeftX");
}

// === The capsule ============================================================

/* "NetArenaPawn": a player's capsule. Its place is its Transform, which the
 * server moves and replicates. */
struct NetArenaPawnComponent
{
    GAMECORE_COMPONENT_BODY()

    /* Which of PawnColors; the RenderComponent already carries it. */
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

/* What a client gets when the capsule appears there. */
GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetArenaPawnComponent, 1)
{
    Ar.Field("ColorIndex", C.ColorIndex);
}

GAMECORE_DEFINE_COMPONENT_RPCS(NetArenaPawnComponent)
{
    Rpc.Server<&NetArenaPawnComponent::ServerSetMove>("ServerSetMove");
}

namespace
{
    /* InEntity becomes a capsule in colour InColor at InPosition: the server's
     * (or a standalone game's) to call. It must already have its NetComponent. */
    void MakePawn(GameCore::World& InWorld, const GameCore::EntityID InEntity, const int InColor)
    {
        const GameCore::Entity Pawn = InWorld.GetEntity(InEntity);
        const PawnColor& Color = PawnColors[InColor % PawnColorCount];

        /* Side by side across the middle, in the order they joined. */
        GameCore::TransformComponent Transform;
        Transform.Position = EngineCore::Vector3(-300.0 + 200.0 * (InColor % 4), PawnScale, 0.0);
        Transform.Scale    = EngineCore::Vector3(PawnScale, PawnScale, PawnScale);
        Pawn.AddComponent(Transform);

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", CapsuleMesh);
        Render.SetMaterial(0, PawnMaterial);
        Render.SetMaterialParameter(0, "BaseColor", Color.R, Color.G, Color.B, 0.0f);
        Pawn.AddComponent(Render);

        NetArenaPawnComponent State;
        State.ColorIndex = uint8_t(InColor % PawnColorCount);
        Pawn.AddComponent(State);

        CORE_LOG(std::string("[NetArena] ") + Color.Name + " capsule for " + Pawn.GetName())
    }

    /* PrePhysics: the server moves every capsule by what its player asked,
     * and keeps it on the plane. Clients see the result by replication. */
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
            Transform->Position = EngineCore::Vector3(X, PawnScale, Z);

            /* Face where it is going. */
            Transform->Rotation = EngineCore::Vector3(0.0, std::atan2(double(Direction.GetX()), double(Direction.GetZ())) * 57.29577951308232, 0.0);
        });
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
            GameCore::World& World = Session->GetWorld();
            const GameCore::Entity Host = World.CreateEntity("Host");
            Host.AddComponent<GameCore::NetComponent>();   // owned by no client: the server's own
            MakePawn(World, Host.GetID(), NextColor++);
        }

        CORE_LOG(std::string("[NetArena] started as ")
                 + (NimbleEngine::IsDedicatedServer() ? "a dedicated server"
                    : NimbleEngine::IsServer()        ? "a listen server"
                    : NimbleEngine::IsClient()        ? "a client"
                                                      : "standalone")
                 + " -- WASD moves your capsule")
    }

    /* Server: the engine made InPlayer for the client; it becomes a capsule. */
    void OnClientConnected(const int32_t InClient, const GameCore::EntityID InPlayer) override
    {
        if(!InPlayer.IsSet())
        {
            CORE_LOG_WARN("[NetArena] client " + std::to_string(InClient) + " has no player entity ([Net] SpawnPlayerPerClient is off)")
            return;
        }
        MakePawn(Session->GetWorld(), InPlayer, NextColor++);
    }

    void OnClientDisconnected(const int32_t InClient, const GameCore::EntityID /*InPlayer*/) override
    {
        CORE_LOG("[NetArena] client " + std::to_string(InClient) + " left; its capsule goes with it")
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        GameCore::World& World = Session->GetWorld();
        SendInput(World);

        ElapsedSeconds += InFrame.DeltaSeconds;
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

    /* Where each capsule is, as this process sees it: the test's evidence. */
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
        CORE_LOG(Line == "[NetArena]" ? std::string("[NetArena] no capsules yet") : Line)
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

/* What every process builds alike -- none of it networked: the plane, the
 * light, the camera over the arena and the local controller. */
class NetArenaLevel : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override { return "NetArena"; }

    bool OnLoad(NimbleEngine::GameSession& /*InSession*/) override
    {
        GameCore::World& World = GetWorld();

        /* The plane: a unit square facing up, scaled to the arena and a margin. */
        {
            const GameCore::Entity Floor = World.CreateEntity("Floor");
            GameCore::TransformComponent Transform;
            const double Size = 2.0 * ArenaHalfSize + 2.0 * PawnScale;
            Transform.Scale = EngineCore::Vector3(Size, 1.0, Size);
            Floor.AddComponent(Transform);
            RenderCore::RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", PlaneMesh);
            Render.bCastShadows = 0;
            Floor.AddComponent(Render);
        }

        /* A sun at an angle, for shadows that show which way is down, and a sky fill. */
        {
            const GameCore::Entity Sun = World.CreateEntity("Sun");
            GameCore::TransformComponent Transform;
            Transform.Rotation = EngineCore::Vector3(55.0, 30.0, 0.0);
            Sun.AddComponent(Transform);
            RenderCore::DirectionalLightComponent Light;
            Light.Intensity    = 2.5f;
            Light.bCastShadows = 1;
            Sun.AddComponent(Light);

            const GameCore::Entity Sky = World.CreateEntity("Sky");
            Sky.AddComponent<GameCore::TransformComponent>();
            RenderCore::SkyLightComponent Fill;
            Fill.Intensity = 0.6f;
            Sky.AddComponent(Fill);
        }

        /* The whole arena from above and behind: positive pitch looks down. */
        {
            const GameCore::Entity Camera = World.CreateEntity("Camera");
            GameCore::TransformComponent Transform;
            Transform.Position = EngineCore::Vector3(0.0, 1250.0, -1350.0);
            Transform.Rotation = EngineCore::Vector3(45.0, 0.0, 0.0);
            Camera.AddComponent(Transform);
            RenderCore::CameraComponent View;
            View.Projection = RenderCore::CP_Perspective;
            View.FovY       = 60.0f;
            View.ZNear      = 10.0f;
            View.ZFar       = 20000.0f;
            View.bActive    = 1;
            Camera.AddComponent(View);
        }

        /* This process's keyboard: read by the mode, never networked. */
        const GameCore::Entity Controller = World.CreateEntity("Controller");
        Controller.AddComponent<NimbleEngine::InputComponent>();

        World.RegisterSystem("NetArena.MovePawns", GameCore::TP_PrePhysics, &MovePawnsSystem);

        Mode.emplace(Controller.GetID());
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
