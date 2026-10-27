#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/RenderComponents.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

/*
 * WorldsLabDestruction: the DestructionLab world's mode
 * (Content/Levels/DestructionLab.nmworld). Fly round the wall, the crates,
 * the glass and the pillars and shoot steel balls at them; they break by
 * what they are made of (Engine/Source/Public/NimbleEngine/Destruction.h).
 *
 *   Fly    mouse looks, WASD flies where you look, E/Space up, Q/Ctrl
 *          down, Shift faster.
 *   Shoot  left click a ball (hold to keep firing), right click a heavy
 *          one: a ball breaks glass, wood and concrete at once, and stone
 *          after a few; a heavy one breaks anything.
 *   Mouse  Escape lets the cursor go, a click takes it back.
 *
 * Balls fall asleep and stay, up to [WorldsLab] MaxBalls; the oldest go first.
 *
 * [WorldsLab] DestructionDemo=true (or the console, while it plays) flies a
 * fixed sweep and fires on its own: what screenshots and a look without a
 * mouse use.
 */

NIMBLE_CONFIG_FLOAT(GWorldsLabBallSpeed,       "WorldsLab", "BallSpeed",       4000.0f, nullptr);   // units/s
NIMBLE_CONFIG_FLOAT(GWorldsLabBallMass,        "WorldsLab", "BallMass",        4.0f,    nullptr);   // kg
NIMBLE_CONFIG_FLOAT(GWorldsLabHeavyBallMass,   "WorldsLab", "HeavyBallMass",   60.0f,   nullptr);   // kg
NIMBLE_CONFIG_INT(GWorldsLabMaxBalls,          "WorldsLab", "MaxBalls",        60,      nullptr);
NIMBLE_CONFIG_BOOL(GWorldsLabDestructionDemo,  "WorldsLab", "DestructionDemo", false,   nullptr);

/* The DestructionLab controls: the Player's Input component reads map "Destruction". */
NIMBLE_DEFINE_INPUT_MAP(Destruction)
{
    Map.Action("Fire",      "Mouse.Left, Gamepad.RightTrigger");
    Map.Action("FireHeavy", "Mouse.Right, Gamepad.LeftTrigger");
    Map.Action("Run",       "LeftShift, RightShift, Gamepad.LeftStick");
    Map.Action("Pause",     "Escape, Gamepad.Start");

    Map.Axis("MoveForward", "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Axis("MoveRight",   "D, Right, -A, -Left, Gamepad.LeftX");
    Map.Axis("MoveUp",      "E, Space, -Q, -LeftCtrl, Gamepad.RightShoulder, -Gamepad.LeftShoulder");

    /* Degrees this frame; +Y down, so moving the mouse down looks down. */
    Map.Axis("LookX", "Mouse.DeltaX*0.15, Gamepad.RightX*2.5");
    Map.Axis("LookY", "Mouse.DeltaY*0.15, Gamepad.RightY*2.5");
}

namespace
{
    constexpr double DegToRad   = 3.14159265358979323846 / 180.0;
    constexpr double MaxPitch   = 89.0;
    constexpr double FlySpeed   = 700.0;
    constexpr double FlyBoost   = 3.0;
    constexpr double FireEvery  = 0.12;   // seconds between balls while the button is held
    constexpr double BallRadius = 12.0;
    constexpr double HeavyRadius = 30.0;
    constexpr double HeavySpeedScale = 0.7;   // a heavy ball leaves slower

    /* The view direction for a (Pitch, Yaw, 0) rotation, as WorldsLabExplore's. */
    EngineCore::Vector3 ViewForward(const double InPitch, const double InYaw)
    {
        const double P = InPitch * DegToRad;
        const double Y = InYaw * DegToRad;
        return EngineCore::Vector3(std::sin(Y) * std::cos(P), -std::sin(P), std::cos(Y) * std::cos(P));
    }

    /* The demo: where it looks from and at, by time, and when it fires. */
    struct DemoKey { double T, X, Y, Z, Yaw, Pitch; };
    const DemoKey GDemo[] = {
        {  0.0,   80.0, 330.0, -980.0,   0.0, 12.0 },
        {  3.0,   80.0, 330.0, -980.0,   0.0, 12.0 },   // the crates
        {  5.0, -300.0, 300.0, -700.0, -15.0, 10.0 },   // the wall
        {  9.0,  450.0, 260.0, -600.0,   5.0,  8.0 },   // the glass
        { 12.0,  800.0, 300.0, -500.0,   5.0, 10.0 },   // the pillars
        { 16.0,  200.0, 600.0, -1300.0,  0.0, 18.0 },   // back and up, over all of it
    };
    /* Each shot is aimed at a point (DemoAim): the crates, the wall, the glass, the pillars. */
    struct DemoShot { double T; double X, Y, Z; bool bHeavy; };
    const DemoShot GDemoShots[] = {
        {  1.0,   25.0, 200.0,  60.0, false }, {  1.4,   70.0,  40.0,  60.0, false }, {  2.0,   25.0, 120.0,  60.0, true },
        {  6.0, -520.0, 220.0, 300.0, false }, {  6.3, -420.0, 120.0, 300.0, false }, {  6.6, -620.0, 250.0, 300.0, false },
        {  7.2, -520.0, 150.0, 300.0, true  },
        { 10.0,  330.0, 160.0, 260.0, false }, { 10.4,  560.0, 140.0, 260.0, false },
        { 13.0,  820.0, 240.0, 420.0, true  }, { 14.0,  980.0, 200.0, 420.0, true  },
    };

    /* From InFrom toward InTarget, raised for the ball's drop on the way. */
    EngineCore::Vector3 DemoAim(const EngineCore::Vector3& InFrom, const DemoShot& InShot, const double InSpeed)
    {
        const double DX = InShot.X - double(InFrom.GetX()), DY = InShot.Y - double(InFrom.GetY()), DZ = InShot.Z - double(InFrom.GetZ());
        const double Flight = std::sqrt(DX * DX + DY * DY + DZ * DZ) / InSpeed;
        const double Drop   = 0.5 * 981.0 * Flight * Flight;
        return EngineCore::Vector3(DX, DY + Drop, DZ).GetNormalize();
    }

    void DemoPose(const double InT, EngineCore::Vector3& OutPosition, double& OutYaw, double& OutPitch)
    {
        const size_t Count = sizeof(GDemo) / sizeof(GDemo[0]);
        size_t At = 0;
        while(At + 1 < Count && GDemo[At + 1].T <= InT) { ++At; }
        const DemoKey& A = GDemo[At];
        const DemoKey& B = GDemo[std::min(At + 1, Count - 1)];
        const double F = B.T > A.T ? std::clamp((InT - A.T) / (B.T - A.T), 0.0, 1.0) : 0.0;
        const double S = F * F * (3.0 - 2.0 * F);
        OutPosition = EngineCore::Vector3(A.X + (B.X - A.X) * S, A.Y + (B.Y - A.Y) * S, A.Z + (B.Z - A.Z) * S);
        OutYaw   = A.Yaw + (B.Yaw - A.Yaw) * S;
        OutPitch = A.Pitch + (B.Pitch - A.Pitch) * S;
    }
}

class WorldsLabDestructionMode : public NimbleEngine::IGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        GameCore::World& World = InSession.GetWorld();
        Player = World.FindEntityByName("Player");
        if(!Player.IsSet())
        {
            CORE_LOG_ERROR("[WorldsLab] DestructionLab has no entity named Player; nothing to fly")
            return true;
        }
        if(const GameCore::TransformComponent* View = World.GetEntity(Player).GetComponent<GameCore::TransformComponent>())
        {
            Pitch = std::clamp(double(View->Rotation.GetX()), -MaxPitch, MaxPitch);
            Yaw   = double(View->Rotation.GetY());
        }
        AddSystem<&WorldsLabDestructionMode::Step>("WorldsLab.Destruction", GameCore::TP_PrePhysics);
        return true;
    }

    void OnStart() override
    {
        Session->SetMouseCaptured(true);
        CORE_LOG("[WorldsLab] fly with WASD + mouse (E/Q up and down, Shift faster); left click shoots a ball, "
                 "right click a heavy one; Escape frees the mouse")
    }

    void OnTick(const NimbleEngine::FrameContext&) override
    {
        const NimbleEngine::InputComponent* Input = Session->GetWorld().GetEntity(Player).GetComponent<NimbleEngine::InputComponent>();
        if(!Input) { return; }
        if(Input->WasPressed("Pause"))
        {
            Session->SetMouseCaptured(false);
        }
        else if(!Session->IsMouseCaptured() && (Input->WasPressed("Fire") || Input->WasPressed("FireHeavy")))
        {
            /* The click that takes the mouse back does not also shoot. */
            Session->SetMouseCaptured(true);
            bSwallowClick = true;
        }
    }

private:
    /* PrePhysics: look, fly, shoot; a ball made here is simulated this frame. */
    void Step(GameCore::World& InWorld, const float InDeltaSeconds)
    {
        const double DT = double(InDeltaSeconds);
        Clock += DT;
        GameCore::TransformComponent* Transform = InWorld.GetEntity(Player).GetComponent<GameCore::TransformComponent>();
        const NimbleEngine::InputComponent* Input = InWorld.GetEntity(Player).GetComponent<NimbleEngine::InputComponent>();
        if(!Transform || !Input) { return; }

        if(GWorldsLabDestructionDemo.GetBool())
        {
            StepDemo(InWorld, *Transform, DT);
            return;
        }
        DemoTime = 0.0;
        NextDemoShot = 0;

        Yaw   = std::fmod(Yaw + double(Input->GetAxis("LookX")), 360.0);
        Pitch = std::clamp(Pitch + double(Input->GetAxis("LookY")), -MaxPitch, MaxPitch);

        const EngineCore::Vector3 Up(0.0, 1.0, 0.0);
        const EngineCore::Vector3 Forward = ViewForward(Pitch, Yaw);
        const EngineCore::Vector3 Right = (Up ^ Forward).GetNormalize();
        const double Speed = FlySpeed * (Input->IsDown("Run") ? FlyBoost : 1.0);
        EngineCore::Vector3 Position = Transform->Position + (Forward * double(Input->GetAxis("MoveForward"))
                                                             + Right * double(Input->GetAxis("MoveRight"))
                                                             + Up * double(Input->GetAxis("MoveUp"))) * (Speed * DT);
        Position = EngineCore::Vector3(Position.GetX(), std::max(double(Position.GetY()), 30.0), Position.GetZ());
        Transform->Position = Position;
        Transform->Rotation = EngineCore::Vector3(Pitch, Yaw, 0.0);

        if(!Session->IsMouseCaptured()) { return; }
        if(bSwallowClick)
        {
            bSwallowClick = Input->IsDown("Fire") || Input->IsDown("FireHeavy");
            return;
        }
        Cooldown -= DT;
        if(Input->WasPressed("FireHeavy"))
        {
            Shoot(InWorld, Position, Forward, true);
        }
        else if(Input->IsDown("Fire") && (Input->WasPressed("Fire") || Cooldown <= 0.0))
        {
            Shoot(InWorld, Position, Forward, false);
            Cooldown = FireEvery;
        }
    }

    void StepDemo(GameCore::World& InWorld, GameCore::TransformComponent& InOutView, const double InDT)
    {
        DemoTime += InDT;
        EngineCore::Vector3 Position;
        DemoPose(DemoTime, Position, Yaw, Pitch);
        InOutView.Position = Position;
        InOutView.Rotation = EngineCore::Vector3(Pitch, Yaw, 0.0);
        const size_t Shots = sizeof(GDemoShots) / sizeof(GDemoShots[0]);
        while(NextDemoShot < Shots && GDemoShots[NextDemoShot].T <= DemoTime)
        {
            const DemoShot& Shot = GDemoShots[NextDemoShot++];
            const double Speed = double(GWorldsLabBallSpeed.GetFloat()) * (Shot.bHeavy ? HeavySpeedScale : 1.0);
            Shoot(InWorld, Position, DemoAim(Position, Shot, Speed), Shot.bHeavy);
        }
    }

    /* A steel ball from just ahead of the eye, along InForward. */
    void Shoot(GameCore::World& InWorld, const EngineCore::Vector3& InFrom, const EngineCore::Vector3& InForward, const bool bInHeavy)
    {
        const double Radius = bInHeavy ? HeavyRadius : BallRadius;
        char Name[32];
        std::snprintf(Name, sizeof(Name), "%s%u", bInHeavy ? "HeavyBall" : "Ball", ++Fired);
        GameCore::Entity Ball = InWorld.CreateEntity(Name);

        GameCore::TransformComponent Transform;
        Transform.Position = InFrom + InForward * (Radius + 40.0);
        Transform.Scale    = EngineCore::Vector3(Radius * 2.0, Radius * 2.0, Radius * 2.0);   // the Sphere mesh is one across
        Ball.AddComponent(Transform);

        RenderCore::RenderComponent Render;
        std::strncpy(Render.Mesh, "Engine/Content/Meshes/Sphere.nmmesh", sizeof(Render.Mesh) - 1);
        Render.SetMaterial(0, "Content/Materials/ShootingRange/Steel.material.nmgraph");
        Ball.AddComponent(Render);

        GameCore::RigidBodyComponent Body;
        Body.Motion      = GameCore::RBM_Dynamic;
        Body.Shape       = GameCore::RBS_Sphere;
        Body.Mass        = bInHeavy ? GWorldsLabHeavyBallMass.GetFloat() : GWorldsLabBallMass.GetFloat();
        Body.Friction    = 0.4f;
        Body.Restitution = 0.2f;
        Body.bContinuous = 1;   // fast and small: it must not pass through a pane
        const double Speed = double(GWorldsLabBallSpeed.GetFloat()) * (bInHeavy ? HeavySpeedScale : 1.0);
        Body.InitialLinearVelocity = InForward * Speed;
        Ball.AddComponent(Body);

        Balls.push_back(Ball.GetID());
        const size_t Max = size_t(std::max(1, GWorldsLabMaxBalls.GetInt()));
        while(Balls.size() > Max)
        {
            InWorld.DestroyEntity(Balls.front());
            Balls.pop_front();
        }
    }

    GameCore::EntityID             Player;
    std::deque<GameCore::EntityID> Balls;
    double   Yaw = 0.0, Pitch = 0.0;
    double   Clock = 0.0, Cooldown = 0.0;
    double   DemoTime = 0.0;
    size_t   NextDemoShot = 0;
    uint32_t Fired = 0;
    bool     bSwallowClick = false;
};

NIMBLE_REGISTER_GAME_MODE(WorldsLabDestructionMode, "WorldsLabDestruction")
