#ifndef JOLTPHYSICS_PHYSICSRAGDOLL_H
#define JOLTPHYSICS_PHYSICSRAGDOLL_H

#include <cstdint>
#include <vector>

#include "Public/GameCore/EntityID.h"

/*
 * What a ragdoll is made of, in the World's terms, for PhysicsWorld's
 * CreateRagdoll: rigid parts in a tree, each jointed to its parent. No
 * skeleton here -- JoltPhysics does not know RenderCore; NimbleEngine's
 * Ragdoll.h builds this from one (RagdollProfile) and maps poses both ways.
 *
 * It becomes a JPH::RagdollSettings (Jolt/Physics/Ragdoll/Ragdoll.h): one
 * body per part, a SwingTwist or Hinge constraint to its parent, parents and
 * parts overlapping at rest kept from colliding (DisableParentChildCollisions),
 * masses evened out for the solver (Stabilize).
 *
 * Lengths are world units, angles radians, masses kilograms.
 */
namespace JoltPhysics
{
    /* A rigid transform: a position and a unit quaternion (X, Y, Z, W). */
    struct RagdollTransform
    {
        float Position[3] = { 0.0f, 0.0f, 0.0f };
        float Rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    };

    enum ERagdollShape : uint8_t
    {
        RGS_Capsule = 0,   // along the shape's local Y: Radius, HalfHeight (the cylinder's half length)
        RGS_Sphere  = 1,   // Radius
        RGS_Box     = 2,   // HalfExtent
    };

    enum ERagdollJoint : uint8_t
    {
        RGJ_SwingTwist = 0,   // a cone round TwistAxis (NormalHalfCone, PlaneHalfCone) and a twist about it (TwistMin..Max)
        RGJ_Hinge      = 1,   // about TwistAxis, TwistMin..Max from where PlaneAxis lies at rest
        RGJ_Fixed      = 2,
    };

    struct RagdollPartDesc
    {
        /* An earlier part; -1 for the root, which must be part 0 (and the only one). */
        int32_t Parent = -1;

        /* The part's frame at rest. Every Rest-space value below is in the
         * same space as these (any space: only how the parts sit relative to
         * one another matters). The part's body is this frame, so a pose is
         * the parts' frames. */
        RagdollTransform Rest;

        /* The shape, placed in the part's frame. */
        uint8_t          Shape = RGS_Capsule;
        float            Radius     = 5.0f;
        float            HalfHeight = 10.0f;
        float            HalfExtent[3] = { 5.0f, 5.0f, 5.0f };
        RagdollTransform ShapeLocal;

        float Mass = 1.0f;

        /* The joint to Parent, in rest space: where it is, and its axes as
         * the child and as the parent hold them -- the same at rest unless
         * the limits are centred off the rest pose (a hip that swings
         * further forward than back). TwistAxis and PlaneAxis perpendicular. */
        uint8_t Joint = RGJ_SwingTwist;
        float   JointPosition[3]   = { 0.0f, 0.0f, 0.0f };
        float   TwistAxis[3]       = { 1.0f, 0.0f, 0.0f };
        float   PlaneAxis[3]       = { 0.0f, 1.0f, 0.0f };
        float   ParentTwistAxis[3] = { 1.0f, 0.0f, 0.0f };
        float   ParentPlaneAxis[3] = { 0.0f, 1.0f, 0.0f };
        float   TwistMin = 0.0f, TwistMax = 0.0f;   // a hinge's limits: TwistMin in [-pi, 0], TwistMax in [0, pi]
        float   NormalHalfCone = 0.0f;              // swing about the normal (TwistAxis x PlaneAxis)
        float   PlaneHalfCone  = 0.0f;              // swing about PlaneAxis
        float   FrictionTorque = 0.0f;              // N m the joint resists turning with: a limp, not a floppy, limb
    };

    struct RagdollDesc
    {
        std::vector<RagdollPartDesc> Parts;

        /* What a raycast that hits a part reports (PhysicsHit::Entity). */
        GameCore::EntityID Owner;

        /* As a Rigid Body's (RigidBodyComponent.h). */
        int32_t Layer = 1;
        int32_t Mask  = -1;
        float   Friction       = 0.8f;
        float   Restitution    = 0.0f;
        float   LinearDamping  = 0.05f;
        float   AngularDamping = 0.5f;
        bool    bContinuous    = true;   // swept motion: a thrown hand does not pass through a wall
    };
}

#endif //JOLTPHYSICS_PHYSICSRAGDOLL_H
