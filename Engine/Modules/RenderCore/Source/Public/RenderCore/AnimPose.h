#ifndef RENDERCORE_ANIMPOSE_H
#define RENDERCORE_ANIMPOSE_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/SkeletonAsset.h"

/*
 * Poses and what is done with them (ANIMATION.md): a pose is one local
 * transform per skeleton bone. Clips are sampled into poses, poses are
 * blended -- whole, or through a bone mask -- and a final pose is composed
 * into model space for skinning and sockets.
 */
namespace RenderCore
{
    using AnimLocalPose = std::vector<AnimTransform>;

    /* Which bone each of a clip's tracks moves on one skeleton (-1: none). */
    struct AnimClipBinding
    {
        std::vector<int32_t> TrackBone;
    };

    RENDERCORE_API void BindClip(const AnimClipData& InClip, const SkeletonData& InSkeleton, AnimClipBinding& OutBinding);

    /* The reference pose: every bone as it binds. */
    RENDERCORE_API void GetRefPose(const SkeletonData& InSkeleton, AnimLocalPose& OutPose);

    /* The bones InClip moves, at InTime, written into InOutPose; the rest
     * are left as they are. */
    RENDERCORE_API void SampleClip(const AnimClipData& InClip, const AnimClipBinding& InBinding, float InTime,
                                   AnimLocalPose& InOutPose);

    /* InOutPose toward InOther by InWeight (0: unchanged, 1: InOther), per
     * bone scaled by InMask when given (one weight per bone, 0..1). */
    RENDERCORE_API void BlendPoses(AnimLocalPose& InOutPose, const AnimLocalPose& InOther, float InWeight,
                                   const std::vector<float>* InMask = nullptr);

    /* Local to model space, parents first (SkeletonAsset.h's order). */
    RENDERCORE_API void LocalToModel(const SkeletonData& InSkeleton, const AnimLocalPose& InPose, std::vector<AnimMatrix>& OutModel);

    /* A mask that is 1 on InRoot and every bone below it, 0 elsewhere;
     * empty InRoot (or one the skeleton lacks): 1 everywhere. */
    RENDERCORE_API void MakeBoneMask(const SkeletonData& InSkeleton, const std::vector<std::string>& InRoots, std::vector<float>& OutMask);

    /* Where a clip's playhead goes from InFrom by InDelta seconds: wrapped
     * when looping, clamped when not. OutWrapped says it passed the end. */
    RENDERCORE_API float AdvanceClipTime(const AnimClipData& InClip, bool bInLoop, float InFrom, float InDelta, bool& OutWrapped);

    /* The notifies InClip passes moving from InFrom to InTo (forward, wrapping
     * once when bInWrapped), in order, appended to OutNotifies. */
    RENDERCORE_API void CollectNotifies(const AnimClipData& InClip, float InFrom, float InTo, bool bInWrapped,
                                        std::vector<AnimNotify>& OutNotifies);
}

#endif //RENDERCORE_ANIMPOSE_H
