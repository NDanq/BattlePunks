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

        /* Advances by InDeltaSeconds (scaled by the play rate) and evaluates. */
        void Update(float InDeltaSeconds);

        /* Evaluates where it is, without moving: after a seek or a change. */
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
        const std::vector<std::string>& GetFiredNotifies() const { return Fired; }

    private:
        struct Sample
        {
            std::shared_ptr<const AnimClipData> Clip;
            AnimClipBinding                     Binding;
            float                               Position = 0.0f;
        };
        struct StateRuntime
        {
            std::string         Name;
            std::vector<Sample> Samples;       // by Position
            int32_t             BlendParameter = -1;
            float               Speed = 1.0f;
            EAnimLoop           Loop  = EAnimLoop::Clip;
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
        };
        struct LayerRuntime
        {
            std::vector<StateRuntime>      States;
            std::vector<TransitionRuntime> Transitions;
            std::vector<float>             Mask;            // per bone; empty: every bone
            float                          Weight = 1.0f;
            int32_t                        WeightParameter = -1;
            int32_t                        Entry = 0;

            Playhead Current, Previous;                     // Previous: what it fades from
            float    FadeTime = 0.0f, FadeDuration = 0.0f;  // fading while FadeTime < FadeDuration
        };

        /* Rebuilds Layers from State, or from Clip alone. */
        void Rebuild();
        void Restart();

        /* The samples' weights for InState now, and its dominant sample. */
        void  SampleWeights(const StateRuntime& InState, std::vector<float>& OutWeights, size_t& OutDominant) const;
        float StateDuration(const StateRuntime& InState) const;
        bool  StateLoops(const StateRuntime& InState) const;
        void  AdvancePlayhead(const LayerRuntime& InLayer, Playhead& InOutHead, float InDelta, bool bInNotify);
        void  EvaluateState(const StateRuntime& InState, float InPhase, AnimLocalPose& OutPose);
        bool  ConditionsHold(const TransitionRuntime& InTransition, const LayerRuntime& InLayer) const;
        void  UpdateTransitions(LayerRuntime& InOutLayer);

        std::shared_ptr<const SkeletonData>  Skeleton;
        std::shared_ptr<const AnimClipData>  Clip;
        std::shared_ptr<const AnimStateData> State;

        std::vector<LayerRuntime>   Layers;
        std::vector<float>          Parameters;     // by State's parameter
        std::vector<std::string>    Consumed;

        float PlayRate     = 1.0f;
        int   LoopOverride = -1;

        AnimLocalPose            LocalPose;
        AnimLocalPose            Scratch[3];        // state, sample and layer poses, reused
        std::vector<AnimMatrix>  ModelPose;
        std::vector<std::string> Fired;
    };
}

#endif //RENDERCORE_ANIMINSTANCE_H
