#include "Private/LightLab/LightLabPhysics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/WorldList.h"
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
 *   Buggy        a Vehicle (Content/Prefabs/Vehicle.nmprefab) on a driving course
 *                down the right: speed bumps, rough ground, a jump, cones, a
 *                crate wall, a see-saw on a Hinge and a bridge. C gets in and
 *                out; the Move axes drive, Space is the handbrake, and a chase
 *                camera follows (LightLab.Drive, LightLab.Chase)
 *
 * Click to throw a ball from the camera (Throw), R puts everything back
 * (ResetLevel), Escape pauses. The scene is Content/Levels/PhysicsLab.nmworld
 * -- [World_PhysicsLab], edited in the editor; this file is its mode.
 *
 * Coordinates are the engine's: centimetres, +Y up, the floor at y = 0.
 */

/* How hard a thrown ball leaves the camera, in world units a second. */
NIMBLE_CONFIG_FLOAT(GLightLabThrowSpeed, "LightLab", "ThrowSpeed", 2600.0f, nullptr);

/* A demo: the buggy drives the course by itself -- about 25 km/h, steering
 * to keep to its line -- with the chase camera. Read every frame, so the
 * console turns it on mid-run. */
NIMBLE_CONFIG_BOOL(GLightLabAutoDrive, "LightLab", "AutoDrive", false, nullptr);

/* Editor builds only: the buggy built in code
 * (BuildVehicle) written to this path as a text prefab -- how
 * Content/Prefabs/Vehicle.nmprefab was made. */
NIMBLE_CONFIG_STRING(GLightLabExportVehiclePrefab, "LightLab", "ExportVehiclePrefab", "", nullptr);

namespace
{
    const char* const PauseMenuPath = "Content/UI/PauseMenu.nmui";

    const char* const BoxMesh      = "Engine/Content/Meshes/Box.nmmesh";
    const char* const SphereMesh   = "Engine/Content/Meshes/Sphere.nmmesh";
    const char* const CylinderMesh = "Engine/Content/Meshes/Cylinder.nmmesh";
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

    // === The buggy ================================================================

    /* A child of InParent at InLocal, drawn as InPiece (whose Position,
     * Rotation and Scale are taken as local). */
    GameCore::Entity SpawnChild(GameCore::World& InWorld, const GameCore::Entity& InParent, const Piece& InPiece)
    {
        const GameCore::Entity Child = Spawn(InWorld, InPiece);
        Child.SetParent(InParent, false);
        return Child;
    }

    /*
     * The buggy, as Content/Prefabs/Vehicle.nmprefab holds it: a Vehicle
     * whose chassis is its root -- a 1200 kg Rigid Body shaped by its Bounds
     * box -- drawn by children made of the engine's meshes: a Box body, cabin
     * and bumpers, Sphere headlights, and four Cylinder wheels on their sides
     * (WheelFL, WheelFR, WheelRL, WheelRR; the Vehicle finds them by name and
     * reads their size from the mesh), each with a Box spoke so its spin shows,
     * and an engine sound on the root. The root is unscaled, so the wheels'
     * positions are in world units.
     */
    GameCore::Entity BuildVehicle(GameCore::World& InWorld, const GameCore::TransformPose& InAt, const char* InName)
    {
        const GameCore::Entity Root = InWorld.CreateEntity(InName);
        GameCore::TransformComponent Transform;
        Transform.Position = InAt.Position;
        Transform.Rotation = InAt.Rotation;
        Root.AddComponent(Transform);

        /* The chassis' collision: under the cabin and low between the
         * wheels -- 30 clear of the ground at rest, so it shoves a crate
         * instead of riding up onto it, and still clears bumps and rocks. */
        GameCore::BoundsComponent Bounds;
        Bounds.Local = GameCore::AABB(Vector3(-85.0, -35.0, -195.0), Vector3(85.0, 30.0, 195.0));
        Root.AddComponent(Bounds);

        GameCore::RigidBodyComponent Body;
        Body.Motion         = GameCore::RBM_Dynamic;
        Body.Mass           = 1200.0f;
        Body.AngularDamping = 0.2f;
        Root.AddComponent(Body);

        GameCore::VehicleComponent Vehicle;
        Vehicle.Drive               = GameCore::VD_All;
        Vehicle.EngineTorque        = 650.0f;
        Vehicle.MaxSteerAngle       = 32.0f;
        Vehicle.SuspensionMinLength = 8.0f;
        Vehicle.SuspensionMaxLength = 45.0f;
        Vehicle.SuspensionFrequency = 1.6f;
        Vehicle.SuspensionDamping   = 0.55f;
        Vehicle.AntiRollStiffness   = 2500.0f;
        Vehicle.MaxPitchRollAngle   = 60.0f;
        Vehicle.HandBrake           = 1.0f;   // parked until someone drives it
        Root.AddComponent(Vehicle);

        /* The engine: a looping idle (Tools/make_engine_loop_sound.py), heard
         * from the buggy, that the prefab's script pitches with the revs and
         * turns up with the throttle (Scripts/EngineSound.script.nmgraph). */
        GameCore::AudioSourceComponent Engine;
        Engine.SetSound("Content/Sounds/EngineLoop.nmsound");
        Engine.bOverride   = 1;
        Engine.bLoop       = 1;
        Engine.bSpatial    = 1;
        Engine.Bus         = GameCore::ASB_SFX;
        Engine.MinDistance = 400.0f;
        Engine.MaxDistance = 8000.0f;
        Root.AddComponent(Engine);

        const Tint Paint   = Colour(0.78f, 0.12f, 0.08f, 0.35f, 0.4f);
        const Tint Glass   = Colour(0.08f, 0.1f, 0.13f, 0.08f, 0.7f);
        const Tint Trim    = Colour(0.12f, 0.12f, 0.13f, 0.6f, 0.2f);
        const Tint Rubber  = Colour(0.05f, 0.05f, 0.05f, 0.9f);
        const Tint Chrome  = Colour(0.8f, 0.8f, 0.82f, 0.15f, 1.0f);

        SpawnChild(InWorld, Root, Box("Body",  Vector3(0.0, 12.0, 0.0),   Vector3(170.0, 40.0, 390.0), Paint));
        SpawnChild(InWorld, Root, Box("Cabin", Vector3(0.0, 52.0, -30.0), Vector3(150.0, 44.0, 180.0), Glass));
        SpawnChild(InWorld, Root, Box("FrontBumper", Vector3(0.0, -2.0, 200.0),  Vector3(176.0, 22.0, 18.0), Trim));
        SpawnChild(InWorld, Root, Box("RearBumper",  Vector3(0.0, -2.0, -200.0), Vector3(176.0, 22.0, 18.0), Trim));

        Piece Light;
        Light.Mesh = SphereMesh; Light.Material = Glow; Light.bCasts = false;
        Light.Scale = Vector3(24.0, 24.0, 10.0);
        Light.Name = "HeadlightL"; Light.Position = Vector3(-58.0, 18.0, 196.0);
        SpawnChild(InWorld, Root, Light);
        Light.Name = "HeadlightR"; Light.Position = Vector3(58.0, 18.0, 196.0);
        SpawnChild(InWorld, Root, Light);

        /* Radius 40, width 30: a unit Cylinder (along Y) on its side, so its axle is the chassis' X. */
        const char* const Names[GameCore::VW_Count] = { "WheelFL", "WheelFR", "WheelRL", "WheelRR" };
        for(int Slot = 0; Slot < GameCore::VW_Count; ++Slot)
        {
            Piece Wheel;
            Wheel.Name = Names[Slot]; Wheel.Mesh = CylinderMesh; Wheel.Look = Rubber;
            Wheel.Position = Vector3(Slot % 2 ? 98.0 : -98.0, -22.0, Slot < 2 ? 135.0 : -135.0);
            Wheel.Rotation = Vector3(0.0, 0.0, 90.0);
            Wheel.Scale    = Vector3(80.0, 30.0, 80.0);
            const GameCore::Entity WheelEntity = SpawnChild(InWorld, Root, Wheel);

            /* In the wheel's own (unit cylinder) space: a bar across its face. */
            Piece Spoke = Box("Spoke", Vector3(0.0, 0.0, 0.0), Vector3(0.8, 1.06, 0.14), Chrome);
            SpawnChild(InWorld, WheelEntity, Spoke);
        }
        return Root;
    }

    /* [LightLab] ExportVehiclePrefab: BuildVehicle captured as a text prefab. */
    void ExportVehiclePrefab(const std::string& InPath)
    {
#if WITH_EDITOR
        GameCore::World Scratch;
        const GameCore::Entity Root = BuildVehicle(Scratch, GameCore::TransformPose(), "Vehicle");
        GameCore::PrefabAsset Asset;
        const bool bCaptured = GameCore::CapturePrefab(Scratch, Root.GetID(), Asset);
        Asset.SetName("Vehicle");
        Asset.SetScriptPath("Scripts/EngineSound.script.nmgraph");
        if(bCaptured && GameCore::SavePrefabFile(InPath.c_str(), Asset))
        {
            CORE_LOG("[PhysicsLab] exported the buggy to " + InPath)
        }
        else
        {
            CORE_LOG_ERROR("[PhysicsLab] could not export the buggy to " + InPath)
        }
#else
        CORE_LOG_WARN("[PhysicsLab] ExportVehiclePrefab needs an editor build (" + InPath + " not written)")
#endif
    }

    // === The driving course ========================================================

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

namespace
{
    /* What LightLab.Drive and LightLab.Chase share: who drives what, and
     * the chase camera's own state. */
    struct DriveContext
    {
        GameCore::EntityID  Vehicle;
        GameCore::EntityID  Player;
        LightLabFlyContext* Fly = nullptr;
        bool                bDriving = false;
        bool                bCameraPlaced = false;
        Vector3             CameraPosition;
        double              CourseX = 3000.0;   // AutoDrive's line: where the buggy was parked
    };

    /* PrePhysics, after the Player's input resolves: the Move axes into the
     * buggy while driving; parked on its handbrake otherwise. */
    void DriveSystem(GameCore::World& InWorld, const float /*InDeltaSeconds*/, void* InUserData)
    {
        auto& Context = *static_cast<DriveContext*>(InUserData);
        GameCore::VehicleComponent* Vehicle = InWorld.GetEntity(Context.Vehicle).GetComponent<GameCore::VehicleComponent>();
        if(!Vehicle) { return; }
        const NimbleEngine::InputComponent* Input = InWorld.GetEntity(Context.Player).GetComponent<NimbleEngine::InputComponent>();
        if(Context.bDriving && GLightLabAutoDrive.GetBool())
        {
            /* Cruise: throttle under 25 km/h, steer back to the course's line. */
            const GameCore::TransformPose Pose = InWorld.GetEntity(Context.Vehicle).GetWorldPose();
            const double Off = (Context.CourseX - double(Pose.Position.GetX())) / 150.0 - double(Pose.Rotation.GetY()) / 10.0;
            Vehicle->SetInput(Vehicle->Speed * 0.036f < 25.0f ? 1.0f : 0.0f, float(std::clamp(Off, -1.0, 1.0)), 0.0f, 0.0f);
            return;
        }
        if(!Context.bDriving || !Input)
        {
            Vehicle->SetInput(0.0f, 0.0f, 0.0f, 1.0f);
            return;
        }
        Vehicle->SetInput(Input->GetAxis("MoveForward"), Input->GetAxis("MoveRight"), 0.0f, Input->IsDown("Handbrake") ? 1.0f : 0.0f);
    }

    /* PostUpdate, after physics has moved the buggy: the camera behind and
     * above it, eased toward that each frame, looking at it. */
    void ChaseSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        auto& Context = *static_cast<DriveContext*>(InUserData);
        if(!Context.bDriving) { return; }
        GameCore::TransformComponent* Camera = InWorld.GetEntity(Context.Player).GetComponent<GameCore::TransformComponent>();
        const GameCore::Entity Vehicle = InWorld.GetEntity(Context.Vehicle);
        if(!Camera || !Vehicle.GetComponent<GameCore::TransformComponent>()) { return; }

        const GameCore::TransformPose Car = Vehicle.GetWorldPose();
        const GameCore::Rotation3 Turn = GameCore::TransformMath::EulerToRotation(Car.Rotation);
        Vector3 Forward = Turn * Vector3(0.0, 0.0, 1.0);
        Forward = Vector3(Forward.GetX(), 0.0, Forward.GetZ());   // level: the camera does not pitch with the bumps
        const double Flat = std::sqrt(double(Forward.GetX()) * Forward.GetX() + double(Forward.GetZ()) * Forward.GetZ());
        Forward = Flat > 1.0e-3 ? Forward * (1.0 / Flat) : Vector3(0.0, 0.0, 1.0);

        const Vector3 Target  = Car.Position + Vector3(0.0, 90.0, 0.0);
        const Vector3 Desired = Target - Forward * 650.0 + Vector3(0.0, 230.0, 0.0);
        if(!Context.bCameraPlaced)
        {
            Context.CameraPosition = Desired;
            Context.bCameraPlaced  = true;
        }
        const double Ease = 1.0 - std::exp(-6.0 * double(InDeltaSeconds));
        Context.CameraPosition = Context.CameraPosition + (Desired - Context.CameraPosition) * Ease;

        const Vector3 Look = Target - Context.CameraPosition;
        const double  Length = std::sqrt(double(Look.GetX()) * Look.GetX() + double(Look.GetY()) * Look.GetY() + double(Look.GetZ()) * Look.GetZ());
        if(Length < 1.0e-3) { return; }
        const double Yaw   = std::atan2(double(Look.GetX()), double(Look.GetZ())) / DegToRad;
        const double Pitch = -std::asin(double(Look.GetY()) / Length) / DegToRad;   // positive looks down
        Camera->Position = Context.CameraPosition;
        Camera->Rotation = Vector3(Pitch, Yaw, 0.0);
    }
}

// === The mode =====================================================================

/* Throwing, resetting, pausing and a periodic report. */
class LightLabPhysicsMode : public NimbleEngine::IGameMode
{
public:
    /* Content/Levels/PhysicsLab.nmworld is loaded: fly the Player, turn the
     * Sweeper, answer the jump pad, drive the level's vehicle. */
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;
        GameCore::World& World = InSession.GetWorld();

        const std::string VehicleExport = GLightLabExportVehiclePrefab.GetString();
        if(!VehicleExport.empty())
        {
            ExportVehiclePrefab(VehicleExport);
        }

        PlayerEntity = World.FindEntityByName("Player");
        if(!PlayerEntity.IsSet())
        {
            CORE_LOG_ERROR("[PhysicsLab] the world has no entity named Player; nothing to fly")
        }
        LightLabStartFly(World, PlayerEntity, Fly);

        Spin.Sweeper = World.FindEntityByName("Sweeper");
        World.RegisterSystem("LightLab.Spin", GameCore::TP_PrePhysics, &SpinSystem, &Spin);

        JumpPad = World.FindEntityByName("JumpPad");
        World.GetEvents().Subscribe("SensorEnter", &LightLabPhysicsMode::OnSensorEnter, this);

        /* The world's vehicle, whatever it is called. */
        World.Each<GameCore::VehicleComponent>([this](GameCore::Entity InEntity, GameCore::VehicleComponent&)
        {
            if(!Drive.Vehicle.IsSet()) { Drive.Vehicle = InEntity.GetID(); }
        });
        Drive.CourseX = double(World.GetEntity(Drive.Vehicle).GetWorldPose().Position.GetX());
        Drive.Player  = PlayerEntity;
        Drive.Fly     = &Fly;
        World.RegisterSystem("LightLab.Drive", GameCore::TP_PrePhysics, &DriveSystem, &Drive);
        World.RegisterSystem("LightLab.Chase", GameCore::TP_PostUpdate, &ChaseSystem, &Drive);
        return true;
    }

    void OnStart() override
    {
        Session->SetMouseCaptured(true);
        CORE_LOG(std::string("[PhysicsLab] fly with WASD + mouse, E/Q up and down, Shift to boost; click throws a ball, "
                             "C gets in and out of the buggy (WASD drives, Space is the handbrake), R resets, Escape pauses")
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

            if(Input->WasPressed("EnterVehicle") || (GLightLabAutoDrive.GetBool() && !Drive.bDriving))
            {
                ToggleDriving(World);
            }

            if(Input->WasPressed("ResetLevel"))
            {
                CORE_LOG("[PhysicsLab] reset")
                Session->RequestLevel(*Session->GetLevel());
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
            std::string Driving;
            if(Drive.bDriving)
            {
                if(const auto* Vehicle = World.GetEntity(Drive.Vehicle).GetComponent<GameCore::VehicleComponent>())
                {
                    Driving = " buggy=" + std::to_string(int(std::lround(double(Vehicle->Speed) * 0.036))) + "km/h gear="
                              + std::to_string(Vehicle->Gear) + " wheels_down=" + std::to_string(Vehicle->WheelsOnGround);
                }
            }
            CORE_LOG("[PhysicsLab] t=" + std::to_string(int(ElapsedSeconds)) + "s bodies=" + std::to_string(Bodies)
                     + " awake=" + std::to_string(Awake) + " thrown=" + std::to_string(Thrown.size()) + Driving)
        }
    }

    void OnUnload() override
    {
        if(Session)
        {
            Session->GetWorld().GetEvents().UnsubscribeAll(this);
        }
        Session = nullptr;
        Thrown.clear();
    }

private:
    /* The jump pad: whatever enters leaves upward at LaunchSpeed, keeping
     * half its way across. */
    static void OnSensorEnter(const GameCore::EventID /*InEvent*/, const GameCore::EventArgs& InArgs, void* InUserData)
    {
        auto* Self = static_cast<LightLabPhysicsMode*>(InUserData);
        if(InArgs.Count < 2 || InArgs.Args[0].Type != GameCore::EventArg::ET_Entity
           || GameCore::EntityID::Unpack(InArgs.Args[0].Entity) != Self->JumpPad)
        {
            return;
        }

        GameCore::Entity Other = Self->Session->GetWorld().GetEntity(GameCore::EntityID::Unpack(InArgs.Args[1].Entity));
        if(GameCore::RigidBodyComponent* Body = Other.GetComponent<GameCore::RigidBodyComponent>())
        {
            const Vector3 Was = Body->LinearVelocity;
            Body->SetLinearVelocity(Vector3(0.5 * Was.GetX(), LaunchSpeed, 0.5 * Was.GetZ()));
        }
    }

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

    /* In: free flight stops and the chase camera takes the view. Out: free
     * flight resumes from where the camera is. */
    void ToggleDriving(GameCore::World& InWorld)
    {
        if(!InWorld.GetEntity(Drive.Vehicle).HasComponent<GameCore::VehicleComponent>())
        {
            CORE_LOG_WARN("[PhysicsLab] no buggy to drive")
            return;
        }
        Drive.bDriving = !Drive.bDriving;
        if(Drive.bDriving)
        {
            Drive.bCameraPlaced = false;
            if(Drive.Fly) { Drive.Fly->bEnabled = false; }
            CORE_LOG("[PhysicsLab] driving the buggy: W/S throttle and reverse, A/D steer, Space handbrake, C to get out")
            return;
        }
        if(Drive.Fly)
        {
            if(const GameCore::TransformComponent* View = InWorld.GetEntity(PlayerEntity).GetComponent<GameCore::TransformComponent>())
            {
                Drive.Fly->Pitch = double(View->Rotation.GetX());
                Drive.Fly->Yaw   = double(View->Rotation.GetY());
            }
            Drive.Fly->bEnabled = true;
        }
        CORE_LOG("[PhysicsLab] out of the buggy: flying")
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

    NimbleEngine::GameSession*   Session = nullptr;
    GameCore::EntityID           PlayerEntity;
    GameCore::EntityID           JumpPad;
    LightLabFlyContext           Fly;
    SpinContext                  Spin;
    DriveContext                 Drive;
    std::deque<GameCore::EntityID> Thrown;
    double ElapsedSeconds    = 0.0;
    double LastReportSeconds = 0.0;
};

namespace LightLabPhysics
{
    void RegisterGameMode()
    {
        NimbleEngine::RegisterGameMode<LightLabPhysicsMode>("PhysicsLab");
    }

    void UnregisterGameMode()
    {
        NimbleEngine::UnregisterGameMode("PhysicsLab");
    }
}
