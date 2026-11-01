#include "WorldsLabWalker.h"

#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/Ragdoll.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/UI/UIAction.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UISystem.h"
#include "Public/UI/UIWidget.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * WorldsLabRange: the Shooting Range world's mode
 * (Content/Levels/ShootingRange.nmworld, Tools/ShootingRange/). An
 * NetGameMode: the level's Player Start spawns the RangeShooter prefab
 * (Content/Prefabs/ShootingRange/RangeShooter.nmprefab) for the local player
 * and possesses it.
 *
 * The shooter moves by RangeMove, an avatar simulation: a third-person walker
 * on the world's physics, made of raycasts (Jolt has no character controller
 * here). What it keeps between frames is the avatar's RangeMover component
 * (WorldsLabWalker.h), so it predicts and replays over the network --
 *
 *   ground   rays down round the feet find the highest walkable surface
 *            within a step: floors, the stairs' ramp, the catwalk's deck
 *   walls    rays across, at three heights, slide the move along what they
 *            hit; rays round the body push it back out of anything it is in
 *   air      gravity, a jump, a head check
 *   view     mouse yaw and pitch on the CameraBoom child; the Camera on its
 *            end pulled in when something is between it and the boom
 *   scope    holding Aim (right mouse) with the pistol drawn slides the camera
 *            in over the shoulder and narrows its view, and slows the look
 *   body     turns to the way it moves, or to the view when armed (the
 *            anim state's Armed), and tells the anim state Speed and Grounded
 *
 * Shooting and the pistol are the prefab's Lua
 * (Scripts/ShootingRange/RangeShooter.lua). Escape pauses and opens the pause
 * menu (Content/UI/ShootingRange/PauseMenu.nmui), which frees the mouse;
 * Resume closes it and takes the mouse back. The mode also publishes the
 * HUD's clock, and registers the menu's Native:Range.Resume action (Engine/Docs/Guides/UI).
 * The HUD itself is the mode's (TickHud), on every process with a player: it
 * reads that player's shooter's RangeGun, which the Lua keeps where it runs (the
 * authority) and which replicates -- so a networked client has its HUD too.
 *
 * [WorldsLab] RangeAutoPlay=1 drives the shooter by itself, through the
 * avatar input filter (what a bot would use): it draws and shoots the plates
 * from the start, reloads, holsters, runs to the stairs, climbs to the
 * mezzanine, walks the west catwalk onto the first cross walk and shoots down
 * range from there -- every path the level has, for testing and a demo.
 */

NIMBLE_CONFIG_FLOAT(GRangeLookSensitivity, "WorldsLab", "RangeLookSensitivity", 1.0f, nullptr);
NIMBLE_CONFIG_BOOL(GRangeAutoPlay,         "WorldsLab", "RangeAutoPlay",        false, nullptr);
NIMBLE_CONFIG_BOOL(GRangeShowHelp,         "WorldsLab", "RangeShowHelp",        true,  nullptr);   // the HUD's controls hint (Settings)
/* The prefab the Player Start spawns instead of its own: "" the start's. MoverShooter
 * (Content/Prefabs/ShootingRange/MoverShooter.nmprefab) walks on the engine's Character Mover. */
NIMBLE_CONFIG_STRING(GRangeAvatar,         "WorldsLab", "RangeAvatar",          "",    nullptr);

/* The shooter's controls. Each entry is also [Input_Range] <Name> in
 * Config/DefaultEngine.ini. */
NIMBLE_DEFINE_INPUT_MAP(Range)
{
    Map.Action("Run",     "LeftShift, RightShift, Gamepad.LeftStick");
    Map.Action("Jump",    "Space, Gamepad.South");
    Map.Action("Fire",    "Mouse.Left, Gamepad.RightTrigger");
    Map.Action("Aim",     "Mouse.Right, Gamepad.LeftTrigger");
    Map.Action("Reload",  "R, Gamepad.West");
    Map.Action("Holster", "H, 1, Gamepad.North");
    Map.Action("Pause",   "Escape, Gamepad.Start");

    Map.Axis("MoveForward", "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Axis("MoveRight",   "D, Right, -A, -Left, Gamepad.LeftX");

    /* Degrees this frame; +Y down, so moving the mouse down looks down. */
    Map.Axis("LookX", "Mouse.DeltaX*0.15, Gamepad.RightX*2.5");
    Map.Axis("LookY", "Mouse.DeltaY*0.15, Gamepad.RightY*2.5");
}

namespace
{
    constexpr double DegToRad    = 3.14159265358979323846 / 180.0;
    constexpr double Radius      = 30.0;     // the body's circle
    constexpr double Height      = 180.0;
    constexpr double StepUp      = 45.0;     // highest ledge walked up
    constexpr double SnapDown    = 35.0;     // lowest drop walked down without falling
    constexpr double WalkSpeed   = 165.0;    // units/s; the anim state's walk is at 150
    constexpr double RunSpeed    = 400.0;    // and its run at 400
    constexpr double AimSpeed    = 140.0;
    constexpr double Accel       = 1500.0;
    constexpr double Decel       = 2000.0;
    constexpr double Gravity     = 981.0;
    constexpr double JumpSpeed   = 400.0;
    constexpr double TurnRate    = 600.0;    // degrees/s
    constexpr double BoomHeight  = 150.0;
    constexpr double CameraBack  = 230.0;
    constexpr double CameraRight = 42.0;
    constexpr double CameraUp    = 12.0;
    constexpr double ScopeBack   = 110.0;    // scoped: in close over the right shoulder
    constexpr double ScopeRight  = 58.0;
    constexpr double ScopeUp     = 16.0;
    constexpr double FovNormal   = 62.0;
    constexpr double FovScope    = 40.0;
    constexpr double ScopeLook   = 0.55;     // look sensitivity, scoped
    constexpr double ScopeRate   = 7.0;      // the blend's speed, 1/s
    constexpr double MinPitch    = -60.0;
    constexpr double MaxPitch    = 70.0;
    const double     ProbeHeights[] = { 38.0, 88.0, 163.0 };   // never a stair tread's 25 cm multiple
    /* Imported characters face -Z (the glTF importer flips Z; glTF's forward is +Z), and this
     * walker's forward is +Z: the body's Transform is turned this much from where it faces. */
    constexpr double MeshYaw     = 180.0;

    /* A step's working copy of the avatar's RangeMover, in doubles. */
    struct MoverState
    {
        double BodyYaw   = 0.0;     // where the body faces (+Z at 0); its Transform adds MeshYaw
        double CameraYaw = 0.0;
        double Pitch     = 8.0;
        double CameraX = 0.0, CameraY = 0.0, CameraZ = 0.0;   // the camera, in the world, last frame
        double Scope = 0.0;         // 0 over the shoulder, 1 scoped in
        double VelX = 0.0, VelY = 0.0, VelZ = 0.0;
        double AirTime   = 0.0;
        bool   bGrounded = false;
        bool   bStarted  = false;
    };

    MoverState LoadMover(const RangeMoverComponent& In)
    {
        MoverState S;
        S.BodyYaw = In.BodyYaw; S.CameraYaw = In.CameraYaw; S.Pitch = In.Pitch; S.Scope = In.Scope;
        S.VelX = In.VelX; S.VelY = In.VelY; S.VelZ = In.VelZ; S.AirTime = In.AirTime;
        S.bGrounded = In.bGrounded != 0; S.bStarted = In.bStarted != 0;
        S.CameraX = In.CameraX; S.CameraY = In.CameraY; S.CameraZ = In.CameraZ;
        return S;
    }

    void StoreMover(const MoverState& S, RangeMoverComponent& Out)
    {
        Out.BodyYaw = float(S.BodyYaw); Out.CameraYaw = float(S.CameraYaw); Out.Pitch = float(S.Pitch); Out.Scope = float(S.Scope);
        Out.VelX = float(S.VelX); Out.VelY = float(S.VelY); Out.VelZ = float(S.VelZ); Out.AirTime = float(S.AirTime);
        Out.bGrounded = S.bGrounded ? 1 : 0; Out.bStarted = S.bStarted ? 1 : 0;
        Out.CameraX = float(S.CameraX); Out.CameraY = float(S.CameraY); Out.CameraZ = float(S.CameraZ);
    }

    struct Vec { double X, Y, Z; };

    /* What the walker stands on and slides along: every layer but 4, where
     * ragdolls go. A ragdoll is simulated only
     * on a process that draws it -- not on a dedicated server -- so a client
     * walking into one would step where the server does not, and prediction
     * would part from it. */
    constexpr int32_t WalkMask = ~int32_t(4);   // Layer and Mask are bits: Layer 4 is the bit worth 4

    bool Ray(GameCore::World& InWorld, const Vec& InFrom, const Vec& InDir, const double InLength,
             NimbleEngine::PhysicsRaycastHit& OutHit)
    {
        return NimbleEngine::PhysicsRaycast(InWorld, EngineCore::Vector3(InFrom.X, InFrom.Y, InFrom.Z),
                                            EngineCore::Vector3(InDir.X, InDir.Y, InDir.Z), float(InLength), OutHit, WalkMask);
    }

    double Lerp(const double InA, const double InB, const double InT) { return InA + (InB - InA) * InT; }

    /* What the look axes are multiplied by: less when scoped. The setting is
     * applied before the input is sent (ApplyLookSensitivity), so the server
     * turns the same. */
    double LookScale(const MoverState& InState)
    {
        return Lerp(1.0, ScopeLook, InState.Scope);
    }

    using WorldsLabWalker::FindChild;
    using WorldsLabWalker::WrapDegrees;

    /* The move (DX, DZ) from (X, FeetY, Z), slid along whatever the probes hit. */
    void SlideMove(GameCore::World& InWorld, double& InOutX, const double InFeetY, double& InOutZ, double DX, double DZ)
    {
        for(int Pass = 0; Pass < 3; ++Pass)
        {
            const double Length = std::sqrt(DX * DX + DZ * DZ);
            if(Length < 1e-4) { return; }
            const Vec Dir { DX / Length, 0.0, DZ / Length };
            double Nearest = Length + Radius;
            Vec    Normal { 0.0, 0.0, 0.0 };
            for(const double H : ProbeHeights)
            {
                NimbleEngine::PhysicsRaycastHit Hit;
                if(Ray(InWorld, { InOutX, InFeetY + H, InOutZ }, Dir, Length + Radius, Hit) && std::fabs(Hit.Normal.GetY()) < 0.7
                   && Hit.Distance < Nearest)
                {
                    Nearest = Hit.Distance;
                    Normal  = { Hit.Normal.GetX(), 0.0, Hit.Normal.GetZ() };
                }
            }
            if(Nearest >= Length + Radius)
            {
                InOutX += DX;
                InOutZ += DZ;
                return;
            }
            /* Up to the wall, then what is left along it. */
            const double Allowed = std::max(0.0, Nearest - Radius);
            InOutX += Dir.X * Allowed;
            InOutZ += Dir.Z * Allowed;
            const double NLength = std::sqrt(Normal.X * Normal.X + Normal.Z * Normal.Z);
            if(NLength < 1e-4) { return; }
            const double NX = Normal.X / NLength, NZ = Normal.Z / NLength;
            const double LeftX = DX - Dir.X * Allowed, LeftZ = DZ - Dir.Z * Allowed;
            const double Into = LeftX * NX + LeftZ * NZ;
            DX = LeftX - NX * Into;
            DZ = LeftZ - NZ * Into;
        }
    }

    /* Out of anything closer than Radius, round the body at two heights. */
    void PushOut(GameCore::World& InWorld, double& InOutX, const double InFeetY, double& InOutZ)
    {
        for(const double H : { ProbeHeights[0], ProbeHeights[2] })
        {
            for(int Spoke = 0; Spoke < 8; ++Spoke)
            {
                const double A = Spoke * 45.0 * DegToRad;
                const Vec Dir { std::sin(A), 0.0, std::cos(A) };
                NimbleEngine::PhysicsRaycastHit Hit;
                if(Ray(InWorld, { InOutX, InFeetY + H, InOutZ }, Dir, Radius, Hit) && std::fabs(Hit.Normal.GetY()) < 0.7)
                {
                    const double Push = Radius - Hit.Distance;
                    InOutX -= Dir.X * Push;
                    InOutZ -= Dir.Z * Push;
                }
            }
        }
    }

    /* The highest walkable surface under the circle at (X, Z), searching from
     * InTop down InDepth; false if none. */
    bool FindGround(GameCore::World& InWorld, const double InX, const double InTop, const double InZ, const double InDepth,
                    double& OutY)
    {
        bool bFound = false;
        const double R = Radius * 0.6;
        const Vec Offsets[] = { { 0, 0, 0 }, { R, 0, 0 }, { -R, 0, 0 }, { 0, 0, R }, { 0, 0, -R } };
        for(const Vec& O : Offsets)
        {
            NimbleEngine::PhysicsRaycastHit Hit;
            if(Ray(InWorld, { InX + O.X, InTop, InZ + O.Z }, { 0.0, -1.0, 0.0 }, InDepth, Hit) && Hit.Normal.GetY() > 0.55)
            {
                const double Y = InTop - Hit.Distance;
                if(!bFound || Y > OutY) { OutY = Y; }
                bFound = true;
            }
        }
        return bFound;
    }
}

NIMBLE_DEFINE_AVATAR_SIMULATION(RangeMove, 0.1f)
{
    static const int MoveForward = NimbleEngine::FindInputAxis("Range", "MoveForward");
    static const int MoveRight   = NimbleEngine::FindInputAxis("Range", "MoveRight");
    static const int LookX       = NimbleEngine::FindInputAxis("Range", "LookX");
    static const int LookY       = NimbleEngine::FindInputAxis("Range", "LookY");
    static const int RunAction   = NimbleEngine::FindInputAction("Range", "Run");
    static const int JumpAction  = NimbleEngine::FindInputAction("Range", "Jump");
    static const int AimAction   = NimbleEngine::FindInputAction("Range", "Aim");

    GameCore::Entity Avatar = InWorld.GetEntity(InAvatar);
    if(InFrame.Seconds <= 0.0f) { return; }
    /* First: adding it may move the avatar's other components. */
    RangeMoverComponent* Mover = Avatar.GetComponent<RangeMoverComponent>();
    if(!Mover) { Mover = Avatar.AddComponent<RangeMoverComponent>(); }
    GameCore::TransformComponent* Body = Avatar.GetComponent<GameCore::TransformComponent>();
    if(!Mover || !Body)
    {
        return;
    }
    RenderCore::AnimationComponent* Animation = Avatar.GetComponent<RenderCore::AnimationComponent>();
    MoverState S = LoadMover(*Mover);
    const double Dt = double(InFrame.Seconds);
    if(!S.bStarted)
    {
        S.BodyYaw   = double(Body->Rotation.GetY());   // where it was placed: the Player Start's facing
        S.CameraYaw = S.BodyYaw;
        S.bStarted  = true;
    }
    double BodyYaw = S.BodyYaw;
    const auto Axis = [&InFrame](const int InIndex) { return InIndex >= 0 ? double(InFrame.Axes[InIndex]) : 0.0; };
    const auto Down = [&InFrame](const int InIndex) { return InIndex >= 0 && (InFrame.Down & (1u << InIndex)) != 0; };
    const auto Pressed = [&InFrame](const int InIndex) { return InIndex >= 0 && (InFrame.Pressed & (1u << InIndex)) != 0; };

    // --- The view, and the scope -------------------------------------------------
    const bool bArmed  = Animation && Animation->GetBool("Armed");
    const bool bAiming = bArmed && Down(AimAction);
    const double Sensitivity = LookScale(S);
    S.CameraYaw = WrapDegrees(S.CameraYaw + Axis(LookX) * Sensitivity);
    S.Pitch     = std::clamp(S.Pitch + Axis(LookY) * Sensitivity, MinPitch, MaxPitch);
    const double ScopeStep = ScopeRate * Dt;
    S.Scope = std::clamp(S.Scope + (bAiming ? ScopeStep : -ScopeStep), 0.0, 1.0);
    const double Ease = S.Scope * S.Scope * (3.0 - 2.0 * S.Scope);

    // --- Walking -----------------------------------------------------------------
    double Forward = Axis(MoveForward), Right = Axis(MoveRight);
    const double Amount = std::sqrt(Forward * Forward + Right * Right);
    if(Amount > 1.0) { Forward /= Amount; Right /= Amount; }
    const double CY = S.CameraYaw * DegToRad;
    const double WishX = std::sin(CY) * Forward + std::cos(CY) * Right;
    const double WishZ = std::cos(CY) * Forward - std::sin(CY) * Right;
    const double Top = bAiming ? AimSpeed : (Down(RunAction) ? RunSpeed : WalkSpeed);
    const double TargetX = WishX * Top, TargetZ = WishZ * Top;
    const double DVX = TargetX - S.VelX, DVZ = TargetZ - S.VelZ;
    const double DV = std::sqrt(DVX * DVX + DVZ * DVZ);
    const double Rate = (Amount > 0.05 ? Accel : Decel) * (S.bGrounded ? 1.0 : 0.25) * Dt;
    if(DV <= Rate) { S.VelX = TargetX; S.VelZ = TargetZ; }
    else           { S.VelX += DVX / DV * Rate; S.VelZ += DVZ / DV * Rate; }

    // --- Turning: to the view when armed, else to where it walks ------------------
    const double Speed = std::sqrt(S.VelX * S.VelX + S.VelZ * S.VelZ);
    double Facing = BodyYaw;
    if(bArmed)             { Facing = S.CameraYaw; }
    else if(Speed > 20.0)  { Facing = std::atan2(S.VelX, S.VelZ) / DegToRad; }
    const double Turn = WrapDegrees(Facing - BodyYaw);
    const double Step = TurnRate * Dt * (bArmed ? 1.6 : 1.0);
    BodyYaw = WrapDegrees(BodyYaw + std::clamp(Turn, -Step, Step));

    // --- Moving: across, then up and down -------------------------------------------
    double X = Body->Position.GetX(), Y = Body->Position.GetY(), Z = Body->Position.GetZ();
    SlideMove(InWorld, X, Y, Z, S.VelX * Dt, S.VelZ * Dt);
    PushOut(InWorld, X, Y, Z);

    if(S.bGrounded && Pressed(JumpAction))
    {
        S.VelY = JumpSpeed;
        S.bGrounded = false;
        if(Animation) { Animation->SetTrigger("Jump"); }
    }
    S.VelY -= Gravity * Dt;
    double NewY = Y + S.VelY * Dt;
    if(S.VelY > 0.0)
    {
        NimbleEngine::PhysicsRaycastHit Hit;
        if(Ray(InWorld, { X, Y + Height - 10.0, Z }, { 0.0, 1.0, 0.0 }, S.VelY * Dt + 10.0, Hit))
        {
            NewY = Y;
            S.VelY = 0.0;
        }
    }
    const double Reach = StepUp + std::max(0.0, Y - NewY) + (S.bGrounded ? SnapDown : 0.0);
    double Ground = 0.0;
    if(S.VelY <= 0.0 && FindGround(InWorld, X, Y + StepUp, Z, Reach, Ground))
    {
        NewY = Ground;
        S.VelY = 0.0;
        S.bGrounded = true;
        S.AirTime = 0.0;
    }
    else
    {
        S.bGrounded = false;
        S.AirTime += Dt;
    }
    if(NewY < -2000.0) { NewY = 0.0; S.VelY = 0.0; }   // out of the world: back on the floor

    Body->Position = EngineCore::Vector3(X, NewY, Z);
    S.BodyYaw = BodyYaw;
    Body->Rotation = EngineCore::Vector3(0.0, WrapDegrees(BodyYaw + MeshYaw), 0.0);

    // --- The anim state ---------------------------------------------------------------
    if(Animation)
    {
        Animation->SetFloat("Speed", float(Speed));
        Animation->SetBool("Grounded", S.bGrounded || S.AirTime < 0.12);
        /* The spine pitches with the view (the anim state's look-at): its angle is up-positive, the camera's down. */
        Animation->SetFloat("AimPitch", float(-S.Pitch));
    }

    StoreMover(S, *Mover);

    // --- The camera: the boom turned to the view, the camera kept out of walls ------------
    const GameCore::EntityID BoomId = FindChild(InWorld, InAvatar, "CameraBoom");
    if(!BoomId.IsSet()) { return; }
    if(GameCore::TransformComponent* Boom = InWorld.GetEntity(BoomId).GetComponent<GameCore::TransformComponent>())
    {
        Boom->Position = EngineCore::Vector3(0.0, BoomHeight, 0.0);
        Boom->Rotation = EngineCore::Vector3(S.Pitch, WrapDegrees(S.CameraYaw - BodyYaw - MeshYaw), 0.0);
    }
    const GameCore::EntityID CameraId = FindChild(InWorld, BoomId, "Camera");
    if(GameCore::TransformComponent* Camera = CameraId.IsSet() ? InWorld.GetEntity(CameraId).GetComponent<GameCore::TransformComponent>()
                                                                : nullptr)
    {
        /* Where the camera would be, in the world: back and right of the boom along the view. */
        const double P = S.Pitch * DegToRad;
        const Vec Fwd   { std::sin(CY) * std::cos(P), -std::sin(P), std::cos(CY) * std::cos(P) };
        const Vec RightV { std::cos(CY), 0.0, -std::sin(CY) };
        const Vec UpV    { RightV.Y * Fwd.Z - RightV.Z * Fwd.Y, RightV.Z * Fwd.X - RightV.X * Fwd.Z, RightV.X * Fwd.Y - RightV.Y * Fwd.X };
        const Vec Pivot { X, NewY + BoomHeight, Z };
        const double Back = Lerp(CameraBack, ScopeBack, Ease), Side = Lerp(CameraRight, ScopeRight, Ease);
        const double Up   = Lerp(CameraUp, ScopeUp, Ease);
        Vec Off { RightV.X * Side + UpV.X * Up - Fwd.X * Back,
                  RightV.Y * Side + UpV.Y * Up - Fwd.Y * Back,
                  RightV.Z * Side + UpV.Z * Up - Fwd.Z * Back };
        const double Length = std::sqrt(Off.X * Off.X + Off.Y * Off.Y + Off.Z * Off.Z);
        double Scale = 1.0;
        NimbleEngine::PhysicsRaycastHit Hit;
        if(Ray(InWorld, Pivot, { Off.X / Length, Off.Y / Length, Off.Z / Length }, Length + 15.0, Hit))
        {
            Scale = std::clamp((Hit.Distance - 15.0) / Length, 0.1, 1.0);
        }
        Camera->Position = EngineCore::Vector3(Side * Scale, Up * Scale, -Back * Scale);
        if(RenderCore::CameraComponent* Lens = InWorld.GetEntity(CameraId).GetComponent<RenderCore::CameraComponent>())
        {
            Lens->FovY = float(Lerp(FovNormal, FovScope, Ease));
        }
        Mover->CameraX = float(Pivot.X + Off.X * Scale);
        Mover->CameraY = float(Pivot.Y + Off.Y * Scale);
        Mover->CameraZ = float(Pivot.Z + Off.Z * Scale);
        Camera->Rotation = EngineCore::Vector3(0.0, 0.0, 0.0);
    }
}


// === The walker, for other modes (WorldsLabWalker.h) =================================

GAMECORE_DEFINE_COMPONENT(RangeMoverComponent, "RangeMover")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(RangeMoverComponent, 1)
{
    Ar.Field("BodyYaw",   C.BodyYaw);
    Ar.Field("CameraYaw", C.CameraYaw);
    Ar.Field("Pitch",     C.Pitch);
    Ar.Field("Scope",     C.Scope);
    Ar.Field("VelX",      C.VelX);
    Ar.Field("VelY",      C.VelY);
    Ar.Field("VelZ",      C.VelZ);
    Ar.Field("AirTime",   C.AirTime);
    Ar.Flag("Grounded",   C.bGrounded);
    Ar.Flag("Started",    C.bStarted);
}

/* Quantized (ComponentReplication.h); the avatar system snaps them after
 * every step on both ends, so prediction stays exact. The view and the
 * velocity are Smooth() for everyone else: another player's spine pitch and
 * stride (the anim state follows them) blend between updates as its
 * Transform does. */
GAMECORE_DEFINE_COMPONENT_REPLICATION(RangeMoverComponent)
{
    Rep.Field("BodyYaw",   &RangeMoverComponent::BodyYaw).Angles().Smooth();
    Rep.Field("CameraYaw", &RangeMoverComponent::CameraYaw).Angles().Smooth();
    Rep.Field("Pitch",     &RangeMoverComponent::Pitch).Precision(0.01f).Smooth();
    Rep.Field("Scope",     &RangeMoverComponent::Scope).Precision(0.001f);
    Rep.Field("VelX",      &RangeMoverComponent::VelX).Precision(0.01f).Smooth();
    Rep.Field("VelY",      &RangeMoverComponent::VelY).Precision(0.01f);
    Rep.Field("VelZ",      &RangeMoverComponent::VelZ).Precision(0.01f).Smooth();
    Rep.Field("AirTime",   &RangeMoverComponent::AirTime).Precision(0.001f);
    Rep.Flag("Grounded",   &RangeMoverComponent::bGrounded);
    Rep.Flag("Started",    &RangeMoverComponent::bStarted);
}

namespace WorldsLabWalker
{
    void Forward(const double InPitch, const double InYaw, double& OutX, double& OutY, double& OutZ)
    {
        const double P = InPitch * DegToRad, Y = InYaw * DegToRad;
        OutX = std::sin(Y) * std::cos(P);
        OutY = -std::sin(P);
        OutZ = std::cos(Y) * std::cos(P);
    }

    double WrapDegrees(double InDegrees)
    {
        InDegrees = std::fmod(InDegrees + 180.0, 360.0);
        return (InDegrees < 0.0 ? InDegrees + 360.0 : InDegrees) - 180.0;
    }

    GameCore::EntityID FindChild(GameCore::World& InWorld, const GameCore::EntityID InParent, const char* InName)
    {
        for(GameCore::EntityID C = InWorld.GetEntity(InParent).GetFirstChild(); C.IsSet(); C = InWorld.GetEntity(C).GetNextSibling())
        {
            if(std::strcmp(InWorld.GetEntity(C).GetName(), InName) == 0) { return C; }
        }
        return GameCore::EntityID();
    }

    void ApplyLookSensitivity(NimbleEngine::InputSnapshot& InOutInput)
    {
        static const int LookX = NimbleEngine::FindInputAxis("Range", "LookX");
        static const int LookY = NimbleEngine::FindInputAxis("Range", "LookY");
        const float Sensitivity = GRangeLookSensitivity.GetFloat();
        if(LookX >= 0) { InOutInput.Axes[LookX] *= Sensitivity; }
        if(LookY >= 0) { InOutInput.Axes[LookY] *= Sensitivity; }
    }

    void PlaceWalker(GameCore::World& InWorld, const GameCore::EntityID InAvatar, const double InX, const double InY, const double InZ,
                     const double InYaw)
    {
        GameCore::Entity Avatar = InWorld.GetEntity(InAvatar);
        RangeMoverComponent* Mover = Avatar.GetComponent<RangeMoverComponent>();
        if(!Mover) { Mover = Avatar.AddComponent<RangeMoverComponent>(); }
        if(Mover)
        {
            Mover->BodyYaw   = float(WrapDegrees(InYaw));
            Mover->CameraYaw = Mover->BodyYaw;
            Mover->Pitch     = 8.0f;
            Mover->Scope     = 0.0f;
            Mover->VelX = Mover->VelY = Mover->VelZ = 0.0f;
            Mover->AirTime   = 0.0f;
            Mover->bGrounded = 0;
            Mover->bStarted  = 1;
        }
        if(GameCore::TransformComponent* Body = Avatar.GetComponent<GameCore::TransformComponent>())
        {
            Body->Position = EngineCore::Vector3(InX, InY, InZ);
            Body->Rotation = EngineCore::Vector3(0.0, WrapDegrees(InYaw + MeshYaw), 0.0);
        }
    }
}

// === Autoplay ====================================================================

namespace
{
    /* One step of the autoplay: wait, press an action, hold or let go of one,
     * turn the view to (Yaw, Pitch), or walk to (X, Z). */
    struct AutoStep
    {
        enum EKind { Wait, Press, Hold, Release, Face, Goto, AimAt } Kind;
        const char* Action = nullptr;
        double A = 0.0, B = 0.0;   // seconds; or yaw, pitch; or x, z (AimAt: x, z, and C the height)
        bool   bRun = false;
        double C = 0.0;
    };

    const AutoStep GAutoPlay[] = {
        { AutoStep::Wait, nullptr, 1.0 },
        { AutoStep::AimAt, nullptr, -100.0, 700.0, false, 128.0 },   // the lane 3 plate (the start is lane 3), 24 m off
        { AutoStep::Press, "Holster" }, { AutoStep::Wait, nullptr, 1.4 },
        { AutoStep::Hold, "Aim" }, { AutoStep::Wait, nullptr, 0.4 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.5 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.5 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.5 },
        { AutoStep::AimAt, nullptr, -165.0, 500.0, false, 135.0 },    // the lane 3 dummy's chest: three shots drop it (a ragdoll)
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.4 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.4 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 2.0 },
        { AutoStep::AimAt, nullptr, -300.0, 1100.0, false, 124.0 },  // the lane 2 paper, 28 m (clear of the booth divider)
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.5 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.5 },
        { AutoStep::Press, "Reload" }, { AutoStep::Wait, nullptr, 2.0 },
        { AutoStep::AimAt, nullptr, -165.0, 500.0, false, 178.0 },   // its replacement (RangeDummy's graph respawns it): one to the head
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 2.0 },
        { AutoStep::Release, "Aim" },
        { AutoStep::Press, "Holster" }, { AutoStep::Wait, nullptr, 1.2 },
        { AutoStep::Goto, nullptr, -1415.0, -2430.0, true },    // the foot of the stairs
        { AutoStep::Goto, nullptr, -1415.0, -1680.0 },          // up them
        { AutoStep::Goto, nullptr, -1405.0, -1000.0 },          // across the mezzanine onto the west walk
        { AutoStep::Goto, nullptr, -1405.0, 400.0, true },
        { AutoStep::Goto, nullptr, -150.0, 400.0 },             // out over the range
        { AutoStep::Face, nullptr, 0.0, 22.0 },
        { AutoStep::Press, "Holster" }, { AutoStep::Wait, nullptr, 1.4 },
        { AutoStep::Hold, "Aim" },
        { AutoStep::AimAt, nullptr, -300.0, 1500.0, false, 128.0 },  // the lane 2 plate, from above
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.6 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.6 },
        { AutoStep::AimAt, nullptr, -100.0, 1100.0, false, 124.0 },  // the lane 3 paper, below
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.6 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.6 },
        { AutoStep::Wait, nullptr, 3.0 },
    };

    struct AutoPlayer
    {
        size_t Step = 0;
        double StepTime = 0.0;
        double Clock = 0.0;
        double LastDt = 1.0 / 60.0;
        std::chrono::steady_clock::time_point Last;
        uint32_t Held = 0;
        bool bStarted = false;
        bool bDone = false;
    };

    constexpr const char* PauseMenuPath = "Content/UI/ShootingRange/PauseMenu.nmui";
    constexpr const char* SettingsPath  = "Content/UI/ShootingRange/Settings.nmui";

    /*
     * With the mouse free (the pause menu closed by its Back, a window
     * switch), the gun takes nothing: Fire, Holster and Reload are dropped
     * here, on the process that drives the shooter, before the frame is sent
     * -- so a server never fires on the click that takes the mouse back. That
     * click takes it back (unless a menu is open, which has the mouse).
     */
    void FreeMouseFilter(NimbleEngine::GameSession& InSession, NimbleEngine::InputSnapshot& InOutInput)
    {
        if(InSession.IsMouseCaptured()) { return; }
        static const auto Bit = [](const char* InAction) { const int I = NimbleEngine::FindInputAction("Range", InAction); return I >= 0 ? 1u << I : 0u; };
        static const uint32_t Fire = Bit("Fire");
        static const uint32_t Gun  = Fire | Bit("Holster") | Bit("Reload");
        const bool bClick = (InOutInput.Pressed & Fire) != 0;
        InOutInput.Down     &= ~Gun;
        InOutInput.Pressed  &= ~Gun;
        InOutInput.Released &= ~Gun;
        if(bClick && !InSession.GetUI().IsOpen(PauseMenuPath) && !InSession.GetUI().IsOpen(SettingsPath))
        {
            InSession.SetMouseCaptured(true);
        }
    }

    /* RangeAutoPlay's frame of the shooter's input: the next of GAutoPlay's
     * steps, from Auto (the mode's, WorldsLabRangeMode::FilterInput). */
    void AutoPlay(GameCore::World& InWorld, GameCore::EntityID InAvatar, AutoPlayer& Auto, NimbleEngine::InputSnapshot& InOutInput)
    {
        if(Auto.bDone) { return; }
        const auto Now = std::chrono::steady_clock::now();
        const double Dt = Auto.bStarted ? std::clamp(std::chrono::duration<double>(Now - Auto.Last).count(), 0.0, 0.1) : 0.0;
        Auto.Last = Now;
        Auto.bStarted = true;
        Auto.StepTime += Dt;
        InOutInput = NimbleEngine::InputSnapshot();
        InOutInput.Down = Auto.Held;

        const auto Bit = [](const char* InAction) { const int I = NimbleEngine::FindInputAction("Range", InAction); return I >= 0 ? 1u << I : 0u; };
        static const int LookX = NimbleEngine::FindInputAxis("Range", "LookX");
        static const int LookY = NimbleEngine::FindInputAxis("Range", "LookY");
        static const int MoveForward = NimbleEngine::FindInputAxis("Range", "MoveForward");

        const RangeMoverComponent* MoverC = InWorld.GetEntity(InAvatar).GetComponent<RangeMoverComponent>();
        const MoverState Mover = MoverC ? LoadMover(*MoverC) : MoverState();
        const GameCore::TransformComponent* Body = InWorld.GetEntity(InAvatar).GetComponent<GameCore::TransformComponent>();
        if(Auto.Step >= sizeof(GAutoPlay) / sizeof(GAutoPlay[0]) || !Body)
        {
            Auto.bDone = true;
            CORE_LOG("[ShootingRange] autoplay done")
            return;
        }
        const AutoStep& Step = GAutoPlay[Auto.Step];
        bool bNext = false;
        /* Turns the view toward (yaw, pitch) at 150 degrees/s; true once there. */
        const auto Turn = [&](const double InYaw, const double InPitch)
        {
            const double DYaw = WrapDegrees(InYaw - Mover.CameraYaw), DPitch = InPitch - Mover.Pitch;
            const double Most = std::max(150.0 * std::max(Dt, 1.0 / 240.0), 0.05);
            if(LookX >= 0) { InOutInput.Axes[LookX] = float(std::clamp(DYaw, -Most, Most) / std::max(LookScale(Mover), 0.01)); }
            if(LookY >= 0) { InOutInput.Axes[LookY] = float(std::clamp(DPitch, -Most, Most) / std::max(LookScale(Mover), 0.01)); }
            return std::fabs(DYaw) < 0.3 && std::fabs(DPitch) < 0.3;
        };
        switch(Step.Kind)
        {
            case AutoStep::Wait:    bNext = Auto.StepTime >= Step.A; break;
            case AutoStep::Press:   InOutInput.Pressed |= Bit(Step.Action); InOutInput.Down |= Bit(Step.Action); bNext = true; break;
            case AutoStep::Hold:    Auto.Held |= Bit(Step.Action); InOutInput.Down |= Bit(Step.Action); bNext = true; break;
            case AutoStep::Release: Auto.Held &= ~Bit(Step.Action); InOutInput.Down &= ~Bit(Step.Action); bNext = true; break;
            case AutoStep::Face:    bNext = Turn(Step.A, Step.B) || Auto.StepTime > 4.0; break;
            case AutoStep::AimAt:
            {
                /* From where the camera is: what the crosshair (the screen's centre) covers. */
                const double DX = Step.A - Mover.CameraX, DY = Step.C - Mover.CameraY, DZ = Step.B - Mover.CameraZ;
                const double Yaw   = std::atan2(DX, DZ) / DegToRad;
                const double Pitch = -std::atan2(DY, std::sqrt(DX * DX + DZ * DZ)) / DegToRad;
                bNext = (Turn(Yaw, Pitch) && Auto.StepTime > 0.3) || Auto.StepTime > 4.0;
                break;
            }
            case AutoStep::Goto:
            {
                const double DX = Step.A - Body->Position.GetX(), DZ = Step.B - Body->Position.GetZ();
                const double Distance = std::sqrt(DX * DX + DZ * DZ);
                const double Yaw = std::atan2(DX, DZ) / DegToRad;
                Turn(Yaw, 10.0);
                const bool bFacing = std::fabs(WrapDegrees(Yaw - Mover.CameraYaw)) < 25.0;
                if(MoveForward >= 0 && bFacing) { InOutInput.Axes[MoveForward] = float(std::clamp(Distance / 120.0, 0.3, 1.0)); }
                if(Step.bRun && Distance > 200.0) { InOutInput.Down |= Bit("Run"); }
                bNext = Distance < 35.0 || Auto.StepTime > 25.0;
                if(Auto.StepTime > 25.0) { CORE_LOG_WARN("[ShootingRange] autoplay: gave up walking to a waypoint (stuck)") }
                break;
            }
        }
        if(bNext)
        {
            ++Auto.Step;
            Auto.StepTime = 0.0;
        }
    }
}

// === The UI ====================================================================

namespace
{

    /* Escape: the menu opens over the game, the game stops, the cursor comes back. */
    void OpenPauseMenu(NimbleEngine::GameSession& InSession)
    {
        if(InSession.GetUI().IsOpen(PauseMenuPath)) { return; }
        InSession.GetUI().Open(PauseMenuPath);
        InSession.SetPaused(true);
        InSession.SetMouseCaptured(false);
    }

    /* Resume: the same three things, undone. */
    void ClosePauseMenu(NimbleEngine::GameSession& InSession)
    {
        InSession.GetUI().Close(SettingsPath);
        InSession.GetUI().Close(PauseMenuPath);
        InSession.SetPaused(false);
        InSession.SetMouseCaptured(true);
    }

    /* "M:SS", for the HUD. */
    std::string FormatClock(const int InSeconds)
    {
        char Text[16];
        std::snprintf(Text, sizeof(Text), "%d:%02d", InSeconds / 60, InSeconds % 60);
        return Text;
    }
}

/* Native:Range.Resume, for the pause menu's Resume button and its Back. HostData is the GameSession. */
UI_DEFINE_ACTIONS(Range)
{
    Actions.Add("Resume", [](const UI::UIActionContext& InContext)
    {
        if(auto* Session = static_cast<NimbleEngine::GameSession*>(InContext.HostData)) { ClosePauseMenu(*Session); }
    }, "Closes the pause menu, unpauses and takes the mouse back");
}

// === The shooter's gun, as the HUD shows it =====================================

/*
 * "RangeGun": what the HUD shows of the shooter's pistol. RangeShooter.lua keeps
 * it, on the authority (a spawned prefab's script runs there only); it
 * replicates, and every process with a player feeds its HUD from its own
 * avatar's (TickHud) -- so a Play Together client sees its own ammo and score.
 */
struct RangeGunComponent
{
    GAMECORE_COMPONENT_BODY()

    int32_t Mag      = 17;
    int32_t Capacity = 17;     // a full magazine, for the HUD's bar
    int32_t Reserve  = 68;
    int32_t Shots    = 0;
    int32_t Hits     = 0;
    uint8_t bDrawn   = 0;
    uint8_t bReady   = 0;      // the hand has the grip: the crosshair shows
    char    Status[32] = "";   // "RELOADING", "HEADSHOT", ...

    /* The pause menu's Reset Score, from the shooter's own process. */
    void ServerResetScore(const GameCore::RpcContext& InContext);

    /* What the script decided, shown and heard on every process with a player
     * (the authority's script calls gun:Shot / gun:Sound): the shot's flash and
     * report, and at InAt -- reached along InDir -- what InHit (ERangeHit) did. InKick
     * (kg cm/s along InDir) is what the authority's physics took -- a plate's
     * swing, a dummy's fall; the rest apply it to their own copy (ReactHere). */
    void MulticastShot(const GameCore::RpcContext& InContext, const EngineCore::Vector3& InAt, const EngineCore::Vector3& InDir, uint8_t InHit,
                       float InKick);
    /* The gun's other sounds (ERangeSound). */
    void MulticastSound(const GameCore::RpcContext& InContext, uint8_t InSound);
};

/*
 * "RangeTarget": a target dummy's wounds, on the dummy -- one count however
 * many shooters hit it (RangeShooter.lua adds it on the first hit). Kept on
 * the authority, where the scripts run; not replicated.
 */
struct RangeTargetComponent
{
    GAMECORE_COMPONENT_BODY()

    int32_t Wounds = 0;   // body shots taken while standing; a head shot is all of them
};

/* What a shot hit, for its effect and sound (gun:Shot's last argument). */
enum ERangeHit : uint8_t { RangeHit_Nothing = 0, RangeHit_Steel, RangeHit_Dummy, RangeHit_Paper, RangeHit_Other,
                          RangeHit_DummyDown };   // the shot that put a dummy down
/* gun:Sound's argument. */
enum ERangeSound : uint8_t { RangeSound_Reload = 0, RangeSound_DryFire };

GAMECORE_DEFINE_COMPONENT(RangeTargetComponent, "RangeTarget")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(RangeTargetComponent, 1)
{
    Ar.Field("Wounds", C.Wounds);
}

/* target:GetWounds() / target:SetWounds(n). */
GAMECORE_DEFINE_COMPONENT_SCRIPT(RangeTargetComponent)
{
    Bind.Field("Wounds", &RangeTargetComponent::Wounds);
}

GAMECORE_DEFINE_COMPONENT(RangeGunComponent, "RangeGun")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(RangeGunComponent, 1)
{
    Ar.Field("Mag",      C.Mag);
    Ar.Field("Capacity", C.Capacity);
    Ar.Field("Reserve",  C.Reserve);
    Ar.Field("Shots",    C.Shots);
    Ar.Field("Hits",     C.Hits);
    Ar.Flag("Drawn",     C.bDrawn);
    Ar.Flag("Ready",     C.bReady);
    Ar.Field("Status",   C.Status);
}

GAMECORE_DEFINE_COMPONENT_REPLICATION(RangeGunComponent)
{
    Rep.Field("Mag",      &RangeGunComponent::Mag);
    Rep.Field("Capacity", &RangeGunComponent::Capacity);
    Rep.Field("Reserve",  &RangeGunComponent::Reserve);
    Rep.Field("Shots",    &RangeGunComponent::Shots);
    Rep.Field("Hits",     &RangeGunComponent::Hits);
    Rep.Flag("Drawn",     &RangeGunComponent::bDrawn);
    Rep.Flag("Ready",     &RangeGunComponent::bReady);
    Rep.Field("Status",   &RangeGunComponent::Status);
}

GAMECORE_DEFINE_COMPONENT_RPCS(RangeGunComponent)
{
    Rpc.Server<&RangeGunComponent::ServerResetScore>("ServerResetScore").RateLimit(2);
    /* Stale a moment later, so sent once. */
    Rpc.Multicast<&RangeGunComponent::MulticastShot>("MulticastShot").Unreliable();
    Rpc.Multicast<&RangeGunComponent::MulticastSound>("MulticastSound").Unreliable();
}

/* RangeShooter.lua's view: gun:GetMag() / gun:SetMag(n) and the rest; Status as text. */
GAMECORE_DEFINE_COMPONENT_SCRIPT(RangeGunComponent)
{
    Bind.Field("Mag",      &RangeGunComponent::Mag);
    Bind.Field("Capacity", &RangeGunComponent::Capacity);
    Bind.Field("Reserve",  &RangeGunComponent::Reserve);
    Bind.Field("Shots",    &RangeGunComponent::Shots);
    Bind.Field("Hits",     &RangeGunComponent::Hits);
    Bind.Flag("Drawn",     &RangeGunComponent::bDrawn);
    Bind.Flag("Ready",     &RangeGunComponent::bReady);
    Bind.Method("GetStatus", [](RangeGunComponent& C, GameCore::ScriptCall& Call) { Call.PushString(C.Status); });
    Bind.Method("SetStatus", [](RangeGunComponent& C, GameCore::ScriptCall& Call)
    {
        std::snprintf(C.Status, sizeof(C.Status), "%s", Call.GetString(1));
    });
    /* gun:Shot(hx, hy, hz, dx, dy, dz, hit, kick): the shot, for every process to show (MulticastShot). */
    Bind.Method("Shot", [](RangeGunComponent& C, GameCore::ScriptCall& Call)
    {
        if(!C.Owner.OwningWorld) { return; }
        GAMECORE_RPC(RangeGunComponent, MulticastShot)(C.Owner.OwningWorld->GetEntity(C.Owner.Entity), Call.GetVector3(1),
                                                       Call.GetVector3(4), uint8_t(Call.GetInt(7)), Call.GetFloat(8));
    });
    /* gun:Sound(n): 0 the reload, 1 a dry fire (MulticastSound). */
    Bind.Method("Sound", [](RangeGunComponent& C, GameCore::ScriptCall& Call)
    {
        if(!C.Owner.OwningWorld) { return; }
        GAMECORE_RPC(RangeGunComponent, MulticastSound)(C.Owner.OwningWorld->GetEntity(C.Owner.Entity), uint8_t(Call.GetInt(1)));
    });
}

void RangeGunComponent::ServerResetScore(const GameCore::RpcContext&)
{
    Shots = 0;
    Hits  = 0;
    /* The script's own tally (its kills, the dummies' wounds) hears it here, on the authority. */
    if(GameCore::World* World = Owner.OwningWorld)
    {
        GameCore::EventArgs None;
        World->GetEvents().Emit("Range.ScoreReset", None);
    }
}

namespace
{
    constexpr const char* RangeSounds    = "Content/Sounds/ShootingRange/";
    constexpr const char* RangeParticles = "Content/Particles/ShootingRange/";

    /* A process that shows the game: not a dedicated server. */
    bool IsPresentation() { return !NimbleEngine::IsDedicatedServer(); }

    /* What this process has shown, for autoplay's report: the mode's. */
    struct RangeShown
    {
        int Shots = 0, Sounds = 0, PistolDraws = 0;
        int Hits[6] = {};                                    // by ERangeHit
        int PlatesKicked = 0, DummiesDowned = 0, BodiesKicked = 0;   // ReactHere's, on a client
    };

    /* A sound from InShooter's gun: in the ear of its own player, from where it stands for everyone else. */
    void PlayGunSound(GameCore::World& InWorld, const GameCore::EntityID InShooter, const char* InName, const float InVolume)
    {
        const std::string Path = std::string(RangeSounds) + InName;
        if(InShooter == NimbleEngine::GetLocalAvatar(InWorld, 0))
        {
            NimbleEngine::PlaySound2D(InWorld, Path.c_str(), InVolume);
        }
        else
        {
            NimbleEngine::PlaySoundAttached(InWorld, Path.c_str(), InShooter, InVolume);
        }
    }

    /* Where the pistol sits: holstered on the spine, or in the right hand (the
     * Blender file's Child Of offsets). Every process with a player poses every
     * shooter's pistol from its RangeGun (PoseShooters). */
    struct PistolSeat { const char* Bone; EngineCore::Vector3 Position, Rotation; };
    const PistolSeat HolsterSeat{ "DEF-spine",  EngineCore::Vector3(-19.719f, 0.128f, -4.773f), EngineCore::Vector3(-75.407f,  7.744f,   2.009f) };
    const PistolSeat HandSeat   { "DEF-hand.R", EngineCore::Vector3( -2.110f, 7.087f, -0.769f), EngineCore::Vector3(-85.592f, 13.264f, 175.197f) };

    void PoseShooters(GameCore::World& InWorld, RangeShown& InOutShown)
    {
        InWorld.Each<RangeGunComponent>([&InWorld, &InOutShown](GameCore::Entity InShooter, RangeGunComponent& InGun)
        {
            if(auto* Animation = InShooter.GetComponent<RenderCore::AnimationComponent>())
            {
                if(Animation->GetBool("Armed") != (InGun.bDrawn != 0)) { Animation->SetBool("Armed", InGun.bDrawn != 0); }
            }
            const GameCore::EntityID Pistol = WorldsLabWalker::FindChild(InWorld, InShooter.GetID(), "Pistol");
            auto* Socket = Pistol.IsSet() ? InWorld.GetEntity(Pistol).GetComponent<RenderCore::AnimSocketComponent>() : nullptr;
            if(!Socket) { return; }
            const PistolSeat& Seat = InGun.bReady ? HandSeat : HolsterSeat;
            if(&Seat == &HandSeat && std::strcmp(Socket->Bone, Seat.Bone) != 0) { ++InOutShown.PistolDraws; }
            if(std::strcmp(Socket->Bone, Seat.Bone) == 0) { return; }
            std::snprintf(Socket->Bone, sizeof(Socket->Bone), "%s", Seat.Bone);
            Socket->PositionOffset = Seat.Position;
            Socket->RotationOffset = Seat.Rotation;
        });
    }

    /* InShooter's muzzle in the world, here: its pistol is posed here (PoseShooters). */
    EngineCore::Vector3 MuzzleOf(GameCore::World& InWorld, const GameCore::EntityID InShooter)
    {
        const GameCore::EntityID Pistol = WorldsLabWalker::FindChild(InWorld, InShooter, "Pistol");
        const GameCore::EntityID Muzzle = Pistol.IsSet() ? WorldsLabWalker::FindChild(InWorld, Pistol, "Muzzle") : GameCore::EntityID();
        if(Muzzle.IsSet()) { return InWorld.GetEntity(Muzzle).GetWorldPose().Position; }
        return InWorld.GetEntity(InShooter).GetWorldPose().Position + EngineCore::Vector3(0.0f, 140.0f, 0.0f);
    }
}

namespace
{
    /* What the script's rounds see: every layer but 2, the catwalks' grating (RangeShooter.lua's SHOT_MASK). */
    constexpr int32_t ShotMask = 32767 - 2;

    /*
     * The authority's physics took the shot's kick; here, a client's copy of
     * the level takes it too, so a plate swings and a dummy falls for every
     * player. The target is not networked (it is the level's), so it is found
     * again here: a short ray along the shot onto where it hit.
     */
    void ReactHere(GameCore::World& InWorld, const EngineCore::Vector3& InAt, const EngineCore::Vector3& InDir, const uint8_t InHit,
                   const float InKick, RangeShown& InOutShown)
    {
        if(NimbleEngine::HasAuthority() || InKick <= 0.0f) { return; }   // the authority's script already did
        NimbleEngine::PhysicsRaycastHit Hit;
        if(!NimbleEngine::PhysicsRaycast(InWorld, InAt - InDir * 20.0f, InDir, 40.0f, Hit, ShotMask) || !InWorld.IsAlive(Hit.Entity)) { return; }
        GameCore::Entity Target(&InWorld, Hit.Entity);
        const EngineCore::Vector3 Impulse = InDir * InKick;
        if(InHit == RangeHit_Steel)
        {
            if(auto* Body = Target.GetComponent<GameCore::RigidBodyComponent>()) { Body->AddImpulse(Impulse); ++InOutShown.PlatesKicked; }
            return;
        }
        auto* Ragdoll = Target.GetComponent<NimbleEngine::RagdollComponent>();
        if(!Ragdoll) { return; }
        if(InHit == RangeHit_DummyDown && !Ragdoll->IsActive()) { Ragdoll->ActivateWithImpulse(Impulse, InAt); ++InOutShown.DummiesDowned; }
        else if(Ragdoll->IsActive())                            { Ragdoll->AddImpulse(Impulse, InAt); ++InOutShown.BodiesKicked; }
    }
}


namespace
{
    constexpr const char* HudPath = "Content/UI/ShootingRange/RangeHUD.nmui";

    /* This process's avatar's gun, if it has a player driving a shooter. */
    RangeGunComponent* LocalGun(GameCore::World& InWorld)
    {
        const GameCore::EntityID Avatar = NimbleEngine::GetLocalAvatar(InWorld, 0);
        return Avatar.IsSet() && InWorld.IsAlive(Avatar) ? InWorld.GetEntity(Avatar).GetComponent<RangeGunComponent>() : nullptr;
    }
}

// === The mode ==================================================================

class WorldsLabRangeMode : public NimbleEngine::NetGameMode
{
public:
    /* The shadow atlas: the sun's, and the two lane spots over the targets. */
    static constexpr int32_t ShadowedLights = 3;

    const char* GetAvatarPrefab(GameCore::EntityID) override
    {
        const char* Override = GRangeAvatar.GetString();
        return Override && Override[0] ? Override : nullptr;
    }

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        if(EngineCore::ConfigVar* MaxShadowed = EngineCore::ConfigFindVarByPath("Shadows.MaxShadowedLights"))
        {
            PreviousShadowedLights = MaxShadowed->GetInt();
            MaxShadowed->SetInt(ShadowedLights);
        }
        if(!NimbleEngine::IsPhysicsAvailable())
        {
            CORE_LOG_WARN("[ShootingRange] built without JoltPhysics: the shooter walks on Bounds alone, and badly")
        }
        /* A free mouse fires nothing (FreeMouseFilter); taken back by the session after OnUnload. */
        SetAvatarInputFilter<&WorldsLabRangeMode::FilterInput>();
        return NetGameMode::OnLoad(InSession);
    }

    void OnStart() override
    {
        NetGameMode::OnStart();
        /* Autoplay leaves the mouse alone: it drives itself, and someone may be using the desktop. */
        WorldsLabWalker::ResetMoverDemo();
        if(!GRangeAutoPlay.GetBool() && !WorldsLabWalker::IsMoverDemo()) { Session->SetMouseCaptured(true); }
        CORE_LOG("[ShootingRange] WASD walks (Shift runs), Space jumps, H draws, right mouse aims, left fires, R reloads; "
                 "Escape pauses")

        /* The HUD's clock starts at zero: a string the HUD's {Bind:Range.Time} follows. */
        RunSeconds   = 0.0;
        ShownSeconds = 0;
        Session->GetUI().SetValue("Range.Time", UI::UIValue::String(FormatClock(0)));

        /* The pause menu's Reset Score (PauseMenu.lua) is an event on this
         * process; the tally is the server's (RangeGun), so ask it. */
        Subscribe<&WorldsLabRangeMode::OnResetScore>("Range.ResetScore");
        ShownHud = HudState();
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        if(WorldsLabWalker::TickMoverParity(*Session, ParityFrames)) { return; }

        /* The clock counts game time: OnTick is not called while paused, so the pause menu stops it.
         * The store is written when the shown second changes, not every frame. */
        RunSeconds += InFrame.DeltaSeconds;
        if(const int Seconds = int(RunSeconds); Seconds != ShownSeconds)
        {
            ShownSeconds = Seconds;
            Session->GetUI().SetValue("Range.Time", UI::UIValue::String(FormatClock(Seconds)));
        }

        GameCore::World& World = Session->GetWorld();
        TickHud(World);
        if(IsPresentation())
        {
            PoseShooters(World, Shown);
            TickEffects(World, InFrame.DeltaSeconds);
        }

        /* This process's own shooter only: a client's Escape is its own pause menu, not the server's.
         * (A click with the mouse free takes it back in the input filter, FreeMouseFilter.) */
        World.Each<NimbleEngine::AvatarHarnessComponent>([this](const GameCore::Entity&, NimbleEngine::AvatarHarnessComponent& InHarness)
        {
            if(!InHarness.IsLocallyControlled()) { return; }
            if(GRangeAutoPlay.GetBool()) { return; }
            if(InHarness.WasPressed("Pause")) { OpenPauseMenu(*Session); }
        });
    }

    /* The event handler and input filter go with the session's take-back; the
     * effects with the World. */
    void OnUnload() override
    {
        Session->GetUI().Close(HudPath);
        EngineCore::ConfigVar* MaxShadowed = EngineCore::ConfigFindVarByPath("Shadows.MaxShadowedLights");
        if(MaxShadowed && PreviousShadowedLights >= 0)
        {
            MaxShadowed->SetInt(PreviousShadowedLights);
        }
        PreviousShadowedLights = -1;
    }

    // === For the gun's multicasts (FindGameMode<WorldsLabRangeMode>) ==========

    /* A particle burst, destroyed after InLife seconds (TickEffects). */
    void SpawnEffect(GameCore::World& InWorld, const char* InSystem, const EngineCore::Vector3& InAt, const double InLife)
    {
        GameCore::Entity Fx = InWorld.CreateEntity("RangeEffect");
        GameCore::TransformComponent Transform;
        Transform.Position = InAt;
        Fx.AddComponent(Transform);
        RenderCore::ParticleSystemComponent Emitter;
        std::snprintf(Emitter.System, sizeof(Emitter.System), "%s%s.nmparticle", RangeParticles, InSystem);
        Fx.AddComponent(Emitter);
        Effects.push_back({ Fx.GetID(), InLife });
    }

    RangeShown& GetShown() { return Shown; }

private:
    /* What the HUD shows, to write the store only when it changes. */
    struct HudState
    {
        int32_t     Mag = -1, Capacity = 0, Reserve = -1, Shots = -1, Hits = -1;
        bool        bCrosshair = false;
        std::string Status = "\x01";   // nothing shown yet: the first tick writes everything

        bool operator==(const HudState& In) const
        {
            return Mag == In.Mag && Capacity == In.Capacity && Reserve == In.Reserve && Shots == In.Shots
                && Hits == In.Hits && bCrosshair == In.bCrosshair && Status == In.Status;
        }
    };

    /*
     * The HUD, on every process with a player (not a dedicated server): open
     * while this process drives a shooter, fed from its avatar's RangeGun --
     * kept by the script where it runs, replicated to the rest.
     */
    void TickHud(GameCore::World& InWorld)
    {
        if(NimbleEngine::IsDedicatedServer()) { return; }
        UI::UISystem& UI = Session->GetUI();
        const RangeGunComponent* Gun = LocalGun(InWorld);
        if(!Gun)
        {
            if(UI.IsOpen(HudPath)) { UI.Close(HudPath); }
            ShownHud = HudState();
            return;
        }
        if(!UI.IsOpen(HudPath))
        {
            UI.Open(HudPath);
            ShownHud = HudState();
            CORE_LOG(std::string("[ShootingRange] HUD open, from this ") + (NimbleEngine::IsClient() ? "client's" : "process's")
                     + " shooter's RangeGun")
        }
        /* Autoplay's result as the HUD has it -- on a client, what the server's script sent. */
        if(GRangeAutoPlay.GetBool() && Auto.bDone && !bReportedAutoPlay)
        {
            bReportedAutoPlay = true;
            CORE_LOG("[ShootingRange] HUD after autoplay: " + std::to_string(Gun->Mag) + " + " + std::to_string(Gun->Reserve)
                     + " rounds, " + std::to_string(Gun->Hits) + " hits of " + std::to_string(Gun->Shots) + " shots; shown here: "
                     + std::to_string(Shown.Shots) + " shots, " + std::to_string(Shown.Sounds) + " gun sounds, "
                     + std::to_string(Shown.PistolDraws) + " draws; hits shown: " + std::to_string(Shown.Hits[RangeHit_Steel]) + " steel, "
                     + std::to_string(Shown.Hits[RangeHit_Dummy] + Shown.Hits[RangeHit_DummyDown]) + " dummy ("
                     + std::to_string(Shown.Hits[RangeHit_DummyDown]) + " down), " + std::to_string(Shown.Hits[RangeHit_Paper]) + " paper, "
                     + std::to_string(Shown.Hits[RangeHit_Other]) + " other; reacted here: " + std::to_string(Shown.PlatesKicked)
                     + " plates kicked, " + std::to_string(Shown.DummiesDowned) + " dummies down, " + std::to_string(Shown.BodiesKicked) + " bodies kicked")
        }

        HudState Now;
        Now.Mag        = Gun->Mag;
        Now.Capacity   = Gun->Capacity;
        Now.Reserve    = Gun->Reserve;
        Now.Shots      = Gun->Shots;
        Now.Hits       = Gun->Hits;
        Now.bCrosshair = Gun->bDrawn && Gun->bReady;
        Now.Status     = Gun->Status;
        if(Now == ShownHud) { return; }
        ShownHud = Now;

        const auto Text = [&UI](const char* InKey, const std::string& InValue) { UI.SetValue(InKey, UI::UIValue::String(InValue)); };
        Text("Range.Mag", std::to_string(Now.Mag));
        Text("Range.Reserve", std::to_string(Now.Reserve));
        UI.SetValue("Range.MagFill", UI::UIValue::Number(Now.Capacity > 0 ? double(Now.Mag) / double(Now.Capacity) : 0.0));
        Text("Range.Hits", std::to_string(Now.Hits));
        Text("Range.Shots", std::to_string(Now.Shots));
        Text("Range.Accuracy", Now.Shots > 0 ? std::to_string(int(100.0 * Now.Hits / Now.Shots + 0.5)) + "%" : std::string("--"));
        Text("Range.Status", Now.Status);
        if(UI::UIDocumentInstance* Hud = UI.Find(HudPath))
        {
            if(UI::UIWidget* Crosshair = Hud->FindWidget("Crosshair"))
            {
                Crosshair->Visibility = Now.bCrosshair ? UI::EUIVisibility::Visible : UI::EUIVisibility::Collapsed;
            }
        }
    }

    /* Range.ResetScore, from the pause menu on this process: the server zeroes the tally. */
    void OnResetScore(const GameCore::EventArgs&)
    {
        GameCore::World& World = GetWorld();
        if(RangeGunComponent* Gun = LocalGun(World))
        {
            GAMECORE_RPC(RangeGunComponent, ServerResetScore)(World.GetEntity(Gun->Owner.Entity));
        }
    }

    /* Every local player's input, before its shooter takes it: the look
     * sensitivity, then autoplay's steps, or nothing for the gun while the
     * mouse is free. */
    void FilterInput(GameCore::World& InWorld, const GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOutInput)
    {
        if(WorldsLabWalker::MoverDemoInput(GetSession(), InWorld, InAvatar, InOutInput)) { return; }
        WorldsLabWalker::ApplyLookSensitivity(InOutInput);
        if(!GRangeAutoPlay.GetBool()) { FreeMouseFilter(GetSession(), InOutInput); return; }
        AutoPlay(InWorld, InAvatar, Auto, InOutInput);
    }

    void TickEffects(GameCore::World& InWorld, const double InSeconds)
    {
        for(size_t Index = Effects.size(); Index-- > 0;)
        {
            Effect& Each = Effects[Index];
            Each.Left -= InSeconds;
            if(Each.Left > 0.0) { continue; }
            if(InWorld.IsAlive(Each.Entity)) { InWorld.DestroyEntity(Each.Entity); }
            Effects.erase(Effects.begin() + std::ptrdiff_t(Index));
        }
    }

    // === This level's, made and gone with the mode ===========================

    struct Effect { GameCore::EntityID Entity; double Left; };

    AutoPlayer          Auto;      // RangeAutoPlay's progress
    RangeShown          Shown;     // what this process showed, for its report
    std::vector<Effect> Effects;   // particle bursts this process made

    HudState ShownHud;
    bool     bReportedAutoPlay = false;
    int32_t PreviousShadowedLights = -1;
    double  RunSeconds   = 0.0;   // the HUD's clock
    int     ShownSeconds = 0;
    int     ParityFrames = 0;   // [WorldsLab] MoverParity's settling
};

// === The gun's multicasts: every process with a player ===========================

void RangeGunComponent::MulticastShot(const GameCore::RpcContext&, const EngineCore::Vector3& InAt, const EngineCore::Vector3& InDir,
                                      const uint8_t InHit, const float InKick)
{
    GameCore::World* World = Owner.OwningWorld;
    WorldsLabRangeMode* Mode = World ? NimbleEngine::FindGameMode<WorldsLabRangeMode>(*World) : nullptr;
    if(!Mode || !IsPresentation()) { return; }
    const GameCore::EntityID Shooter = Owner.Entity;

    RangeShown& Shown = Mode->GetShown();
    ++Shown.Shots;
    if(InHit < sizeof(Shown.Hits) / sizeof(Shown.Hits[0])) { ++Shown.Hits[InHit]; }
    Mode->SpawnEffect(*World, "MuzzleFlash", MuzzleOf(*World, Shooter), 1.2);
    PlayGunSound(*World, Shooter, "Gunshot.nmsound", 1.0f);
    ReactHere(*World, InAt, InDir, InHit, InKick, Shown);

    /* The effect a little short of the hit, out of the surface. */
    const EngineCore::Vector3 Short = InAt - InDir * 3.0f;
    const auto Impact = [World, Mode, &InAt, &Short](const char* InSystem, const double InLife, const char* InSound, const float InVolume)
    {
        Mode->SpawnEffect(*World, InSystem, Short, InLife);
        NimbleEngine::PlaySoundAt(*World, (std::string(RangeSounds) + InSound).c_str(), InAt, InVolume);
    };
    switch(InHit)
    {
        case RangeHit_Steel:     Impact("Sparks", 1.0, "SteelHit.nmsound", 1.0f); break;
        case RangeHit_Dummy:
        case RangeHit_DummyDown: Impact("Dust",   1.0, "Impact.nmsound",   0.8f); break;
        case RangeHit_Paper:     Impact("Dust",   1.4, "PaperHit.nmsound", 0.7f); break;
        case RangeHit_Other:     Impact("Dust",   1.4, "Impact.nmsound",   0.6f); break;
        default: break;
    }
}

void RangeGunComponent::MulticastSound(const GameCore::RpcContext&, const uint8_t InSound)
{
    GameCore::World* World = Owner.OwningWorld;
    WorldsLabRangeMode* Mode = World ? NimbleEngine::FindGameMode<WorldsLabRangeMode>(*World) : nullptr;
    if(!Mode || !IsPresentation()) { return; }
    ++Mode->GetShown().Sounds;
    if(InSound == RangeSound_Reload)  { PlayGunSound(*World, Owner.Entity, "Reload.nmsound", 0.8f); }
    if(InSound == RangeSound_DryFire) { PlayGunSound(*World, Owner.Entity, "DryFire.nmsound", 0.9f); }
}

NIMBLE_REGISTER_GAME_MODE(WorldsLabRangeMode, "WorldsLabRange")
