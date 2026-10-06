#include "Public/GameCore/Components/VehicleComponent.h"

#include <algorithm>

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* VehicleComponent: registration and what it exposes. The simulation is
 * Engine/Modules/JoltPhysics. */

namespace GameCore
{
    GAMECORE_DEFINE_COMPONENT(VehicleComponent, "Vehicle")

    void VehicleComponent::SetInput(const float InForward, const float InRight, const float InBrake, const float InHandBrake)
    {
        Forward   = std::clamp(InForward, -1.0f, 1.0f);
        Right     = std::clamp(InRight, -1.0f, 1.0f);
        Brake     = std::clamp(InBrake, 0.0f, 1.0f);
        HandBrake = std::clamp(InHandBrake, 0.0f, 1.0f);
    }

    /* The authored fields; the driving inputs and readouts are this run's. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(VehicleComponent, 1)
    {
        Ar.Flag("Enabled",              C.bEnabled);
        Ar.Entity("WheelFL",            C.Wheels[VW_FrontLeft]);
        Ar.Entity("WheelFR",            C.Wheels[VW_FrontRight]);
        Ar.Entity("WheelRL",            C.Wheels[VW_RearLeft]);
        Ar.Entity("WheelRR",            C.Wheels[VW_RearRight]);
        Ar.Field("Drive",               C.Drive);
        Ar.Field("MaxSteerAngle",       C.MaxSteerAngle);
        Ar.Field("EngineTorque",        C.EngineTorque);
        Ar.Field("MaxRPM",              C.MaxRPM);
        Ar.Field("BrakeTorque",         C.BrakeTorque);
        Ar.Field("HandBrakeTorque",     C.HandBrakeTorque);
        Ar.Field("SuspensionMinLength", C.SuspensionMinLength);
        Ar.Field("SuspensionMaxLength", C.SuspensionMaxLength);
        Ar.Field("SuspensionFrequency", C.SuspensionFrequency);
        Ar.Field("SuspensionDamping",   C.SuspensionDamping);
        Ar.Field("WheelRadius",         C.WheelRadius);
        Ar.Field("WheelWidth",          C.WheelWidth);
        Ar.Field("AntiRollStiffness",   C.AntiRollStiffness);
        Ar.Field("MaxPitchRollAngle",   C.MaxPitchRollAngle);
    }

    /* SetInput(forward, right, brake, handbrake) or the four fields one by
     * one; GetSpeed, GetEngineRPM, GetGear, GetWheelsOnGround as of the last
     * step. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(VehicleComponent)
    {
        Bind.Flag("Enabled",      &VehicleComponent::bEnabled);
        Bind.Field("Forward",     &VehicleComponent::Forward);
        Bind.Field("Right",       &VehicleComponent::Right);
        Bind.Field("Brake",       &VehicleComponent::Brake);
        Bind.Field("HandBrake",   &VehicleComponent::HandBrake);
        Bind.Field("EngineTorque", &VehicleComponent::EngineTorque);
        Bind.Field("MaxSteerAngle", &VehicleComponent::MaxSteerAngle);
        Bind.ReadOnly("Speed",          &VehicleComponent::Speed);
        Bind.ReadOnly("EngineRPM",      &VehicleComponent::EngineRPM);
        Bind.ReadOnly("Gear",           &VehicleComponent::Gear);
        Bind.ReadOnly("WheelsOnGround", &VehicleComponent::WheelsOnGround);
        Bind.Native<&VehicleComponent::SetInput>("SetInput", "Forward, Right, Brake, HandBrake");
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(VehicleComponent)
    {
        Props.Flag("Enabled", &VehicleComponent::bEnabled);
        Props.Category("Drivetrain");
        Props.Choice("Drive", &VehicleComponent::Drive, "Rear wheels|Front wheels|All wheels");   // EVehicleDrive
        Props.Field("Engine Torque",     &VehicleComponent::EngineTorque);
        Props.Field("Max RPM",           &VehicleComponent::MaxRPM);
        Props.Field("Max Steer Angle",   &VehicleComponent::MaxSteerAngle);
        Props.Field("Brake Torque",      &VehicleComponent::BrakeTorque);
        Props.Field("Hand Brake Torque", &VehicleComponent::HandBrakeTorque);
        Props.Category("Suspension");
        Props.Field("Min Length", &VehicleComponent::SuspensionMinLength);
        Props.Field("Max Length", &VehicleComponent::SuspensionMaxLength);
        Props.Field("Frequency",  &VehicleComponent::SuspensionFrequency);
        Props.Field("Damping",    &VehicleComponent::SuspensionDamping);
        Props.Field("Anti-Roll Stiffness",  &VehicleComponent::AntiRollStiffness);
        Props.Field("Max Pitch/Roll Angle", &VehicleComponent::MaxPitchRollAngle);
        Props.Category("Wheels");
        Props.Field("Wheel Radius", &VehicleComponent::WheelRadius);
        Props.Field("Wheel Width",  &VehicleComponent::WheelWidth);
        Props.Category("State");
        Props.Field("Speed",      &VehicleComponent::Speed, PF_ReadOnly);
        Props.Field("Engine RPM", &VehicleComponent::EngineRPM, PF_ReadOnly);
        Props.Field("Gear",       &VehicleComponent::Gear, PF_ReadOnly);
    }
#endif // WITH_EDITOR
}
