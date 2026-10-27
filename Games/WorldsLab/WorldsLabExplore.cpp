#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/TerrainFacade.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

/*
 * WorldsLabExplore: the mode of WorldsLab's visual-style worlds (Project
 * Settings > Worlds names it; Content/Levels/Steampunk/SteampunkStreet.nmworld
 * is the first). Nothing in it is about any one world: it walks the entity
 * named Player round whatever the world holds.
 *
 *   Walk   mouse looks, WASD walks at eye height (Shift runs), and every
 *          entity with a Bounds component taller than knee height is a wall:
 *          the player's circle is pushed out of its footprint.
 *   Fly    F toggles free flight (E/Q or Space/Ctrl up and down, Shift
 *          faster), through everything; F again lands where you are.
 *   Mouse  Escape lets the cursor go, a click takes it back.
 *
 * [WorldsLab] PerfReport=1 (or the console) logs what "stat unit" and "stat
 * frame" show every two seconds -- thread times, draws, culling, each stage's
 * GPU time -- so a world's cost can be read from the log.
 *
 * An entity named Airship, if the world has one, drifts slowly along its
 * heading and back, bobbing.
 *
 * [WorldsLab] HoldCamera=1 leaves the Player where the world put it, input
 * ignored: what captures of a world set.
 *
 * WorldsLabCourtyard is the same mode for the Courtyard world
 * (Tools/Courtyard/make_courtyard_level.py, brought over from LightLab): its
 * hearth's light, the entity named Fire, flickers (WorldsLab.Flicker), and
 * while it runs it shadows all four point and spot lights the renderer can
 * (Shadows.MaxShadowedLights) and turns on the sun's contact shadows
 * (Shadows.ContactShadows), giving the project's settings back when it goes.
 */

NIMBLE_CONFIG_FLOAT(GWorldsLabWalkSpeed,       "WorldsLab", "WalkSpeed",       180.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GWorldsLabFlySpeed,        "WorldsLab", "FlySpeed",        900.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GWorldsLabLookSensitivity, "WorldsLab", "LookSensitivity", 1.0f,   nullptr);
NIMBLE_CONFIG_FLOAT(GWorldsLabEyeHeight,       "WorldsLab", "EyeHeight",       170.0f, nullptr);
NIMBLE_CONFIG_BOOL(GWorldsLabPerfReport,       "WorldsLab", "PerfReport",      false,  nullptr);
NIMBLE_CONFIG_FLOAT(GWorldsLabPerfInterval,    "WorldsLab", "PerfInterval",    2.0f,   nullptr);   // seconds between reports

/* PerfTour=1: the Player flies a fixed path through SteampunkStreet (an
 * overview, down to the front street, along it, up the cross street, and out
 * again) over TourSeconds, then logs "[WorldsLab] tour done" -- the same
 * frames every run, for comparing renderer settings. */
NIMBLE_CONFIG_BOOL(GWorldsLabPerfTour,         "WorldsLab", "PerfTour",        false,  nullptr);
NIMBLE_CONFIG_FLOAT(GWorldsLabPerfTourHold,    "WorldsLab", "PerfTourHold",    -1.0f,  nullptr);   // >= 0: the tour stays at this time (screenshots)

/* HoldCamera=1: the Player stays where the world put it and reads no input -- a
 * capture's window takes focus, and a mouse moved or a key typed meanwhile
 * would turn or walk it (Tools/TranslucencyLab/capture.ps1 sets it). */
NIMBLE_CONFIG_BOOL(GWorldsLabHoldCamera,       "WorldsLab", "HoldCamera",      false,  nullptr);

/* The explore controls: the Player's Input component reads map "Default".
 * Each entry is also [Input_Default] <Name> in Config/DefaultEngine.ini. */
NIMBLE_DEFINE_INPUT_MAP(Default)
{
    Map.Action("Run",          "LeftShift, RightShift, Gamepad.LeftStick");
    Map.Action("ToggleFly",    "F, Gamepad.North");
    Map.Action("Pause",        "Escape, Gamepad.Start");
    Map.Action("CaptureMouse", "Mouse.Left");

    Map.Axis("MoveForward", "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Axis("MoveRight",   "D, Right, -A, -Left, Gamepad.LeftX");
    Map.Axis("MoveUp",      "E, Space, -Q, -LeftCtrl, Gamepad.RightShoulder, -Gamepad.LeftShoulder");

    /* Degrees this frame; +Y down, so moving the mouse down looks down. */
    Map.Axis("LookX", "Mouse.DeltaX*0.15, Gamepad.RightX*2.5");
    Map.Axis("LookY", "Mouse.DeltaY*0.15, Gamepad.RightY*2.5");
}

namespace
{
    constexpr double DegToRad      = 3.14159265358979323846 / 180.0;
    constexpr double MaxPitch      = 89.0;
    constexpr double PlayerRadius  = 28.0;
    constexpr double KneeHeight    = 40.0;
    constexpr double RunMultiplier = 2.2;
    constexpr double FlyBoost      = 3.0;

    /* A wall's footprint on the ground (x, z). */
    struct Footprint
    {
        double MinX, MaxX, MinZ, MaxZ;
    };

    struct ExploreContext
    {
        GameCore::EntityID Player;
        GameCore::EntityID Airship;
        EngineCore::Vector3 AirshipHome;
        double AirshipYaw = 0.0;
        double Time    = 0.0;
        double Yaw     = 0.0;
        double Pitch   = 0.0;
        double BobPhase = 0.0;
        bool   bFlying = false;
        double TourTime = 0.0;
        bool   bTourDone = false;
        std::vector<Footprint> Walls;
    };

    /* The view direction for a (Pitch, Yaw, 0) rotation: (sin Y cos P, -sin P, cos Y cos P). */
    EngineCore::Vector3 ViewForward(const double InPitch, const double InYaw)
    {
        const double P = InPitch * DegToRad;
        const double Y = InYaw * DegToRad;
        return EngineCore::Vector3(std::sin(Y) * std::cos(P), -std::sin(P), std::cos(Y) * std::cos(P));
    }

    /* Bounds.Local turned by the Transform's yaw and placed: its footprint.
     * Entities here are only ever turned about Y. */
    Footprint FootprintOf(const GameCore::BoundsComponent& InBounds, const GameCore::TransformComponent& InTransform)
    {
        const double Yaw = double(InTransform.Rotation.GetY()) * DegToRad;
        const double C = std::cos(Yaw), S = std::sin(Yaw);
        const double SX = double(InTransform.Scale.GetX()), SZ = double(InTransform.Scale.GetZ());
        Footprint Out { 1e30, -1e30, 1e30, -1e30 };
        for(int Corner = 0; Corner < 4; ++Corner)
        {
            const double LX = ((Corner & 1) ? InBounds.Local.Max.GetX() : InBounds.Local.Min.GetX()) * SX;
            const double LZ = ((Corner & 2) ? InBounds.Local.Max.GetZ() : InBounds.Local.Min.GetZ()) * SZ;
            /* +X turns to (cos, 0, -sin), +Z to (sin, 0, cos). */
            const double WX = double(InTransform.Position.GetX()) + LX * C + LZ * S;
            const double WZ = double(InTransform.Position.GetZ()) - LX * S + LZ * C;
            Out.MinX = std::min(Out.MinX, WX); Out.MaxX = std::max(Out.MaxX, WX);
            Out.MinZ = std::min(Out.MinZ, WZ); Out.MaxZ = std::max(Out.MaxZ, WZ);
        }
        return Out;
    }

    /* Pushes the circle at (X, Z) out of every footprint it overlaps, along the shortest way. */
    void PushOut(const std::vector<Footprint>& InWalls, double& InOutX, double& InOutZ)
    {
        for(int Pass = 0; Pass < 3; ++Pass)
        {
            for(const Footprint& Wall : InWalls)
            {
                const double NearX = std::clamp(InOutX, Wall.MinX, Wall.MaxX);
                const double NearZ = std::clamp(InOutZ, Wall.MinZ, Wall.MaxZ);
                const double DX = InOutX - NearX, DZ = InOutZ - NearZ;
                const double Dist2 = DX * DX + DZ * DZ;
                if(Dist2 >= PlayerRadius * PlayerRadius)
                {
                    continue;
                }
                if(Dist2 > 1e-8)
                {
                    const double Dist = std::sqrt(Dist2);
                    InOutX = NearX + DX / Dist * PlayerRadius;
                    InOutZ = NearZ + DZ / Dist * PlayerRadius;
                }
                else
                {
                    /* Inside: out through the nearest side. */
                    const double Left = InOutX - Wall.MinX, Right = Wall.MaxX - InOutX;
                    const double Back = InOutZ - Wall.MinZ, Front = Wall.MaxZ - InOutZ;
                    const double Least = std::min(std::min(Left, Right), std::min(Back, Front));
                    if(Least == Left)       { InOutX = Wall.MinX - PlayerRadius; }
                    else if(Least == Right) { InOutX = Wall.MaxX + PlayerRadius; }
                    else if(Least == Back)  { InOutZ = Wall.MinZ - PlayerRadius; }
                    else                    { InOutZ = Wall.MaxZ + PlayerRadius; }
                }
            }
        }
    }

    /* PerfTour's keys: time, position, yaw, pitch; linear between them. */
    struct TourKey { double T, X, Y, Z, Yaw, Pitch; };
    const TourKey GTour[] = {
        {  0.0,     0.0, 2500.0, -7000.0,   0.0, 20.0 },   // the town from far off: small mips
        {  6.0,     0.0, 2500.0, -7000.0,   0.0, 20.0 },
        { 14.0, -4000.0,  170.0, -1545.0,  90.0,  0.0 },   // down to the front street's west end
        { 27.0,     0.0,  170.0, -1545.0,  90.0,  0.0 },   // walking east
        { 29.0,     0.0,  170.0, -1545.0,   0.0,  0.0 },   // turn up the cross street
        { 40.0,     0.0,  170.0,  2500.0,   0.0,  0.0 },
        { 46.0,     0.0, 3000.0,  6000.0, 180.0, 25.0 },   // up and out, looking back
        { 50.0,     0.0, 3000.0,  6000.0, 180.0, 25.0 },
    };

    bool TourPose(const double InT, EngineCore::Vector3& OutPosition, double& OutYaw, double& OutPitch)
    {
        const size_t Count = sizeof(GTour) / sizeof(GTour[0]);
        size_t At = 0;
        while(At + 1 < Count && GTour[At + 1].T <= InT) { ++At; }
        const TourKey& A = GTour[At];
        const TourKey& B = GTour[std::min(At + 1, Count - 1)];
        const double F = B.T > A.T ? std::clamp((InT - A.T) / (B.T - A.T), 0.0, 1.0) : 0.0;
        OutPosition = EngineCore::Vector3(A.X + (B.X - A.X) * F, A.Y + (B.Y - A.Y) * F, A.Z + (B.Z - A.Z) * F);
        OutYaw   = A.Yaw + (B.Yaw - A.Yaw) * F;
        OutPitch = A.Pitch + (B.Pitch - A.Pitch) * F;
        return InT < GTour[Count - 1].T;
    }

    /* PrePhysics (WorldsLabExploreMode::Step): look, then walk (colliding) or fly. */
    void StepExplore(GameCore::World& InWorld, const float InDeltaSeconds, ExploreContext& Context)
    {
        const double DT = double(InDeltaSeconds);
        Context.Time += DT;

        if(Context.Airship.IsSet())
        {
            if(GameCore::TransformComponent* Ship = InWorld.GetEntity(Context.Airship).GetComponent<GameCore::TransformComponent>())
            {
                /* 25 m each way along its heading over two minutes, bobbing a metre. */
                const double Along = std::sin(Context.Time * 0.05) * 2500.0;
                const double Yaw = Context.AirshipYaw * DegToRad;
                Ship->Position = Context.AirshipHome + EngineCore::Vector3(std::cos(Yaw) * Along,
                                                                           std::sin(Context.Time * 0.4) * 100.0,
                                                                           -std::sin(Yaw) * Along);
            }
        }

        const GameCore::Entity Player = InWorld.GetEntity(Context.Player);
        GameCore::TransformComponent* Transform = Player.GetComponent<GameCore::TransformComponent>();
        const NimbleEngine::InputComponent* Input = Player.GetComponent<NimbleEngine::InputComponent>();
        if(!Transform || !Input)
        {
            return;
        }

        if(GWorldsLabPerfTour.GetBool())
        {
            EngineCore::Vector3 Position;
            if(GWorldsLabPerfTourHold.GetFloat() >= 0.0f) { Context.TourTime = double(GWorldsLabPerfTourHold.GetFloat()); }
            const bool bGoing = TourPose(Context.TourTime, Position, Context.Yaw, Context.Pitch);
            Context.TourTime += DT;
            Transform->Position = Position;
            Transform->Rotation = EngineCore::Vector3(Context.Pitch, Context.Yaw, 0.0);
            if(!bGoing && !Context.bTourDone)
            {
                Context.bTourDone = true;
                CORE_LOG("[WorldsLab] tour done")
            }
            return;
        }
        if(GWorldsLabHoldCamera.GetBool())
        {
            return;
        }

        const double Look = double(GWorldsLabLookSensitivity.GetFloat());
        Context.Yaw   = std::fmod(Context.Yaw + double(Input->GetAxis("LookX")) * Look, 360.0);
        Context.Pitch = std::clamp(Context.Pitch + double(Input->GetAxis("LookY")) * Look, -MaxPitch, MaxPitch);

        if(Input->WasPressed("ToggleFly"))
        {
            Context.bFlying = !Context.bFlying;
            CORE_LOG(Context.bFlying ? "[WorldsLab] flying (F to land)" : "[WorldsLab] walking (F to fly)")
        }

        const EngineCore::Vector3 Up(0.0, 1.0, 0.0);
        EngineCore::Vector3 Position = Transform->Position;
        if(Context.bFlying)
        {
            const EngineCore::Vector3 Forward = ViewForward(Context.Pitch, Context.Yaw);
            const EngineCore::Vector3 Right = (Up ^ Forward).GetNormalize();
            const double Speed = double(GWorldsLabFlySpeed.GetFloat()) * (Input->IsDown("Run") ? FlyBoost : 1.0);
            Position = Position + (Forward * double(Input->GetAxis("MoveForward")) + Right * double(Input->GetAxis("MoveRight"))
                                   + Up * double(Input->GetAxis("MoveUp"))) * (Speed * DT);
            /* Never under the floor, nor under a terrain. */
            double Floor = 40.0;
            float Ground = 0.0f;
            if(NimbleEngine::SampleGround(InWorld, Position, Ground)) { Floor = double(Ground) + 40.0; }
            Position = EngineCore::Vector3(Position.GetX(), std::max(double(Position.GetY()), Floor), Position.GetZ());
        }
        else
        {
            /* Along the ground: the view's heading, never its pitch. */
            const EngineCore::Vector3 Forward = ViewForward(0.0, Context.Yaw);
            const EngineCore::Vector3 Right = (Up ^ Forward).GetNormalize();
            EngineCore::Vector3 Move = Forward * double(Input->GetAxis("MoveForward")) + Right * double(Input->GetAxis("MoveRight"));
            const double Length = double(Move.GetLength());
            if(Length > 1.0) { Move = Move * (1.0 / Length); }
            const double Speed = double(GWorldsLabWalkSpeed.GetFloat()) * (Input->IsDown("Run") ? RunMultiplier : 1.0);
            double X = double(Position.GetX()) + double(Move.GetX()) * Speed * DT;
            double Z = double(Position.GetZ()) + double(Move.GetZ()) * Speed * DT;
            PushOut(Context.Walls, X, Z);

            /* A gentle step bob while moving; the eye eases down from a flight. */
            const double Moving = std::min(Length, 1.0) * (Input->IsDown("Run") ? 1.6 : 1.0);
            Context.BobPhase += DT * 9.0 * Moving;
            const double Bob = std::sin(Context.BobPhase) * 2.5 * std::min(Moving, 1.0);
            double Eye = double(GWorldsLabEyeHeight.GetFloat()) + Bob;

            /* On a terrain: over its ground, slid along slopes too steep to climb (TerrainFacade.h). */
            NimbleEngine::GroundMoveSettings Walk;
            Walk.RideHeight = GWorldsLabEyeHeight.GetFloat();
            EngineCore::Vector3 Next(X, Position.GetY(), Z);
            bool bOnTerrain = NimbleEngine::MoveOnGround(InWorld, Position, Next, Walk);
            if(bOnTerrain)
            {
                X   = Next.GetX();
                Z   = Next.GetZ();
                Eye = Next.GetY() + Bob;
            }
            /* Eased down from a flight; on the ground, it follows at once. */
            const bool bLanding = std::abs(Eye - double(Position.GetY())) > 60.0;
            const double Y = bOnTerrain && !bLanding ? Eye : double(Position.GetY()) + (Eye - double(Position.GetY())) * std::min(1.0, DT * 6.0);
            Position = EngineCore::Vector3(X, Y, Z);
        }
        Transform->Position = Position;
        Transform->Rotation = EngineCore::Vector3(Context.Pitch, Context.Yaw, 0.0);
    }

    /* What WorldsLab.Flicker keeps (the Courtyard mode's): a fire's light, its
     * intensity and place as the world set them. */
    struct FlickerContext
    {
        GameCore::EntityID  Light;
        EngineCore::Vector3 Center;
        float               Intensity      = 0.0f;
        double              ElapsedSeconds = 0.0;
    };

    /* PrePhysics: a fire's light. Three sines out of step give the flicker --
     * 60-100% of its intensity, never quite repeating -- and the light wanders
     * a few units round its place, so the shadows the flames would throw shift
     * with it. */
    void StepFlicker(GameCore::World& InWorld, const float InDeltaSeconds, FlickerContext& Context)
    {
        /* Under [Rendering] FixedStep a frame is that long for the fire too, so a capture's
         * frame N has the same light every run. */
        static EngineCore::ConfigVar* const FixedStep = EngineCore::ConfigFindVarByPath("Rendering.FixedStep");
        const float Fixed = FixedStep ? FixedStep->GetFloat() : 0.0f;
        Context.ElapsedSeconds += Fixed > 0.0f ? double(Fixed) : double(InDeltaSeconds);
        GameCore::Entity Fire = InWorld.GetEntity(Context.Light);
        /* A point light, or a spot aimed out of a fireplace (so none of it reaches through the wall behind). */
        RenderCore::PointLightComponent* Point = Fire.GetComponent<RenderCore::PointLightComponent>();
        RenderCore::SpotLightComponent*  Spot  = Fire.GetComponent<RenderCore::SpotLightComponent>();
        GameCore::TransformComponent* Transform = Fire.GetComponent<GameCore::TransformComponent>();
        float* Intensity = Point ? &Point->Intensity : (Spot ? &Spot->Intensity : nullptr);
        if(!Intensity || !Transform)
        {
            return;
        }
        const double T = Context.ElapsedSeconds;
        const double Flicker = 0.8 + 0.1 * std::sin(T * 7.3) + 0.06 * std::sin(T * 13.1 + 1.3) + 0.04 * std::sin(T * 23.7 + 0.4);
        *Intensity = Context.Intensity * float(Flicker);
        Transform->Position = Context.Center + EngineCore::Vector3(2.0 * std::sin(T * 5.1), 3.0 * std::sin(T * 6.7 + 0.8),
                                                                   3.0 * std::sin(T * 4.3 + 2.1));
    }
}

class WorldsLabExploreMode : public NimbleEngine::IGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        GameCore::World& World = InSession.GetWorld();
        Context.Player  = World.FindEntityByName("Player");
        Context.Airship = World.FindEntityByName("Airship");
        if(!Context.Player.IsSet())
        {
            CORE_LOG_ERROR("[WorldsLab] the world has no entity named Player; nothing to walk")
            return true;
        }
        if(const GameCore::TransformComponent* View = World.GetEntity(Context.Player).GetComponent<GameCore::TransformComponent>())
        {
            Context.Pitch = std::clamp(double(View->Rotation.GetX()), -MaxPitch, MaxPitch);
            Context.Yaw   = double(View->Rotation.GetY());
        }
        if(Context.Airship.IsSet())
        {
            if(const GameCore::TransformComponent* Ship = World.GetEntity(Context.Airship).GetComponent<GameCore::TransformComponent>())
            {
                Context.AirshipHome = Ship->Position;
                Context.AirshipYaw  = double(Ship->Rotation.GetY());
            }
        }

        /* Walls: every Bounds taller than knee height, by its footprint. */
        struct Gather { GameCore::World* World; std::vector<Footprint>* Walls; } Args { &World, &Context.Walls };
        World.ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
        {
            auto& A = *static_cast<Gather*>(InUserData);
            const GameCore::Entity E = A.World->GetEntity(InEntity);
            const GameCore::BoundsComponent* Bounds = E.GetComponent<GameCore::BoundsComponent>();
            const GameCore::TransformComponent* Transform = E.GetComponent<GameCore::TransformComponent>();
            if(Bounds && Transform && !Bounds->Local.IsEmpty()
               && double(Bounds->Local.Max.GetY()) * double(Transform->Scale.GetY()) > KneeHeight)
            {
                A.Walls->push_back(FootprintOf(*Bounds, *Transform));
            }
        }, &Args);

        AddSystem<&WorldsLabExploreMode::Step>("WorldsLab.Explore", GameCore::TP_PrePhysics);
        return true;
    }

    void OnStart() override
    {
        Session->SetMouseCaptured(true);
        CORE_LOG("[WorldsLab] walk with WASD + mouse, Shift to run; F flies (E/Q up and down); Escape frees the mouse")
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        if(GWorldsLabPerfReport.GetBool())
        {
            ReportPerf(InFrame.DeltaSeconds);
        }
        GameCore::World& World = Session->GetWorld();
        if(const NimbleEngine::InputComponent* Input = World.GetEntity(Context.Player).GetComponent<NimbleEngine::InputComponent>())
        {
            if(Input->WasPressed("Pause"))
            {
                Session->SetMouseCaptured(false);
            }
            else if(!Session->IsMouseCaptured() && Input->WasPressed("CaptureMouse"))
            {
                Session->SetMouseCaptured(true);
            }
        }
    }

private:
    void Step(GameCore::World& InWorld, const float InDeltaSeconds) { StepExplore(InWorld, InDeltaSeconds, Context); }

    /* Every two seconds: the renderer's half-second averages and the draws per frame since the last report. */
    void ReportPerf(const double InDeltaSeconds)
    {
        PerfClock += InDeltaSeconds;
        PerfWorst  = std::max(PerfWorst, InDeltaSeconds);
        if(PerfClock < double(GWorldsLabPerfInterval.GetFloat()))
        {
            return;
        }
        const double Worst = PerfWorst;
        PerfClock = 0.0;
        PerfWorst = 0.0;
        RenderCore::RenderStats Stats;
        Session->GetRenderer().GetStats(Stats);
        const uint64_t Frames = Stats.FramesRendered - LastFrames;
        const uint64_t Draws  = Stats.DrawCalls - LastDraws;
        LastFrames = Stats.FramesRendered;
        LastDraws  = Stats.DrawCalls;
        char Line[512];
        std::snprintf(Line, sizeof(Line),
                      "[WorldsLab] perf: frame %.2f ms (game %.2f, render %.2f, wait %.2f, present %.2f, idle %.2f); "
                      "%.0f draws/frame, %llu proxies, %llu culled, %llu occluded, %llu queried, %llu static",
                      double(Stats.FrameMs), double(Stats.GameThreadMs), double(Stats.RenderThreadMs), double(Stats.WaitMs),
                      double(Stats.PresentMs), double(Stats.IdleMs), Frames ? double(Draws) / double(Frames) : 0.0,
                      (unsigned long long)Stats.ProxiesLive, (unsigned long long)Stats.ProxiesCulledLastFrame,
                      (unsigned long long)Stats.ProxiesOccludedLastFrame, (unsigned long long)Stats.OcclusionQueriesLastFrame,
                      (unsigned long long)Stats.StaticProxiesDrawn);
        std::string Cpu = "[WorldsLab] perf stages (GPU ms / CPU ms / draws):";
        for(uint32_t Timer = 0; Timer < RenderCore::RGT_Count; ++Timer)
        {
            char Part[80];
            std::snprintf(Part, sizeof(Part), " %s %.2f/%.2f/%d", RenderCore::GetRenderGpuTimerName(RenderCore::ERenderGpuTimer(Timer)),
                          double(Stats.GpuMs[Timer]), double(Stats.CpuMs[Timer]), int(Stats.StageDraws[Timer]));
            Cpu += Part;
        }
        char Queue[96];
        std::snprintf(Queue, sizeof(Queue), "; queue %.2f ms; parallel recording %s, %u workers", double(Stats.QueueCpuMs),
                      Stats.bParallelRecordingAvailable ? "available" : "unavailable", Stats.ParallelRecordingWorkers);
        const RenderCore::TextureStreamingStats& Tex = Stats.TextureStreaming;
        const auto Mb = [](const uint64_t InBytes) { return double(InBytes) / (1024.0 * 1024.0); };
        char Streaming[700];
        std::snprintf(Streaming, sizeof(Streaming),
                      "[WorldsLab] perf streaming: t %.1f s, worst frame %.2f ms; %s, %u textures, resident %.1f MB, wanted %.1f MB, "
                      "all mips %.1f MB, pool %.0f MB bias %d; loading %u waiting %u; loads %llu (%.1f MB read), drops %llu; "
                      "update %.3f ms, last load %.2f ms, create peak %.2f ms; cpu %.1f MB kept %.1f; gpu blocks %.1f/%.1f MB dedicated %.1f MB ring %.0f MB",
                      Context.TourTime, Worst * 1000.0, Tex.bEnabled ? "on" : "off", Tex.Textures, Mb(Tex.ResidentBytes),
                      Mb(Tex.WantedBytes), Mb(Tex.FullBytes), Mb(Tex.PoolBytes), Tex.BudgetBias, Tex.PendingLoads, Tex.WaitingLoads,
                      (unsigned long long)Tex.Loads, Mb(Tex.BytesRead), (unsigned long long)Tex.Drops, double(Tex.UpdateMs), double(Tex.LoadMs), double(Tex.CreateMsPeak),
                      Mb(Tex.CpuBytes), Mb(Tex.CpuCachedBytes), Mb(Tex.GpuBlockUsed), Mb(Tex.GpuBlockBytes), Mb(Tex.GpuDedicated), Mb(Tex.GpuStaging));
        CORE_LOG(std::string(Line))
        CORE_LOG(Cpu + Queue)
        CORE_LOG(std::string(Streaming))
    }

    double   PerfClock  = 0.0;
    double   PerfWorst  = 0.0;
    uint64_t LastFrames = 0;
    uint64_t LastDraws  = 0;
    /* This level's walk; made and gone with the mode. */
    ExploreContext Context;
};

/* The Courtyard world's mode: explore, with the hearth flickering and every
 * lantern and chandelier shadowed (an unshadowed point light shines through a
 * wall onto the floor behind it). */
class WorldsLabCourtyardMode : public WorldsLabExploreMode
{
public:
    /* The shadow atlas holds four point lights. */
    static constexpr int32_t ShadowedLights = 4;

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        if(EngineCore::ConfigVar* MaxShadowed = EngineCore::ConfigFindVarByPath("Shadows.MaxShadowedLights"))
        {
            PreviousShadowedLights = MaxShadowed->GetInt();
            MaxShadowed->SetInt(ShadowedLights);
        }
        /* Where things meet the ground, between roof tiles: what the maps are too coarse for. */
        if(EngineCore::ConfigVar* Contact = EngineCore::ConfigFindVarByPath("Shadows.ContactShadows"))
        {
            PreviousContactShadows = Contact->GetBool() ? 1 : 0;
            Contact->SetBool(true);
        }

        GameCore::World& World = InSession.GetWorld();
        Flicker.Light = World.FindEntityByName("Fire");
        const GameCore::Entity Fire = World.GetEntity(Flicker.Light);
        const RenderCore::PointLightComponent* FirePoint = Fire.GetComponent<RenderCore::PointLightComponent>();
        const RenderCore::SpotLightComponent*  FireSpot  = Fire.GetComponent<RenderCore::SpotLightComponent>();
        const GameCore::TransformComponent* FirePlace = Fire.GetComponent<GameCore::TransformComponent>();
        if((FirePoint || FireSpot) && FirePlace)
        {
            Flicker.Intensity = FirePoint ? FirePoint->Intensity : FireSpot->Intensity;
            Flicker.Center    = FirePlace->Position;
            AddSystem<&WorldsLabCourtyardMode::StepFire>("WorldsLab.Flicker", GameCore::TP_PrePhysics);
        }
        return WorldsLabExploreMode::OnLoad(InSession);
    }

    void OnUnload() override
    {
        /* Looked up again: a config var is not held across a module's life (CoreConfig.h). */
        EngineCore::ConfigVar* MaxShadowed = EngineCore::ConfigFindVarByPath("Shadows.MaxShadowedLights");
        if(MaxShadowed && PreviousShadowedLights >= 0)
        {
            MaxShadowed->SetInt(PreviousShadowedLights);
        }
        PreviousShadowedLights = -1;
        EngineCore::ConfigVar* Contact = EngineCore::ConfigFindVarByPath("Shadows.ContactShadows");
        if(Contact && PreviousContactShadows >= 0)
        {
            Contact->SetBool(PreviousContactShadows != 0);
        }
        PreviousContactShadows = -1;
    }

private:
    void StepFire(GameCore::World& InWorld, const float InDeltaSeconds) { StepFlicker(InWorld, InDeltaSeconds, Flicker); }

    int32_t        PreviousShadowedLights = -1;
    int32_t        PreviousContactShadows = -1;   // -1: not changed
    FlickerContext Flicker;
};

NIMBLE_REGISTER_GAME_MODE(WorldsLabExploreMode, "WorldsLabExplore")
NIMBLE_REGISTER_GAME_MODE(WorldsLabCourtyardMode, "WorldsLabCourtyard")
