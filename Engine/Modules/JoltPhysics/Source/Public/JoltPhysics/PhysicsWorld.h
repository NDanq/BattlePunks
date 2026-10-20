#ifndef JOLTPHYSICS_PHYSICSWORLD_H
#define JOLTPHYSICS_PHYSICSWORLD_H

#include <cstdint>

#include "Public/JoltPhysics/JoltPhysicsAPI.h"
#include "Public/JoltPhysics/PhysicsRagdoll.h"
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

        /* A ragdoll's part (Entity is the ragdoll's owner): its handle and
         * the part; 0 and -1 for anything else. */
        uint32_t Ragdoll     = 0;
        int32_t  RagdollPart = -1;
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

        /* Ragdolls, and their parts' bodies (not among Bodies). */
        uint32_t Ragdolls     = 0;
        uint32_t RagdollParts = 0;

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
         * world units whose Layer is in InMask; sensors are skipped, and so is
         * every body of InIgnore when set (its Rigid Body, its ragdoll's parts). */
        bool Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDirection,
                     float InMaxDistance, PhysicsHit& OutHit, int32_t InMask = -1,
                     GameCore::EntityID InIgnore = GameCore::EntityID()) const;

        void GetStats(PhysicsStats& OutStats) const;

        // --- Ragdolls (PhysicsRagdoll.h) ----------------------------------
        /*
         * Not components: NimbleEngine's Ragdoll component makes and drives
         * these through a handle. A ragdoll is kinematic -- its parts follow
         * SetRagdollTarget, hit boxes for raycasts that push dynamic bodies
         * aside -- or dynamic, simulated. Its parts tell no contact events.
         * Detach destroys what is left.
         */

        /* A ragdoll from InDesc, its parts at InPose (InDesc.Parts.size()
         * of them, world space); 0 if InDesc is malformed or Jolt is out of
         * bodies (both logged). */
        uint32_t CreateRagdoll(const RagdollDesc& InDesc, const RagdollTransform* InPose, bool bInDynamic);
        void     DestroyRagdoll(uint32_t InRagdoll);
        bool     HasRagdoll(uint32_t InRagdoll) const;
        bool     IsRagdollDynamic(uint32_t InRagdoll) const;

        /* Kinematic: where the parts are to be after this frame's steps;
         * they move there over the steps (with the velocity that takes, so
         * what they hit is pushed, and a switch to dynamic keeps it). */
        void SetRagdollTarget(uint32_t InRagdoll, const RagdollTransform* InPose);

        /* Simulated from the next step. Each part's velocity (world units
         * per second, degrees per second) if given, else what it had --
         * a kinematic ragdoll's is its animation's. */
        void SetRagdollDynamic(uint32_t InRagdoll, const EngineCore::Vector3* InLinear = nullptr,
                               const EngineCore::Vector3* InAngular = nullptr);

        /* Its parts now (world space), blended between the last two steps
         * as dynamic bodies' Transforms are; false if no such ragdoll. */
        bool GetRagdollPose(uint32_t InRagdoll, RagdollTransform* OutPose) const;

        /* Any part awake: false once a dynamic ragdoll has come to rest. */
        bool IsRagdollAwake(uint32_t InRagdoll) const;

        /* An impulse (world units: kg * units / s) at InAt on part InPart,
         * or the part nearest InAt when -1; wakes it. A kinematic ragdoll
         * ignores it. */
        void AddRagdollImpulse(uint32_t InRagdoll, const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt,
                               int32_t InPart = -1);

        /* The PhysicsWorld attached to InWorld, or nullptr. */
        static PhysicsWorld* Find(const GameCore::World& InWorld);

        struct Impl;

    private:
        Impl* P = nullptr;
    };
}

#endif //JOLTPHYSICS_PHYSICSWORLD_H
