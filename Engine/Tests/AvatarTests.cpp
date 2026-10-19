/*
 * NimbleEngineAvatarTests -- avatars (Avatar.h, AVATARS.md): possession and
 * its rules, the harness reading its player's devices, the server's half of
 * the input stream (ServerInput, run here directly as NetSession would), a
 * simulation stepping, and a GameSession's AutoPossess, view target and
 * NetGameMode. Standalone: this process has authority, as a server does.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Renderer.h"

using namespace NimbleEngine;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

NIMBLE_DEFINE_INPUT_MAP(AvatarTest)
{
    Map.Action("Jump",  "Space, Gamepad.South");
    Map.Action("Fire",  "Mouse.Left");
    Map.Axis  ("MoveX", "D, -A, Gamepad.LeftX");
    Map.Axis  ("LookX", "Mouse.DeltaX");
}

/* Moves the avatar along X by MoveX per second: something to predict. */
NIMBLE_DEFINE_AVATAR_SIMULATION(AvatarTestSlide, 0.1f)
{
    static const int MoveX = FindInputAxis("AvatarTest", "MoveX");
    GameCore::TransformComponent* Transform = InWorld.GetEntity(InAvatar).GetComponent<GameCore::TransformComponent>();
    if(!Transform || MoveX < 0) { return; }
    Transform->Position.SetX(Transform->Position.GetX() + double(InFrame.Axes[MoveX]) * double(InFrame.Seconds));
}

/* What NetGameMode spawns here: a harness, a place, and a camera child. */
static void BuildTestPawn(GameCore::PrefabBuilder& B)
{
    GameCore::PrefabBuilder::Node Root = B.Root("TestPawn");
    Root.Add<GameCore::TransformComponent>();
    Root.Add<GameCore::NetComponent>();
    AvatarHarnessComponent Harness;
    std::snprintf(Harness.InputMap, sizeof(Harness.InputMap), "%s", "AvatarTest");
    Root.Add(Harness);
    GameCore::PrefabBuilder::Node Eye = Root.Child("Eye");
    Eye.Add<GameCore::TransformComponent>();
    RenderCore::CameraComponent Camera;
    Camera.bActive = 0;   // a view target wins regardless; inactive, it never wins as "first active"
    Eye.Add(Camera);
}
GAMECORE_DEFINE_NATIVE_PREFAB(AvatarTestPawn, "AvatarTest.Pawn", &BuildTestPawn)

namespace
{
    using GameCore::Entity;
    using GameCore::EntityID;

    struct EventLog
    {
        std::vector<std::string> Lines;

        static void OnPossessed(GameCore::EventID, const GameCore::EventArgs& InArgs, void* InUserData)
        {
            static_cast<EventLog*>(InUserData)->Lines.push_back("possessed " + std::to_string(InArgs.Count));
        }
        static void OnUnpossessed(GameCore::EventID, const GameCore::EventArgs& InArgs, void* InUserData)
        {
            static_cast<EventLog*>(InUserData)->Lines.push_back("unpossessed " + std::to_string(InArgs.Count));
        }
    };

    Entity MakePlayer(GameCore::World& InWorld, const char* InName, const GameCore::NetClientID InClient, const uint8_t InLocal = 0)
    {
        Entity Player = InWorld.CreateEntity(InName);
        GameCore::NetComponent Net;
        Net.OwnerClient = InClient;
        Player.AddComponent(Net);
        PossessorComponent Possessor;
        Possessor.LocalPlayer = InLocal;
        Player.AddComponent(Possessor);
        Player.AddComponent<PlayerStateComponent>();
        return Player;
    }

    Entity MakeAvatar(GameCore::World& InWorld, const char* InName, const GameCore::NetClientID InOwner = GameCore::NET_CLIENT_NONE,
                      const char* InSimulation = "")
    {
        Entity Avatar = InWorld.CreateEntity(InName);
        Avatar.AddComponent<GameCore::TransformComponent>();
        GameCore::NetComponent Net;
        Net.OwnerClient = InOwner;
        Avatar.AddComponent(Net);
        AvatarHarnessComponent Harness;
        std::snprintf(Harness.InputMap, sizeof(Harness.InputMap), "%s", "AvatarTest");
        std::snprintf(Harness.Simulation, sizeof(Harness.Simulation), "%s", InSimulation);
        Avatar.AddComponent(Harness);
        return Avatar;
    }

    AvatarHarnessComponent* HarnessOf(const Entity& InAvatar) { return InAvatar.GetComponent<AvatarHarnessComponent>(); }

    void TestPossession()
    {
        std::printf("[possession]\n");
        GameCore::World World;
        AvatarWorld Avatars;
        Avatars.Attach(World);
        EventLog Log;
        World.GetEvents().Subscribe("Possessed", &EventLog::OnPossessed, &Log);
        World.GetEvents().Subscribe("Unpossessed", &EventLog::OnUnpossessed, &Log);

        Entity Alice = MakePlayer(World, "Alice", GameCore::NET_CLIENT_NONE);
        Entity Bob   = MakePlayer(World, "Bob", 4);
        Entity Pawn  = MakeAvatar(World, "Pawn");
        Entity Car   = MakeAvatar(World, "Car");

        Expect(Possess(World, Alice.GetID(), Pawn.GetID()), "a player possesses an avatar");
        Expect(GetAvatar(World, Alice.GetID()) == Pawn.GetID() && GetPossessor(World, Pawn.GetID()) == Alice.GetID(),
               "both ends know the link");
        Expect(Log.Lines.size() == 1 && Log.Lines[0] == "possessed 2", "Possessed(Player, Avatar) is emitted");
        Expect(Possess(World, Alice.GetID(), Pawn.GetID()) && Log.Lines.size() == 1, "possessing it again changes nothing");

        Expect(!Possess(World, Bob.GetID(), Pawn.GetID()), "a driven avatar is not taken without POSSESS_Steal");
        Expect(Possess(World, Bob.GetID(), Pawn.GetID(), POSSESS_Steal), "with it, it is");
        Expect(!GetAvatar(World, Alice.GetID()).IsSet() && GetPossessor(World, Pawn.GetID()) == Bob.GetID(),
               "the first player let go");
        Expect(Pawn.GetComponent<GameCore::NetComponent>()->OwnerClient == 4, "the avatar is owned by its player's client");

        Expect(Possess(World, Bob.GetID(), Car.GetID()), "a player changes avatars");
        Expect(!GetPossessor(World, Pawn.GetID()).IsSet() && Pawn.GetComponent<GameCore::NetComponent>()->OwnerClient == GameCore::NET_CLIENT_NONE,
               "the old one goes back to its owner undriven");

        Expect(Unpossess(World, Bob.GetID()) && !GetAvatar(World, Bob.GetID()).IsSet(), "Unpossess lets go");
        Expect(!Unpossess(World, Bob.GetID()), "and is false with nothing driven");

        /* Destroying either end lets go. */
        Possess(World, Alice.GetID(), Car.GetID());
        World.DestroyEntity(Car.GetID());
        Expect(!GetAvatar(World, Alice.GetID()).IsSet(), "a destroyed avatar is let go");
        Expect(!Possess(World, Alice.GetID(), Car.GetID()), "a stale id is refused");
        Entity Reused = MakeAvatar(World, "Reused");
        Expect(Reused.GetID() != Car.GetID() && !Possess(World, Alice.GetID(), Car.GetID()),
               "even once its index is used again");

        Possess(World, Bob.GetID(), Pawn.GetID());
        World.DestroyEntity(Bob.GetID());
        Expect(!GetPossessor(World, Pawn.GetID()).IsSet() && Pawn.GetComponent<GameCore::NetComponent>()->OwnerClient == GameCore::NET_CLIENT_NONE,
               "a destroyed player lets go, and its avatar goes back to the server");

        Expect(!Possess(World, Alice.GetID(), World.CreateEntity("Rock").GetID()), "something without a harness is refused");
        Expect(Possess(World, World.CreateEntity("Newcomer").GetID(), Pawn.GetID()), "a player without a Possessor gets one");
    }

    void TestClientRelease()
    {
        std::printf("[a client leaves]\n");
        GameCore::World World;
        AvatarWorld Avatars;
        Avatars.Attach(World);
        Entity Player  = MakePlayer(World, "Client 7", 7);
        Entity Vehicle = MakeAvatar(World, "Vehicle");        // the level's: the server's undriven
        Entity Own     = MakeAvatar(World, "Own Pawn", 7);    // spawned for the client: its own

        Possess(World, Player.GetID(), Vehicle.GetID());
        Expect(Vehicle.GetComponent<GameCore::NetComponent>()->OwnerClient == 7, "a level vehicle is lent to its driver's client");
        ReleaseClientAvatars(World, 7);
        Expect(Vehicle.GetComponent<GameCore::NetComponent>()->OwnerClient == GameCore::NET_CLIENT_NONE,
               "and given back when the client leaves, so it is not destroyed with its things");

        Possess(World, Player.GetID(), Own.GetID());
        ReleaseClientAvatars(World, 7);
        Expect(Own.GetComponent<GameCore::NetComponent>()->OwnerClient == 7, "the client's own pawn stays its, to go with it");
    }

    void TestLocalInput()
    {
        std::printf("[local input]\n");
        GameCore::World World;
        AvatarWorld Avatars;
        Avatars.Attach(World);
        InputState Devices;
        Avatars.SetInput(&Devices);

        Entity One = MakePlayer(World, "One", GameCore::NET_CLIENT_NONE, 0);
        Entity Two = MakePlayer(World, "Two", GameCore::NET_CLIENT_NONE, 1);
        Entity A   = MakeAvatar(World, "A");
        Entity B   = MakeAvatar(World, "B");
        Entity Idle = MakeAvatar(World, "Idle");

        const InputKey Space = FindInputKey("Space"), South = FindInputKey("Gamepad.South"), D = FindInputKey("D");

        Devices.BeginFrame(1);
        Devices.Press(Space);
        Avatars.Update(0.016f);
        Expect(!HarnessOf(A)->IsDown("Jump"), "an avatar nobody drives reads nothing");

        Possess(World, One.GetID(), A.GetID());
        Possess(World, Two.GetID(), B.GetID());
        Devices.BeginFrame(2);
        Devices.Press(D);
        Devices.Press(South, 1);
        Avatars.Update(0.016f);
        Expect(HarnessOf(A)->IsDown("Jump") && HarnessOf(A)->GetAxis("MoveX") == 1.0f, "local player 1 reads the keyboard");
        Expect(HarnessOf(B)->WasPressed("Jump"), "local player 2 reads its own pad");
        Expect(HarnessOf(A)->IsLocallyControlled() && HarnessOf(B)->IsLocallyControlled(), "both are controlled here");
        Expect(!HarnessOf(Idle)->IsDown("Jump"), "the undriven one still reads nothing");
        Expect(GetLocalAvatar(World, 0) == A.GetID() && GetLocalAvatar(World, 1) == B.GetID(), "GetLocalAvatar by local player");

        Devices.BeginFrame(3);
        Devices.Release(Space);
        Avatars.Update(0.016f);
        Expect(HarnessOf(A)->WasReleased("Jump") && !HarnessOf(A)->WasPressed("Jump"), "edges are the action's");

        Devices.BeginFrame(4);
        Devices.Press(Space);
        Devices.Release(Space);
        Avatars.Update(0.016f);
        Expect(HarnessOf(A)->WasPressed("Jump") && HarnessOf(A)->WasReleased("Jump") && !HarnessOf(A)->IsDown("Jump"),
               "a tap is a press and a release");

        HarnessOf(A)->bEnabled = 0;
        Devices.BeginFrame(5);
        Avatars.Update(0.016f);
        Expect(HarnessOf(A)->GetAxis("MoveX") == 0.0f, "a disabled harness reads nothing");
        HarnessOf(A)->bEnabled = 1;

        /* A filter rewrites a frame: a bot, a menu blocking the game. */
        Avatars.SetInputFilter([](GameCore::World&, GameCore::EntityID, InputSnapshot& InOut, void*)
        {
            InOut.Axes[0] = -0.5f;
        }, nullptr);
        Devices.BeginFrame(6);
        Avatars.Update(0.016f);
        Expect(HarnessOf(A)->GetAxis("MoveX") == -0.5f, "an input filter sees and changes the frame");
        Avatars.SetInputFilter(nullptr, nullptr);
    }

    void TestServerInput()
    {
        std::printf("[server input]\n");
        GameCore::World World;
        AvatarWorld Avatars;
        Avatars.Attach(World);
        Entity Player = MakePlayer(World, "Remote", 3);
        Entity Avatar = MakeAvatar(World, "Remote Pawn", 3, "AvatarTestSlide");
        Possess(World, Player.GetID(), Avatar.GetID());
        Expect(!HarnessOf(Avatar)->IsLocallyControlled(), "a remote client's avatar is not controlled here");

        const int Jump = FindInputAction("AvatarTest", "Jump");
        const int MoveX = FindInputAxis("AvatarTest", "MoveX");
        const int LookX = FindInputAxis("AvatarTest", "LookX");
        const uint32_t JumpBit = 1u << Jump;
        GameCore::RpcContext Context;
        Context.OwningWorld = &World;
        Context.Entity      = Avatar.GetID();
        Context.bRemote     = true;
        const auto Send = [&](const int32_t InSeq, const uint32_t InDown, const uint32_t InPressed, const uint32_t InReleased,
                              const float InMove, const float InLook)
        {
            GameCore::RpcFloats<INPUT_AXES_MAX> Axes;
            Axes.Count = uint8_t(INPUT_AXES_MAX);
            Axes.Values[MoveX] = InMove;
            Axes.Values[LookX] = InLook;
            HarnessOf(Avatar)->ServerInput(Context, InSeq, 0.05f, InDown, InPressed, InReleased, Axes);
        };

        /* Three frames before the server ticks: a tap in the middle. */
        Send(1, 0, 0, 0, 1.0f, 3.0f);
        Send(2, 0, JumpBit, JumpBit, 1.0f, 4.0f);
        Send(3, 0, 0, 0, 1.0f, 0.0f);
        Avatars.Update(0.033f);
        Expect(HarnessOf(Avatar)->WasPressed("Jump") && HarnessOf(Avatar)->WasReleased("Jump"),
               "a press between ticks is not lost");
        Expect(HarnessOf(Avatar)->GetAxis("MoveX") == 1.0f, "a stick reads its latest position");
        Expect(HarnessOf(Avatar)->GetAxis("LookX") == 7.0f, "mouse motion adds up");
        Expect(Avatar.GetComponent<GameCore::NetComponent>()->InputAck == 3, "the newest frame is acknowledged");
        Expect(std::fabs(Avatar.GetComponent<GameCore::TransformComponent>()->Position.GetX() - 0.15) < 1e-5,
               "the simulation stepped once per frame received");

        Avatars.Update(0.033f);
        Expect(!HarnessOf(Avatar)->WasPressed("Jump") && HarnessOf(Avatar)->GetAxis("LookX") == 0.0f,
               "the next tick without input: no edges, no motion");
        Expect(HarnessOf(Avatar)->GetAxis("MoveX") == 1.0f, "but the stick is still held");

        Send(2, JumpBit, JumpBit, 0, 0.0f, 0.0f);
        Avatars.Update(0.033f);
        Expect(!HarnessOf(Avatar)->IsDown("Jump") && Avatar.GetComponent<GameCore::NetComponent>()->InputAck == 3,
               "an old or repeated frame is dropped");

        Send(4, JumpBit, JumpBit, 0, 1.0f, 0.0f);
        Avatars.Update(0.033f);
        Expect(HarnessOf(Avatar)->IsDown("Jump"), "a held key stays down");
        Avatars.Update(1.0f);
        Expect(!HarnessOf(Avatar)->IsDown("Jump") && HarnessOf(Avatar)->GetAxis("MoveX") == 0.0f,
               "a client gone quiet past [Avatar] InputTimeoutMs reads nothing");

        Unpossess(World, Player.GetID());
        const double Was = Avatar.GetComponent<GameCore::TransformComponent>()->Position.GetX();
        Send(5, 0, 0, 0, 1.0f, 0.0f);
        Expect(Avatar.GetComponent<GameCore::TransformComponent>()->Position.GetX() == Was,
               "an avatar nobody drives takes no input");
    }

    void TestLocalSimulation()
    {
        std::printf("[local simulation]\n");
        GameCore::World World;
        AvatarWorld Avatars;
        Avatars.Attach(World);
        InputState Devices;
        Avatars.SetInput(&Devices);
        Entity Player = MakePlayer(World, "Host", GameCore::NET_CLIENT_NONE);
        Entity Avatar = MakeAvatar(World, "Host Pawn", GameCore::NET_CLIENT_NONE, "AvatarTestSlide");
        Possess(World, Player.GetID(), Avatar.GetID());

        Devices.BeginFrame(1);
        Devices.Press(FindInputKey("D"));
        Avatars.Update(0.05f);
        Avatars.Update(0.5f);   // a long frame: clamped to the simulation's 0.1
        Expect(std::fabs(Avatar.GetComponent<GameCore::TransformComponent>()->Position.GetX() - 0.15) < 1e-5,
               "the server's own avatar steps each frame, clamped to the simulation's step");
        Expect(!Avatars.HandleCorrection(Player.GetID(), 1), "a correction for something not simulated is not handled");
    }

    // === Through a GameSession ==================================================

    class TestHost : public IGameHost
    {
    public:
        void     SetFrameRateLimit(uint32_t) override {}
        uint32_t GetFrameRateLimit() const override { return 0; }
        const InputState& GetInput() const override { return Input; }
        InputState Input;
    };

    class PlacedLevel : public ILevel
    {
    public:
        const char* GetName() const override { return "AvatarPlaced"; }
        bool OnLoad(GameSession&) override
        {
            Entity Pawn = MakeAvatar(GetWorld(), "Placed");
            HarnessOf(Pawn)->AutoPossess = 1;
            Entity Eye = GetWorld().CreateEntity("Placed Eye");
            Eye.AddComponent<GameCore::TransformComponent>();
            Eye.AddComponent<RenderCore::CameraComponent>();
            Eye.SetParent(Pawn.GetID(), false);
            Entity Other = GetWorld().CreateEntity("Level Camera");
            Other.AddComponent<RenderCore::CameraComponent>();
            Placed = Pawn.GetID();
            PlacedEye = Eye.GetID();
            return true;
        }
        EntityID Placed, PlacedEye;
    };

    void TestAutoPossess()
    {
        std::printf("[auto possess]\n");
        TestHost Host;
        GameCore::ScriptEngine Scripts;
        RenderCore::Renderer Rendering;
        Scripts.Initialize();
        auto* Session = new GameSession(Host, Scripts, Rendering);
        PlacedLevel Level;
        Expect(Session->LoadLevel(Level), "the level loads");
        GameCore::World& World = Session->GetWorld();

        const EntityID Player = FindLocalPlayer(World, 0);
        Expect(Player.IsSet() && GetAvatar(World, Player) == Level.Placed, "a placed AutoPossess avatar is local player 1's");
        Expect(World.GetEntity(Player).HasComponent<PlayerStateComponent>(), "a player was made for it");

        Host.Input.BeginFrame(1);
        Host.Input.Press(FindInputKey("Space"));
        FrameContext Frame;
        Frame.DeltaSeconds = 0.016;
        Session->Tick(Frame);
        Expect(HarnessOf(World.GetEntity(Level.Placed))->IsDown("Jump"), "it reads the session's input");
        Expect(Session->GetViewTarget() == Level.PlacedEye, "its camera is the view");

        const EntityID Elsewhere = World.FindEntityByName("Level Camera");
        Session->SetViewTarget(Elsewhere);
        Expect(Session->GetViewTarget() == Elsewhere, "SetViewTarget overrides it");
        Session->SetViewTarget(EntityID());
        Unpossess(World, Player);
        Session->Tick(Frame);
        Expect(!Session->GetViewTarget().IsSet(), "let go, the level's camera is used again");

        Session->UnloadLevel();
        delete Session;
    }

    /* SPLITSCREEN: two local players, each possessing an avatar with its own camera. */
    class TwoPlayerLevel : public ILevel
    {
    public:
        const char* GetName() const override { return "AvatarTwoPlayers"; }
        bool OnLoad(GameSession&) override
        {
            for(uint8_t Local = 0; Local < 2; ++Local)
            {
                Entity Pawn = MakeAvatar(GetWorld(), Local == 0 ? "First" : "Second");
                Entity Eye = GetWorld().CreateEntity(Local == 0 ? "First Eye" : "Second Eye");
                Eye.AddComponent<GameCore::TransformComponent>();
                Eye.AddComponent<RenderCore::CameraComponent>()->bActive = false;
                Eye.SetParent(Pawn.GetID(), false);
                Possess(GetWorld(), CreateLocalPlayer(GetWorld(), Local), Pawn.GetID());
                Eyes[Local] = Eye.GetID();
            }
            return true;
        }
        EntityID Eyes[2];
    };

    void TestSplitscreenViews()
    {
        std::printf("[split-screen views]\n");
        TestHost Host;
        GameCore::ScriptEngine Scripts;
        RenderCore::Renderer Rendering;
        Scripts.Initialize();
        auto* Session = new GameSession(Host, Scripts, Rendering);
        TwoPlayerLevel Level;
        Expect(Session->LoadLevel(Level), "the level loads");
        GameCore::World& World = Session->GetWorld();

        FrameContext Frame;
        Frame.DeltaSeconds = 0.016;
        Session->Tick(Frame);
        Expect(Session->GetViewTarget(0) == Level.Eyes[0] && Session->GetViewTarget(1) == Level.Eyes[1],
               "each local player views through its own avatar's camera");
        Expect(Session->GetViewCount() == 2, "two local players with cameras: two views");

        Session->SetViewCount(1);
        Expect(Session->GetViewCount() == 1, "SetViewCount fixes the count");
        Session->SetViewCount(0);
        Expect(Session->GetViewCount() == 2, "0 counts the players' cameras again");

        const EntityID Elsewhere = World.CreateEntity("Death Cam").GetID();
        World.GetEntity(Elsewhere).AddComponent<RenderCore::CameraComponent>();
        Session->SetViewTarget(Elsewhere, 1);
        Expect(Session->GetViewTarget(1) == Elsewhere && Session->GetViewTarget(0) == Level.Eyes[0],
               "SetViewTarget(camera, player) is that player's alone");
        Session->SetViewTarget(EntityID(), 1);
        Expect(Session->GetViewTarget(1) == Level.Eyes[1], "unset, it goes back to the avatar's camera");
        Expect(!Session->GetViewTarget(3).IsSet() && !Session->GetViewTarget(9).IsSet(), "a player with no avatar, or none at all, has no target");

        Unpossess(World, FindLocalPlayer(World, 1));
        Session->Tick(Frame);
        Expect(Session->GetViewCount() == 1, "player 2 let go of its avatar: one view");

        Session->UnloadLevel();
        delete Session;
    }

    class SpawnMode : public NetGameMode
    {
    public:
        void OnTick(const FrameContext&) override {}
        void OnAvatarPossessed(EntityID, EntityID) override { ++Possessed; }
        void OnAvatarUnpossessed(EntityID, EntityID) override { ++Unpossessed; }

        int Possessed = 0, Unpossessed = 0;
        std::vector<uint32_t> Ordinals;

    protected:
        const char* GetAvatarPrefab(EntityID) override { return "native:AvatarTest.Pawn"; }
        void OnAvatarSpawned(EntityID, EntityID, const uint32_t InOrdinal) override { Ordinals.push_back(InOrdinal); }
    };

    class SpawnLevel : public ILevel
    {
    public:
        const char* GetName() const override { return "AvatarSpawn"; }
        bool OnLoad(GameSession&) override
        {
            for(uint8_t Slot = 2; Slot >= 1; --Slot)
            {
                Entity Start = GetWorld().CreateEntity(Slot == 1 ? "Start 1" : "Start 2");
                GameCore::TransformComponent At;
                At.Position.SetX(100.0 * Slot);
                Start.AddComponent(At);
                PlayerStartComponent Point;
                Point.PlayerSlot = Slot;
                Start.AddComponent(Point);
            }
            return true;
        }
        IGameMode* GetGameMode() override { return &Mode; }
        SpawnMode Mode;
    };

    void TestNetGameMode()
    {
        std::printf("[NetGameMode]\n");
        TestHost Host;
        GameCore::ScriptEngine Scripts;
        RenderCore::Renderer Rendering;
        Scripts.Initialize();
        auto* Session = new GameSession(Host, Scripts, Rendering);
        SpawnLevel Level;
        Expect(Session->LoadLevel(Level), "the level loads");
        GameCore::World& World = Session->GetWorld();

        const EntityID Player = FindLocalPlayer(World, 0);
        const EntityID Avatar = GetAvatar(World, Player);
        Expect(Avatar.IsSet(), "OnStart gave the local player an avatar");
        const GameCore::TransformComponent* At = Avatar.IsSet() ? World.GetEntity(Avatar).GetComponent<GameCore::TransformComponent>() : nullptr;
        Expect(At && At->Position.GetX() == 100.0, "at the start kept for player 1");
        Expect(Level.Mode.Possessed == 1 && Level.Mode.Ordinals.size() == 1 && Level.Mode.Ordinals[0] == 0,
               "the mode heard OnAvatarPossessed and OnAvatarSpawned");

        /* A second player, as a joining client would be. */
        const EntityID Second = MakePlayer(World, "Joined", 9).GetID();
        Level.Mode.OnClientConnected(9, Second);
        const EntityID SecondAvatar = GetAvatar(World, Second);
        At = SecondAvatar.IsSet() ? World.GetEntity(SecondAvatar).GetComponent<GameCore::TransformComponent>() : nullptr;
        Expect(At && At->Position.GetX() == 200.0, "the next player spawns at the next start");
        Expect(SecondAvatar.IsSet() && World.GetEntity(SecondAvatar).GetComponent<GameCore::NetComponent>()->OwnerClient == 9,
               "owned by its client from the start");

        FrameContext Frame;
        Frame.DeltaSeconds = 0.016;
        Session->Tick(Frame);
        Expect(Session->GetViewTarget().IsSet() && World.GetEntity(Session->GetViewTarget()).HasComponent<RenderCore::CameraComponent>(),
               "the local avatar's child camera is the view");

        World.DestroyEntity(Avatar);
        Expect(Level.Mode.Unpossessed == 1, "the mode heard the avatar go");

        Session->UnloadLevel();
        delete Session;
    }

    void TestFreePlayerIndex()
    {
        std::printf("[player index]\n");
        GameCore::World World;
        MakePlayer(World, "0", GameCore::NET_CLIENT_NONE);
        Entity Second = MakePlayer(World, "1", 0);
        Second.GetComponent<PlayerStateComponent>()->PlayerIndex = 1;
        Expect(FindFreePlayerIndex(World) == 2, "a new player takes the first free index");
        World.DestroyEntity(World.FindEntityByName("0"));
        Expect(FindFreePlayerIndex(World) == 0, "including one given back");
    }
}

int main()
{
    std::printf("NimbleEngineAvatarTests\n");
    TestPossession();
    TestClientRelease();
    TestLocalInput();
    TestServerInput();
    TestLocalSimulation();
    TestAutoPossess();
    TestSplitscreenViews();
    TestNetGameMode();
    TestFreePlayerIndex();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
