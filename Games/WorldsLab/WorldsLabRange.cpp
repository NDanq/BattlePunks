#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

/*
 * WorldsLabRange: the Shooting Range world's mode
 * (Content/Levels/ShootingRange.nmworld, Tools/ShootingRange/). An
 * AvatarGameMode: the level's Player Start spawns the RangeShooter prefab
 * (Content/Prefabs/ShootingRange/RangeShooter.nmprefab) for the local player
 * and possesses it.
 *
 * The shooter moves by RangeMove, an avatar simulation: a third-person walker
 * on the world's physics, made of raycasts (Jolt has no character controller
 * here) --
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
 * Shooting, the HUD and the pistol are the prefab's Lua
 * (Scripts/ShootingRange/RangeShooter.lua). Escape frees the mouse; a
 * click takes it back.
 *
 * [WorldsLab] RangeAutoPlay=1 drives the shooter by itself, through the
 * avatar input filter (what a bot would use): it draws and shoots the plates
 * from the start, reloads, holsters, runs to the stairs, climbs to the
 * mezzanine, walks the west catwalk onto the first cross walk and shoots down
 * range from there -- every path the level has, for testing and a demo.
 */

NIMBLE_CONFIG_FLOAT(GRangeLookSensitivity, "WorldsLab", "RangeLookSensitivity", 1.0f, nullptr);
NIMBLE_CONFIG_BOOL(GRangeAutoPlay,         "WorldsLab", "RangeAutoPlay",        false, nullptr);

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

    /* What RangeMove keeps between frames, per avatar. */
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
    std::unordered_map<uint64_t, MoverState> GMovers;

    struct Vec { double X, Y, Z; };

    bool Ray(GameCore::World& InWorld, const Vec& InFrom, const Vec& InDir, const double InLength,
             NimbleEngine::PhysicsRaycastHit& OutHit)
    {
        return NimbleEngine::PhysicsRaycast(InWorld, EngineCore::Vector3(InFrom.X, InFrom.Y, InFrom.Z),
                                            EngineCore::Vector3(InDir.X, InDir.Y, InDir.Z), float(InLength), OutHit);
    }

    double Lerp(const double InA, const double InB, const double InT) { return InA + (InB - InA) * InT; }

    /* What the look axes are multiplied by: the setting, less when scoped. */
    double LookScale(const MoverState& InState)
    {
        return double(GRangeLookSensitivity.GetFloat()) * Lerp(1.0, ScopeLook, InState.Scope);
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
    GameCore::TransformComponent* Body = Avatar.GetComponent<GameCore::TransformComponent>();
    if(!Body || InFrame.Seconds <= 0.0f)
    {
        return;
    }
    RenderCore::AnimationComponent* Animation = Avatar.GetComponent<RenderCore::AnimationComponent>();
    MoverState& S = GMovers[InAvatar.Pack()];
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
    }

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
        S.CameraX = Pivot.X + Off.X * Scale;
        S.CameraY = Pivot.Y + Off.Y * Scale;
        S.CameraZ = Pivot.Z + Off.Z * Scale;
        Camera->Rotation = EngineCore::Vector3(0.0, 0.0, 0.0);
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
        { AutoStep::AimAt, nullptr, -300.0, 1100.0, false, 124.0 },  // the lane 2 paper, 28 m (clear of the booth divider)
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.5 },
        { AutoStep::Press, "Fire" }, { AutoStep::Wait, nullptr, 0.5 },
        { AutoStep::Press, "Reload" }, { AutoStep::Wait, nullptr, 2.0 },
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
    AutoPlayer GAuto;

    void AutoPlayFilter(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOutInput, void*)
    {
        if(!GRangeAutoPlay.GetBool() || GAuto.bDone) { return; }
        const auto Now = std::chrono::steady_clock::now();
        const double Dt = GAuto.bStarted ? std::clamp(std::chrono::duration<double>(Now - GAuto.Last).count(), 0.0, 0.1) : 0.0;
        GAuto.Last = Now;
        GAuto.bStarted = true;
        GAuto.StepTime += Dt;
        InOutInput = NimbleEngine::InputSnapshot();
        InOutInput.Down = GAuto.Held;

        const auto Bit = [](const char* InAction) { const int I = NimbleEngine::FindInputAction("Range", InAction); return I >= 0 ? 1u << I : 0u; };
        static const int LookX = NimbleEngine::FindInputAxis("Range", "LookX");
        static const int LookY = NimbleEngine::FindInputAxis("Range", "LookY");
        static const int MoveForward = NimbleEngine::FindInputAxis("Range", "MoveForward");

        const MoverState& Mover = GMovers[InAvatar.Pack()];
        const GameCore::TransformComponent* Body = InWorld.GetEntity(InAvatar).GetComponent<GameCore::TransformComponent>();
        if(GAuto.Step >= sizeof(GAutoPlay) / sizeof(GAutoPlay[0]) || !Body)
        {
            GAuto.bDone = true;
            CORE_LOG("[ShootingRange] autoplay done")
            return;
        }
        const AutoStep& Step = GAutoPlay[GAuto.Step];
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
            case AutoStep::Wait:    bNext = GAuto.StepTime >= Step.A; break;
            case AutoStep::Press:   InOutInput.Pressed |= Bit(Step.Action); InOutInput.Down |= Bit(Step.Action); bNext = true; break;
            case AutoStep::Hold:    GAuto.Held |= Bit(Step.Action); InOutInput.Down |= Bit(Step.Action); bNext = true; break;
            case AutoStep::Release: GAuto.Held &= ~Bit(Step.Action); InOutInput.Down &= ~Bit(Step.Action); bNext = true; break;
            case AutoStep::Face:    bNext = Turn(Step.A, Step.B) || GAuto.StepTime > 4.0; break;
            case AutoStep::AimAt:
            {
                /* From where the camera is: what the crosshair (the screen's centre) covers. */
                const double DX = Step.A - Mover.CameraX, DY = Step.C - Mover.CameraY, DZ = Step.B - Mover.CameraZ;
                const double Yaw   = std::atan2(DX, DZ) / DegToRad;
                const double Pitch = -std::atan2(DY, std::sqrt(DX * DX + DZ * DZ)) / DegToRad;
                bNext = (Turn(Yaw, Pitch) && GAuto.StepTime > 0.3) || GAuto.StepTime > 4.0;
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
                bNext = Distance < 35.0 || GAuto.StepTime > 25.0;
                if(GAuto.StepTime > 25.0) { CORE_LOG_WARN("[ShootingRange] autoplay: gave up walking to a waypoint (stuck)") }
                break;
            }
        }
        if(bNext)
        {
            ++GAuto.Step;
            GAuto.StepTime = 0.0;
        }
    }
}

// === The mode ==================================================================

class WorldsLabRangeMode : public NimbleEngine::AvatarGameMode
{
public:
    /* The shadow atlas: the sun's, and the two lane spots over the targets. */
    static constexpr int32_t ShadowedLights = 3;

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        GMovers.clear();
        if(EngineCore::ConfigVar* MaxShadowed = EngineCore::ConfigFindVarByPath("Shadows.MaxShadowedLights"))
        {
            PreviousShadowedLights = MaxShadowed->GetInt();
            MaxShadowed->SetInt(ShadowedLights);
        }
        if(!NimbleEngine::IsPhysicsAvailable())
        {
            CORE_LOG_WARN("[ShootingRange] built without JoltPhysics: the shooter walks on Bounds alone, and badly")
        }
        GAuto = AutoPlayer();
        if(NimbleEngine::AvatarWorld* Avatars = InSession.GetAvatars())
        {
            Avatars->SetInputFilter(&AutoPlayFilter, nullptr);
        }
        return AvatarGameMode::OnLoad(InSession);
    }

    void OnStart() override
    {
        AvatarGameMode::OnStart();
        /* Autoplay leaves the mouse alone: it drives itself, and someone may be using the desktop. */
        if(!GRangeAutoPlay.GetBool()) { Session->SetMouseCaptured(true); }
        CORE_LOG("[ShootingRange] WASD walks (Shift runs), Space jumps, H draws, right mouse aims, left fires, R reloads; "
                 "Escape frees the mouse")
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        (void)InFrame;
        GameCore::World& World = Session->GetWorld();
        World.Each<NimbleEngine::AvatarHarnessComponent>([this](const GameCore::Entity&, NimbleEngine::AvatarHarnessComponent& InHarness)
        {
            if(!InHarness.Possessor.IsSet()) { return; }
            if(GRangeAutoPlay.GetBool()) { return; }
            if(InHarness.WasPressed("Pause"))
            {
                Session->SetMouseCaptured(false);
            }
            else if(!Session->IsMouseCaptured() && InHarness.WasPressed("Fire"))
            {
                Session->SetMouseCaptured(true);
            }
        });
    }

    void OnUnload() override
    {
        EngineCore::ConfigVar* MaxShadowed = EngineCore::ConfigFindVarByPath("Shadows.MaxShadowedLights");
        if(MaxShadowed && PreviousShadowedLights >= 0)
        {
            MaxShadowed->SetInt(PreviousShadowedLights);
        }
        PreviousShadowedLights = -1;
        if(NimbleEngine::AvatarWorld* Avatars = Session ? Session->GetAvatars() : nullptr)
        {
            Avatars->SetInputFilter(nullptr, nullptr);
        }
        GMovers.clear();
    }

private:
    int32_t PreviousShadowedLights = -1;
};

void WorldsLabRegisterRange()
{
    NimbleEngine::RegisterGameMode<WorldsLabRangeMode>("WorldsLabRange");
}

void WorldsLabUnregisterRange()
{
    NimbleEngine::UnregisterGameMode("WorldsLabRange");
}
