/*
 * NimbleEnginePhysicsTests -- JoltPhysics simulating GameCore's Rigid Body and
 * Physics Constraint components: falling and resting, stacking, impulses and
 * velocities, kinematic and teleported bodies, joints, removal, shapes
 * inferred from meshes, layers, contact and sensor events, raycasts, and
 * terrain heightfields.
 *
 * Built only with the module (NIMBLE_WITH_JOLTPHYSICS). Runs from the
 * binaries folder, where Engine/Content/Meshes is staged.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/Terrain.h"
#include "Public/GameCore/World.h"
#include "Public/JoltPhysics/PhysicsWorld.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/TerrainFacade.h"
#include "Public/RenderCore/RenderComponents.h"

using namespace GameCore;

/* The Character Mover's walk axes, for the own-hit-box test. */
NIMBLE_DEFINE_INPUT_MAP(PhysicsMoverTest)
{
    Map.Axis("MoveForward", "W, -S");
    Map.Axis("MoveRight",   "D, -A");
}

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

namespace
{
    constexpr float Frame = 1.0f / 60.0f;

    std::string Num(const double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.2f", InValue);
        return Buffer;
    }

    JoltPhysics::PhysicsSettings TestSettings()
    {
        JoltPhysics::PhysicsSettings Settings;   // the defaults: 100 units a metre, -981 gravity, 60 Hz
        Settings.bInterpolate = false;           // each step's pose, so a check reads the simulation exactly
        return Settings;
    }

    void Run(World& InWorld, const double InSeconds)
    {
        const int Frames = int(std::lround(InSeconds / Frame));
        for(int Index = 0; Index < Frames; ++Index)
        {
            InWorld.Tick(Frame);
        }
    }

    EngineCore::Vector3 PositionOf(const Entity& InEntity)
    {
        return InEntity.GetWorldPose().Position;
    }

    /* A Transform at InPosition and a Bounds of InHalf either side. */
    Entity MakeBoxed(World& InWorld, const char* InName, const EngineCore::Vector3& InPosition, const EngineCore::Vector3& InHalf)
    {
        Entity E = InWorld.CreateEntity(InName);
        TransformComponent T;
        T.Position = InPosition;
        E.AddComponent(T);
        BoundsComponent B;
        B.Local = AABB(EngineCore::Vector3(0.0, 0.0, 0.0) - InHalf, InHalf);
        E.AddComponent(B);
        return E;
    }

    RigidBodyComponent Body(const uint8_t InMotion, const uint8_t InShape = RBS_Auto)
    {
        RigidBodyComponent R;
        R.Motion = InMotion;
        R.Shape  = InShape;
        return R;
    }

    RigidBodyComponent MakeChassisBody()
    {
        RigidBodyComponent R;
        R.Motion = RBM_Dynamic;
        R.Mass   = 1200.0f;
        return R;
    }

    /* A 2000-wide static floor whose top is y = 0. */
    Entity MakeFloor(World& InWorld)
    {
        Entity Floor = MakeBoxed(InWorld, "Floor", EngineCore::Vector3(0.0, -10.0, 0.0), EngineCore::Vector3(1000.0, 10.0, 1000.0));
        Floor.AddComponent(Body(RBM_Static));
        return Floor;
    }

    Entity MakeBall(World& InWorld, const char* InName, const EngineCore::Vector3& InPosition, const double InRadius)
    {
        Entity Ball = MakeBoxed(InWorld, InName, InPosition, EngineCore::Vector3(InRadius, InRadius, InRadius));
        Ball.AddComponent(Body(RBM_Dynamic, RBS_Sphere));
        return Ball;
    }

    void TestFallAndRest()
    {
        std::printf("[fall and rest]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Expect(Physics.Attach(W, TestSettings()), "attaches to a World");
        Expect(JoltPhysics::PhysicsWorld::Find(W) == &Physics, "found by its World");
        Expect(NimbleEngine::IsPhysicsAvailable() && NimbleEngine::IsWorldSimulated(W), "the facade sees it");

        MakeFloor(W);
        Entity Ball = MakeBall(W, "Ball", EngineCore::Vector3(0.0, 300.0, 0.0), 25.0);

        Run(W, 0.25);
        const double Falling = PositionOf(Ball).GetY();
        Expect(Falling < 300.0 && Falling > 200.0, "falls under gravity (y " + Num(Falling) + " after 0.25 s, ~269 expected)");
        Expect(Ball.GetComponent<RigidBodyComponent>()->LinearVelocity.GetY() < -200.0f, "its velocity is written back");

        Run(W, 3.0);
        const double Rest = PositionOf(Ball).GetY();
        Expect(std::fabs(Rest - 25.0) < 2.0, "rests on the floor (y " + Num(Rest) + ", 25 expected)");

        JoltPhysics::PhysicsStats Stats;
        Physics.GetStats(Stats);
        Expect(Stats.Bodies == 2 && Stats.Steps >= 190, "two bodies, a step a frame (" + std::to_string(Stats.Steps) + ")");

        Physics.Detach();
        Expect(!JoltPhysics::PhysicsWorld::Find(W) && !NimbleEngine::IsWorldSimulated(W), "detached");
    }

    void TestStack()
    {
        std::printf("[stack]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        MakeFloor(W);

        Entity Boxes[5];
        for(int Index = 0; Index < 5; ++Index)
        {
            Boxes[Index] = MakeBoxed(W, "Crate", EngineCore::Vector3(0.0, 25.0 + 50.0 * Index, 0.0), EngineCore::Vector3(25.0, 25.0, 25.0));
            Boxes[Index].AddComponent(Body(RBM_Dynamic));
        }
        Run(W, 3.0);

        const EngineCore::Vector3 Top = PositionOf(Boxes[4]);
        Expect(std::fabs(Top.GetY() - 225.0) < 3.0 && std::fabs(Top.GetX()) < 3.0 && std::fabs(Top.GetZ()) < 3.0,
               "a five-crate stack stands (top at " + Num(Top.GetX()) + ", " + Num(Top.GetY()) + ", " + Num(Top.GetZ()) + ")");

        JoltPhysics::PhysicsStats Stats;
        Physics.GetStats(Stats);
        Expect(Stats.ActiveBodies == 0, "and goes to sleep (" + std::to_string(Stats.ActiveBodies) + " awake)");
        Expect(!Boxes[0].GetComponent<RigidBodyComponent>()->IsAwake(), "the component says it sleeps");
    }

    void TestImpulseAndVelocity()
    {
        std::printf("[impulse and velocity]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());

        /* Weightless and undamped: momentum alone moves it. */
        Entity Puck = MakeBall(W, "Puck", EngineCore::Vector3(0.0, 0.0, 0.0), 10.0);
        RigidBodyComponent* R = Puck.GetComponent<RigidBodyComponent>();
        R->GravityFactor = 0.0f;
        R->LinearDamping = 0.0f;
        R->Mass          = 2.0f;
        W.Tick(Frame);

        Puck.GetComponent<RigidBodyComponent>()->AddImpulse(EngineCore::Vector3(200.0, 0.0, 0.0));   // 2 kg: 100 units/s
        Run(W, 1.0);
        const EngineCore::Vector3 After = PositionOf(Puck);
        Expect(std::fabs(After.GetX() - 100.0) < 3.0, "an impulse of 200 on 2 kg moves 100 units in a second (x " + Num(After.GetX()) + ")");
        Expect(std::fabs(Puck.GetComponent<RigidBodyComponent>()->LinearVelocity.GetX() - 100.0) < 1.0, "velocity 100 read back");

        Puck.GetComponent<RigidBodyComponent>()->SetLinearVelocity(EngineCore::Vector3(0.0, 0.0, 50.0));
        Run(W, 1.0);
        const EngineCore::Vector3 Then = PositionOf(Puck);
        Expect(std::fabs(Then.GetX() - After.GetX()) < 1.0 && std::fabs(Then.GetZ() - 50.0) < 2.0,
               "SetLinearVelocity replaces it (z " + Num(Then.GetZ()) + ")");

        /* A force lasts one frame: F = m * a over 1/60 s. */
        Puck.GetComponent<RigidBodyComponent>()->SetLinearVelocity(EngineCore::Vector3(0.0, 0.0, 0.0));
        W.Tick(Frame);
        Puck.GetComponent<RigidBodyComponent>()->AddForce(EngineCore::Vector3(0.0, 120.0, 0.0));   // 2 kg: 60 u/s^2 for 1/60 s
        Run(W, 0.1);   // not longer: 1 unit/s is slow enough to fall asleep at
        const double Up = Puck.GetComponent<RigidBodyComponent>()->LinearVelocity.GetY();
        Expect(std::fabs(Up - 1.0) < 0.1, "a force acts for one frame (vy " + Num(Up) + ", 1 expected)");

        /* Initial velocity, authored. */
        Entity Thrown = MakeBall(W, "Thrown", EngineCore::Vector3(0.0, 500.0, 0.0), 10.0);
        RigidBodyComponent* T = Thrown.GetComponent<RigidBodyComponent>();
        T->GravityFactor = 0.0f;
        T->LinearDamping = 0.0f;
        T->InitialLinearVelocity = EngineCore::Vector3(-40.0, 0.0, 0.0);
        Run(W, 1.0);
        Expect(std::fabs(PositionOf(Thrown).GetX() + 40.0) < 2.0, "starts with its initial velocity (x " + Num(PositionOf(Thrown).GetX()) + ")");
    }

    void TestKinematicAndTeleport()
    {
        std::printf("[kinematic and teleport]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        MakeFloor(W);

        /* A kinematic lift with a crate on it, raised 100 over a second. */
        Entity Lift = MakeBoxed(W, "Lift", EngineCore::Vector3(0.0, 10.0, 0.0), EngineCore::Vector3(100.0, 10.0, 100.0));
        Lift.AddComponent(Body(RBM_Kinematic));
        Entity Crate = MakeBoxed(W, "Crate", EngineCore::Vector3(0.0, 45.0, 0.0), EngineCore::Vector3(25.0, 25.0, 25.0));
        Crate.AddComponent(Body(RBM_Dynamic));
        Run(W, 1.0);
        const double Before = PositionOf(Crate).GetY();
        for(int Index = 0; Index < 60; ++Index)
        {
            Lift.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(0.0, 10.0 + 100.0 * (Index + 1) / 60.0, 0.0);
            W.Tick(Frame);
        }
        Run(W, 0.5);
        const double Raised = PositionOf(Crate).GetY();
        Expect(std::fabs(Before - 45.0) < 2.0 && std::fabs(Raised - 145.0) < 3.0,
               "a kinematic lift carries a crate up (" + Num(Before) + " -> " + Num(Raised) + ")");
        Expect(std::fabs(PositionOf(Lift).GetY() - 110.0) < 1e-3, "and its Transform is its own (not written back)");

        /* Game code moves a dynamic body: a teleport. */
        Crate.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(500.0, 400.0, 0.0);
        Run(W, 3.0);
        const EngineCore::Vector3 Landed = PositionOf(Crate);
        Expect(std::fabs(Landed.GetX() - 500.0) < 2.0 && std::fabs(Landed.GetY() - 25.0) < 2.0,
               "a moved Transform teleports the body, which falls from there (" + Num(Landed.GetX()) + ", " + Num(Landed.GetY()) + ")");
    }

    void TestConstraints()
    {
        std::printf("[constraints]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());

        /* A pendulum: a ball on a Point joint 200 above it, pushed sideways. */
        Entity Bob = MakeBall(W, "Bob", EngineCore::Vector3(0.0, 0.0, 0.0), 20.0);
        PhysicsConstraintComponent Joint;
        Joint.Type  = PCT_Point;
        Joint.Pivot = EngineCore::Vector3(0.0, 200.0, 0.0);
        Bob.AddComponent(Joint);
        Bob.GetComponent<RigidBodyComponent>()->InitialLinearVelocity = EngineCore::Vector3(300.0, 0.0, 0.0);

        double Worst = 0.0, Farthest = 0.0;
        for(int Index = 0; Index < 180; ++Index)
        {
            W.Tick(Frame);
            const EngineCore::Vector3 P = PositionOf(Bob);
            const double Length = std::sqrt(double(P.GetX()) * P.GetX() + (P.GetY() - 200.0) * (P.GetY() - 200.0) + double(P.GetZ()) * P.GetZ());
            Worst    = std::max(Worst, std::fabs(Length - 200.0));
            Farthest = std::max(Farthest, std::fabs(double(P.GetX())));
        }
        Expect(Worst < 3.0 && Farthest > 50.0, "a Point joint swings at its length (off by at most " + Num(Worst)
                                              + ", swung " + Num(Farthest) + ")");

        JoltPhysics::PhysicsStats Stats;
        Physics.GetStats(Stats);
        Expect(Stats.Constraints == 1, "one constraint");

        /* A motorised hinge to the world: a paddle turning about Y at 90 deg/s. */
        Entity Paddle = MakeBoxed(W, "Paddle", EngineCore::Vector3(500.0, 0.0, 0.0), EngineCore::Vector3(100.0, 10.0, 10.0));
        RigidBodyComponent PaddleBody = Body(RBM_Dynamic);
        PaddleBody.GravityFactor = 0.0f;
        Paddle.AddComponent(PaddleBody);
        PhysicsConstraintComponent Hinge;
        Hinge.Type       = PCT_Hinge;
        Hinge.Axis       = EngineCore::Vector3(0.0, 1.0, 0.0);
        Hinge.bMotor     = 1;
        Hinge.MotorSpeed = 90.0f;
        Paddle.AddComponent(Hinge);
        Run(W, 1.0);
        const double Turned = std::fabs(double(Paddle.GetComponent<RigidBodyComponent>()->AngularVelocity.GetY()));
        Expect(std::fabs(Turned - 90.0) < 5.0 && std::fabs(PositionOf(Paddle).GetX() - 500.0) < 1.0,
               "a hinge motor turns it in place (" + Num(Turned) + " deg/s)");

        /* Disabled: the joint goes; the bob falls. */
        Bob.GetComponent<PhysicsConstraintComponent>()->bEnabled = 0;
        Run(W, 1.0);
        Physics.GetStats(Stats);
        Expect(Stats.Constraints == 1 && PositionOf(Bob).GetY() < -100.0, "a disabled joint lets go");
    }

    void TestRemoval()
    {
        std::printf("[removal]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        MakeFloor(W);
        Entity A = MakeBall(W, "A", EngineCore::Vector3(0.0, 100.0, 0.0), 10.0);
        Entity B = MakeBall(W, "B", EngineCore::Vector3(100.0, 100.0, 0.0), 10.0);
        W.Tick(Frame);

        JoltPhysics::PhysicsStats Stats;
        Physics.GetStats(Stats);
        Expect(Stats.Bodies == 3, "three bodies");

        A.RemoveComponent<RigidBodyComponent>();
        W.DestroyEntity(B.GetID());
        W.Tick(Frame);
        Physics.GetStats(Stats);
        Expect(Stats.Bodies == 1, "a removed component and a destroyed entity take their bodies (" + std::to_string(Stats.Bodies) + " left)");

        Entity C = MakeBall(W, "C", EngineCore::Vector3(0.0, 100.0, 0.0), 10.0);
        C.GetComponent<RigidBodyComponent>()->bEnabled = 0;
        W.Tick(Frame);
        Physics.GetStats(Stats);
        Expect(Stats.Bodies == 1, "a disabled body has none");
        C.GetComponent<RigidBodyComponent>()->bEnabled = 1;
        W.Tick(Frame);
        Physics.GetStats(Stats);
        Expect(Stats.Bodies == 2, "re-enabled, it does");
    }

    Entity MakeMeshed(World& InWorld, const char* InName, const char* InMesh, const EngineCore::Vector3& InPosition,
                      const double InScale, const uint8_t InShape)
    {
        Entity E = InWorld.CreateEntity(InName);
        TransformComponent T;
        T.Position = InPosition;
        T.Scale    = EngineCore::Vector3(InScale, InScale, InScale);
        E.AddComponent(T);
        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InMesh);
        E.AddComponent(Render);
        E.AddComponent(Body(RBM_Dynamic, InShape));
        return E;
    }

    void TestMeshShapes()
    {
        std::printf("[mesh shapes]\n");
        NimbleEngine::InstallCollisionMeshProvider();   // what a GameSession does
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        MakeFloor(W);

        /* No Bounds at all: the Box mesh, 100x, is a box from its bounds. */
        Entity Box    = MakeMeshed(W, "Box", "Engine/Content/Meshes/Box.nmmesh", EngineCore::Vector3(0.0, 300.0, 0.0), 100.0, RBS_Auto);
        /* The Sphere mesh's hull, asked for: rests on its lowest vertex. */
        Entity Hull   = MakeMeshed(W, "Hull", "Engine/Content/Meshes/Sphere.nmmesh", EngineCore::Vector3(300.0, 300.0, 0.0), 100.0, RBS_ConvexHull);
        /* The Sphere mesh, Auto: a sphere fitted to it, which rolls. */
        Entity Sphere = MakeMeshed(W, "Sphere", "Engine/Content/Meshes/Sphere.nmmesh", EngineCore::Vector3(-300.0, 300.0, 0.0), 100.0, RBS_Auto);
        Run(W, 4.0);

        Expect(std::fabs(PositionOf(Box).GetY() - 50.0) < 2.0, "Box.nmmesh at 100x rests as a 100 box (y " + Num(PositionOf(Box).GetY()) + ")");
        Expect(std::fabs(PositionOf(Hull).GetY() - 50.0) < 3.0, "a convex hull of the sphere mesh rests (y " + Num(PositionOf(Hull).GetY()) + ")");
        Expect(std::fabs(PositionOf(Sphere).GetY() - 50.0) < 2.0, "Sphere.nmmesh at 100x rests as a radius-50 sphere (y "
                                                                  + Num(PositionOf(Sphere).GetY()) + ")");

        /* A static triangle mesh: Auto on a mesh that is no primitive. The
         * Box mesh named explicitly as TriangleMesh stands in for one. */
        World W2;
        JoltPhysics::PhysicsWorld Physics2;
        Physics2.Attach(W2, TestSettings());
        Entity Ground = W2.CreateEntity("Ground");
        TransformComponent T;
        T.Position = EngineCore::Vector3(0.0, -50.0, 0.0);
        T.Scale    = EngineCore::Vector3(1000.0, 100.0, 1000.0);
        Ground.AddComponent(T);
        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Box.nmmesh");
        Ground.AddComponent(Render);
        Ground.AddComponent(Body(RBM_Static, RBS_TriangleMesh));
        Entity Ball = MakeBall(W2, "Ball", EngineCore::Vector3(0.0, 200.0, 0.0), 25.0);
        Run(W2, 3.0);
        Expect(std::fabs(PositionOf(Ball).GetY() - 25.0) < 2.0, "lands on a static triangle mesh (y " + Num(PositionOf(Ball).GetY()) + ")");

        /* The mesh changes: the shape follows (a 100 box becomes a 50 box). */
        std::snprintf(Box.GetComponent<RenderCore::RenderComponent>()->Mesh, RenderCore::RENDER_MESH_PATH_MAX, "%s", "Engine/Content/Meshes/Box.nmmesh");
        Box.GetComponent<TransformComponent>()->Scale = EngineCore::Vector3(50.0, 50.0, 50.0);
        Run(W, 2.0);
        Expect(std::fabs(PositionOf(Box).GetY() - 25.0) < 2.0, "a new scale reshapes it (y " + Num(PositionOf(Box).GetY()) + ")");
    }

    void TestLayers()
    {
        std::printf("[layers]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        MakeFloor(W);   // Layer 1, Mask everything

        Entity Ghost = MakeBall(W, "Ghost", EngineCore::Vector3(0.0, 100.0, 0.0), 20.0);
        Ghost.GetComponent<RigidBodyComponent>()->Layer = 2;
        Ghost.GetComponent<RigidBodyComponent>()->Mask  = 2;   // collides with layer 2 only: not the floor
        Entity Solid = MakeBall(W, "Solid", EngineCore::Vector3(200.0, 100.0, 0.0), 20.0);
        Run(W, 2.0);
        Expect(PositionOf(Ghost).GetY() < -500.0, "a body masking out the floor falls through it");
        Expect(std::fabs(PositionOf(Solid).GetY() - 20.0) < 2.0, "one that does not lands");
    }

    struct EventLog
    {
        int ContactBegins = 0;
        int ContactEnds   = 0;
        int SensorEnters  = 0;
        int SensorExits   = 0;
        EntityID LastSensor;
    };

    void OnEvent(const EventID InEvent, const EventArgs& InArgs, void* InUserData)
    {
        (void)InEvent;
        (void)InArgs;
        ++*static_cast<int*>(InUserData);
    }

    void TestEvents()
    {
        std::printf("[events]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        EventLog Log;
        W.GetEvents().Subscribe("ContactBegin", &OnEvent, &Log.ContactBegins);
        W.GetEvents().Subscribe("ContactEnd",   &OnEvent, &Log.ContactEnds);
        W.GetEvents().Subscribe("SensorEnter",  &OnEvent, &Log.SensorEnters);
        W.GetEvents().Subscribe("SensorExit",   &OnEvent, &Log.SensorExits);

        MakeFloor(W);
        /* A static sensor box the ball falls through on its way down. */
        Entity Zone = MakeBoxed(W, "Zone", EngineCore::Vector3(0.0, 300.0, 0.0), EngineCore::Vector3(100.0, 50.0, 100.0));
        RigidBodyComponent ZoneBody = Body(RBM_Static);
        ZoneBody.bSensor = 1;
        Zone.AddComponent(ZoneBody);
        Entity Ball = MakeBall(W, "Ball", EngineCore::Vector3(0.0, 500.0, 0.0), 20.0);
        Run(W, 3.0);

        Expect(Log.SensorEnters == 1 && Log.SensorExits == 1, "the sensor told of the ball entering and leaving ("
               + std::to_string(Log.SensorEnters) + ", " + std::to_string(Log.SensorExits) + ")");
        Expect(std::fabs(PositionOf(Ball).GetY() - 20.0) < 2.0, "a sensor never stops what passes");
        Expect(Log.ContactBegins == 2, "landing: ContactBegin, once for each side (" + std::to_string(Log.ContactBegins) + ")");

        Expect(Log.ContactEnds == 0, "resting, then asleep: no ContactEnd (" + std::to_string(Log.ContactEnds) + ")");
        /* Kicked up off the floor: they part, each side told; it lands again. */
        Ball.GetComponent<RigidBodyComponent>()->SetLinearVelocity(EngineCore::Vector3(0.0, 400.0, 0.0));
        Run(W, 0.2);
        Expect(Log.ContactEnds == 2, "parted: ContactEnd, once for each side (" + std::to_string(Log.ContactEnds) + ")");
        Run(W, 3.0);
        Expect(Log.ContactBegins == 4, "and begins again on landing (" + std::to_string(Log.ContactBegins) + ")");

        W.DestroyEntity(Ball.GetID());
        W.Tick(Frame);
        Expect(Log.ContactEnds == 3, "destroyed: the survivor hears ContactEnd (" + std::to_string(Log.ContactEnds) + ")");
        W.GetEvents().UnsubscribeAll(&Log.ContactBegins);
        W.GetEvents().UnsubscribeAll(&Log.ContactEnds);
        W.GetEvents().UnsubscribeAll(&Log.SensorEnters);
        W.GetEvents().UnsubscribeAll(&Log.SensorExits);
    }

    void TestRaycast()
    {
        std::printf("[raycast]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        Entity Floor = MakeFloor(W);
        Entity Zone = MakeBoxed(W, "Zone", EngineCore::Vector3(0.0, 300.0, 0.0), EngineCore::Vector3(100.0, 50.0, 100.0));
        RigidBodyComponent ZoneBody = Body(RBM_Static);
        ZoneBody.bSensor = 1;
        Zone.AddComponent(ZoneBody);
        W.Tick(Frame);

        NimbleEngine::PhysicsRaycastHit Hit;
        const bool bHit = NimbleEngine::PhysicsRaycast(W, EngineCore::Vector3(10.0, 1000.0, 0.0), EngineCore::Vector3(0.0, -2.0, 0.0), 5000.0f, Hit);
        Expect(bHit && Hit.Entity == Floor.GetID(), "a ray down hits the floor, through the sensor");
        Expect(std::fabs(Hit.Distance - 1000.0f) < 0.5f && std::fabs(Hit.Point.GetY()) < 0.5f, "at 1000 (" + Num(Hit.Distance) + ")");
        Expect(std::fabs(Hit.Normal.GetY() - 1.0f) < 1e-3f, "facing up");

        Expect(!NimbleEngine::PhysicsRaycast(W, EngineCore::Vector3(10.0, 1000.0, 0.0), EngineCore::Vector3(0.0, -1.0, 0.0), 500.0f, Hit),
               "too short: no hit");
        Expect(!NimbleEngine::PhysicsRaycast(W, EngineCore::Vector3(10.0, 1000.0, 0.0), EngineCore::Vector3(0.0, -1.0, 0.0), 5000.0f, Hit, 2),
               "masked out: no hit");
    }

    /* The Character Mover's rays pass through its own bodies -- a hit box on a
     * child, on the layer it walks on -- yet still meet another's. */
    void TestCharacterMoverIgnoresOwnBodies()
    {
        std::printf("[character mover: own bodies]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        MakeFloor(W);

        Entity Walker = W.CreateEntity("Walker");
        Walker.AddComponent<TransformComponent>();
        NimbleEngine::AvatarHarnessComponent Harness;
        std::snprintf(Harness.InputMap, sizeof(Harness.InputMap), "%s", "PhysicsMoverTest");
        Walker.AddComponent(Harness);
        Walker.AddComponent<NimbleEngine::CharacterMoverComponent>();   // Mask -1: every layer, the hit box's too
        NimbleEngine::TeleportCharacter(W, Walker.GetID(), EngineCore::Vector3(0.0, 0.0, 0.0), 0.0f);

        /* A hit box round the legs and chest, on a mesh child, as a ragdoll's would be. */
        Entity Mesh = W.CreateEntity("Mesh");
        Mesh.AddComponent<TransformComponent>();
        Mesh.SetParent(Walker.GetID());
        Entity HitBox = MakeBoxed(W, "HitBox", EngineCore::Vector3(0.0, 80.0, 0.0), EngineCore::Vector3(25.0, 75.0, 25.0));
        HitBox.SetParent(Mesh.GetID());
        HitBox.AddComponent(Body(RBM_Kinematic));

        /* And someone else's, ahead. */
        Entity Other = MakeBoxed(W, "Other", EngineCore::Vector3(0.0, 80.0, 400.0), EngineCore::Vector3(25.0, 75.0, 25.0));
        Other.AddComponent(Body(RBM_Static));
        W.Tick(Frame);

        NimbleEngine::PhysicsRaycastHit Hit;
        Expect(NimbleEngine::PhysicsRaycast(W, EngineCore::Vector3(0.0, 500.0, 0.0), EngineCore::Vector3(0.0, -1.0, 0.0), 1000.0f, Hit)
                   && Hit.Entity == HitBox.GetID(),
               "a plain ray meets the hit box");
        Expect(NimbleEngine::PhysicsRaycast(W, EngineCore::Vector3(0.0, 500.0, 0.0), EngineCore::Vector3(0.0, -1.0, 0.0), 1000.0f, Hit, -1,
                                            Walker.GetID())
                   && Hit.Entity != HitBox.GetID() && std::fabs(Hit.Point.GetY()) < 0.5f,
               "ignoring the walker passes through its grandchild's body to the floor");

        NimbleEngine::AvatarInputFrame Input;
        Input.Seconds = Frame;
        const int Forward = NimbleEngine::FindInputAxis("PhysicsMoverTest", "MoveForward");
        Expect(Forward >= 0, "the test map has MoveForward");
        if(Forward >= 0) { Input.Axes[Forward] = 1.0f; }
        double HighestFeet = 0.0;
        for(int I = 0; I < 120; ++I)
        {
            NimbleEngine::StepCharacter(W, Walker.GetID(), Input);
            W.Tick(Frame);
            HighestFeet = (std::max)(HighestFeet, double(Walker.GetComponent<TransformComponent>()->Position.GetY()));
        }
        const TransformComponent& Placed = *Walker.GetComponent<TransformComponent>();
        const NimbleEngine::CharacterMoverComponent& Mover = *Walker.GetComponent<NimbleEngine::CharacterMoverComponent>();
        Expect(HighestFeet < 1.0 && Mover.bGrounded != 0, "it stands on the floor, not on its own hit box (" + Num(HighestFeet) + ")");
        Expect(Placed.Position.GetZ() > 250.0, "it walks out of its own hit box (" + Num(Placed.Position.GetZ()) + ")");
        Expect(Placed.Position.GetZ() < 400.0 - 25.0 - 25.0, "but another's stops it (" + Num(Placed.Position.GetZ()) + ")");
    }
}

namespace
{
    /* A terrain entity: 65 x 65 samples 50 apart (32 m a side), rising 1 in 4 along X, plus a bump. */
    Entity MakeTerrain(World& InWorld, const char* InPath, const EngineCore::Vector3& InAt)
    {
        auto Data = std::make_shared<TerrainData>();
        Data->Reset(65, 50.0f);
        {
            std::lock_guard<std::mutex> Guard(Data->GetLock());
            TerrainRect All;
            for(int32_t Z = 0; Z < 65; ++Z)
            {
                for(int32_t X = 0; X < 65; ++X)
                {
                    const float DX = float(X - 40), DZ = float(Z - 32);
                    Data->SetHeight(X, Z, 0.25f * float(X) * 50.0f + 150.0f * std::exp(-(DX * DX + DZ * DZ) / 18.0f));
                    All.Add(X, Z);
                }
            }
            Data->MarkDirty(All);
        }
        TerrainAssetCache::Store(InPath, Data);

        Entity Ground = InWorld.CreateEntity("Terrain");
        TransformComponent T;
        T.Position = InAt;
        Ground.AddComponent(T);
        RenderCore::TerrainComponent C;
        std::snprintf(C.Terrain, sizeof(C.Terrain), "%s", InPath);
        Ground.AddComponent(C);
        Ground.AddComponent(Body(RBM_Static));   // Shape Auto: the heightfield
        return Ground;
    }

    void TestHeightField()
    {
        std::printf("[heightfield]\n");
        const char* Path = "Content/Levels/PhysicsTest/Terrain/Ground.nmterrain";
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        Entity Ground = MakeTerrain(W, Path, EngineCore::Vector3(-800.0, 0.0, -800.0));
        W.Tick(Frame);
        std::shared_ptr<TerrainData> Data = TerrainAssetCache::Find(Path);

        /* Rays land where the terrain's own sampling says, everywhere: one triangulation. */
        int Off = 0, Missed = 0;
        for(int Each = 0; Each < 60; ++Each)
        {
            const double X = -780.0 + double(Each * 53 % 1560), Z = -780.0 + double(Each * 31 % 1560);
            NimbleEngine::PhysicsRaycastHit Hit;
            if(!NimbleEngine::PhysicsRaycast(W, EngineCore::Vector3(X, 2000.0, Z), EngineCore::Vector3(0.0, -1.0, 0.0), 5000.0f, Hit)
               || Hit.Entity != Ground.GetID())
            {
                ++Missed;
                continue;
            }
            const float Expected = Data->SampleHeight(float(X + 800.0), float(Z + 800.0));
            if(std::fabs(float(Hit.Point.GetY()) - Expected) > 2.0f) { ++Off; }   // Jolt quantises to 8 bits a 4 x 4 block
        }
        Expect(Missed == 0, "rays down hit the heightfield body (" + std::to_string(Missed) + " missed)");
        Expect(Off == 0, "where SampleHeight says, within Jolt's quantisation (" + std::to_string(Off) + " off)");

        /* A ball dropped on the slope rolls downhill (toward -X) and stays on the surface. */
        Entity Ball = MakeBall(W, "Ball", EngineCore::Vector3(0.0, 800.0, -400.0), 30.0);
        Run(W, 3.0);
        const EngineCore::Vector3 At = PositionOf(Ball);
        float Ground0 = 0.0f;
        NimbleEngine::SampleGround(W, At, Ground0);
        Expect(At.GetX() < -50.0, "the ball rolled downhill: x " + Num(At.GetX()));
        Expect(At.GetY() > double(Ground0) + 20.0 && At.GetY() < double(Ground0) + 45.0,
               "and rests on the surface (y " + Num(At.GetY()) + ", ground " + Num(Ground0) + ")");

        /* An edit is picked up: flatten everything and the body is rebuilt. */
        {
            std::lock_guard<std::mutex> Guard(Data->GetLock());
            TerrainRect All;
            for(int32_t Z = 0; Z < 65; ++Z) { for(int32_t X = 0; X < 65; ++X) { Data->SetHeight(X, Z, 100.0f); All.Add(X, Z); } }
            Data->MarkDirty(All);
        }
        W.Tick(Frame);
        NimbleEngine::PhysicsRaycastHit Hit;
        Expect(NimbleEngine::PhysicsRaycast(W, EngineCore::Vector3(500.0, 2000.0, 300.0), EngineCore::Vector3(0.0, -1.0, 0.0), 5000.0f, Hit)
               && std::fabs(Hit.Point.GetY() - 100.0) < 1.0, "an edited terrain's body is rebuilt (y " + Num(Hit.Point.GetY()) + ")");
        TerrainAssetCache::Invalidate(Path);
    }
}

namespace
{
    /* A car: a 180 x 40 x 400 chassis (Bounds), 1200 kg, and four wheels of
     * radius 40 as children named for their slots -- no meshes, so the
     * Vehicle names their size. */
    Entity MakeCar(World& InWorld, const EngineCore::Vector3& InAt)
    {
        Entity Car = MakeBoxed(InWorld, "Car", InAt, EngineCore::Vector3(90.0, 20.0, 200.0));
        RigidBodyComponent Body = MakeChassisBody();
        Car.AddComponent(Body);
        VehicleComponent Vehicle;
        Vehicle.WheelRadius = 40.0f;
        Vehicle.WheelWidth  = 30.0f;
        Car.AddComponent(Vehicle);

        const char* const Names[] = { "WheelFL", "WheelFR", "WheelRL", "WheelRR" };
        for(int Slot = 0; Slot < 4; ++Slot)
        {
            Entity Wheel = InWorld.CreateEntity(Names[Slot]);
            TransformComponent T;
            T.Position = EngineCore::Vector3(Slot % 2 ? 100.0 : -100.0, -25.0, Slot < 2 ? 140.0 : -140.0);
            Wheel.AddComponent(T);
            Wheel.SetParent(Car, false);
        }
        return Car;
    }

    void TestVehicle()
    {
        std::printf("[vehicle]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        Entity Floor = MakeBoxed(W, "Floor", EngineCore::Vector3(0.0, -10.0, 0.0), EngineCore::Vector3(5000.0, 10.0, 5000.0));
        Floor.AddComponent(Body(RBM_Static));

        Entity Car = MakeCar(W, EngineCore::Vector3(0.0, 100.0, 0.0));
        Run(W, 3.0);
        const VehicleComponent* V = Car.GetComponent<VehicleComponent>();
        const GameCore::TransformPose Rest = Car.GetWorldPose();
        JoltPhysics::PhysicsStats Stats;
        Physics.GetStats(Stats);
        Expect(Stats.Vehicles == 1 && V->Handle != 0, "the Vehicle is running");
        Expect(V->WheelsOnGround == 4, "it settles on four wheels (" + std::to_string(V->WheelsOnGround) + ")");
        Expect(Rest.Position.GetY() > 40.0f && Rest.Position.GetY() < 120.0f && std::fabs(Rest.Rotation.GetX()) < 3.0f
               && std::fabs(Rest.Rotation.GetZ()) < 3.0f, "on its suspension, level (y " + Num(Rest.Position.GetY()) + ")");
        const Entity WheelFL = W.GetEntity(W.FindEntityByName("WheelFL"));
        const double WheelY = WheelFL.GetWorldPose().Position.GetY();
        Expect(std::fabs(WheelY - 40.0) < 3.0, "the wheel's Transform is where the suspension holds it: on the ground (y "
                                              + Num(WheelY) + ", radius 40)");

        /* Throttle: away along +Z, through the gears. */
        Car.GetComponent<VehicleComponent>()->SetInput(1.0f, 0.0f, 0.0f, 0.0f);
        Run(W, 3.0);
        V = Car.GetComponent<VehicleComponent>();
        const EngineCore::Vector3 Driven = Car.GetWorldPose().Position;
        Expect(Driven.GetZ() > 300.0f && std::fabs(Driven.GetX()) < 30.0f && V->Speed > 200.0f && V->Gear >= 1,
               "throttle drives it forward (z " + Num(Driven.GetZ()) + ", " + Num(V->Speed) + " units/s, gear " + std::to_string(V->Gear) + ")");

        /* Steering right turns toward +X. */
        Car.GetComponent<VehicleComponent>()->SetInput(1.0f, 1.0f, 0.0f, 0.0f);
        Run(W, 1.5);
        const EngineCore::Vector3 Turned = Car.GetWorldPose().Position;
        Expect(Turned.GetX() - Driven.GetX() > 50.0f, "Right = 1 turns it toward +X (x " + Num(Driven.GetX()) + " -> " + Num(Turned.GetX()) + ")");

        /* Reverse while rolling forward: brakes, then backs up. */
        const float Moving = Car.GetComponent<VehicleComponent>()->Speed;
        Car.GetComponent<VehicleComponent>()->SetInput(-1.0f, 0.0f, 0.0f, 0.0f);
        Run(W, 1.0);
        const float Slowing = Car.GetComponent<VehicleComponent>()->Speed;
        Run(W, 5.0);
        const float Backing = Car.GetComponent<VehicleComponent>()->Speed;
        Expect(Slowing < Moving - 300.0f && Slowing > 0.0f && Backing < -20.0f,
               "reverse brakes first, then backs up (" + Num(Moving) + ", " + Num(Slowing) + ", then " + Num(Backing) + ")");
    }

    void TestVehicleOverBump()
    {
        std::printf("[vehicle over a bump]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        Entity Floor = MakeBoxed(W, "Floor", EngineCore::Vector3(0.0, -10.0, 0.0), EngineCore::Vector3(5000.0, 10.0, 5000.0));
        Floor.AddComponent(Body(RBM_Static));
        Entity Bump = MakeBoxed(W, "Bump", EngineCore::Vector3(0.0, 7.5, 900.0), EngineCore::Vector3(400.0, 7.5, 40.0));
        Bump.AddComponent(Body(RBM_Static));

        Entity Car = MakeCar(W, EngineCore::Vector3(0.0, 100.0, 0.0));
        Run(W, 2.0);
        Car.GetComponent<VehicleComponent>()->SetInput(0.6f, 0.0f, 0.0f, 0.0f);
        double MaxLift = 0.0;
        const double Base = Car.GetWorldPose().Position.GetY();
        for(int Index = 0; Index < 300; ++Index)
        {
            W.Tick(Frame);
            MaxLift = std::max(MaxLift, double(Car.GetWorldPose().Position.GetY()) - Base);
        }
        const GameCore::TransformPose After = Car.GetWorldPose();
        Expect(After.Position.GetZ() > 1300.0f, "it drives over a 15 cm bump (z " + Num(After.Position.GetZ()) + ")");
        Expect(MaxLift > 3.0 && MaxLift < 60.0, "its chassis rises over it on the suspension (by " + Num(MaxLift) + ")");
        Expect(std::fabs(After.Rotation.GetZ()) < 5.0f && Car.GetComponent<VehicleComponent>()->WheelsOnGround == 4,
               "and stays upright, all wheels down");
    }
}

int main()
{
    std::printf("NimbleEnginePhysicsTests\n");
    TestFallAndRest();
    TestStack();
    TestImpulseAndVelocity();
    TestKinematicAndTeleport();
    TestConstraints();
    TestRemoval();
    TestMeshShapes();
    TestLayers();
    TestEvents();
    TestRaycast();
    TestCharacterMoverIgnoresOwnBodies();
    TestHeightField();
    TestVehicle();
    TestVehicleOverBump();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
