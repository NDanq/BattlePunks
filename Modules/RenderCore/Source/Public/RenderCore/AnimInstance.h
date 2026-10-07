#ifndef RENDERCORE_ANIMINSTANCE_H
#define RENDERCORE_ANIMINSTANCE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/AnimPose.h"
#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * One animated skeleton's playback (ANIMATION.md): what an Animation
 * component drives on the game thread, and what the editor's previews drive
 * on their own. Not thread-safe; one owner.
 *
 * It plays a single clip. Update advances it and evaluates the pose;
 * GetModelPose is then the skeleton in model space, one matrix per bone, and
 * GetFiredNotifies what the update passed.
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

        /* The skeleton it poses; null: nothing to pose. Resets playback. */
        void SetSkeleton(std::shared_ptr<const SkeletonData> InSkeleton);
        const std::shared_ptr<const SkeletonData>& GetSkeleton() const { return Skeleton; }

        /* Plays InClip from the start (null: the reference pose). */
        void PlayClip(std::shared_ptr<const AnimClipData> InClip);
        const std::shared_ptr<const AnimClipData>& GetClip() const { return Clip; }

        /* Advances by InDeltaSeconds (scaled by the play rate) and evaluates. */
        void Update(float InDeltaSeconds);

        /* Evaluates where it is, without moving: after a seek or a change. */
        void Evaluate();

        /* The clip's playhead, seconds; a seek fires no notifies. */
        float GetTime() const { return Time; }
        void  SetTime(float InSeconds);

        void  SetPlayRate(float InRate) { PlayRate = InRate; }
        float GetPlayRate() const       { return PlayRate; }

        /* A clip plays as it says (AnimClipData::bLoop) unless overridden. */
        void SetLoopOverride(int InLoop) { LoopOverride = InLoop; }   // -1: the clip's; 0: once; 1: loop

        const AnimLocalPose&           GetLocalPose() const { return LocalPose; }
        const std::vector<AnimMatrix>& GetModelPose() const { return ModelPose; }
        bool HasPose() const { return !ModelPose.empty(); }

        /* The notifies the last Update passed, in order. */
        const std::vector<std::string>& GetFiredNotifies() const { return Fired; }

    private:
        bool IsLooping() const;

        std::shared_ptr<const SkeletonData> Skeleton;
        std::shared_ptr<const AnimClipData> Clip;
        AnimClipBinding                     Binding;

        float Time     = 0.0f;
        float PlayRate = 1.0f;
        int   LoopOverride = -1;

        AnimLocalPose            LocalPose;
        std::vector<AnimMatrix>  ModelPose;
        std::vector<std::string> Fired;
    };
}

#endif //RENDERCORE_ANIMINSTANCE_H
