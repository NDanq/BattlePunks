#ifndef JOLTPHYSICS_PHYSICSWORLD_H
#define JOLTPHYSICS_PHYSICSWORLD_H

#include <cstdint>

#include "Public/JoltPhysics/JoltPhysicsAPI.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

namespace GameCore { class World; }

namespace JoltPhysics
{
    /*
     * How a PhysicsWorld simulates. FromConfig() reads [Physics] (defaults in
     * PhysicsSettings.cpp, documented in README.md).
     */
    struct PhysicsSettings
    {
        /* World units in a metre: Jolt works in metres and is tuned for
         * human-scale objects, so the World's centimetres are converted. */
        double UnitsPerMeter = 100.0;

        /* World units per second squared. */
        EngineCore::Vector3 Gravity = EngineCore::Vector3(0.0, -981.0, 0.0);

        /* Fixed steps per second, and at most this many in one frame (a
         * longer frame drops the rest: the game slows instead of spiralling). */
        float    FixedHz     = 60.0f;
        uint32_t MaxSubSteps = 4;

        /* Dynamic bodies' Transforms are blended between the last two steps,
         * so motion is smooth at any frame rate; off writes each step's pose. */
        bool bInterpolate = true;

        /* A network client: dynamic bodies on networked entities (NetComponent)
         * follow their replicated Transform, kinematically -- the server
         * simulates them. */
        bool bReplicaKinematic = false;

        /* Jolt's fixed capacities. */
        uint32_t MaxBodies             = 16384;
        uint32_t MaxBodyPairs          = 65536;
        uint32_t MaxContactConstraints = 16384;

        JOLTPHYSICS_API static PhysicsSettings FromConfig();
    };

    /* What a physics raycast hit. Distance and Point are in world units. */
    struct PhysicsHit
    {
        GameCore::EntityID  Entity;
        float               Distance = 0.0f;
        EngineCore::Vector3 Point;
        EngineCore::Vector3 Normal;
    };

    struct PhysicsStats
    {
        uint32_t Bodies       = 0;
        uint32_t ActiveBodies = 0;   // awake, dynamic or kinematic
        uint32_t Constraints  = 0;
        uint64_t Steps        = 0;   // fixed steps taken since Attach

        /* Bodies by motion; sensors are counted in their motion too. */
        uint32_t DynamicBodies   = 0;
        uint32_t KinematicBodies = 0;
        uint32_t StaticBodies    = 0;
        uint32_t Sensors         = 0;

        /* Body pairs touching now (a sleeping pair still counts). */
        uint32_t Contacts = 0;

        /* Vehicle components with a running vehicle. */
        uint32_t Vehicles = 0;

        /* The last frame: fixed steps taken, milliseconds in Jolt's steps and
         * in the sync before them (making, moving and pushing bodies). */
        uint32_t StepsLastFrame = 0;
        float    StepMs         = 0.0f;
        float    SyncMs         = 0.0f;
    };

    /*
     * One World's simulation. Attach registers "Physics.Step" in TP_Physics;
     * from then on every frame:
     *
     *   1. Rigid Body components are synchronised: a body is made for each new
     *      or re-enabled one, remade when its shape changed (fields, mesh,
     *      bounds, scale), teleported when game code moved its Transform,
     *      moved to its Transform if Kinematic; what the component's methods
     *      recorded (impulses, forces, velocities) is applied and cleared.
     *      Physics Constraints are made once both ends have bodies, remade
     *      when changed.
     *   2. Fixed steps, as many as the frame's time holds.
     *   3. Dynamic bodies write their world pose to the Transform, and their
     *      velocities to the component.
     *   4. Contacts and sensors that began or ended become World events,
     *      ContactBegin / ContactEnd / SensorEnter / SensorExit (Self, Other),
     *      told once for each side, and the prefab script methods of the same
     *      names: OnContactBegin(self, other, mine) and so on.
     *
     * Components removed and entities destroyed take their bodies with them.
     *
     * NimbleEngine's GameSession attaches one to each level's World (so
     * Play-in-Editor simulates and the edited world does not); a test or tool
     * may attach its own. Detach (or destruction) before the World goes.
     * Game thread only; Jolt's own worker threads run inside the step.
     */
    class JOLTPHYSICS_API PhysicsWorld
    {
    public:
        PhysicsWorld();
        ~PhysicsWorld();   // Detach()es

        PhysicsWorld(const PhysicsWorld&)            = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        /* False, logged, if already attached or Jolt could not start. */
        bool Attach(GameCore::World& InWorld, const PhysicsSettings& InSettings);
        bool Attach(GameCore::World& InWorld) { return Attach(InWorld, PhysicsSettings::FromConfig()); }

        /* Every body and constraint removed, the system and delegates unbound.
         * Idempotent. */
        void Detach();

        bool IsAttached() const;
        GameCore::World* GetWorld() const;

        /* The frame's work, as the TP_Physics system does it; a test may call
         * it directly instead of ticking the World. */
        void Step(float InDeltaSeconds);

        /* The nearest body along InDirection (any length) within InMaxDistance
         * world units whose Layer is in InMask; sensors are skipped. */
        bool Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDirection,
                     float InMaxDistance, PhysicsHit& OutHit, int32_t InMask = -1) const;

        void GetStats(PhysicsStats& OutStats) const;

        /* The PhysicsWorld attached to InWorld, or nullptr. */
        static PhysicsWorld* Find(const GameCore::World& InWorld);

        struct Impl;

    private:
        Impl* P = nullptr;
    };
}

#endif //JOLTPHYSICS_PHYSICSWORLD_H
