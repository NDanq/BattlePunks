#ifndef GAMECORE_RIGIDBODYCOMPONENT_H
#define GAMECORE_RIGIDBODYCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Math/Vector.h"

namespace GameCore
{
    /* How a RigidBodyComponent moves. */
    enum ERigidBodyMotion : uint8_t
    {
        RBM_Dynamic   = 0,   // simulated: gravity, contacts and forces move it; it writes the Transform
        RBM_Kinematic = 1,   // follows its Transform (moved by game code) and pushes dynamic bodies aside
        RBM_Static    = 2,   // never moves; level geometry
    };

    /* The collision shape. Auto picks one from what the entity has (below). */
    enum ERigidBodyShape : uint8_t
    {
        RBS_Auto         = 0,
        RBS_Box          = 1,   // fitted to the source box
        RBS_Sphere       = 2,   // radius: the source box's largest half extent
        RBS_Capsule      = 3,   // along Y: radius from X/Z, height from Y
        RBS_Cylinder     = 4,   // along Y, likewise
        RBS_ConvexHull   = 5,   // the hull of the RenderComponent mesh's vertices
        RBS_TriangleMesh = 6,   // the mesh's triangles; Static and Kinematic only (Dynamic falls back to a hull)
        RBS_HeightField  = 7,   // the entity's Terrain (RenderCore's Terrain component); Static and Kinematic only
    };

    /* Where a fitted shape's box comes from. */
    enum ERigidBodyShapeSource : uint8_t
    {
        RBSS_Auto   = 0,   // the RenderComponent mesh's bounds if it has a mesh, else the BoundsComponent
        RBSS_Bounds = 1,   // BoundsComponent::Local, as authored
        RBSS_Mesh   = 2,   // the bounds of the sibling RenderComponent's mesh
    };

    /*
     * "Rigid Body": the entity is simulated by the physics module (JoltPhysics,
     * Engine/Modules/JoltPhysics/README.md) -- it falls, collides, stacks and
     * can be pushed.
     *
     * Here, not in JoltPhysics, so a level or prefab that carries one still
     * loads, edits and saves in a build with physics compiled out
     * (NIMBLE_WITH_JOLTPHYSICS=OFF, the default); it is inert there, and the
     * engine warns when a level has any.
     *
     * THE SHAPE is inferred from the entity, in its world scale:
     *   - Shape Auto on one of the engine's primitive meshes
     *     (Engine/Content/Meshes/Box, Sphere, Capsule, Cylinder, Plane) is that
     *     primitive, fitted to the mesh's bounds;
     *   - Shape Auto on any other mesh is its triangles when Static or
     *     Kinematic, its convex hull when Dynamic;
     *   - Shape Auto with no mesh is a box: the BoundsComponent's.
     *   - Shape Auto (or Height Field) on an entity with a Terrain is its
     *     heightfield: the same triangles the terrain draws.
     *   - Box, Sphere, Capsule and Cylinder fit the box ShapeSource names.
     * The shape is rebuilt when any of that changes (mesh, bounds, scale,
     * these fields).
     *
     * THE TRANSFORM. A Dynamic body writes the entity's world pose (its
     * Transform) every frame; set the Transform yourself to teleport it. A
     * Kinematic body is moved to wherever its Transform is. Static bodies are
     * placed once.
     *
     * FORCES. The methods only record what is asked; the physics step applies
     * it next and clears it. A force or torque lasts for one frame -- apply it
     * every frame for a steady push. Velocities are as of the last step.
     *
     * Units are the World's (centimetres in the shipped content); mass is in
     * kilograms. Overlap layers work as CollisionComponent's: two bodies
     * collide only if each one's Layer is in the other's Mask (15 bits).
     */
    struct GAMECORE_API RigidBodyComponent
    {
        GAMECORE_COMPONENT_BODY()

        uint8_t Motion      = RBM_Dynamic;   // ERigidBodyMotion
        uint8_t Shape       = RBS_Auto;      // ERigidBodyShape
        uint8_t ShapeSource = RBSS_Auto;     // ERigidBodyShapeSource
        uint8_t bEnabled    = 1;             // off: no body at all

        float Mass           = 0.0f;   // kilograms; 0 computes it from the shape's volume at water's density
        float Friction       = 0.5f;
        float Restitution    = 0.0f;   // bounciness, 0..1
        float LinearDamping  = 0.05f;
        float AngularDamping = 0.05f;
        float GravityFactor  = 1.0f;

        uint8_t bContinuous    = 0;   // continuous collision: fast, small bodies do not tunnel
        uint8_t bSensor        = 0;   // detects (SensorEnter/SensorExit events) but never collides
        uint8_t bAllowSleeping = 1;
        uint8_t Reserved0      = 0;

        int32_t Layer = 1;    // what this is (bits)
        int32_t Mask  = -1;   // what it collides with (bits; -1 is everything)

        /* The velocity a Dynamic body starts with, in units (degrees for
         * angular) per second. */
        EngineCore::Vector3 InitialLinearVelocity;
        EngineCore::Vector3 InitialAngularVelocity;

        // === Engine-written: never saved ====================================

        /* As of the last physics step; units / degrees per second. */
        EngineCore::Vector3 LinearVelocity;
        EngineCore::Vector3 AngularVelocity;

        /* What the methods below recorded, for the next step. */
        EngineCore::Vector3 PendingImpulse;
        EngineCore::Vector3 PendingAngularImpulse;
        EngineCore::Vector3 PendingForce;
        EngineCore::Vector3 PendingTorque;
        EngineCore::Vector3 PendingLinearVelocity;
        EngineCore::Vector3 PendingAngularVelocity;
        uint8_t bSetLinearVelocity  = 0;
        uint8_t bSetAngularVelocity = 0;

        /* The physics module's: awake as of the last step. */
        uint8_t bAwake = 0;
        uint8_t Reserved1 = 0;

        /* The physics module's body id; 0xFFFFFFFF for none. */
        uint32_t BodyHandle = 0xFFFFFFFFu;

        /* Units * kg / s: an instant change of momentum (a hit, a jump). */
        void AddImpulse(const EngineCore::Vector3& InImpulse);
        /* Units * kg / s^2, for this frame. */
        void AddForce(const EngineCore::Vector3& InForce);
        void AddAngularImpulse(const EngineCore::Vector3& InImpulse);
        void AddTorque(const EngineCore::Vector3& InTorque);
        void SetLinearVelocity(const EngineCore::Vector3& InVelocity);
        void SetAngularVelocity(const EngineCore::Vector3& InDegreesPerSecond);
        EngineCore::Vector3 GetLinearVelocity() const  { return LinearVelocity; }
        EngineCore::Vector3 GetAngularVelocity() const { return AngularVelocity; }
        bool IsAwake() const { return bAwake != 0; }

        /* "Dynamic", "Kinematic" or "Static". Set ignores an unknown name. */
        const char* GetMotionName() const;
        void        SetMotionName(const char* InName);
    };
}

#endif //GAMECORE_RIGIDBODYCOMPONENT_H
