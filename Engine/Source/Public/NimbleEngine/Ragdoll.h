#ifndef NIMBLEENGINE_RAGDOLL_H
#define NIMBLEENGINE_RAGDOLL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/RenderCore/AnimMath.h"

namespace GameCore { class World; }
namespace RenderCore { struct SkeletonData; }

/*
 * Ragdolls: a skeletal mesh posed by physics -- a character's death.
 *
 * Give an entity with an Animation component (and its skinned Render mesh) a
 * Ragdoll component. Its skeleton is read as a humanoid (BuildRagdollProfile:
 * pelvis, spine, chest, head, arms and legs found by bone names and the
 * hierarchy) and becomes about fifteen capsules jointed with anatomical
 * limits. Then:
 *
 *   - Alive (HitBoxes on), the capsules are kinematic and follow the
 *     animation: raycasts hit limbs, and the hit reports the entity.
 *   - Activate() (Lua: ragdoll:Activate(), or ragdoll:ActivateWithImpulse(
 *     impulse, at) for the shot that killed it) hands the skeleton to
 *     physics. The parts keep the animation's velocities, the impulse lands
 *     on the part nearest `at`, the Animation component pauses, and each
 *     socketed child (Anim Socket) holding a disabled Rigid Body -- the
 *     dummy's pistol -- is let go: its socket removed, unparented (unless a
 *     prefab instance holds it), its body enabled, thrown with the hand's
 *     velocity (DropProps).
 *   - GetPartAt(point) names the part a hit landed on ("Head": a head shot).
 *   - When every part has come to rest, RagdollSettled (Entity) is emitted.
 *   - Reset() gives the skeleton back to the animation (a respawn).
 *
 * Bones that are not a part (fingers, twist bones, the neck's upper half)
 * keep their animated pose under whichever part now carries them
 * (AnimInstance::SetPoseOverride).
 *
 * The simulation is Jolt's ragdoll (JoltPhysics' PhysicsRagdoll.h:
 * JPH::RagdollSettings, SwingTwist and Hinge constraints). Without the
 * JoltPhysics module there is no ragdoll yet: Activate logs once and the
 * character keeps animating. RAGDOLL.md has the design, its sources and the
 * fallback still to come.
 */
namespace NimbleEngine
{
    enum : size_t { RAGDOLL_IMPULSES_MAX = 4 };   // impulses one frame holds

    /* "Ragdoll". The authored half is saved; the rest is this run's. */
    struct NE_API RagdollComponent
    {
        GAMECORE_COMPONENT_BODY()

        // === Authored ===========================================================
        uint8_t bEnabled     = 1;
        uint8_t bHitBoxes    = 1;   // kinematic parts follow the animation while it plays
        uint8_t bStartActive = 0;   // ragdolled from the start: a body left in a level
        uint8_t bDropProps   = 1;   // activation lets go of socketed children with a disabled Rigid Body
        float   Mass           = 70.0f;   // kg, the whole body, shared out by segment
        float   Thickness      = 1.0f;    // the limbs' radii, scaled
        float   JointFriction  = 1.0f;    // the joints' resistance, scaled: 0 floppy, more stiff
        float   Friction       = 0.8f;
        float   LinearDamping  = 0.05f;
        float   AngularDamping = 0.5f;
        int32_t Layer = 1;    // as a Rigid Body's
        int32_t Mask  = -1;

        // === This run ===========================================================
        uint8_t  bActive  = 0;   // physics poses the skeleton
        uint8_t  bSettled = 0;   // active, and come to rest
        uint32_t Handle   = 0;   // the physics world's ragdoll; 0: none

        /* What the methods asked for: the next update after the animation
         * (TP_PostUpdate) applies and clears them. */
        uint8_t             bPendingActivate = 0;
        uint8_t             bPendingReset    = 0;
        uint8_t             PendingImpulses  = 0;
        EngineCore::Vector3 ImpulseAmounts[RAGDOLL_IMPULSES_MAX];
        EngineCore::Vector3 ImpulsePoints[RAGDOLL_IMPULSES_MAX];

        void Activate();
        /* World units (kg * units / s), at the part nearest InAt; before
         * activation it waits for it. */
        void AddImpulse(const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt);
        void ActivateWithImpulse(const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt);
        void Reset();

        /* RagdollWorld::FindPart for this component's entity. */
        std::string GetPartAt(const EngineCore::Vector3& InAt) const;

        bool IsActive() const  { return bActive != 0 || bPendingActivate != 0; }
        bool IsSettled() const { return bSettled != 0; }
    };

    enum ERagdollPartShape : uint8_t { RPS_Capsule = 0, RPS_Sphere = 1, RPS_Box = 2 };
    enum ERagdollJointType : uint8_t { RJT_SwingTwist = 0, RJT_Hinge = 1, RJT_Fixed = 2 };

    /*
     * One rigid part of a skeleton's ragdoll, in the skeleton's model space
     * at rest (its reference pose). The part's body is its bone's frame; the
     * shape sits in that frame. Joint axes are model space: as the child
     * holds them and as the parent does (the parent's turned toward where the
     * limits are centred). Angles in degrees.
     */
    struct RagdollProfilePart
    {
        std::string Name;          // its role: Pelvis, Spine, Chest, Head, Thigh.L, Shin.L, Foot.L, UpperArm.L, ...
        int32_t     Bone   = -1;   // the skeleton's bone it moves
        int32_t     Parent = -1;   // an earlier part; -1: the root (the pelvis)

        uint8_t                   Shape = RPS_Capsule;
        float                     Radius = 0.0f;
        float                     HalfHeight = 0.0f;   // a capsule's cylinder, along the shape's Y
        RenderCore::AnimVec3      HalfExtent;
        RenderCore::AnimTransform ShapeLocal;          // in the bone's frame; scale unused

        float MassShare = 0.0f;   // of the whole body (the shares sum to 1)

        uint8_t              Joint = RJT_SwingTwist;
        RenderCore::AnimVec3 TwistAxis, PlaneAxis;               // a hinge: its axis, and the zero-angle normal
        RenderCore::AnimVec3 ParentTwistAxis, ParentPlaneAxis;
        float                TwistMin = 0.0f, TwistMax = 0.0f;   // a hinge's limits
        float                NormalHalfCone = 0.0f;              // side to side (about TwistAxis x PlaneAxis)
        float                PlaneHalfCone  = 0.0f;              // forward and back (about PlaneAxis)
        float                FrictionTorque = 0.0f;              // N m, for a 70 kg body
    };

    struct RagdollProfile
    {
        std::vector<RagdollProfilePart>     Parts;       // parents first, the pelvis first
        std::vector<int32_t>                BonePart;    // by bone: the part that moves it, or -1
        std::vector<RenderCore::AnimMatrix> RestModel;   // the skeleton's reference pose, model space
    };

    /*
     * InSkeleton read as a humanoid: false, with OutError saying what it
     * could not find, if it is not one (no thighs, upper arms, shins,
     * forearms, or a chest above the pelvis). Hands, feet and a spine part
     * between pelvis and chest are made when found.
     *
     * Bones are found by name -- thigh/upleg, shin/calf/lowerleg, foot,
     * upper_arm/upperarm/arm, forearm/lowerarm, hand -- with prefixes such
     * as "DEF-" and "mixamorig:" ignored, the side by which side of the body
     * the bone lies; the pelvis is where the thighs meet, the chest where the
     * arms do, the head the chest's longest other chain. Sizes are fractions
     * of the body's height, masses Dempster's segment fractions (RAGDOLL.md).
     */
    NE_API bool BuildRagdollProfile(const RenderCore::SkeletonData& InSkeleton, RagdollProfile& OutProfile, std::string& OutError);

    /*
     * A World's Ragdoll components, run: GameSession attaches one to each
     * level's World (after its physics, before its level loads). Two
     * systems -- "Engine.Ragdoll.Pose" in TP_PostPhysics hands the
     * simulated parts to the animation; "Engine.Ragdoll" in TP_PostUpdate
     * (after RenderCore.Animation) makes ragdolls, moves hit boxes and
     * applies what the components asked for. Each Attach needs a Detach,
     * before the World's physics goes.
     */
    class NE_API RagdollWorld
    {
    public:
        static RagdollWorld* Attach(GameCore::World& InWorld);
        static void          Detach(GameCore::World& InWorld);
        static RagdollWorld* Find(const GameCore::World* InWorld);

        /* Ragdolls simulating now, and made (hit boxes included). */
        size_t GetActiveCount() const;
        size_t GetRagdollCount() const;

        /* The part of InEntity's ragdoll whose capsule is nearest InAt (world
         * units), as posed now -- "Head", "Chest", "Hand.L"...; empty if it
         * has no profile yet. Which limb a shot hit (Lua: ragdoll:GetPartAt). */
        std::string FindPart(GameCore::EntityID InEntity, const EngineCore::Vector3& InAt) const;

        struct Impl;

    private:
        explicit RagdollWorld(GameCore::World& InWorld);
        ~RagdollWorld();

        RagdollWorld(const RagdollWorld&)            = delete;
        RagdollWorld& operator=(const RagdollWorld&) = delete;

        Impl* P = nullptr;
    };
}

#endif //NIMBLEENGINE_RAGDOLL_H
