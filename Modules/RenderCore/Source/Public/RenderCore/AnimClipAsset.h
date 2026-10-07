#ifndef RENDERCORE_ANIMCLIPASSET_H
#define RENDERCORE_ANIMCLIPASSET_H

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
 * .nmanim -- an animation clip: bone tracks sampled at a fixed rate, and the
 * notifies that fire as it plays (ANIMATION.md). An EngineCore::Archive of
 * chunks:
 *
 *   NANM v1                the file
 *     AHDR v1              sample rate, frame count, looping, root motion, the
 *                          skeleton it was imported with, its root bone, and
 *                          the track and notify counts
 *     ATRK v1              each track: its bone's name, then its translation,
 *                          rotation and scale keys -- one (constant) or one
 *                          per frame each
 *     ANTF v1              notifies: a name and a time in seconds
 *     ISRC v1 (optional)   AssetImportInfo, for Reimport
 *
 * Tracks name bones rather than index them, so a clip plays on any skeleton
 * with those bone names; a bone with no track keeps its reference pose.
 * Keys are local to the bone's parent, engine convention (ANIMATION.md says
 * how the importer converts glTF's).
 */
namespace RenderCore
{
    /* What playback does with the root bone's horizontal travel. */
    enum class EAnimRootMotion : uint8_t
    {
        Keep    = 0,   // as authored: the character walks off its entity
        InPlace = 1,   // its model-space X and Z held at the first frame's: it walks on the spot
        Extract = 2    // held as InPlace, and the travel moves the entity instead
                       // (AnimInstance::GetRootMotion; the animation system applies it)
    };

    struct AnimTrack
    {
        std::string           Bone;
        std::vector<AnimVec3> Translations;   // 1, or FrameCount
        std::vector<AnimQuat> Rotations;      // 1, or FrameCount
        std::vector<AnimVec3> Scales;         // 1, or FrameCount
    };

    struct AnimNotify
    {
        std::string Name;
        float       Time = 0.0f;   // seconds from the start
    };

    struct AnimClipData
    {
        float                   SampleRate = 30.0f;
        uint32_t                FrameCount = 1;
        bool                    bLoop      = true;
        EAnimRootMotion         RootMotion = EAnimRootMotion::Keep;
        std::string             Skeleton;   // the .nmskel it was imported with; informational
        std::string             RootBone;   // whose travel RootMotion governs
        std::vector<AnimTrack>  Tracks;
        std::vector<AnimNotify> Notifies;   // by time
        AssetImportInfo         ImportInfo;

        /* Seconds from the first frame to the last. */
        float GetDuration() const { return FrameCount > 1 && SampleRate > 0.0f ? float(FrameCount - 1) / SampleRate : 0.0f; }

        /* Track InTrack at InTime (clamped to the clip), as authored: root
         * motion is the playback's (AnimInstance), which knows the skeleton. */
        RENDERCORE_API AnimTransform SampleTrack(size_t InTrack, float InTime) const;

        /* The track for InBone; -1 if none. */
        RENDERCORE_API int32_t FindTrack(const std::string& InBone) const;

        /* How far the root bone's translation moves, in its parent's space,
         * from InFrom to InTo seconds -- across the end and round again when
         * bInWrapped. Zero unless RootMotion is Extract. */
        RENDERCORE_API AnimVec3 GetRootTravel(float InFrom, float InTo, bool bInWrapped) const;
    };

    /* Both directions. False, logged, for an invalid clip. */
    RENDERCORE_API bool SerializeAnimClip(EngineCore::Archive& Ar, AnimClipData& InOutClip);

    RENDERCORE_API bool LoadAnimClipFile(const std::string& InPath, AnimClipData& OutClip);

#if WITH_EDITOR
    RENDERCORE_API bool SaveAnimClipFile(const std::string& InPath, AnimClipData& InClip);
#endif

    /* Loaded clips by path; MeshAssetCache's contract. */
    class RENDERCORE_API AnimClipAssetCache
    {
    public:
        static std::shared_ptr<const AnimClipData> Find(const std::string& InPath);
        static void     Invalidate(const std::string& InPath);
        static uint64_t GetGeneration();
    };
}

#endif //RENDERCORE_ANIMCLIPASSET_H
