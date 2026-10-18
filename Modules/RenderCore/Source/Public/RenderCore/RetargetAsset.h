#ifndef RENDERCORE_RETARGETASSET_H
#define RENDERCORE_RETARGETASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/AssetImportInfo.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace EngineCore
{
    class Archive;
}

/*
 * .nmretarget -- how clips made for one skeleton play on another
 * (ANIMATION.md, "Retargeting"): one TARGET skeleton, and for each SOURCE
 * skeleton whose clips it plays, which source bone drives each target bone
 * and what becomes of its translation. The pairing is many to many -- a rig
 * plays several clip packs, a pack drives several rigs -- so it is its own
 * asset rather than a field of the skeleton or the clip. An
 * EngineCore::Archive of chunks:
 *
 *   NRTG v1                the file
 *     RHDR v1              the target skeleton and the source count
 *     RSRC v1              per source: its skeleton, root scale and bone map
 *     ISRC v1 (optional)   AssetImportInfo; unused for now
 *
 * Skeletons are named by runtime path, as clips name theirs; whether they
 * exist is checked when a clip binds (AnimRetarget.h), not on load.
 */
namespace RenderCore
{
    /* What a retargeted bone's local translation is. */
    enum class ERetargetTranslation : uint8_t
    {
        Skeleton          = 0,   // the target's reference pose: its own bone lengths (every bone but the pelvis, usually)
        Animation         = 1,   // the source's, as animated, turned into the target bone's parent's frame
        AnimationScaled   = 2,   // Animation, times the source's root scale: the pelvis
        AnimationRelative = 3    // the target's reference plus how far the source moved from its own, scaled
    };

    struct RetargetBoneMap
    {
        std::string          Target;   // a target bone
        std::string          Source;   // the source bone that drives it; empty: none (it keeps its reference pose)
        ERetargetTranslation Translation = ERetargetTranslation::Skeleton;
        AnimQuat             PoseOffset;   // model space: turns the source's reference bone to lie as the target's
                                           // does (an A pose against a T pose); identity: they already match
    };

    struct RetargetSource
    {
        std::string                  Skeleton;          // the source .nmskel, by runtime path
        std::vector<RetargetBoneMap> Bones;             // one per mapped target bone; a target bone left out keeps its reference pose
        float                        RootScale = 0.0f;  // what scaled translations are scaled by; 0: automatic (the
                                                        // target pelvis's height over the source's)

        /* The entry for target bone InTarget; null if there is none. */
        RENDERCORE_API const RetargetBoneMap* FindBone(const std::string& InTarget) const;
    };

    struct RetargetData
    {
        std::string                 Target;   // the target .nmskel, by runtime path
        std::vector<RetargetSource> Sources;
        AssetImportInfo             ImportInfo;

        /* The source for skeleton InSkeleton (a runtime path); null if there is none. */
        RENDERCORE_API const RetargetSource* FindSource(const std::string& InSkeleton) const;

        /* Why it cannot be used; null if it can. Checked on load and save. */
        RENDERCORE_API const char* FindDefect() const;
    };

    constexpr const char* RETARGET_EXTENSION = ".nmretarget";

    /* Both directions. False, logged, for an invalid retarget. */
    RENDERCORE_API bool SerializeRetarget(EngineCore::Archive& Ar, RetargetData& InOutRetarget);

    RENDERCORE_API bool LoadRetargetFile(const std::string& InPath, RetargetData& OutRetarget);

#if WITH_EDITOR
    RENDERCORE_API bool SaveRetargetFile(const std::string& InPath, RetargetData& InRetarget);
#endif

    /* Loaded retargets by path; MeshAssetCache's contract. */
    class RENDERCORE_API RetargetAssetCache
    {
    public:
        static std::shared_ptr<const RetargetData> Find(const std::string& InPath);
        static void     Invalidate(const std::string& InPath);
        static uint64_t GetGeneration();
    };
}

#endif //RENDERCORE_RETARGETASSET_H
