#include "Private/LightLab/LightLabPhysics.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/UI/UISystem.h"

/*
 * PhysicsLab: what the JoltPhysics module does, one corner each, around a
 * yard you fly over as in LightLab. Every moving thing is a GameCore Rigid
 * Body whose shape the module infers from the entity -- a box from the Box
 * mesh, a sphere from the Sphere mesh, a capsule, a cylinder, the convex hull
 * of the InsetLattice mesh, or its triangles when it stands still:
 *
 *   Stacks       a pyramid of 21 crates and a tower of 10 (Box mesh -> box)
 *   Dominoes     a ball rolls down a ramp into an arc of 22 dominoes
 *   Materials    four bodies let go on one slope: a bouncing ball
 *                (Restitution), a heavy steel ball (Mass), an ice block that
 *                slides (Friction 0) and a rubber one that stays
 *   Meshes       balls rain into a static lattice cage (TriangleMesh) while a
 *                lattice, a capsule and a cylinder tumble (ConvexHull,
 *                Capsule, Cylinder)
 *   Sweeper      a Kinematic bar, turned by LightLab.Spin, bats crates aside
 *   Joints       a wrecking ball on a chain of Point constraints swings into
 *                a wall; a revolving door turns on a motorised Hinge
 *   Jump pad     a Sensor: whatever enters is launched (its SensorEnter event)
 *
 * Click to throw a ball from the camera (Throw), R puts everything back
 * (ResetLevel), Escape pauses. The level loads Content/Levels/PhysicsLab.nmworld
 * -- edit it in the editor -- or, without one, builds the same scene here;
 * [LightLab] ExportPhysicsLevel writes it out, which is how the file was made.
 *
 * Coordinates are the engine's: centimetres, +Y up, the floor at y = 0.
 */

/* An editor build only: when set, the level is built in code (not loaded) and
 * written to this path as a text world before the run starts. */
NIMBLE_CONFIG_STRING(GLightLabExportPhysicsLevel, "LightLab", "ExportPhysicsLevel", "", nullptr);

/* How hard a thrown ball leaves the camera, in world units a second. */
NIMBLE_CONFIG_FLOAT(GLightLabThrowSpeed, "LightLab", "ThrowSpeed", 2600.0f, nullptr);

namespace
{
    const char* const LevelFile = "Content/Levels/PhysicsLab.nmworld";
    const char* const PauseMenuPath = "Content/UI/PauseMenu.nmui";

    const char* const BoxMesh      = "Engine/Content/Meshes/Box.nmmesh";
    const char* const SphereMesh   = "Engine/Content/Meshes/Sphere.nmmesh";
    const char* const PlaneMesh    = "Engine/Content/Meshes/Plane.nmmesh";
    const char* const CapsuleMesh  = "Engine/Content/Meshes/Capsule.nmmesh";
    const char* const CylinderMesh = "Engine/Content/Meshes/Cylinder.nmmesh";
    const char* const LatticeMesh  = "Content/Meshes/InsetLattice.nmmesh";
    const char* const PBR          = "Content/Materials/PBR.material.nmgraph";
    const char* const Glow         = "Content/Materials/Glow.material.nmgraph";

    constexpr double Pi = 3.14159265358979323846;
    constexpr double DegToRad = Pi / 180.0;

    /* Thrown balls kept at once; the oldest goes first. */
    constexpr size_t MaxThrown = 40;

    /* The jump pad's launch, world units a second upward. */
    constexpr double LaunchSpeed = 1100.0;

    /* LightLab.Spin's turn rate, degrees a second. */
    constexpr double SweeperDegreesPerSecond = 55.0;

    /* Seconds between the mode's log reports. */
    constexpr double ReportIntervalSeconds = 5.0;

    using EngineCore::Vector3;

    struct Tint
    {
        float R = 0.8f, G = 0.8f, B = 0.8f;
        float Metallic  = 0.0f;
        float Roughness = 0.6f;
    };

    /* Built in code: how each entity is drawn and what kind of body it is. */
    struct Piece
    {
        const char* Name     = "";
        const char* Mesh     = BoxMesh;
        Vector3     Position;
        Vector3     Scale    = Vector3(1.0, 1.0, 1.0);
        Vector3     Rotation;
        Tint        Look;
        const char* Material = PBR;
        bool        bCasts   = true;
    };

    /* Piece as an entity: a Transform and a Render component drawing its mesh
     * with the PBR material tinted (or InPiece.Material untinted). */
    GameCore::Entity Spawn(GameCore::World& InWorld, const Piece& InPiece)
    {
        const GameCore::Entity Entity = InWorld.CreateEntity(InPiece.Name);
        GameCore::TransformComponent Transform;
        Transform.Position = InPiece.Position;
        Transform.Rotation = InPiece.Rotation;
        Transform.Scale    = InPiece.Scale;
        Entity.AddComponent(Transform);

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InPiece.Mesh);
        Render.SetMaterial(0, InPiece.Material);
        if(std::string(InPiece.Material) == PBR)
        {
            Render.SetMaterialParameter(0, "BaseColor", InPiece.Look.R, InPiece.Look.G, InPiece.Look.B, 0.0f);
            Render.SetMaterialParameter(0, "Metallic", InPiece.Look.Metallic, 0.0f, 0.0f, 0.0f);
            Render.SetMaterialParameter(0, "Roughness", InPiece.Look.Roughness, 0.0f, 0.0f, 0.0f);
        }
        Render.bCastShadows = InPiece.bCasts ? 1 : 0;
        Entity.AddComponent(Render);
        return Entity;
    }

    /* Level geometry: never moves, drawn from the static lists. */
    GameCore::Entity SpawnStatic(GameCore::World& InWorld, const Piece& InPiece, const float InFriction = 0.5f)
    {
        const GameCore::Entity Entity = Spawn(InWorld, InPiece);
        Entity.SetMobility(GameCore::EMobility::Static);
        GameCore::RigidBodyComponent Body;
        Body.Motion   = GameCore::RBM_Static;
        Body.Friction = InFriction;
        Entity.AddComponent(Body);
        return Entity;
    }

    GameCore::RigidBodyComponent* SpawnDynamic(GameCore::World& InWorld, const Piece& InPiece, GameCore::Entity* OutEntity = nullptr)
    {
        const GameCore::Entity Entity = Spawn(InWorld, InPiece);
        if(OutEntity) { *OutEntity = Entity; }
        return Entity.AddComponent<GameCore::RigidBodyComponent>();
    }

    Tint Colour(const float InR, const float InG, const float InB, const float InRoughness = 0.55f, const float InMetallic = 0.0f)
    {
        Tint Look;
        Look.R = InR; Look.G = InG; Look.B = InB;
        Look.Roughness = InRoughness;
        Look.Metallic  = InMetallic;
        return Look;
    }

    Piece Box(const char* InName, const Vector3& InPosition, const Vector3& InSize, const Tint& InLook,
              const Vector3& InRotation = Vector3(0.0, 0.0, 0.0))
    {
        Piece P;
        P.Name = InName; P.Mesh = BoxMesh; P.Position = InPosition; P.Scale = InSize; P.Rotation = InRotation; P.Look = InLook;
        return P;
    }

    Piece Ball(const char* InName, const Vector3& InPosition, const double InRadius, const Tint& InLook)
    {
        Piece P;
        P.Name = InName; P.Mesh = SphereMesh; P.Position = InPosition;
        P.Scale = Vector3(2.0 * InRadius, 2.0 * InRadius, 2.0 * InRadius); P.Look = InLook;
        return P;
    }

    /* The view direction for a (Pitch, Yaw, 0) rotation, as LightLab.Fly flies it. */
    Vector3 ViewForward(const Vector3& InRotation)
    {
        const double P = double(InRotation.GetX()) * DegToRad;
        const double Y = double(InRotation.GetY()) * DegToRad;
        return Vector3(std::sin(Y) * std::cos(P), -std::sin(P), std::cos(Y) * std::cos(P));
    }

    // === The scene ================================================================

    void SpawnGround(GameCore::World& InWorld)
    {
        /* The Plane mesh is flat: the body thickens below its surface. */
        Piece Floor;
        Floor.Name = "Floor"; Floor.Mesh = PlaneMesh; Floor.Scale = Vector3(8000.0, 1.0, 8000.0);
        Floor.Look = Colour(0.62f, 0.63f, 0.66f, 0.85f); Floor.bCasts = false;
        SpawnStatic(InWorld, Floor, 0.6f);
    }

    /* A pyramid of 21 crates and a tower of 10: stacking, and sleeping once settled. */
    void SpawnStacks(GameCore::World& InWorld)
    {
        const double Size = 60.0;
        for(int Row = 0; Row < 6; ++Row)
        {
            const int Count = 6 - Row;
            for(int Index = 0; Index < Count; ++Index)
            {
                const double X = -1650.0 + (Index - (Count - 1) * 0.5) * (Size + 1.0);
                const float  Shade = 0.55f + 0.07f * float(Row);
                SpawnDynamic(InWorld, Box("PyramidCrate", Vector3(X, Size * 0.5 + Size * Row, -650.0), Vector3(Size, Size, Size),
                                          Colour(Shade, 0.42f, 0.22f)));
            }
        }
        for(int Level = 0; Level < 10; ++Level)
        {
            SpawnDynamic(InWorld, Box("TowerBlock", Vector3(-1650.0, 25.0 + 50.0 * Level, -150.0), Vector3(50.0, 50.0, 50.0),
                                      Level % 2 ? Colour(0.85f, 0.85f, 0.82f) : Colour(0.2f, 0.45f, 0.75f)));
        }
    }

    /* An arc of dominoes round a point, set off by a ball rolling down a ramp
     * into the first. Each is thin along the arc, so it topples into the next. */
    void SpawnDominoes(GameCore::World& InWorld)
    {
        const Vector3 Center(-650.0, 0.0, -300.0);
        const double  Radius = 350.0;
        const int     Count  = 22;
        for(int Index = 0; Index < Count; ++Index)
        {
            const double Angle = 180.0 - 180.0 * Index / (Count - 1);   // round the far side, -X to +X
            const double A = Angle * DegToRad;
            const Vector3 At(Center.GetX() + Radius * std::cos(A), 45.0, Center.GetZ() + Radius * std::sin(A));
            const float Hue = float(Index) / float(Count);
            SpawnDynamic(InWorld, Box("Domino", At, Vector3(12.0, 90.0, 45.0),
                                      Colour(0.9f - 0.6f * Hue, 0.3f + 0.4f * Hue, 0.35f + 0.5f * Hue, 0.4f),
                                      Vector3(0.0, 270.0 - Angle, 0.0)));
        }

        /* The ramp runs down toward +Z, ending short of the first domino. */
        const double Tilt = 15.0, Length = 520.0;
        const Vector3 RampCenter(Center.GetX() - Radius, 62.0, Center.GetZ() - 75.0 - 0.5 * Length * std::cos(Tilt * DegToRad));
        SpawnStatic(InWorld, Box("DominoRamp", RampCenter, Vector3(120.0, 20.0, Length), Colour(0.35f, 0.35f, 0.38f, 0.7f),
                                 Vector3(Tilt, 0.0, 0.0)));
        const double Up = 0.5 * Length * std::sin(Tilt * DegToRad) - 15.0;
        SpawnDynamic(InWorld, Ball("DominoStarter", Vector3(RampCenter.GetX(), RampCenter.GetY() + Up + 50.0,
                                                            RampCenter.GetZ() - 0.5 * Length * std::cos(Tilt * DegToRad) + 40.0),
                                   25.0, Colour(0.95f, 0.8f, 0.15f, 0.3f)));
    }

    /* One slope, four materials let go at its top. */
    void SpawnMaterials(GameCore::World& InWorld)
    {
        const double Tilt = 20.0, Length = 900.0;
        const Vector3 Center(1300.0, 170.0, -700.0);
        SpawnStatic(InWorld, Box("MaterialRamp", Center, Vector3(720.0, 20.0, Length), Colour(0.32f, 0.36f, 0.4f, 0.6f),
                                 Vector3(Tilt, 0.0, 0.0)));

        /* Along the slope's top, just above its surface. */
        const double Along = -0.5 * Length + 90.0;
        auto OnSlope = [&](const double InX, const double InLift)
        {
            const double T = Tilt * DegToRad;
            return Vector3(InX, Center.GetY() - Along * std::sin(T) + 10.0 / std::cos(T) + InLift, Center.GetZ() + Along * std::cos(T));
        };

        GameCore::RigidBodyComponent* Bouncy = SpawnDynamic(InWorld, Ball("BouncyBall", OnSlope(1030.0, 40.0), 30.0, Colour(0.2f, 0.85f, 0.3f, 0.35f)));
        Bouncy->Restitution = 0.95f;

        GameCore::RigidBodyComponent* Steel = SpawnDynamic(InWorld, Ball("SteelBall", OnSlope(1210.0, 50.0), 40.0, Colour(0.8f, 0.8f, 0.82f, 0.15f, 1.0f)));
        Steel->Mass = 400.0f;

        GameCore::RigidBodyComponent* Ice = SpawnDynamic(InWorld, Box("IceBlock", OnSlope(1390.0, 30.0), Vector3(60.0, 60.0, 60.0),
                                                                      Colour(0.65f, 0.85f, 1.0f, 0.1f), Vector3(Tilt, 0.0, 0.0)));
        Ice->Friction = 0.0f;

        GameCore::RigidBodyComponent* Rubber = SpawnDynamic(InWorld, Box("RubberBlock", OnSlope(1570.0, 30.0), Vector3(60.0, 60.0, 60.0),
                                                                         Colour(0.85f, 0.15f, 0.12f, 0.9f), Vector3(Tilt, 0.0, 0.0)));
        Rubber->Friction = 1.2f;
    }

    /* A static lattice cage (its triangles) catching a rain of balls, and
     * dynamic hulls and primitives tumbling onto a tilted plank. */
    void SpawnMeshes(GameCore::World& InWorld)
    {
        Piece Cage;
        Cage.Name = "LatticeCage"; Cage.Mesh = LatticeMesh; Cage.Position = Vector3(0.0, 220.0, 1100.0);
        Cage.Scale = Vector3(380.0, 380.0, 380.0); Cage.Rotation = Vector3(0.0, 20.0, 0.0);
        Cage.Look = Colour(0.9f, 0.62f, 0.22f, 0.35f, 1.0f);
        SpawnStatic(InWorld, Cage);   // Shape Auto, Static, not a primitive: its triangles

        for(int Index = 0; Index < 12; ++Index)
        {
            const double X = -90.0 + 60.0 * double(Index % 4);
            const double Z = 1040.0 + 60.0 * double((Index / 4) % 3);
            GameCore::RigidBodyComponent* Body = SpawnDynamic(InWorld, Ball("RainBall", Vector3(X, 700.0 + 70.0 * Index, Z), 16.0,
                                                                            Colour(0.15f + 0.06f * Index, 0.4f, 0.9f - 0.05f * Index, 0.3f)));
            Body->bContinuous = 1;   // small and fast: no tunnelling through the bars
        }

        SpawnStatic(InWorld, Box("TumblePlank", Vector3(-550.0, 120.0, 1100.0), Vector3(500.0, 20.0, 300.0), Colour(0.45f, 0.38f, 0.3f, 0.8f),
                                 Vector3(0.0, 0.0, -18.0)));

        Piece Lattice;
        Lattice.Name = "TumblingLattice"; Lattice.Mesh = LatticeMesh; Lattice.Position = Vector3(-620.0, 520.0, 1060.0);
        Lattice.Scale = Vector3(110.0, 110.0, 110.0); Lattice.Rotation = Vector3(25.0, 10.0, 40.0);
        Lattice.Look = Colour(0.75f, 0.75f, 0.8f, 0.3f, 1.0f);
        SpawnDynamic(InWorld, Lattice);   // Shape Auto, Dynamic: its convex hull

        Piece Capsule;
        Capsule.Name = "TumblingCapsule"; Capsule.Mesh = CapsuleMesh; Capsule.Position = Vector3(-480.0, 650.0, 1180.0);
        Capsule.Scale = Vector3(60.0, 90.0, 60.0); Capsule.Rotation = Vector3(0.0, 0.0, 70.0);
        Capsule.Look = Colour(0.9f, 0.5f, 0.15f, 0.4f);
        SpawnDynamic(InWorld, Capsule);

        Piece Cylinder;
        Cylinder.Name = "TumblingCylinder"; Cylinder.Mesh = CylinderMesh; Cylinder.Position = Vector3(-700.0, 780.0, 1000.0);
        Cylinder.Scale = Vector3(80.0, 60.0, 80.0); Cylinder.Rotation = Vector3(60.0, 0.0, 10.0);
        Cylinder.Look = Colour(0.3f, 0.75f, 0.7f, 0.4f);
        SpawnDynamic(InWorld, Cylinder);
    }

    /* A Kinematic bar swept round by LightLab.Spin through a ring of crates. */
    void SpawnSweeper(GameCore::World& InWorld)
    {
        const Vector3 Center(1400.0, 0.0, 1100.0);
        GameCore::Entity Bar;
        GameCore::RigidBodyComponent* Body = SpawnDynamic(InWorld, Box("Sweeper", Center + Vector3(0.0, 35.0, 0.0), Vector3(760.0, 50.0, 40.0),
                                                                       Colour(0.95f, 0.75f, 0.1f, 0.4f, 0.6f)), &Bar);
        Body->Motion = GameCore::RBM_Kinematic;

        SpawnStatic(InWorld, Box("SweeperHub", Center + Vector3(0.0, 80.0, 0.0), Vector3(60.0, 160.0, 60.0), Colour(0.2f, 0.2f, 0.22f)));

        for(int Index = 0; Index < 16; ++Index)
        {
            /* Clear of the bar where it starts, along X. */
            const double Angle  = (25.0 + 130.0 * (Index % 8) / 7.0 + (Index >= 8 ? 180.0 : 0.0)) * DegToRad;
            const double Radius = 140.0 + 55.0 * (Index % 4);
            SpawnDynamic(InWorld, Box("SweptCrate", Vector3(Center.GetX() + Radius * std::cos(Angle), 22.0, Center.GetZ() + Radius * std::sin(Angle)),
                                      Vector3(44.0, 44.0, 44.0), Colour(0.5f + 0.03f * Index, 0.3f, 0.6f - 0.02f * Index)));
        }
    }

    /* A chain of six links on Point joints from a static anchor, ending in a
     * heavy ball, held out sideways to swing down into a wall of crates; and
     * a revolving door turning on a motorised Hinge. */
    void SpawnJoints(GameCore::World& InWorld)
    {
        const Vector3 Anchor(-1550.0, 800.0, 1300.0);
        SpawnStatic(InWorld, Box("WreckAnchor", Anchor, Vector3(40.0, 40.0, 40.0), Colour(0.2f, 0.2f, 0.22f)));
        SpawnStatic(InWorld, Box("WreckGantry", Vector3(Anchor.GetX(), 0.5 * (Anchor.GetY() - 20.0), Anchor.GetZ() - 400.0),
                                 Vector3(30.0, Anchor.GetY() - 20.0, 30.0), Colour(0.2f, 0.2f, 0.22f)));
        SpawnStatic(InWorld, Box("WreckBeam", Vector3(Anchor.GetX(), Anchor.GetY() + 35.0, Anchor.GetZ() - 190.0),
                                 Vector3(30.0, 30.0, 440.0), Colour(0.2f, 0.2f, 0.22f)));

        char Name[32];
        char Previous[32] = "WreckAnchor";
        const int Links = 6;
        for(int Index = 0; Index < Links; ++Index)
        {
            std::snprintf(Name, sizeof(Name), "WreckLink%d", Index);
            GameCore::Entity Link;
            GameCore::RigidBodyComponent* Body = SpawnDynamic(InWorld, Ball(Name, Anchor - Vector3(80.0 * (Index + 1), 0.0, 0.0), 18.0,
                                                                            Colour(0.3f, 0.3f, 0.32f, 0.3f, 1.0f)), &Link);
            Body->Mass = 15.0f;
            GameCore::PhysicsConstraintComponent Joint;
            Joint.Type  = GameCore::PCT_Point;
            Joint.Pivot = Vector3(40.0, 0.0, 0.0);   // halfway back to the previous link (or the anchor)
            std::snprintf(Joint.TargetName, sizeof(Joint.TargetName), "%s", Previous);
            Link.AddComponent(Joint);
            std::snprintf(Previous, sizeof(Previous), "%s", Name);
        }

        GameCore::Entity Wrecker;
        GameCore::RigidBodyComponent* Heavy = SpawnDynamic(InWorld, Ball("WreckingBall", Anchor - Vector3(80.0 * Links + 110.0, 0.0, 0.0), 70.0,
                                                                         Colour(0.15f, 0.15f, 0.16f, 0.25f, 1.0f)), &Wrecker);
        Heavy->Mass = 900.0f;
        GameCore::PhysicsConstraintComponent Hook;
        Hook.Type  = GameCore::PCT_Point;
        Hook.Pivot = Vector3(70.0, 0.0, 0.0);
        std::snprintf(Hook.TargetName, sizeof(Hook.TargetName), "%s", Previous);
        Wrecker.AddComponent(Hook);

        /* The wall, where the ball comes through the bottom of its swing. */
        for(int Row = 0; Row < 5; ++Row)
        {
            for(int Column = 0; Column < 4; ++Column)
            {
                SpawnDynamic(InWorld, Box("WallCrate", Vector3(Anchor.GetX() + 140.0, 30.0 + 60.0 * Row, Anchor.GetZ() - 90.0 + 60.0 * Column),
                                          Vector3(58.0, 58.0, 58.0), (Row + Column) % 2 ? Colour(0.7f, 0.25f, 0.2f, 0.8f)
                                                                                         : Colour(0.82f, 0.78f, 0.7f, 0.8f)));
            }
        }

        /* The revolving door: one wide panel hinged to the world about Y
         * through its middle, turned by its motor through a ring of balls. */
        GameCore::Entity Door;
        GameCore::RigidBodyComponent* DoorBody = SpawnDynamic(InWorld, Box("RevolvingDoor", Vector3(-750.0, 125.0, 1900.0), Vector3(420.0, 230.0, 16.0),
                                                                           Colour(0.55f, 0.75f, 0.9f, 0.15f, 0.2f)), &Door);
        DoorBody->Mass = 200.0f;
        GameCore::PhysicsConstraintComponent Hinge;
        Hinge.Type       = GameCore::PCT_Hinge;
        Hinge.Axis       = Vector3(0.0, 1.0, 0.0);
        Hinge.bMotor     = 1;
        Hinge.MotorSpeed = 45.0f;
        Door.AddComponent(Hinge);

        for(int Index = 0; Index < 6; ++Index)
        {
            const double Angle = (30.0 + 60.0 * Index) * DegToRad;
            SpawnDynamic(InWorld, Ball("DoorBall", Vector3(-750.0 + 150.0 * std::cos(Angle), 30.0, 1900.0 + 150.0 * std::sin(Angle)), 30.0,
                                       Colour(0.9f, 0.9f, 0.3f, 0.3f)));
        }
    }

    /* A Sensor pad: what enters is launched (LightLabPhysicsLevel::OnSensorEnter).
     * Three balls start above it -- a fountain that keeps going. */
    void SpawnJumpPad(GameCore::World& InWorld)
    {
        Piece Pad;
        Pad.Name = "JumpPad"; Pad.Mesh = BoxMesh; Pad.Position = Vector3(0.0, 15.0, -1350.0);
        Pad.Scale = Vector3(300.0, 30.0, 300.0); Pad.Material = Glow; Pad.bCasts = false;
        const GameCore::Entity Entity = Spawn(InWorld, Pad);
        Entity.SetMobility(GameCore::EMobility::Static);
        GameCore::RigidBodyComponent Sensor;
        Sensor.Motion  = GameCore::RBM_Static;
        Sensor.bSensor = 1;
        Entity.AddComponent(Sensor);

        for(int Index = 0; Index < 3; ++Index)
        {
            SpawnDynamic(InWorld, Ball("FountainBall", Vector3(-80.0 + 80.0 * Index, 300.0 + 150.0 * Index, -1350.0), 25.0,
                                       Colour(0.95f, 0.3f + 0.25f * Index, 0.6f, 0.3f)));
        }
    }

    void SpawnLightsAndView(GameCore::World& InWorld)
    {
        const GameCore::Entity Sun = InWorld.CreateEntity("Sun");
        GameCore::TransformComponent SunAt;
        SunAt.Position = Vector3(0.0, 2000.0, 0.0);
        SunAt.Rotation = Vector3(55.0, 30.0, 0.0);
        Sun.AddComponent(SunAt);
        RenderCore::DirectionalLightComponent Light;
        Light.Color           = EngineCore::Color(255, 244, 225);
        Light.Intensity       = 2.5f;
        Light.bCastShadows    = 1;
        Light.ShadowTechnique = RenderCore::LST_Cascaded;
        Light.ShadowDistance  = 6000.0f;
        Light.CascadeCount    = 4;
        Sun.AddComponent(Light);

        const GameCore::Entity Sky = InWorld.CreateEntity("Sky");
        Sky.AddComponent<GameCore::TransformComponent>();
        RenderCore::SkyLightComponent Fill;
        Fill.SkyColor    = EngineCore::Color(120, 140, 175);
        Fill.GroundColor = EngineCore::Color(70, 60, 50);
        Fill.Intensity   = 0.6f;
        Sky.AddComponent(Fill);

        const GameCore::Entity Post = InWorld.CreateEntity("PostProcessGlobal");
        Post.AddComponent<GameCore::TransformComponent>();
        RenderCore::PostProcessComponent Everywhere;
        std::snprintf(Everywhere.Stack, sizeof(Everywhere.Stack), "%s", "Content/PostProcess/Global.pp.nmgraph");
        Post.AddComponent(Everywhere);

        const GameCore::Entity Player = InWorld.CreateEntity("Player");
        GameCore::TransformComponent View;
        View.Position = Vector3(0.0, 1100.0, -2700.0);
        View.Rotation = Vector3(22.0, 0.0, 0.0);
        Player.AddComponent(View);
        RenderCore::CameraComponent Camera;
        Camera.Projection = RenderCore::CP_Perspective;
        Camera.FovY       = 70.0f;
        Camera.ZNear      = 5.0f;
        Camera.ZFar       = 20000.0f;
        Player.AddComponent(Camera);
        Player.AddComponent<NimbleEngine::InputComponent>();
    }

    void BuildScene(GameCore::World& InWorld)
    {
        SpawnGround(InWorld);
        SpawnStacks(InWorld);
        SpawnDominoes(InWorld);
        SpawnMaterials(InWorld);
        SpawnMeshes(InWorld);
        SpawnSweeper(InWorld);
        SpawnJoints(InWorld);
        SpawnJumpPad(InWorld);
        SpawnLightsAndView(InWorld);
    }

    // === Systems ==================================================================

    struct SpinContext
    {
        GameCore::EntityID Sweeper;
        double             Yaw = 0.0;
    };

    /* PrePhysics: turns the Sweeper's Transform; its Kinematic body follows,
     * pushing whatever is in the way. */
    void SpinSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        auto& Context = *static_cast<SpinContext*>(InUserData);
        if(GameCore::TransformComponent* Transform = InWorld.GetEntity(Context.Sweeper).GetComponent<GameCore::TransformComponent>())
        {
            Context.Yaw = std::fmod(Context.Yaw + SweeperDegreesPerSecond * double(InDeltaSeconds), 360.0);
            Transform->Rotation = Vector3(0.0, Context.Yaw, 0.0);
        }
    }
}

// === The mode =====================================================================

/* Throwing, resetting, pausing and a periodic report. */
class LightLabPhysicsMode : public NimbleEngine::IGameMode
{
public:
    LightLabPhysicsMode(NimbleEngine::ILevel& InLevel, const GameCore::EntityID InPlayer) : Level(InLevel), PlayerEntity(InPlayer) {}

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;
        return true;
    }

    void OnStart() override
    {
        Session->SetMouseCaptured(true);
        CORE_LOG(std::string("[PhysicsLab] fly with WASD + mouse, E/Q up and down, Shift to boost; click throws a ball, "
                             "R resets, Escape pauses")
                 + (NimbleEngine::IsPhysicsAvailable() ? "" : " -- physics is compiled out (NIMBLE_WITH_JOLTPHYSICS=OFF): nothing will move"))
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        GameCore::World& World = Session->GetWorld();
        ElapsedSeconds += InFrame.DeltaSeconds;

        if(const NimbleEngine::InputComponent* Input = World.GetEntity(PlayerEntity).GetComponent<NimbleEngine::InputComponent>())
        {
            if(Input->WasPressed("Pause"))
            {
                OpenPauseMenu();
            }
            else if(!Session->IsMouseCaptured())
            {
                if(Input->WasPressed("CaptureMouse")) { Session->SetMouseCaptured(true); }
            }
            else if(Input->WasPressed("Throw"))
            {
                Throw(World);
            }

            if(Input->WasPressed("ResetLevel"))
            {
                CORE_LOG("[PhysicsLab] reset")
                Session->RequestLevel(Level);
            }
        }

        if(ElapsedSeconds - LastReportSeconds >= ReportIntervalSeconds)
        {
            LastReportSeconds = ElapsedSeconds;
            size_t Bodies = 0, Awake = 0;
            World.Each<GameCore::RigidBodyComponent>([&](GameCore::Entity, GameCore::RigidBodyComponent& InBody)
            {
                ++Bodies;
                Awake += InBody.IsAwake() ? 1 : 0;
            });
            CORE_LOG("[PhysicsLab] t=" + std::to_string(int(ElapsedSeconds)) + "s bodies=" + std::to_string(Bodies)
                     + " awake=" + std::to_string(Awake) + " thrown=" + std::to_string(Thrown.size()))
        }
    }

    void OnUnload() override
    {
        Session = nullptr;
        Thrown.clear();
    }

private:
    /* A ball from just ahead of the camera, along the view. */
    void Throw(GameCore::World& InWorld)
    {
        const GameCore::TransformComponent* View = InWorld.GetEntity(PlayerEntity).GetComponent<GameCore::TransformComponent>();
        if(!View) { return; }
        const Vector3 Forward = ViewForward(View->Rotation);

        const float Hue = float(Thrown.size() % 7) / 7.0f;
        GameCore::Entity Spawned;
        GameCore::RigidBodyComponent* Body = SpawnDynamic(InWorld, Ball("Thrown", View->Position + Forward * 120.0, 24.0,
                                                                        Colour(0.9f - 0.5f * Hue, 0.4f + 0.5f * Hue, 0.3f + 0.4f * Hue, 0.25f, 0.5f)),
                                                          &Spawned);
        Body->Mass                  = 20.0f;
        Body->bContinuous           = 1;
        Body->InitialLinearVelocity = Forward * double(GLightLabThrowSpeed.GetFloat());

        Thrown.push_back(Spawned.GetID());
        while(Thrown.size() > MaxThrown)
        {
            InWorld.DestroyEntity(Thrown.front());
            Thrown.pop_front();
        }
    }

    void OpenPauseMenu()
    {
        Session->SetMouseCaptured(false);
        if(Session->GetUI().Open(PauseMenuPath) == UI::UI_DOCUMENT_INVALID)
        {
            return;
        }
        Session->SetPaused(true);
    }

    NimbleEngine::ILevel&        Level;
    NimbleEngine::GameSession*   Session = nullptr;
    GameCore::EntityID           PlayerEntity;
    std::deque<GameCore::EntityID> Thrown;
    double ElapsedSeconds    = 0.0;
    double LastReportSeconds = 0.0;
};

// === The level ====================================================================

class LightLabPhysicsLevel : public NimbleEngine::ILevel
{
public:
    const char* GetName() const override      { return "PhysicsLab"; }
    const char* GetWorldFile() const override { return LevelFile; }
    const char* GetStartupScript() const override { return ""; }

    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        (void)InSession;
        GameCore::World& World = GetWorld();
        Spin = SpinContext();

        const std::string Export = GLightLabExportPhysicsLevel.GetString();
        if(!Export.empty() || !LoadLevelFile(World))
        {
            BuildScene(World);
            if(!Export.empty())
            {
                ExportLevel(World, Export);
            }
        }

        const GameCore::EntityID Player = World.FindEntityByName("Player");
        if(!Player.IsSet())
        {
            CORE_LOG_ERROR("[PhysicsLab] the level has no entity named Player; nothing to fly")
        }
        LightLabStartFly(World, Player, Fly);

        Spin.Sweeper = World.FindEntityByName("Sweeper");
        World.RegisterSystem("LightLab.Spin", GameCore::TP_PrePhysics, &SpinSystem, &Spin);

        JumpPad = World.FindEntityByName("JumpPad");
        World.GetEvents().Subscribe("SensorEnter", &LightLabPhysicsLevel::OnSensorEnter, this);

        Mode.emplace(*this, Player);
        return true;
    }

    void OnUnload() override
    {
        GetWorld().GetEvents().UnsubscribeAll(this);
        Mode.reset();
    }

    NimbleEngine::IGameMode* GetGameMode() override { return Mode ? &*Mode : nullptr; }

private:
    /* The jump pad: whatever enters leaves upward at LaunchSpeed, keeping
     * half its way across. */
    static void OnSensorEnter(const GameCore::EventID /*InEvent*/, const GameCore::EventArgs& InArgs, void* InUserData)
    {
        auto* Self = static_cast<LightLabPhysicsLevel*>(InUserData);
        if(InArgs.Count < 2 || InArgs.Args[0].Type != GameCore::EventArg::ET_Entity
           || GameCore::EntityID::Unpack(InArgs.Args[0].Entity) != Self->JumpPad)
        {
            return;
        }

        GameCore::Entity Other = Self->GetWorld().GetEntity(GameCore::EntityID::Unpack(InArgs.Args[1].Entity));
        if(GameCore::RigidBodyComponent* Body = Other.GetComponent<GameCore::RigidBodyComponent>())
        {
            const Vector3 Was = Body->LinearVelocity;
            Body->SetLinearVelocity(Vector3(0.5 * Was.GetX(), LaunchSpeed, 0.5 * Was.GetZ()));
        }
    }

    bool LoadLevelFile(GameCore::World& InWorld)
    {
        const std::string Path = GameCore::ResolveContentWorld(LevelFile);
        if(GameCore::DetectWorldFileFormat(Path.c_str()) == GameCore::EWorldFileFormat::Unknown)
        {
            CORE_LOG("[PhysicsLab] no " + Path + "; building the level in code")
            return false;
        }
        if(GameCore::LoadWorldFile(Path.c_str(), InWorld))
        {
            CORE_LOG("[PhysicsLab] loaded " + Path)
            return true;
        }
        CORE_LOG_WARN("[PhysicsLab] could not read " + Path + "; building the level in code")
        InWorld.ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
        {
            static_cast<std::deque<GameCore::EntityID>*>(InUserData)->push_back(InEntity);
        }, &Doomed);
        for(const GameCore::EntityID Each : Doomed) { InWorld.DestroyEntity(Each); }
        Doomed.clear();
        return false;
    }

    void ExportLevel(GameCore::World& InWorld, const std::string& InPath)
    {
#if WITH_EDITOR
        InWorld.RefreshSpatialIndex();
        if(GameCore::SaveWorldPackage(InPath.c_str(), InWorld))
        {
            CORE_LOG("[PhysicsLab] exported the level to " + InPath)
        }
        else
        {
            CORE_LOG_ERROR("[PhysicsLab] could not export the level to " + InPath)
        }
#else
        (void)InWorld;
        CORE_LOG_WARN("[PhysicsLab] ExportPhysicsLevel needs an editor build (" + InPath + " not written)")
#endif
    }

    LightLabFlyContext                 Fly;
    SpinContext                        Spin;
    GameCore::EntityID                 JumpPad;
    std::deque<GameCore::EntityID>     Doomed;
    std::optional<LightLabPhysicsMode> Mode;
};

namespace LightLabPhysics
{
    NimbleEngine::ILevel& GetLevel()
    {
        static LightLabPhysicsLevel Level;
        return Level;
    }
}
