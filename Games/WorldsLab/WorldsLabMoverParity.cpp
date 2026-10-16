#include "WorldsLabWalker.h"

#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/AnimationComponent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

/*
 * [WorldsLab] MoverParity=1: the engine's Character Mover (CharacterMove,
 * CharacterMover.h) against the walker it was generalised from (RangeMove),
 * side by side in the Shooting Range on its physics.
 *
 * A RangeMove walker is driven from the Player Start along the autoplay's
 * walk -- across the floor, up the stairs, over the mezzanine, along the west
 * catwalk and out over the range, running some of it, jumping twice -- and
 * every frame of input it was given is kept. A Character Mover with RangeMove's
 * numbers is then given those same frames from the same start. Each frame's
 * place, facing, fall and footing are compared; the worst is logged with
 * "[MoverParity] PASS" or "FAIL", and the session quits. ctest runs it
 * headless (WorldsLab.MoverParity).
 */

NIMBLE_CONFIG_BOOL(GMoverParity, "WorldsLab", "MoverParity", false, nullptr);

/*
 * [WorldsLab] MoverDemo=<pose>, with RangeAvatar=Content/Prefabs/ShootingRange/
 * MoverShooter.nmprefab: the Character Mover's guide pictures. The shooter is
 * driven into a pose and the game paused there, so a screenshot taken any
 * time after shows it: 1 standing at the start, 2 aiming over the shoulder,
 * 3 mid-jump on the stairs.
 */
NIMBLE_CONFIG_INT(GMoverDemo, "WorldsLab", "MoverDemo", 0, nullptr);

namespace
{
    using GameCore::EntityID;

    constexpr double DegToRad      = 3.14159265358979323846 / 180.0;
    constexpr float  StepSeconds   = 1.0f / 60.0f;
    constexpr int    MaxFrames     = 60 * 75;
    constexpr int    SettleFrames  = 20;     // the level's bodies are in the physics world by then
    constexpr double PlaceTolerance = 0.5;   // units
    constexpr double YawTolerance   = 0.5;   // degrees

    /* The autoplay's walk (WorldsLabRange.cpp's GAutoPlay), with the run flags. */
    struct Waypoint { double X, Z; bool bRun; };
    const Waypoint GRoute[] = {
        { -1415.0, -2430.0, true },    // the foot of the stairs
        { -1415.0, -1680.0, false },   // up them
        { -1405.0, -1000.0, false },   // across the mezzanine onto the west walk
        { -1405.0,   400.0, true },
        {  -150.0,   400.0, false },   // out over the range
    };

    struct Sample
    {
        double X = 0.0, Y = 0.0, Z = 0.0, BodyYaw = 0.0, VelY = 0.0;
        bool   bGrounded = false;
    };

    int Axis(const char* InName)   { return NimbleEngine::FindInputAxis("Range", InName); }
    int Action(const char* InName) { return NimbleEngine::FindInputAction("Range", InName); }

    /* One frame of the walk for a walker at (X, Z) viewing (CameraYaw, Pitch). */
    NimbleEngine::AvatarInputFrame Drive(const size_t InWaypoint, const double InX, const double InZ, const double InCameraYaw,
                                         const double InPitch, const int InFrame, bool& OutArrived)
    {
        NimbleEngine::AvatarInputFrame Frame;
        Frame.Sequence = InFrame + 1;
        Frame.Seconds  = StepSeconds;
        const Waypoint& To = GRoute[InWaypoint];
        const double DX = To.X - InX, DZ = To.Z - InZ;
        const double Distance = std::sqrt(DX * DX + DZ * DZ);
        const double Yaw = std::atan2(DX, DZ) / DegToRad;
        const double DYaw = WorldsLabWalker::WrapDegrees(Yaw - InCameraYaw), DPitch = 10.0 - InPitch;
        const double Most = 150.0 * StepSeconds;   // degrees this frame
        const int LookX = Axis("LookX"), LookY = Axis("LookY"), Forward = Axis("MoveForward");
        if(LookX >= 0) { Frame.Axes[LookX] = float(std::clamp(DYaw, -Most, Most)); }
        if(LookY >= 0) { Frame.Axes[LookY] = float(std::clamp(DPitch, -Most, Most)); }
        if(Forward >= 0 && std::fabs(DYaw) < 25.0) { Frame.Axes[Forward] = float(std::clamp(Distance / 120.0, 0.3, 1.0)); }
        const int Run = Action("Run"), Jump = Action("Jump");
        if(Run >= 0 && To.bRun && Distance > 200.0) { Frame.Down |= 1u << Run; }
        /* Two jumps: on the floor early on, and on the mezzanine. */
        if(Jump >= 0 && (InFrame == 90 || (InWaypoint == 2 && InFrame % 240 == 0)))
        {
            Frame.Down    |= 1u << Jump;
            Frame.Pressed |= 1u << Jump;
        }
        OutArrived = Distance < 35.0;
        return Frame;
    }

    Sample SampleOf(GameCore::World& InWorld, const EntityID InWalker, const double InBodyYaw, const double InVelY, const bool bInGrounded)
    {
        Sample S;
        const GameCore::TransformComponent* Body = InWorld.GetEntity(InWalker).GetComponent<GameCore::TransformComponent>();
        S.X = Body->Position.GetX(); S.Y = Body->Position.GetY(); S.Z = Body->Position.GetZ();
        S.BodyYaw = InBodyYaw; S.VelY = InVelY; S.bGrounded = bInGrounded;
        return S;
    }

    EntityID MakeWalker(GameCore::World& InWorld, const char* InName)
    {
        GameCore::Entity Walker = InWorld.CreateEntity(InName);
        Walker.AddComponent<GameCore::TransformComponent>();
        /* For its InputMap only: no Simulation, so the avatar system leaves it to us. */
        NimbleEngine::AvatarHarnessComponent Harness;
        std::snprintf(Harness.InputMap, sizeof(Harness.InputMap), "%s", "Range");
        Harness.bViewTarget = 0;
        Walker.AddComponent(Harness);
        return Walker.GetID();
    }

    /* The comparison; true if it passed. */
    bool RunParity(GameCore::World& InWorld)
    {
        const NimbleEngine::AvatarStepFn RangeStep = NimbleEngine::FindAvatarSimulation("RangeMove");
        const NimbleEngine::AvatarStepFn MoverStep = NimbleEngine::FindAvatarSimulation("CharacterMove");
        if(!RangeStep || !MoverStep)
        {
            CORE_LOG_ERROR("[MoverParity] FAIL: RangeMove or CharacterMove is not registered")
            return false;
        }
        if(!NimbleEngine::IsWorldSimulated(InWorld)) { CORE_LOG_WARN("[MoverParity] the level is not simulated: comparing on Bounds") }

        EngineCore::Vector3 Start(0.0, 0.0, 0.0);
        double StartYaw = 0.0;
        const EntityID StartId = NimbleEngine::FindPlayerStart(InWorld, 0);
        if(const GameCore::TransformComponent* At = StartId.IsSet() ? InWorld.GetEntity(StartId).GetComponent<GameCore::TransformComponent>() : nullptr)
        {
            Start = At->Position;
            StartYaw = At->Rotation.GetY();
        }

        // --- RangeMove, driven: the frames and where each left it ------------------------------
        const EntityID A = MakeWalker(InWorld, "MoverParity.RangeMove");
        WorldsLabWalker::PlaceWalker(InWorld, A, Start.GetX(), Start.GetY(), Start.GetZ(), StartYaw);
        std::vector<NimbleEngine::AvatarInputFrame> Frames;
        std::vector<Sample> Expected;
        size_t Waypoint = 0;
        int Jumps = 0;
        for(int F = 0; F < MaxFrames && Waypoint < sizeof(GRoute) / sizeof(GRoute[0]); ++F)
        {
            const RangeMoverComponent* Mover = InWorld.GetEntity(A).GetComponent<RangeMoverComponent>();
            const GameCore::TransformComponent* Body = InWorld.GetEntity(A).GetComponent<GameCore::TransformComponent>();
            bool bArrived = false;
            const NimbleEngine::AvatarInputFrame Frame = Drive(Waypoint, Body->Position.GetX(), Body->Position.GetZ(), Mover->CameraYaw,
                                                               Mover->Pitch, F, bArrived);
            if(bArrived) { ++Waypoint; }
            const bool bWasGrounded = Mover->bGrounded != 0;
            RangeStep(InWorld, A, Frame);
            Mover = InWorld.GetEntity(A).GetComponent<RangeMoverComponent>();
            if(bWasGrounded && Mover->VelY > 0.0f) { ++Jumps; }
            Frames.push_back(Frame);
            Expected.push_back(SampleOf(InWorld, A, Mover->BodyYaw, Mover->VelY, Mover->bGrounded != 0));
        }
        const GameCore::TransformComponent* EndA = InWorld.GetEntity(A).GetComponent<GameCore::TransformComponent>();
        CORE_LOG("[MoverParity] RangeMove: " + std::to_string(Frames.size()) + " frames, waypoints " + std::to_string(Waypoint) + "/"
                 + std::to_string(sizeof(GRoute) / sizeof(GRoute[0])) + ", " + std::to_string(Jumps) + " jumps, ending at ("
                 + std::to_string(EndA->Position.GetX()) + ", " + std::to_string(EndA->Position.GetY()) + ", "
                 + std::to_string(EndA->Position.GetZ()) + ")")
        InWorld.DestroyEntity(A);

        // --- CharacterMove, given the same frames ----------------------------------------------
        const EntityID B = MakeWalker(InWorld, "MoverParity.CharacterMove");
        NimbleEngine::CharacterMoverComponent Tuned;
        Tuned.Mask     = ~int32_t(4);   // RangeMove's WalkMask: not the ragdolls' layer
        Tuned.MaxSlope = float(std::acos(0.55) / DegToRad);   // RangeMove's ground: normal Y over 0.55
        InWorld.GetEntity(B).AddComponent(Tuned);
        NimbleEngine::TeleportCharacter(InWorld, B, Start, float(StartYaw));

        double WorstPlace = 0.0, WorstYaw = 0.0, WorstVelY = 0.0;
        int WorstFrame = -1, FootingMismatches = 0, FirstOff = -1;
        for(size_t F = 0; F < Frames.size(); ++F)
        {
            MoverStep(InWorld, B, Frames[F]);
            const NimbleEngine::CharacterMoverComponent* Mover = InWorld.GetEntity(B).GetComponent<NimbleEngine::CharacterMoverComponent>();
            const Sample Got = SampleOf(InWorld, B, Mover->BodyYaw, Mover->VelY, Mover->bGrounded != 0);
            const Sample& Want = Expected[F];
            const double Place = std::sqrt((Got.X - Want.X) * (Got.X - Want.X) + (Got.Y - Want.Y) * (Got.Y - Want.Y)
                                           + (Got.Z - Want.Z) * (Got.Z - Want.Z));
            const double Yaw = std::fabs(WorldsLabWalker::WrapDegrees(Got.BodyYaw - Want.BodyYaw));
            if(Place > WorstPlace) { WorstPlace = Place; WorstFrame = int(F); }
            WorstYaw  = (std::max)(WorstYaw, Yaw);
            WorstVelY = (std::max)(WorstVelY, std::fabs(Got.VelY - Want.VelY));
            if(Got.bGrounded != Want.bGrounded) { ++FootingMismatches; }
            if(FirstOff < 0 && (Place > PlaceTolerance || Yaw > YawTolerance || Got.bGrounded != Want.bGrounded))
            {
                FirstOff = int(F);
                CORE_LOG_WARN("[MoverParity] frame " + std::to_string(F) + " parts: RangeMove (" + std::to_string(Want.X) + ", "
                              + std::to_string(Want.Y) + ", " + std::to_string(Want.Z) + ") yaw " + std::to_string(Want.BodyYaw)
                              + (Want.bGrounded ? " grounded" : " airborne") + "; CharacterMove (" + std::to_string(Got.X) + ", "
                              + std::to_string(Got.Y) + ", " + std::to_string(Got.Z) + ") yaw " + std::to_string(Got.BodyYaw)
                              + (Got.bGrounded ? " grounded" : " airborne"))
            }
        }
        InWorld.DestroyEntity(B);

        const bool bRouteWalked = Waypoint >= 3 && Jumps >= 1;
        const bool bPass = FirstOff < 0 && bRouteWalked;
        CORE_LOG(std::string("[MoverParity] ") + (bPass ? "PASS" : "FAIL") + ": worst place " + std::to_string(WorstPlace) + " (frame "
                 + std::to_string(WorstFrame) + "), worst yaw " + std::to_string(WorstYaw) + ", worst fall speed "
                 + std::to_string(WorstVelY) + ", footing mismatches " + std::to_string(FootingMismatches)
                 + (bRouteWalked ? "" : "; the route was not walked far enough to compare"))
        return bPass;
    }
}

namespace
{
    struct MoverDemo
    {
        bool   bStarted = false;
        bool   bHeld    = false;
        bool   bJumped  = false;
        size_t Waypoint = 0;
        double Clock    = 0.0;
        double PhaseClock = 0.0;
        double StartYaw = 0.0;
        std::chrono::steady_clock::time_point Last;
    };
    MoverDemo GDemo;

    void Hold(NimbleEngine::GameSession& InSession, const char* InPose)
    {
        GDemo.bHeld = true;
        InSession.SetPaused(true);
        CORE_LOG(std::string("[MoverDemo] holding: ") + InPose)
    }
}

namespace WorldsLabWalker
{
    bool IsMoverDemo() { return GMoverDemo.GetInt() != 0; }

    void ResetMoverDemo() { GDemo = MoverDemo(); }

    bool MoverDemoInput(NimbleEngine::GameSession& InSession, GameCore::World& InWorld, const GameCore::EntityID InAvatar,
                        NimbleEngine::InputSnapshot& InOutInput)
    {
        const int Pose = GMoverDemo.GetInt();
        if(Pose == 0) { return false; }
        InOutInput = NimbleEngine::InputSnapshot();
        GameCore::Entity Avatar = InWorld.GetEntity(InAvatar);
        const NimbleEngine::CharacterMoverComponent* Mover = Avatar.GetComponent<NimbleEngine::CharacterMoverComponent>();
        const GameCore::TransformComponent* Body = Avatar.GetComponent<GameCore::TransformComponent>();
        if(GDemo.bHeld || !Mover || !Body || !Mover->bStarted) { return true; }

        const auto Now = std::chrono::steady_clock::now();
        const double Dt = GDemo.bStarted ? std::clamp(std::chrono::duration<double>(Now - GDemo.Last).count(), 0.0, 0.1) : 0.0;
        if(!GDemo.bStarted) { GDemo.StartYaw = Mover->CameraYaw; }
        GDemo.bStarted = true;
        GDemo.Last = Now;
        GDemo.Clock += Dt;
        GDemo.PhaseClock += Dt;

        const int LookX = Axis("LookX"), LookY = Axis("LookY"), Forward = Axis("MoveForward");
        const auto Turn = [&](const double InYaw, const double InPitch)
        {
            const double DYaw = WrapDegrees(InYaw - Mover->CameraYaw), DPitch = InPitch - Mover->Pitch;
            const double Most = (std::max)(120.0 * Dt, 0.05);
            const double Scale = 1.0 + (double(Mover->ScopeLook) - 1.0) * Mover->Scope;   // the step slows the look aimed in
            if(LookX >= 0) { InOutInput.Axes[LookX] = float(std::clamp(DYaw, -Most, Most) / (std::max)(Scale, 0.01)); }
            if(LookY >= 0) { InOutInput.Axes[LookY] = float(std::clamp(DPitch, -Most, Most) / (std::max)(Scale, 0.01)); }
            return std::fabs(DYaw) < 0.5 && std::fabs(DPitch) < 0.5;
        };
        const auto Press = [&InOutInput](const char* InAction, const bool bInPress)
        {
            const int I = Action(InAction);
            if(I < 0) { return; }
            InOutInput.Down |= 1u << I;
            if(bInPress) { InOutInput.Pressed |= 1u << I; }
        };

        if(Pose == 1)
        {
            Turn(GDemo.StartYaw + 20.0, 6.0);
            if(GDemo.Clock > 2.0) { Hold(InSession, "standing"); }
        }
        else if(Pose == 2)
        {
            if(RenderCore::AnimationComponent* Animation = Avatar.GetComponent<RenderCore::AnimationComponent>()) { Animation->SetBool("Armed", true); }
            const bool bThere = Turn(GDemo.StartYaw - 12.0, 3.0);
            if(GDemo.Clock > 0.8) { Press("Aim", false); }
            if(bThere && Mover->Scope >= 1.0f && GDemo.Clock > 2.5) { Hold(InSession, "aiming"); }
        }
        else
        {
            /* The autoplay's way to the stairs, then up them with a jump. */
            const Waypoint& To = GRoute[(std::min)(GDemo.Waypoint, size_t(1))];
            const double DX = To.X - Body->Position.GetX(), DZ = To.Z - Body->Position.GetZ();
            const double Distance = std::sqrt(DX * DX + DZ * DZ);
            const double Yaw = std::atan2(DX, DZ) / DegToRad;
            Turn(Yaw, 14.0);
            if(Forward >= 0 && std::fabs(WrapDegrees(Yaw - Mover->CameraYaw)) < 25.0) { InOutInput.Axes[Forward] = 1.0f; }
            if(To.bRun && Distance > 200.0) { Press("Run", false); }
            if(GDemo.Waypoint == 0 && Distance < 35.0) { GDemo.Waypoint = 1; GDemo.PhaseClock = 0.0; }
            if(GDemo.Waypoint == 1 && !GDemo.bJumped && Body->Position.GetY() > 60.0 && Mover->bGrounded)
            {
                Press("Jump", true);
                GDemo.bJumped = true;
            }
            if(GDemo.bJumped && !Mover->bGrounded && Mover->VelY < 120.0f) { Hold(InSession, "mid-jump on the stairs"); }
            if(GDemo.Clock > 40.0) { Hold(InSession, "gave up (stuck)"); }
        }
        return true;
    }

    bool TickMoverParity(NimbleEngine::GameSession& InSession, int& InOutFrames)
    {
        if(!GMoverParity.GetBool()) { return false; }
        if(++InOutFrames != SettleFrames) { return false; }   // once
        RunParity(InSession.GetWorld());
        InSession.RequestQuit();
        return true;
    }
}
