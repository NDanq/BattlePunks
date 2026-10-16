/*
 * NimbleEngineCharacterMoverTests -- the Character Mover (CharacterMover.h,
 * AVATARS.md "Character Mover"): the CharacterMove step deterministic from
 * the same input (what prediction's replay relies on), walking, running,
 * turning, jumping and landing, walls, crouching under a ceiling, aiming,
 * TeleportCharacter, GetCharacterAim, the presentation system (anim params,
 * the camera boom), the simulation through an AvatarWorld, and the examples
 * of Engine/Docs/Guides/CharacterMoverGuide.md. On Bounds: no physics.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AIFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"

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

static bool Near(const double InA, const double InB, const double InTolerance) { return std::fabs(InA - InB) <= InTolerance; }

/* Without physics a ray meets a Bounds box padded by the spatial index's fat
 * margin (0.5): heights stood on are within it. */
constexpr double Fat = 0.51;

/* The Character Mover's default names, as a game's map would have them. */
NIMBLE_DEFINE_INPUT_MAP(MoverTest)
{
    Map.Action("Run",    "LeftShift");
    Map.Action("Jump",   "Space");
    Map.Action("Crouch", "C");
    Map.Action("Aim",    "Mouse.Right");
    Map.Axis("MoveForward", "W, -S");
    Map.Axis("MoveRight",   "D, -A");
    Map.Axis("LookX", "Mouse.DeltaX");
    Map.Axis("LookY", "Mouse.DeltaY");
}

// === The guide's examples (Engine/Docs/Guides/CharacterMoverGuide.md) =================
// Kept as the guide shows them: change one, change the other.

/* --- guide: a player prefab in C++ --- */
static void BuildHero(GameCore::PrefabBuilder& B)
{
    GameCore::PrefabBuilder::Node Root = B.Root("Hero");
    Root.Add<GameCore::TransformComponent>();

    GameCore::NetComponent Net;
    Net.bPredicted = 1;                       // the owning client steps it at once
    Root.Add(Net);

    NimbleEngine::AvatarHarnessComponent Harness;
    std::snprintf(Harness.InputMap, sizeof(Harness.InputMap), "%s", "MoverTest");
    std::snprintf(Harness.Simulation, sizeof(Harness.Simulation), "%s", "CharacterMove");
    Root.Add(Harness);

    NimbleEngine::CharacterMoverComponent Mover;
    Mover.RunSpeed  = 450.0f;
    Mover.JumpSpeed = 420.0f;
    Root.Add(Mover);

    GameCore::PrefabBuilder::Node Boom = Root.Child("CameraBoom");
    Boom.Add<GameCore::TransformComponent>();
    GameCore::PrefabBuilder::Node Eye = Boom.Child("Camera");
    Eye.Add<GameCore::TransformComponent>();
    RenderCore::CameraComponent Camera;
    Camera.bActive = 0;                       // the view target; never the level's first active
    Eye.Add(Camera);
}
GAMECORE_DEFINE_NATIVE_PREFAB(MoverTestHero, "MoverTest.Hero", &BuildHero)

/* --- guide: a respawn --- */
static void Respawn(GameCore::World& InWorld, GameCore::EntityID InAvatar, GameCore::EntityID InStart)
{
    const GameCore::TransformComponent* Start = InWorld.GetEntity(InStart).GetComponent<GameCore::TransformComponent>();
    if(!Start) { return; }
    NimbleEngine::TeleportCharacter(InWorld, InAvatar, Start->Position, float(Start->Rotation.GetY()));
}

/* --- guide: a hit-scan shot, the same on the server as on the client that fired --- */
static GameCore::EntityID TraceShot(GameCore::World& InWorld, GameCore::EntityID InShooter, float InRange)
{
    EngineCore::Vector3 Origin, Dir;
    if(!NimbleEngine::GetCharacterAim(InWorld, InShooter, Origin, Dir)) { return GameCore::EntityID(); }
    NimbleEngine::PhysicsRaycastHit Hit;
    if(!NimbleEngine::PhysicsRaycast(InWorld, Origin, Dir, InRange, Hit)) { return GameCore::EntityID(); }
    return Hit.Entity;
}

/* --- guide: a game's own simulation on top of the walk --- */
NIMBLE_DEFINE_AVATAR_SIMULATION(MoverTestStaminaMove, 0.1f)
{
    NimbleEngine::StepCharacter(InWorld, InAvatar, InFrame);

    /* After the walk: anything else this game's avatars keep -- in a
     * replicated component, so prediction replays it too. */
    GameCore::Entity Avatar = InWorld.GetEntity(InAvatar);
    if(NimbleEngine::CharacterMoverComponent* Mover = Avatar.GetComponent<NimbleEngine::CharacterMoverComponent>())
    {
        Mover->RunSpeed = Mover->bGrounded ? 400.0f : 300.0f;
    }
}

/* --- guide: a bot steering the same mover --- */
static NimbleEngine::SteerInputMap SteerMapFor(GameCore::World& InWorld, GameCore::EntityID InAvatar)
{
    NimbleEngine::SteerInputMap Map;
    GameCore::Entity Avatar = InWorld.GetEntity(InAvatar);
    const NimbleEngine::AvatarHarnessComponent*  Harness = Avatar.GetComponent<NimbleEngine::AvatarHarnessComponent>();
    const NimbleEngine::CharacterMoverComponent* Mover   = Avatar.GetComponent<NimbleEngine::CharacterMoverComponent>();
    if(!Harness || !Mover) { return Map; }
    Map.MoveForward = NimbleEngine::FindInputAxis(Harness->InputMap, Mover->MoveForward);
    Map.MoveRight   = NimbleEngine::FindInputAxis(Harness->InputMap, Mover->MoveRight);
    Map.LookX       = NimbleEngine::FindInputAxis(Harness->InputMap, Mover->LookX);
    const int Run   = NimbleEngine::FindInputAction(Harness->InputMap, Mover->RunAction);
    Map.RunBits     = Run >= 0 ? 1u << Run : 0u;
    Map.WalkSpeed   = Mover->WalkSpeed;
    Map.RunSpeed    = Mover->RunSpeed;
    Map.TurnDegreesPerSecond = Mover->TurnRate * 0.5f;
    return Map;
}

// === Fixtures ========================================================================

namespace
{
    using GameCore::Entity;
    using GameCore::EntityID;

    constexpr float Dt = 1.0f / 60.0f;

    /* A box with no physics: what PhysicsRaycast tests without JoltPhysics. */
    Entity MakeBox(GameCore::World& InWorld, const char* InName, const EngineCore::Vector3& InMin, const EngineCore::Vector3& InMax)
    {
        Entity Box = InWorld.CreateEntity(InName);
        Box.AddComponent<GameCore::TransformComponent>();
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(InMin, InMax);
        Box.AddComponent(Bounds);
        InWorld.RefreshEntityBounds(Box.GetID());
        return Box;
    }

    Entity MakeFloor(GameCore::World& InWorld)
    {
        return MakeBox(InWorld, "Floor", EngineCore::Vector3(-5000.0, -100.0, -5000.0), EngineCore::Vector3(5000.0, 0.0, 5000.0));
    }

    Entity MakeWalker(GameCore::World& InWorld, const char* InName = "Walker", const char* InSimulation = "CharacterMove")
    {
        Entity Avatar = InWorld.CreateEntity(InName);
        Avatar.AddComponent<GameCore::TransformComponent>();
        AvatarHarnessComponent Harness;
        std::snprintf(Harness.InputMap, sizeof(Harness.InputMap), "%s", "MoverTest");
        std::snprintf(Harness.Simulation, sizeof(Harness.Simulation), "%s", InSimulation);
        Avatar.AddComponent(Harness);
        Avatar.AddComponent<CharacterMoverComponent>();
        TeleportCharacter(InWorld, Avatar.GetID(), EngineCore::Vector3(0.0, 0.0, 0.0), 0.0f);
        return Avatar;
    }

    CharacterMoverComponent& MoverOf(GameCore::World& InWorld, const EntityID InAvatar)
    {
        return *InWorld.GetEntity(InAvatar).GetComponent<CharacterMoverComponent>();
    }

    GameCore::TransformComponent& BodyOf(GameCore::World& InWorld, const EntityID InAvatar)
    {
        return *InWorld.GetEntity(InAvatar).GetComponent<GameCore::TransformComponent>();
    }

    uint32_t Bit(const char* InAction)
    {
        const int Index = FindInputAction("MoverTest", InAction);
        return Index >= 0 ? 1u << Index : 0u;
    }

    struct FrameSpec
    {
        float    Forward = 0.0f, Right = 0.0f, LookX = 0.0f, LookY = 0.0f;
        uint32_t Down = 0, Pressed = 0;
    };

    AvatarInputFrame MakeFrame(const FrameSpec& InSpec, const int32_t InSequence = 0, const float InSeconds = Dt)
    {
        AvatarInputFrame Frame;
        Frame.Sequence = InSequence;
        Frame.Seconds  = InSeconds;
        Frame.Down     = InSpec.Down | InSpec.Pressed;
        Frame.Pressed  = InSpec.Pressed;
        const int F = FindInputAxis("MoverTest", "MoveForward"), R = FindInputAxis("MoverTest", "MoveRight");
        const int LX = FindInputAxis("MoverTest", "LookX"), LY = FindInputAxis("MoverTest", "LookY");
        if(F  >= 0) { Frame.Axes[F]  = InSpec.Forward; }
        if(R  >= 0) { Frame.Axes[R]  = InSpec.Right; }
        if(LX >= 0) { Frame.Axes[LX] = InSpec.LookX; }
        if(LY >= 0) { Frame.Axes[LY] = InSpec.LookY; }
        return Frame;
    }

    /* One frame as the avatar system steps it: the step, then the snap to
     * what replicates (both ends do it, so a replay starts where the server was). */
    void Step(GameCore::World& InWorld, const EntityID InAvatar, const AvatarInputFrame& InFrame)
    {
        StepCharacter(InWorld, InAvatar, InFrame);
        GameCore::QuantizeEntityReplicatedFields(InWorld, InAvatar);
    }

    void Hold(GameCore::World& InWorld, const EntityID InAvatar, const FrameSpec& InSpec, const int InFrames)
    {
        for(int I = 0; I < InFrames; ++I) { Step(InWorld, InAvatar, MakeFrame(InSpec, I)); }
    }

    /* 120 frames of everything: walking, turning, running, a jump, a crouch, aiming. */
    std::vector<AvatarInputFrame> MixedInput()
    {
        std::vector<AvatarInputFrame> Frames;
        for(int I = 0; I < 120; ++I)
        {
            FrameSpec S;
            S.Forward = I < 90 ? 1.0f : 0.0f;
            S.Right   = (I % 40) < 20 ? 0.3f : -0.5f;
            S.LookX   = (I % 30) < 15 ? 1.7f : -0.9f;
            S.LookY   = I < 60 ? 0.2f : -0.25f;
            if(I >= 10 && I < 50)  { S.Down |= Bit("Run"); }
            if(I == 20 || I == 75) { S.Pressed |= Bit("Jump"); }
            if(I >= 95 && I < 110) { S.Down |= Bit("Crouch"); }
            if(I >= 60 && I < 85)  { S.Down |= Bit("Aim"); }
            Frames.push_back(MakeFrame(S, I + 1, I % 7 == 0 ? Dt * 1.5f : Dt));
        }
        return Frames;
    }

    bool SameState(GameCore::World& InA, const EntityID InIdA, GameCore::World& InB, const EntityID InIdB)
    {
        const CharacterMoverComponent& A = MoverOf(InA, InIdA);
        const CharacterMoverComponent& B = MoverOf(InB, InIdB);
        const GameCore::TransformComponent& TA = BodyOf(InA, InIdA);
        const GameCore::TransformComponent& TB = BodyOf(InB, InIdB);
        return A.BodyYaw == B.BodyYaw && A.CameraYaw == B.CameraYaw && A.Pitch == B.Pitch && A.Scope == B.Scope
            && A.VelX == B.VelX && A.VelY == B.VelY && A.VelZ == B.VelZ && A.AirTime == B.AirTime && A.JumpCount == B.JumpCount
            && A.bGrounded == B.bGrounded && A.bCrouched == B.bCrouched && A.bStarted == B.bStarted
            && TA.Position.GetX() == TB.Position.GetX() && TA.Position.GetY() == TB.Position.GetY()
            && TA.Position.GetZ() == TB.Position.GetZ() && TA.Rotation.GetY() == TB.Rotation.GetY();
    }

    // === Tests =======================================================================

    void TestDeterminism()
    {
        std::printf("[determinism]\n");
        const std::vector<AvatarInputFrame> Frames = MixedInput();

        GameCore::World A, B;
        MakeFloor(A);
        MakeFloor(B);
        const EntityID WalkerA = MakeWalker(A).GetID();
        const EntityID WalkerB = MakeWalker(B).GetID();
        for(const AvatarInputFrame& Frame : Frames) { Step(A, WalkerA, Frame); }
        for(const AvatarInputFrame& Frame : Frames) { Step(B, WalkerB, Frame); }
        Expect(SameState(A, WalkerA, B, WalkerB), "the same input from the same start ends in the same state, exactly");
        Expect(MoverOf(A, WalkerA).JumpCount == 2, "both jumps were taken");

        /* Prediction's replay: the state put back as the server sent it, the frames after it stepped again. */
        GameCore::World C;
        MakeFloor(C);
        const EntityID WalkerC = MakeWalker(C).GetID();
        for(size_t I = 0; I < 60; ++I) { Step(C, WalkerC, Frames[I]); }
        const CharacterMoverComponent SavedMover = MoverOf(C, WalkerC);
        const GameCore::TransformComponent SavedBody = BodyOf(C, WalkerC);
        for(size_t I = 60; I < Frames.size(); ++I) { Step(C, WalkerC, Frames[I]); }
        Expect(SameState(A, WalkerA, C, WalkerC), "stepping in two runs is stepping in one");

        MoverOf(C, WalkerC) = SavedMover;
        BodyOf(C, WalkerC)  = SavedBody;
        for(size_t I = 60; I < Frames.size(); ++I) { Step(C, WalkerC, Frames[I]); }
        Expect(SameState(A, WalkerA, C, WalkerC), "a replay from the components alone lands where the first run did");
    }

    void TestWalk()
    {
        std::printf("[walk]\n");
        GameCore::World World;
        MakeFloor(World);
        const EntityID Walker = MakeWalker(World).GetID();
        const CharacterMoverComponent& Mover = MoverOf(World, Walker);

        Hold(World, Walker, FrameSpec(), 5);
        Expect(Mover.bGrounded != 0 && Near(BodyOf(World, Walker).Position.GetY(), 0.0, Fat), "it stands on the floor");

        FrameSpec Walk;
        Walk.Forward = 1.0f;
        Hold(World, Walker, Walk, 60);
        Expect(Near(Mover.VelZ, Mover.WalkSpeed, 0.05) && Near(Mover.VelX, 0.0, 0.05), "forward at yaw 0 walks along +Z at Walk Speed");
        Expect(BodyOf(World, Walker).Position.GetZ() > 140.0, "and covers the ground");

        Walk.Down = Bit("Run");
        Hold(World, Walker, Walk, 60);
        Expect(Near(Mover.VelZ, Mover.RunSpeed, 0.05), "Run held: Run Speed");

        Hold(World, Walker, FrameSpec(), 30);
        Expect(Near(std::hypot(Mover.VelX, Mover.VelZ), 0.0, 0.05), "let go: it stops");

        /* A quarter turn of the view: forward is +X. */
        FrameSpec Turn;
        Turn.LookX = 90.0f;
        Step(World, Walker, MakeFrame(Turn));
        Expect(Near(Mover.CameraYaw, 90.0, 0.01), "LookX turns the view, in degrees");
        const double StartX = BodyOf(World, Walker).Position.GetX();
        Hold(World, Walker, Walk, 60);
        Expect(BodyOf(World, Walker).Position.GetX() - StartX > 200.0, "forward follows the view");
        Expect(Near(Mover.BodyYaw, 90.0, 0.5), "and the body turns to where it walks");
        Expect(Near(BodyOf(World, Walker).Rotation.GetY(), -90.0, 0.5), "its Transform adds Mesh Yaw (180)");

        FrameSpec Look;
        Look.LookY = 500.0f;
        Step(World, Walker, MakeFrame(Look));
        Expect(Near(Mover.Pitch, Mover.PitchMax, 1e-3), "the pitch stops at Pitch Max");
    }

    void TestJump()
    {
        std::printf("[jump and land]\n");
        GameCore::World World;
        MakeFloor(World);
        const EntityID Walker = MakeWalker(World).GetID();
        const CharacterMoverComponent& Mover = MoverOf(World, Walker);
        Hold(World, Walker, FrameSpec(), 5);

        FrameSpec Jump;
        Jump.Pressed = Bit("Jump");
        Step(World, Walker, MakeFrame(Jump));
        Expect(Mover.bGrounded == 0 && Mover.JumpCount == 1, "Jump leaves the ground, once");
        Expect(Near(Mover.VelY, Mover.JumpSpeed - Mover.Gravity * Dt, 0.05), "at Jump Speed, less a frame of gravity");

        /* Held through the air: no second jump. */
        double Peak = 0.0;
        int Frames = 0;
        FrameSpec Held;
        Held.Down = Bit("Jump");
        while(Mover.bGrounded == 0 && Frames < 300)
        {
            Step(World, Walker, MakeFrame(Held));
            Peak = (std::max)(Peak, double(BodyOf(World, Walker).Position.GetY()));
            ++Frames;
        }
        const double Expected = double(Mover.JumpSpeed) * Mover.JumpSpeed / (2.0 * Mover.Gravity);
        Expect(Near(Peak, Expected, 8.0), "it rises JumpSpeed^2 / 2g (" + std::to_string(Peak) + ")");
        Expect(Mover.bGrounded != 0 && Near(BodyOf(World, Walker).Position.GetY(), 0.0, Fat), "and lands on the floor");
        Expect(Near(Frames * Dt, 2.0 * Mover.JumpSpeed / Mover.Gravity, 0.05), "after the time a jump takes");
        Expect(Mover.JumpCount == 1 && Mover.AirTime == 0.0f, "one jump; air time reset on landing");

        /* Off a ledge: a fall, AirTime counting. */
        GameCore::World Ledge;
        MakeBox(Ledge, "Ledge", EngineCore::Vector3(-100.0, -100.0, -100.0), EngineCore::Vector3(100.0, 200.0, 100.0));
        MakeFloor(Ledge);
        const EntityID Faller = MakeWalker(Ledge).GetID();
        TeleportCharacter(Ledge, Faller, EngineCore::Vector3(0.0, 200.0, 50.0), 0.0f);
        Hold(Ledge, Faller, FrameSpec(), 3);
        Expect(MoverOf(Ledge, Faller).bGrounded != 0 && Near(BodyOf(Ledge, Faller).Position.GetY(), 200.0, Fat), "on the ledge");
        FrameSpec Off;
        Off.Forward = 1.0f;
        Hold(Ledge, Faller, Off, 40);
        Expect(MoverOf(Ledge, Faller).bGrounded == 0 && MoverOf(Ledge, Faller).AirTime > 0.1f, "walked off: falling, AirTime counting");
        Hold(Ledge, Faller, FrameSpec(), 120);
        Expect(MoverOf(Ledge, Faller).bGrounded != 0 && Near(BodyOf(Ledge, Faller).Position.GetY(), 0.0, Fat), "down on the floor");
    }

    void TestWalls()
    {
        std::printf("[walls and steps]\n");
        GameCore::World World;
        MakeFloor(World);
        MakeBox(World, "Wall", EngineCore::Vector3(-1000.0, 0.0, 300.0), EngineCore::Vector3(1000.0, 300.0, 400.0));
        MakeBox(World, "Step", EngineCore::Vector3(-1000.0, 0.0, -400.0), EngineCore::Vector3(1000.0, 30.0, -200.0));
        const EntityID Walker = MakeWalker(World).GetID();
        const CharacterMoverComponent& Mover = MoverOf(World, Walker);
        Hold(World, Walker, FrameSpec(), 3);

        FrameSpec Walk;
        Walk.Forward = 1.0f;
        Hold(World, Walker, Walk, 180);
        const double Z = BodyOf(World, Walker).Position.GetZ();
        Expect(Z <= 300.0 - Mover.Radius + 0.01 && Z > 300.0 - Mover.Radius - 5.0, "a wall stops it a Radius short (" + std::to_string(Z) + ")");

        /* Along it, half into it: it slides. */
        FrameSpec Slide;
        Slide.Forward = 0.7f;
        Slide.Right   = 0.7f;
        const double X = BodyOf(World, Walker).Position.GetX();
        Hold(World, Walker, Slide, 60);
        Expect(BodyOf(World, Walker).Position.GetX() - X > 60.0, "into a wall at an angle, it slides along it");
        Expect(BodyOf(World, Walker).Position.GetZ() <= 300.0 - Mover.Radius + 0.01, "and stays out of it");

        /* Back and up a 30 step. */
        TeleportCharacter(World, Walker, EngineCore::Vector3(0.0, 0.0, 0.0), 180.0f);
        Hold(World, Walker, Walk, 120);
        Expect(Near(BodyOf(World, Walker).Position.GetY(), 30.0, Fat) && BodyOf(World, Walker).Position.GetZ() < -250.0,
               "a ledge under Step Height is walked up");
    }

    void TestCrouchAndAim()
    {
        std::printf("[crouch and aim]\n");
        GameCore::World World;
        MakeFloor(World);
        MakeBox(World, "Ceiling", EngineCore::Vector3(-100.0, 140.0, 200.0), EngineCore::Vector3(100.0, 400.0, 400.0));
        const EntityID Walker = MakeWalker(World).GetID();
        const CharacterMoverComponent& Mover = MoverOf(World, Walker);
        Hold(World, Walker, FrameSpec(), 3);

        FrameSpec Crouch;
        Crouch.Forward = 1.0f;
        Crouch.Down    = Bit("Crouch") | Bit("Run");
        Hold(World, Walker, Crouch, 60);
        Expect(Mover.bCrouched != 0 && Near(Mover.VelZ, Mover.CrouchSpeed, 0.05), "Crouch held: crouched, at Crouch Speed (over Run)");
        Hold(World, Walker, Crouch, 150);
        const double Under = BodyOf(World, Walker).Position.GetZ();
        Expect(Under > 200.0 && Under < 400.0, "crouched, it walks under a low ceiling");

        FrameSpec Stand;
        Hold(World, Walker, Stand, 10);
        Expect(Mover.bCrouched != 0, "let go under the ceiling: it stays down");
        FrameSpec Out;
        Out.Forward = 1.0f;
        Hold(World, Walker, Out, 120);
        Expect(Mover.bCrouched == 0 && BodyOf(World, Walker).Position.GetZ() > 400.0, "out from under it: it stands");

        /* Aim: the blend in, the slower walk, the body to the view. */
        FrameSpec Turn;
        Turn.LookX = 60.0f;
        Step(World, Walker, MakeFrame(Turn));
        FrameSpec Aim;
        Aim.Forward = 1.0f;
        Aim.Down    = Bit("Aim");
        Hold(World, Walker, Aim, 30);
        Expect(Mover.Scope == 1.0f, "Aim held blends Scope to 1");
        Expect(Near(std::hypot(Mover.VelX, Mover.VelZ), Mover.AimSpeed, 0.05), "at Aim Speed");
        Expect(Near(Mover.BodyYaw, Mover.CameraYaw, 0.5), "facing the view");
        const float Yaw = Mover.CameraYaw;
        Aim.LookX = 10.0f;
        Step(World, Walker, MakeFrame(Aim));
        Expect(Near(Mover.CameraYaw - Yaw, 10.0 * Mover.ScopeLook, 0.01), "aimed in, the look is scaled by Aim Look");
    }

    void TestTeleportAndAim()
    {
        std::printf("[teleport, aim]\n");
        GameCore::World World;
        MakeFloor(World);
        const EntityID Walker = MakeWalker(World).GetID();
        CharacterMoverComponent& Mover = MoverOf(World, Walker);
        FrameSpec Run;
        Run.Forward = 1.0f;
        Run.Down    = Bit("Run");
        Hold(World, Walker, Run, 30);

        TeleportCharacter(World, Walker, EngineCore::Vector3(100.0, 0.0, 200.0), 90.0f);
        const GameCore::TransformComponent& Body = BodyOf(World, Walker);
        Expect(Body.Position.GetX() == 100.0 && Body.Position.GetZ() == 200.0, "teleported to the place");
        Expect(Mover.BodyYaw == 90.0f && Mover.CameraYaw == 90.0f && Near(Body.Rotation.GetY(), -90.0, 1e-4),
               "facing the yaw, its Transform turned by Mesh Yaw");
        Expect(Mover.VelX == 0.0f && Mover.VelZ == 0.0f && Mover.bStarted == 1, "at rest");
        FrameSpec Walk;
        Walk.Forward = 1.0f;
        Hold(World, Walker, Walk, 30);
        Expect(Body.Position.GetX() > 120.0 && Near(Body.Position.GetZ(), 200.0, 0.01), "and walks on from there, along its yaw");

        /* GetCharacterAim: the view, from where the camera sits. */
        Mover.Pitch = 0.0f;
        EngineCore::Vector3 Origin, Dir;
        Expect(GetCharacterAim(World, Walker, Origin, Dir), "a mover has an aim");
        Expect(Near(Dir.GetX(), 1.0, 1e-6) && Near(Dir.GetY(), 0.0, 1e-6), "level, at yaw 90: along +X");
        Expect(Near(Origin.GetY(), Body.Position.GetY() + Mover.BoomHeight + Mover.CameraUp, 1e-3)
               && Near(Origin.GetX(), Body.Position.GetX() - Mover.CameraBack, 1e-3)
               && Near(Origin.GetZ(), Body.Position.GetZ() - Mover.CameraRight, 1e-3),
               "from behind, over the right shoulder");
        Expect(!GetCharacterAim(World, World.CreateEntity("Bare").GetID(), Origin, Dir), "nothing without a mover");

        /* A step on an avatar with only a Transform adds the mover. */
        Entity Bare = World.CreateEntity("Plain");
        Bare.AddComponent<GameCore::TransformComponent>();
        StepCharacter(World, Bare.GetID(), MakeFrame(FrameSpec()));
        Expect(Bare.HasComponent<CharacterMoverComponent>(), "the step adds a Character Mover where it is missing");
    }

    void TestPresentation()
    {
        std::printf("[presentation]\n");
        GameCore::World World;
        MakeFloor(World);
        Entity Walker = MakeWalker(World);
        Walker.AddComponent<RenderCore::AnimationComponent>();
        Entity Boom = World.CreateEntity("CameraBoom");
        Boom.AddComponent<GameCore::TransformComponent>();
        Boom.SetParent(Walker.GetID(), false);
        Entity Eye = World.CreateEntity("Camera");
        Eye.AddComponent<GameCore::TransformComponent>();
        Eye.AddComponent<RenderCore::CameraComponent>();
        Eye.SetParent(Boom.GetID(), false);
        const EntityID Id = Walker.GetID();
        CharacterMoverComponent& Mover = MoverOf(World, Id);
        Mover.AnimCrouch[0] = 'C'; Mover.AnimCrouch[1] = '\0';

        FrameSpec Walk;
        Walk.Forward = 1.0f;
        Walk.LookX   = 0.5f;
        Hold(World, Id, Walk, 40);
        UpdateCharacterMovers(World, Dt);
        RenderCore::AnimationComponent* Animation = Walker.GetComponent<RenderCore::AnimationComponent>();
        Expect(Near(Animation->GetFloat("Speed"), std::hypot(Mover.VelX, Mover.VelZ), 1e-3), "Speed from the velocity");
        Expect(Animation->GetBool("Grounded") && !Animation->GetBool("C"), "Grounded, and the crouch param (named) off");
        Expect(Near(Animation->GetFloat("AimPitch"), -Mover.Pitch, 1e-4), "AimPitch up-positive");
        Expect(Animation->GetFloat("Jump") == 0.0f, "no jump, no trigger");

        FrameSpec Jump;
        Jump.Pressed = Bit("Jump");
        Step(World, Id, MakeFrame(Jump));
        UpdateCharacterMovers(World, Dt);
        Expect(Animation->GetFloat("Jump") == 1.0f, "a jump fires the trigger");
        Animation->SetFloat("Jump", 0.0f);   // the anim state took it
        Step(World, Id, MakeFrame(FrameSpec()));
        UpdateCharacterMovers(World, Dt);
        Expect(Animation->GetFloat("Jump") == 0.0f, "once per jump");

        const GameCore::TransformComponent* BoomT = Boom.GetComponent<GameCore::TransformComponent>();
        Expect(Near(BoomT->Position.GetY(), Mover.BoomHeight, 1e-4), "the boom at Boom Height");
        Expect(Near(BoomT->Rotation.GetX(), Mover.Pitch, 1e-4), "pitched with the view");
        double Relative = std::fmod(double(Mover.CameraYaw) - Mover.BodyYaw - Mover.MeshYaw + 540.0, 360.0) - 180.0;
        Expect(Near(BoomT->Rotation.GetY(), Relative, 1e-3), "turned to the view from the body");
        const GameCore::TransformComponent* EyeT = Eye.GetComponent<GameCore::TransformComponent>();
        Expect(Near(EyeT->Position.GetX(), Mover.CameraRight, 1e-4) && Near(EyeT->Position.GetZ(), -Mover.CameraBack, 1e-4),
               "the camera back and right of it, nothing in the way");
        Expect(Near(Eye.GetComponent<RenderCore::CameraComponent>()->FovY, Mover.FovNormal, 1e-4), "at the normal field of view");
        EngineCore::Vector3 Origin, Dir;
        GetCharacterAim(World, Id, Origin, Dir);
        Expect(Mover.bCameraPlaced == 1 && Near(Mover.CameraX, Origin.GetX(), 0.01) && Near(Mover.CameraY, Origin.GetY(), 0.01)
               && Near(Mover.CameraZ, Origin.GetZ(), 0.01), "the camera where GetCharacterAim says, in the open");

        /* A wall behind: the camera comes in. */
        const double BodyZ = BodyOf(World, Id).Position.GetZ();
        TeleportCharacter(World, Id, EngineCore::Vector3(0.0, 0.0, BodyZ), 0.0f);
        MakeBox(World, "Behind", EngineCore::Vector3(-500.0, 0.0, BodyZ - 120.0), EngineCore::Vector3(500.0, 500.0, BodyZ - 100.0));
        UpdateCharacterMovers(World, Dt);
        Expect(-EyeT->Position.GetZ() < 100.0, "a wall behind pulls the camera in (" + std::to_string(EyeT->Position.GetZ()) + ")");
    }

    void TestThroughAvatarWorld()
    {
        std::printf("[through an AvatarWorld]\n");
        GameCore::World World;
        AvatarWorld Avatars;
        Avatars.Attach(World);
        InputState Devices;
        Avatars.SetInput(&Devices);
        MakeFloor(World);
        Entity Player = World.CreateEntity("Player");
        Player.AddComponent<GameCore::NetComponent>();
        Player.AddComponent<PossessorComponent>();
        Player.AddComponent<PlayerStateComponent>();
        Entity Walker = MakeWalker(World);
        Expect(Possess(World, Player.GetID(), Walker.GetID()), "possessed");

        Devices.BeginFrame(1);
        Devices.Press(FindInputKey("W"));
        for(int I = 0; I < 30; ++I) { Avatars.Update(Dt); }
        Expect(BodyOf(World, Walker.GetID()).Position.GetZ() > 30.0, "Simulation \"CharacterMove\" walks it from its player's keys");

    }

    void TestGuideExamples()
    {
        std::printf("[guide examples]\n");
        GameCore::World World;
        MakeFloor(World);
        const EntityID Hero = World.SpawnPrefab("native:MoverTest.Hero").GetRoot().GetID();
        Expect(Hero.IsSet(), "the guide's prefab spawns");
        CharacterMoverComponent* Mover = World.GetEntity(Hero).GetComponent<CharacterMoverComponent>();
        Expect(Mover && Mover->RunSpeed == 450.0f, "with its tunables");

        Entity Start = World.CreateEntity("Start");
        GameCore::TransformComponent At;
        At.Position = EngineCore::Vector3(300.0, 0.0, -50.0);
        At.Rotation = EngineCore::Vector3(0.0, 45.0, 0.0);
        Start.AddComponent(At);
        Respawn(World, Hero, Start.GetID());
        Expect(Mover->BodyYaw == 45.0f && BodyOf(World, Hero).Position.GetX() == 300.0, "Respawn puts it at the start");

        Mover->Pitch = 0.0f;
        EngineCore::Vector3 Origin, Dir;
        GetCharacterAim(World, Hero, Origin, Dir);
        const EngineCore::Vector3 Ahead = Origin + Dir * 1000.0;
        MakeBox(World, "Target", Ahead - EngineCore::Vector3(50.0, 50.0, 50.0), Ahead + EngineCore::Vector3(50.0, 50.0, 50.0));
        const EntityID Shot = TraceShot(World, Hero, 5000.0f);
        Expect(Shot.IsSet() && std::strcmp(World.GetEntity(Shot).GetName(), "Target") == 0, "TraceShot hits what it faces");

        const SteerInputMap Map = SteerMapFor(World, Hero);
        Expect(Map.MoveForward == FindInputAxis("MoverTest", "MoveForward") && Map.LookX == FindInputAxis("MoverTest", "LookX")
               && Map.RunBits == Bit("Run") && Map.RunSpeed == 450.0f, "the bot's steer map is the mover's bindings");

        /* The game's own simulation: the walk, then its rule. */
        const AvatarStepFn Stamina = FindAvatarSimulation("MoverTestStaminaMove");
        Expect(Stamina != nullptr && FindAvatarSimulation("CharacterMove") != nullptr, "both simulations are registered");
        GameCore::World Air;
        const EntityID Racer = MakeWalker(Air, "Racer", "MoverTestStaminaMove").GetID();
        TeleportCharacter(Air, Racer, EngineCore::Vector3(0.0, 500.0, 0.0), 0.0f);
        if(Stamina) { Stamina(Air, Racer, MakeFrame(FrameSpec())); }
        Expect(MoverOf(Air, Racer).RunSpeed == 300.0f && BodyOf(Air, Racer).Position.GetY() < 500.0, "it walked (fell) and applied its rule");
    }
}

namespace
{
    /* The guide's Lua: the bindings it uses exist and do what it says. */
    void TestLua()
    {
        std::printf("[lua]\n");
        GameCore::World World;
        MakeFloor(World);
        const EntityID Walker = MakeWalker(World, "LuaWalker").GetID();
        GameCore::ScriptEngine Scripts;
        Scripts.Initialize();
        Scripts.BindWorld(&World);
        Expect(Scripts.RunString(
            "local m = World.FindEntity('LuaWalker'):GetComponent('Character Mover')\n"
            "m:SetRunSpeed(450)\n"
            "assert(m:GetRunSpeed() == 450)\n"
            "assert(m:GetSpeed() == 0)\n"
            "assert(m:GetGrounded() == false)\n"
            "m:Teleport(10, 0, 20, 90)\n"
            "assert(m:GetBodyYaw() == 90)\n"
            "local ox, oy, oz, dx, dy, dz = m:GetAim()\n"
            "assert(math.abs(dx - math.cos(math.rad(8))) < 1e-4, dx)\n"
            "assert(oy > 100)\n", "mover-lua"), "the guide's Lua runs");
        Expect(MoverOf(World, Walker).RunSpeed == 450.0f && BodyOf(World, Walker).Position.GetX() == 10.0, "and reaches the component");
        Scripts.BindWorld(nullptr);
    }
}

int main()
{
    TestDeterminism();
    TestWalk();
    TestJump();
    TestWalls();
    TestCrouchAndAim();
    TestTeleportAndAim();
    TestPresentation();
    TestThroughAvatarWorld();
    TestGuideExamples();
    TestLua();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
