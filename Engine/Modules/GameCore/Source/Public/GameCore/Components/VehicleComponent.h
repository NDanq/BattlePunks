#ifndef GAMECORE_VEHICLECOMPONENT_H
#define GAMECORE_VEHICLECOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

namespace GameCore
{
    /* Which wheels the engine turns. */
    enum EVehicleDrive : uint8_t
    {
        VD_Rear  = 0,
        VD_Front = 1,
        VD_All   = 2,
    };

    /* VehicleComponent::Wheels' slots. Front wheels steer, rear wheels take the handbrake. */
    enum EVehicleWheel : uint8_t
    {
        VW_FrontLeft  = 0,
        VW_FrontRight = 1,
        VW_RearLeft   = 2,
        VW_RearRight  = 3,
        VW_Count      = 4,
    };

    /*
     * "Vehicle": a wheeled vehicle, simulated by the physics module
     * (JoltPhysics; README.md, "Vehicles") -- suspension that follows the
     * ground over bumps and ramps, tyres with grip, an engine, an automatic
     * gearbox, differentials and anti-roll bars. Inert without the module,
     * like the Rigid Body it needs.
     *
     * THE CHASSIS is this entity's Dynamic Rigid Body; its shape is the
     * body's (a Bounds box, usually). Up is the entity's +Y, forward its +Z.
     *
     * THE WHEELS are other entities -- best, this entity's children -- drawn
     * with a Render mesh (a Cylinder on its side). Wheels[] names them; an
     * unset slot is the child named WheelFL, WheelFR, WheelRL or WheelRR.
     * Each wheel is read as the vehicle is made:
     *   - where it is (relative to the chassis) is the wheel's centre with
     *     the suspension half way between SuspensionMinLength and Max;
     *   - its radius and width are its mesh's bounds through its rotation
     *     and scale -- width along the chassis' X, radius from the rest --
     *     unless WheelRadius / WheelWidth say otherwise;
     *   - it carries no Rigid Body of its own: the vehicle's wheels are
     *     cylinders cast against the world, not bodies.
     * From then on the physics step writes each wheel's Transform: how far
     * its suspension is compressed, its steering and its spin.
     *
     * DRIVING. Set Forward (-1..1: throttle, negative for reverse), Right
     * (-1..1: steering, +1 toward the chassis' +X), Brake and HandBrake
     * (0..1) every frame, or once and leave them; SetInput does all four.
     * Asking to go the other way while moving brakes first, then reverses.
     * Speed, EngineRPM, Gear and WheelsOnGround are written back each frame.
     *
     * Units are the World's (centimetres); torques are N m.
     */
    struct GAMECORE_API VehicleComponent
    {
        GAMECORE_COMPONENT_BODY()

        EntityID Wheels[VW_Count];   // EVehicleWheel order; unset: the child named WheelFL/FR/RL/RR

        uint8_t bEnabled = 1;
        uint8_t Drive    = VD_Rear;   // EVehicleDrive
        uint8_t Reserved0 = 0;
        uint8_t Reserved1 = 0;

        float MaxSteerAngle   = 30.0f;     // degrees, front wheels
        float EngineTorque    = 500.0f;    // N m
        float MaxRPM          = 6000.0f;
        float BrakeTorque     = 1500.0f;   // N m per wheel
        float HandBrakeTorque = 4000.0f;   // N m per rear wheel

        float SuspensionMinLength = 10.0f;   // units: fully compressed
        float SuspensionMaxLength = 40.0f;   // units: fully extended
        float SuspensionFrequency = 1.5f;    // Hz: stiffer is higher
        float SuspensionDamping   = 0.5f;    // 0..1

        float WheelRadius = 0.0f;   // units; 0: from the wheel's mesh
        float WheelWidth  = 0.0f;   // units; 0: from the wheel's mesh

        float AntiRollStiffness = 1000.0f;   // N/m, both axles; 0 for none
        float MaxPitchRollAngle = 80.0f;     // degrees the chassis may tip from upright; 180 for no limit

        // === Driving: set by the game, never saved =========================

        float Forward   = 0.0f;
        float Right     = 0.0f;
        float Brake     = 0.0f;
        float HandBrake = 0.0f;

        // === Engine-written, never saved ====================================

        float   Speed          = 0.0f;   // units/s along the chassis' forward
        float   EngineRPM      = 0.0f;
        int32_t Gear           = 0;      // -1 reverse, 0 neutral, 1.. forward
        int32_t WheelsOnGround = 0;

        /* The physics module's vehicle; 0 for none. */
        uint64_t Handle = 0;

        void SetInput(float InForward, float InRight, float InBrake, float InHandBrake);
        float GetSpeed() const { return Speed; }
    };
}

#endif //GAMECORE_VEHICLECOMPONENT_H
