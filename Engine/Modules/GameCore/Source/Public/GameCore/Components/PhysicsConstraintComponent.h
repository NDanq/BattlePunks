#ifndef GAMECORE_PHYSICSCONSTRAINTCOMPONENT_H
#define GAMECORE_PHYSICSCONSTRAINTCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Math/Vector.h"

namespace GameCore
{
    /* What a PhysicsConstraintComponent allows. */
    enum EPhysicsConstraintType : uint8_t
    {
        PCT_Fixed    = 0,   // welded: no relative motion
        PCT_Point    = 1,   // a ball joint at Pivot: turns freely, never separates
        PCT_Hinge    = 2,   // turns about Axis through Pivot (a door, a wheel); limits in degrees
        PCT_Slider   = 3,   // slides along Axis (a piston); limits in units
        PCT_Distance = 4,   // keeps the pivots between LimitMin and LimitMax apart (a rope); 0/0 keeps today's distance
    };

    /*
     * "Physics Constraint": joins this entity's Rigid Body to another's -- or to
     * the world -- for the physics module (JoltPhysics). Inert without it, like
     * the Rigid Body; see RigidBodyComponent.h.
     *
     * The other end is Target if set (code, or a reference saved with the
     * level), else the entity named TargetName (the editor's way), else the
     * world itself: the joint then pins this body where it stands.
     *
     * Pivot and Axis are in this entity's space (world units, before scale is
     * applied), taken where both bodies stand when the constraint is made.
     * LimitMin == LimitMax means no limit. A motor drives a Hinge at
     * MotorSpeed degrees a second, or a Slider at MotorSpeed units a second.
     */
    struct GAMECORE_API PhysicsConstraintComponent
    {
        GAMECORE_COMPONENT_BODY()

        uint8_t Type     = PCT_Point;   // EPhysicsConstraintType
        uint8_t bEnabled = 1;
        uint8_t bMotor   = 0;
        uint8_t Reserved = 0;

        EntityID Target;                 // the other body; unset: TargetName, then the world
        char     TargetName[64] = {};    // used when Target is unset; "" is the world

        EngineCore::Vector3 Pivot;                                  // this entity's space
        EngineCore::Vector3 Axis = EngineCore::Vector3(0.0, 1.0, 0.0);

        float LimitMin   = 0.0f;
        float LimitMax   = 0.0f;
        float MotorSpeed = 0.0f;

        /* The physics module's constraint handle; 0 for none. Never saved. */
        uint64_t Handle = 0;
    };
}

#endif //GAMECORE_PHYSICSCONSTRAINTCOMPONENT_H
