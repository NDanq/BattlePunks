#ifndef RENDERCORE_ANIMINSTANCE_H
#define RENDERCORE_ANIMINSTANCE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/AnimPose.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * One animated skeleton's playback (ANIMATION.md): what an Animation
 * component drives on the game thread, and what the editor's previews drive
 * on their own. Not thread-safe; one owner.
 *
 * It plays a single clip, or an anim state (AnimStateAsset.h) -- layers of
 * state machines driven by parameters. A clip alone is one layer of one
 * state. Update advances it and evaluates the pose; GetModelPose is then the
 * skeleton in model space, one matrix per bone, and GetFiredNotifies what
 * the update passed.
 */
namespace RenderCore
{
    class RENDERCORE_API AnimInstance
    {
    public:
        AnimInstance();
        ~AnimInstance();

        AnimInstance(const AnimInstance&)            = delete;
        AnimInstance& operator=(const AnimInstance&) = delete;

        /* The skeleton it poses; null: nothing to pose. Restarts playback. */
        void SetSkeleton(std::shared_ptr<const SkeletonData> InSkeleton);
        const std::shared_ptr<const SkeletonData>& GetSkeleton() const { return Skeleton; }

        /* Plays InClip alone from the start (null: the reference pose). */
        void PlayClip(std::shared_ptr<const AnimClipData> InClip);
        const std::shared_ptr<const AnimClipData>& GetClip() const { return Clip; }

        /* Runs InState from its layers' entry states, its clips loaded through
         * AnimClipAssetCache; parameters start at its defaults. Null: nothing
         * plays (the reference pose). */
        void SetState(std::shared_ptr<const AnimStateData> InState);
        const std::shared_ptr<const AnimStateData>& GetState() const { return State; }

        /* An anim state's parameter, by name: false if it has none so named. */
        bool  SetParameter(const std::string& InName, float InValue);
        float GetParameter(const std::string& InName, float InDefault = 0.0f) const;

        /* The triggers transitions used in the last Update: the component's
         * copies go back to 0 too. */
        const std::vector<std::string>& GetConsumedTriggers() const { return Consumed; }

        /* Advances by InDeltaSeconds (scaled by the play rate) and evaluates
         * -- unless bInEvaluate is false: time passes (notifies, transitions,
         * root motion) and the pose stays as it was. */
        void Update(float InDeltaSeconds, bool bInEvaluate = true);

        /* Evaluates where it is, without moving: after a seek or a change.
         * The look-at goes straight to its target (Update eases it there). */
        void Evaluate();

        /* The first layer's current state: its dominant clip's playhead and
         * length, in seconds. A seek fires no notifies, and leaves any
         * crossfade finished. */
        float GetTime() const;
        float GetDuration() const;
        void  SetTime(float InSeconds);

        void  SetPlayRate(float InRate) { PlayRate = InRate; }
        float GetPlayRate() const       { return PlayRate; }

        /* How a lone clip plays (PlayClip): -1 as it says, 0 once, 1 looping. */
        void SetLoopOverride(int InLoop);

        /* A clip blended over whatever plays, by InWeight (0..1), sampled at
         * InSeconds: what a sequence's overlapping clips crossfade with
         * (Engine/Modules/Sequencer). Applied by Evaluate after the layers;
         * nothing advances it. Null, or a weight of 0, clears it. Call
         * Evaluate after. */
        void SetOverlayClip(std::shared_ptr<const AnimClipData> InClip, float InSeconds, float InWeight);
        bool HasOverlayClip() const { return Overlay != nullptr && OverlayWeight > 0.0f; }

        /*
         * A pose something else owns -- a ragdoll's (NimbleEngine's Ragdoll.h).
         * Where InMask (one per bone) is set, the bone's model-space rotation
         * and translation are InModel's, blended over the animated ones by
         * InWeight (0..1), its scale the animation's; a bone InMask leaves
         * out keeps its animated transform under its parent, so a ragdoll's
         * fingers stay curled round the hand physics moves. Applied by every
         * evaluation after everything else until cleared. At a weight of 1
         * the layers are not evaluated at all: the bones left out hold the
         * last animated pose. InModel or InMask of the wrong size, or a
         * weight of 0, clears it. Call Evaluate after, or let Update.
         */
        void SetPoseOverride(const std::vector<AnimMatrix>& InModel, const std::vector<uint8_t>& InMask, float InWeight);
        void ClearPoseOverride();
        bool HasPoseOverride() const { return OverrideWeight > 0.0f; }

        /* Layers, and what each plays now: its state's name ("" for a lone
         * clip), and whether it is crossfading into it. */
        size_t      GetLayerCount() const { return Layers.size(); }
        const char* GetCurrentStateName(size_t InLayer) const;
        int32_t     GetCurrentState(size_t InLayer) const;
        bool        IsInTransition(size_t InLayer) const;

        const AnimLocalPose&           GetLocalPose() const { return LocalPose; }
        const std::vector<AnimMatrix>& GetModelPose() const { return ModelPose; }
        bool HasPose() const { return !ModelPose.empty(); }

        /* The notifies the last Update passed, in order. */
        const std::vector<AnimNotify>& GetFiredNotifies() const { return Fired; }

        /* A state that played once and reached its end in the last Update:
         * its layer, and its name ("" for a lone clip). */
        struct FinishedState
        {
            size_t      Layer = 0;
            std::string Name;
        };
        const std::vector<FinishedState>& GetFinishedStates() const { return Finished; }

        /* How far the last Update's root motion moves the entity, in model
         * space (X and Z): the travel of the first layer's clips set to
         * EAnimRootMotion::Extract, blended as their poses are. */
        const AnimVec3& GetRootMotion() const { return RootMotion; }

        /* The look-at's angle now, degrees: its parameter clamped, weighted
         * and eased toward (AnimStateAsset.h). 0 without one. */
        float GetLookAngle() const { return LookAngle; }

    private:
        struct Sample
        {
            std::shared_ptr<const AnimClipData> Clip;
            AnimClipBinding                     Binding;
            float                               Position  = 0.0f;
            float                               PositionY = 0.0f;
            int32_t                             RootBone  = -1;   // the clip's root bone, when its root motion is not Keep
            int32_t                             RootTrack = -1;
        };
        struct StateRuntime
        {
            std::string         Name;
            std::vector<Sample> Samples;       // by Position
            int32_t             BlendParameter  = -1;
            int32_t             BlendParameterY = -1;   // >= 0: a 2D blend space
            float               Speed = 1.0f;
            EAnimLoop           Loop  = EAnimLoop::Clip;
            int32_t             SyncGroup = -1;   // in SyncGroups; -1: none
        };
        struct TransitionRuntime
        {
            int32_t From = -1, To = 0;
            float   Duration = 0.0f;
            bool    bAtEnd = false;
            struct Condition { int32_t Parameter = -1; EAnimCompare Compare = EAnimCompare::IsTrue; float Value = 0.0f; };
            std::vector<Condition> Conditions;
        };
        /* Where a state is playing: Phase is 0..1 through it. */
        struct Playhead
        {
            int32_t State = -1;
            float   Phase = 0.0f;
            bool    bEnded = false;   // a one-shot reached its end, or a loop wrapped, this update
            bool    bSynced = false;  // followed its sync group's leader last update (false: it locks on, firing nothing)
        };
        struct LayerRuntime
        {
            std::vector<StateRuntime>      States;
            std::vector<TransitionRuntime> Transitions;
            std::vector<float>             Mask;            // per bone; empty: every bone
            float                          Weight = 1.0f;
            int32_t                        WeightParameter = -1;
            bool                           bAdditive = false;
            EAnimAdditiveBase              AdditiveBase = EAnimAdditiveBase::FirstFrame;
            int32_t                        Entry = 0;

            Playhead Current, Previous;                     // Previous: what it fades from
            float    FadeTime = 0.0f, FadeDuration = 0.0f;  // fading while FadeTime < FadeDuration
        };

        /* Rebuilds Layers from State, or from Clip alone. */
        void Rebuild();
        void Restart();

        /* The samples' weights for InState now, and its dominant sample. */
        void  SampleWeights(const StateRuntime& InState, std::vector<float>& OutWeights, size_t& OutDominant) const;
        static void SampleWeights2D(const StateRuntime& InState, float InX, float InY, std::vector<float>& OutWeights, size_t& OutDominant);
        float StateDuration(const StateRuntime& InState) const;
        bool  StateLoops(const StateRuntime& InState) const;
        /* Moves InOutHead -- by InDelta, or to InSyncPhase (0..1) when it
         * follows its sync group's leader; returns its root motion (model space). */
        AnimVec3 AdvancePlayhead(const LayerRuntime& InLayer, Playhead& InOutHead, float InDelta, bool bInNotify,
                                 float InSyncPhase = -1.0f);
        /* InPose's InBone parent's model matrix: its ancestors composed. */
        AnimMatrix ParentModel(const AnimLocalPose& InPose, int32_t InBone) const;
        /* A clip with root motion keeps its root bone's model X and Z at its first frame's. */
        void  HoldRoot(const Sample& InSample, AnimLocalPose& InOutPose) const;
        /* InLayer's state InState at InPhase; an additive layer's as its additive pose. */
        void  EvaluateState(const LayerRuntime& InLayer, const StateRuntime& InState, float InPhase, AnimLocalPose& OutPose);
        /* The pose where it is, the look-at where it is. */
        void  EvaluatePose();
        /* The angle the look-at heads for: its parameter, clamped and weighted. */
        float LookTarget() const;
        /* Turns the look-at's bones by LookAngle, after the layers. */
        void  ApplyLookAt(AnimLocalPose& InOutPose) const;
        /* ModelPose from LocalPose with the override's bones placed. */
        void  ApplyPoseOverride();
        bool  ConditionsHold(const TransitionRuntime& InTransition, const LayerRuntime& InLayer) const;
        void  UpdateTransitions(LayerRuntime& InOutLayer);

        std::shared_ptr<const SkeletonData>  Skeleton;
        std::shared_ptr<const AnimClipData>  Clip;
        std::shared_ptr<const AnimStateData> State;

        std::vector<LayerRuntime>   Layers;
        std::vector<float>          Parameters;     // by State's parameter
        std::vector<std::string>    SyncGroups;     // the states' sync group names, each once
        std::vector<float>          LeaderPhase;    // by sync group, this update: its leader's phase; < 0: none leads
        std::vector<std::string>    Consumed;

        float PlayRate     = 1.0f;
        int   LoopOverride = -1;

        AnimLocalPose            LocalPose;
        AnimLocalPose            Scratch[4];        // state, previous state, sample and additive base poses, reused
        std::vector<AnimMatrix>  ModelPose;
        std::vector<AnimNotify>    Fired;
        std::vector<FinishedState> Finished;
        AnimVec3                 RootMotion;
        AnimLocalPose            RefPose;          // the skeleton's: what root travel is measured in

        /* The look-at, from State's; no bones: none. */
        struct LookBoneRuntime { int32_t Bone = -1; float Share = 0.0f; };
        std::vector<LookBoneRuntime> LookBones;
        int32_t  LookParameter = -1, LookWeightParameter = -1;
        float    LookMin = 0.0f, LookMax = 0.0f, LookResponse = 0.0f;
        AnimVec3 LookAxis;
        float    LookAngle   = 0.0f;
        bool     bLookPlaced = false;   // false: the next update starts it at its target, not easing there

        /* SetOverlayClip's. */
        std::shared_ptr<const AnimClipData> Overlay;
        AnimClipBinding                     OverlayBinding;
        const SkeletonData*                 OverlayBoundTo = nullptr;
        float                               OverlayTime   = 0.0f;
        float                               OverlayWeight = 0.0f;

        /* SetPoseOverride's; OverrideAnimated is the animated model pose it blends over. */
        std::vector<AnimMatrix> OverrideModel;
        std::vector<uint8_t>    OverrideMask;
        std::vector<AnimMatrix> OverrideAnimated;
        float                   OverrideWeight = 0.0f;
    };
}

#endif //RENDERCORE_ANIMINSTANCE_H
