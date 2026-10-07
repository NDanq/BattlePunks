#include "Public/GameCore/Components/RigidBodyComponent.h"

#include <cstring>

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* RigidBodyComponent: registration, its methods, and what it exposes to saves,
 * Lua and the editor. The simulation is Engine/Modules/JoltPhysics. */

namespace GameCore
{
    GAMECORE_DEFINE_COMPONENT(RigidBodyComponent, "Rigid Body")

    void RigidBodyComponent::AddImpulse(const EngineCore::Vector3& InImpulse)        { PendingImpulse = PendingImpulse + InImpulse; }
    void RigidBodyComponent::AddForce(const EngineCore::Vector3& InForce)            { PendingForce = PendingForce + InForce; }
    void RigidBodyComponent::AddAngularImpulse(const EngineCore::Vector3& InImpulse) { PendingAngularImpulse = PendingAngularImpulse + InImpulse; }
    void RigidBodyComponent::AddTorque(const EngineCore::Vector3& InTorque)          { PendingTorque = PendingTorque + InTorque; }

    void RigidBodyComponent::SetLinearVelocity(const EngineCore::Vector3& InVelocity)
    {
        PendingLinearVelocity = InVelocity;
        LinearVelocity        = InVelocity;   // reads back what was asked until the step
        bSetLinearVelocity    = 1;
    }

    void RigidBodyComponent::SetAngularVelocity(const EngineCore::Vector3& InDegreesPerSecond)
    {
        PendingAngularVelocity = InDegreesPerSecond;
        AngularVelocity        = InDegreesPerSecond;
        bSetAngularVelocity    = 1;
    }

    const char* RigidBodyComponent::GetMotionName() const
    {
        switch(Motion)
        {
            case RBM_Kinematic: return "Kinematic";
            case RBM_Static:    return "Static";
            default:            return "Dynamic";
        }
    }

    void RigidBodyComponent::SetMotionName(const char* InName)
    {
        if(!InName) { return; }
        if(std::strcmp(InName, "Dynamic") == 0)   { Motion = RBM_Dynamic; }
        if(std::strcmp(InName, "Kinematic") == 0) { Motion = RBM_Kinematic; }
        if(std::strcmp(InName, "Static") == 0)    { Motion = RBM_Static; }
    }

    /* The authored fields; velocities and the body are this run's. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(RigidBodyComponent, 1)
    {
        Ar.Flag("Enabled",          C.bEnabled);
        Ar.Field("Motion",          C.Motion);
        Ar.Field("Shape",           C.Shape);
        Ar.Field("ShapeSource",     C.ShapeSource);
        Ar.Field("Mass",            C.Mass);
        Ar.Field("Friction",        C.Friction);
        Ar.Field("Restitution",     C.Restitution);
        Ar.Field("LinearDamping",   C.LinearDamping);
        Ar.Field("AngularDamping",  C.AngularDamping);
        Ar.Field("GravityFactor",   C.GravityFactor);
        Ar.Flag("Continuous",       C.bContinuous);
        Ar.Flag("Sensor",           C.bSensor);
        Ar.Flag("AllowSleeping",    C.bAllowSleeping);
        Ar.Field("Layer",           C.Layer);
        Ar.Field("Mask",            C.Mask);
        Ar.Field("InitialLinearVelocity",  C.InitialLinearVelocity);
        Ar.Field("InitialAngularVelocity", C.InitialAngularVelocity);
    }

    /* Get/Set for the authored fields; GetLinearVelocity / GetAngularVelocity
     * as of the last step; AddImpulse, AddForce (this frame), AddAngularImpulse,
     * AddTorque, SetLinearVelocity, SetAngularVelocity; GetMotion / SetMotion
     * ("Dynamic", "Kinematic", "Static"). */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(RigidBodyComponent)
    {
        Bind.Flag("Enabled",        &RigidBodyComponent::bEnabled);
        Bind.Field("Mass",          &RigidBodyComponent::Mass);
        Bind.Field("Friction",      &RigidBodyComponent::Friction);
        Bind.Field("Restitution",   &RigidBodyComponent::Restitution);
        Bind.Field("GravityFactor", &RigidBodyComponent::GravityFactor);
        Bind.Field("Layer",         &RigidBodyComponent::Layer);
        Bind.Field("Mask",          &RigidBodyComponent::Mask);
        Bind.Flag("Sensor",         &RigidBodyComponent::bSensor);
        Bind.ReadOnly("Awake",      &RigidBodyComponent::bAwake);

        Bind.NativePure<&RigidBodyComponent::GetLinearVelocity>("GetLinearVelocity", "-> Velocity");
        Bind.NativePure<&RigidBodyComponent::GetAngularVelocity>("GetAngularVelocity", "-> Velocity");
        Bind.Native<&RigidBodyComponent::SetLinearVelocity>("SetLinearVelocity", "Velocity");
        Bind.Native<&RigidBodyComponent::SetAngularVelocity>("SetAngularVelocity", "Velocity");
        Bind.Native<&RigidBodyComponent::AddImpulse>("AddImpulse", "Impulse");
        Bind.Native<&RigidBodyComponent::AddForce>("AddForce", "Force");
        Bind.Native<&RigidBodyComponent::AddAngularImpulse>("AddAngularImpulse", "Impulse");
        Bind.Native<&RigidBodyComponent::AddTorque>("AddTorque", "Torque");
        Bind.NativePure<&RigidBodyComponent::GetMotionName>("GetMotion", "-> Motion");
        Bind.Native<&RigidBodyComponent::SetMotionName>("SetMotion", "Motion");
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(RigidBodyComponent)
    {
        Props.Flag("Enabled", &RigidBodyComponent::bEnabled);
        Props.Choice("Motion", &RigidBodyComponent::Motion, "Dynamic|Kinematic|Static");   // ERigidBodyMotion
        Props.Category("Shape");
        Props.Choice("Shape", &RigidBodyComponent::Shape,
                     "Auto|Box|Sphere|Capsule|Cylinder|Convex hull|Triangle mesh");         // ERigidBodyShape
        Props.Choice("Shape Source", &RigidBodyComponent::ShapeSource,
                     "Auto|Bounds component|Render component mesh");                        // ERigidBodyShapeSource
        Props.Category("Material");
        Props.Field("Mass",            &RigidBodyComponent::Mass);
        Props.Field("Friction",        &RigidBodyComponent::Friction);
        Props.Field("Restitution",     &RigidBodyComponent::Restitution);
        Props.Field("Linear Damping",  &RigidBodyComponent::LinearDamping);
        Props.Field("Angular Damping", &RigidBodyComponent::AngularDamping);
        Props.Field("Gravity Factor",  &RigidBodyComponent::GravityFactor);
        Props.Category("Simulation");
        Props.Flag("Continuous",       &RigidBodyComponent::bContinuous);
        Props.Flag("Sensor",           &RigidBodyComponent::bSensor);
        Props.Flag("Allow Sleeping",   &RigidBodyComponent::bAllowSleeping);
        Props.Field("Initial Linear Velocity",  &RigidBodyComponent::InitialLinearVelocity);
        Props.Field("Initial Angular Velocity", &RigidBodyComponent::InitialAngularVelocity);
        Props.Category("Filtering");
        Props.Field("Layer", &RigidBodyComponent::Layer);
        Props.Field("Mask",  &RigidBodyComponent::Mask);
        Props.Category("State");
        Props.Field("Linear Velocity",  &RigidBodyComponent::LinearVelocity,  PF_ReadOnly);
        Props.Field("Angular Velocity", &RigidBodyComponent::AngularVelocity, PF_ReadOnly);
    }
#endif // WITH_EDITOR
}
