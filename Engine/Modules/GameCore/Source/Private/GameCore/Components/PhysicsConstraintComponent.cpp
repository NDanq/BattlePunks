#include "Public/GameCore/Components/PhysicsConstraintComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* PhysicsConstraintComponent: registration and what it exposes. The joint
 * itself is Engine/Modules/JoltPhysics. */

namespace GameCore
{
    GAMECORE_DEFINE_COMPONENT(PhysicsConstraintComponent, "Physics Constraint")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(PhysicsConstraintComponent, 1)
    {
        Ar.Flag("Enabled",     C.bEnabled);
        Ar.Field("Type",       C.Type);
        Ar.Entity("Target",    C.Target);
        Ar.Field("TargetName", C.TargetName);
        Ar.Field("Pivot",      C.Pivot);
        Ar.Field("Axis",       C.Axis);
        Ar.Field("LimitMin",   C.LimitMin);
        Ar.Field("LimitMax",   C.LimitMax);
        Ar.Flag("Motor",       C.bMotor);
        Ar.Field("MotorSpeed", C.MotorSpeed);
    }

    /* Changing any of these remakes the joint at the next physics step. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(PhysicsConstraintComponent)
    {
        Bind.Flag("Enabled",     &PhysicsConstraintComponent::bEnabled);
        Bind.Field("TargetName", &PhysicsConstraintComponent::TargetName);
        Bind.Field("LimitMin",   &PhysicsConstraintComponent::LimitMin);
        Bind.Field("LimitMax",   &PhysicsConstraintComponent::LimitMax);
        Bind.Flag("Motor",       &PhysicsConstraintComponent::bMotor);
        Bind.Field("MotorSpeed", &PhysicsConstraintComponent::MotorSpeed);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(PhysicsConstraintComponent)
    {
        Props.TypeCategory("Physics");
        Props.Flag("Enabled", &PhysicsConstraintComponent::bEnabled);
        Props.Choice("Type", &PhysicsConstraintComponent::Type, "Fixed|Point|Hinge|Slider|Distance");   // EPhysicsConstraintType
        Props.Field("Target Name", &PhysicsConstraintComponent::TargetName);
        Props.Category("Frame");
        Props.Field("Pivot", &PhysicsConstraintComponent::Pivot);
        Props.Field("Axis",  &PhysicsConstraintComponent::Axis);
        Props.Category("Limits");
        Props.Field("Limit Min", &PhysicsConstraintComponent::LimitMin);
        Props.Field("Limit Max", &PhysicsConstraintComponent::LimitMax);
        Props.Category("Motor");
        Props.Flag("Motor",        &PhysicsConstraintComponent::bMotor);
        Props.Field("Motor Speed", &PhysicsConstraintComponent::MotorSpeed);
    }
#endif // WITH_EDITOR
}
