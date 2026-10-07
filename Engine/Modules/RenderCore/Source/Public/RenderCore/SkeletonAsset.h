#ifndef RENDERCORE_SKELETONASSET_H
#define RENDERCORE_SKELETONASSET_H

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
 * .nmskel -- a skeleton: the bone hierarchy skinned meshes bind to and
 * animation clips move (ANIMATION.md). An EngineCore::Archive of chunks:
 *
 *   NSKL v1                the file
 *     SBON v1              bone count; then each bone's name, parent index
 *                          (-1 for a root) and reference pose: translation,
 *                          rotation (x y z w), scale, local to its parent
 *     ISRC v1 (optional)   AssetImportInfo, for Reimport
 *
 * Bones are in hierarchy order -- a parent before its children -- so one
 * pass from the front composes model-space transforms. Names are unique;
 * clips find their bones by name, so meshes and clips imported from different
 * files share a skeleton when their bone names match. At most
 * SKELETON_MAX_BONES (the skinning shader's palette).
 */
namespace RenderCore
{
    enum : uint32_t { SKELETON_MAX_BONES = 256 };

    struct SkeletonBone
    {
        std::string   Name;
        int32_t       Parent = -1;
        AnimTransform RefPose;      // local to Parent; the pose with no animation
    };

    struct SkeletonData
    {
        std::vector<SkeletonBone> Bones;
        AssetImportInfo           ImportInfo;

        /* -1 if no bone is named InName. */
        RENDERCORE_API int32_t FindBone(const std::string& InName) const;

        /* The reference pose in model space, one matrix per bone. */
        RENDERCORE_API void ComputeRefPoseModel(std::vector<AnimMatrix>& OutModel) const;
    };

    /* Both directions. False, logged, for an invalid skeleton. */
    RENDERCORE_API bool SerializeSkeleton(EngineCore::Archive& Ar, SkeletonData& InOutSkeleton);

    /* Through the file provider, so runtime-relative paths work. */
    RENDERCORE_API bool LoadSkeletonFile(const std::string& InPath, SkeletonData& OutSkeleton);

#if WITH_EDITOR
    RENDERCORE_API bool SaveSkeletonFile(const std::string& InPath, SkeletonData& InSkeleton);
#endif

    /* Loaded skeletons by path; MeshAssetCache's contract. */
    class RENDERCORE_API SkeletonAssetCache
    {
    public:
        static std::shared_ptr<const SkeletonData> Find(const std::string& InPath);
        static void     Invalidate(const std::string& InPath);
        static uint64_t GetGeneration();
    };
}

#endif //RENDERCORE_SKELETONASSET_H
