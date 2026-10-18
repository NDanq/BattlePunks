#ifndef RENDERCORE_ANIMRETARGET_H
#define RENDERCORE_ANIMRETARGET_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/AnimPose.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RetargetAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"

/*
 * A clip made for one skeleton, played on another (ANIMATION.md,
 * "Retargeting"; the map is a RetargetSource of a .nmretarget). The clip is
 * sampled onto its own skeleton, and each mapped target bone then turns, in
 * model space, as far from its reference pose as its source bone turned from
 * the source's:
 *
 *   Qt[j] = Qs[i] * inverse(PoseOffset[j] * QsRef[i]) * QtRef[j]
 *
 * so the target keeps its own bone axes (roll) and lengths whatever the
 * source's are; an unmapped bone keeps its local rotation under its parent.
 * Translations follow each bone's ERetargetTranslation; scale is the target's
 * reference scale (scale tracks are not retargeted). Model-space rotations
 * are composed without the bones' scales: the skeletons are expected to have
 * none, or a uniform one, as imported.
 *
 * A binding is built once per clip, map and pair of skeletons; sampling with
 * it allocates nothing once its scratch has grown.
 */
namespace RenderCore
{
    struct RetargetBinding
    {
        AnimClipBinding                   SourceTracks;       // the clip's tracks on the source skeleton
        std::vector<int32_t>              SourceParent;       // by source bone
        AnimLocalPose                     SourceRef;          // the source's reference pose, local
        std::vector<int32_t>              TargetParent;       // by target bone
        AnimLocalPose                     TargetRef;          // the target's reference pose, local
        std::vector<int32_t>              TargetSource;       // by target bone: the source bone driving it; -1: none
        std::vector<ERetargetTranslation> TargetTranslation;  // by target bone
        std::vector<AnimQuat>             TargetRefModel;     // by target bone: QtRef, its model-space reference rotation
        std::vector<AnimQuat>             Correction;         // by mapped target bone: inverse(PoseOffset * QsRef) * QtRef,
                                                              // so Qt = Qs * Correction
        float                             RootScale = 1.0f;   // what scaled translations are scaled by

        /* Root motion: the clip's RootBone, and the target bone it drives (-1: none, so no root motion). */
        int32_t  SourceRootBone = -1;
        int32_t  TargetRootBone = -1;
        int32_t  RootTrack      = -1;
        AnimQuat RootTravelRotation;      // from the source root's parent frame to the target root's (reference poses)
        float    RootTravelScale = 1.0f;
        AnimVec3 FirstRootTranslation;    // the target root's local translation at the clip's first frame

        /* Where the clip's notifies' bones are on the target: each source bone
         * a notify names, and the target bone of it (or of its nearest mapped
         * ancestor; "": none, the entity). */
        std::vector<std::pair<std::string, std::string>> NotifyBones;

        bool IsBound() const { return !TargetSource.empty(); }
    };

    /* What SampleClipRetargeted works in; one per caller, reused. */
    struct RetargetScratch
    {
        AnimLocalPose         Source;
        std::vector<AnimQuat> SourceModel;
        std::vector<AnimQuat> TargetModel;
    };

    /* OutBinding for InClip, made on InSource, played on InTarget through InMap.
     * False (OutBinding empty) for an empty skeleton. Target bones the map
     * names that InTarget lacks, and source bones InSource lacks, are left
     * unmapped. */
    RENDERCORE_API bool BindClipRetargeted(const AnimClipData& InClip, const SkeletonData& InSource, const SkeletonData& InTarget,
                                           const RetargetSource& InMap, RetargetBinding& OutBinding);

    /* SampleClip's retargeted twin: InClip at InTime, onto the target's
     * InOutPose (sized to the target skeleton). Mapped bones are written;
     * the rest are left as they are, and their children turn with them. */
    RENDERCORE_API void SampleClipRetargeted(const AnimClipData& InClip, const RetargetBinding& InBinding, float InTime,
                                             RetargetScratch& InOutScratch, AnimLocalPose& InOutPose);

    /* AnimClipData::GetRootTravel, on the target: in the target root's
     * parent frame and scaled as its translation is. Zero with no target root. */
    RENDERCORE_API AnimVec3 GetRootTravelRetargeted(const AnimClipData& InClip, const RetargetBinding& InBinding,
                                                    float InFrom, float InTo, bool bInWrapped);

    /* The notifies from InFirst on (CollectNotifies' just appended) with their
     * bones said on the target. */
    RENDERCORE_API void RemapNotifies(const RetargetBinding& InBinding, std::vector<AnimNotify>& InOutNotifies, size_t InFirst);

    /* The automatic RootScale: the first target bone with a scaled or relative
     * translation, mapped, its reference height (model Y) over its source's. 1
     * when there is none, or a height is zero. */
    RENDERCORE_API float ComputeRetargetRootScale(const SkeletonData& InSource, const SkeletonData& InTarget, const RetargetSource& InMap);

    /* The PoseOffset that lays target bone InTargetBone's source bone along
     * it in the reference poses (the editor's Align Arms): the shortest turn,
     * in model space, from the source bone's direction -- toward the source
     * bone of its first mapped descendant -- to the target bone's, toward that
     * descendant. Identity when InTargetBone is unmapped, has no mapped
     * descendant, or either bone has no length. */
    RENDERCORE_API AnimQuat ComputeRetargetPoseOffset(const SkeletonData& InSource, const SkeletonData& InTarget,
                                                      const RetargetSource& InMap, const std::string& InTargetBone);

    /* InClip retargeted once and for all (the editor's Bake): OutClip plays on
     * InTarget by bone name, as InClip plays through InBinding -- a track per
     * mapped target bone, sampled at InClip's frames (a constant one kept as
     * one key), the notifies' bones moved over, the root bone the target's.
     * Its Skeleton is InTargetPath; its ImportInfo is left for the caller. */
    RENDERCORE_API void BakeRetargetedClip(const AnimClipData& InClip, const RetargetBinding& InBinding, const SkeletonData& InTarget,
                                           const std::string& InTargetPath, AnimClipData& OutClip);

    /*
     * Bindings shared by everything that plays a clip through a retarget: one
     * per (clip, retarget, target skeleton), the source skeleton loaded
     * through SkeletonAssetCache by the clip's Skeleton path. Dropped whole
     * when a clip, skeleton or retarget is reloaded (their caches'
     * generations). Thread-safe.
     */
    class RENDERCORE_API RetargetBindingCache
    {
    public:
        /* InClip's binding onto InTarget through InRetarget -- null when it
         * plays by bone name instead: its Skeleton is empty or InRetarget's
         * target, InRetarget (or InTarget) is null, or InRetarget has no source
         * for it (warned once per pair), or its skeleton cannot load. */
        static std::shared_ptr<const RetargetBinding> Find(const std::shared_ptr<const AnimClipData>& InClip,
                                                           const std::shared_ptr<const RetargetData>& InRetarget,
                                                           const std::shared_ptr<const SkeletonData>& InTarget);
        static void Clear();
    };
}

#endif //RENDERCORE_ANIMRETARGET_H
