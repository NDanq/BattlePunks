#include "Public/RenderCore/AnimClipAsset.h"

#include <algorithm>
#include <cmath>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/AssetCacheCommon.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_CLIP     = EngineCore::MakeArchiveTag('N','A','N','M');
        constexpr uint32_t TAG_HEADER   = EngineCore::MakeArchiveTag('A','H','D','R');
        constexpr uint32_t TAG_TRACKS   = EngineCore::MakeArchiveTag('A','T','R','K');
        constexpr uint32_t TAG_NOTIFIES = EngineCore::MakeArchiveTag('A','N','T','F');

        /* An implausible rate or length is a corrupt file, not a long clip. */
        constexpr uint32_t MAX_FRAMES = 1u << 20;

        void SerializeVec3(EngineCore::Archive& Ar, AnimVec3& InOutV) { Ar << InOutV.X << InOutV.Y << InOutV.Z; }
        void SerializeQuat(EngineCore::Archive& Ar, AnimQuat& InOutQ) { Ar << InOutQ.X << InOutQ.Y << InOutQ.Z << InOutQ.W; }

        bool ValidKeyCount(const size_t InCount, const uint32_t InFrames) { return InCount == 1 || InCount == InFrames; }

        const char* FindDefect(const AnimClipData& InClip)
        {
            if(InClip.FrameCount == 0 || InClip.FrameCount > MAX_FRAMES) { return "clip: no frames, or implausibly many"; }
            if(!(InClip.SampleRate > 0.0f) || InClip.SampleRate > 1000.0f) { return "clip: a bad sample rate"; }
            for(const AnimTrack& Track : InClip.Tracks)
            {
                if(!ValidKeyCount(Track.Translations.size(), InClip.FrameCount)
                   || !ValidKeyCount(Track.Rotations.size(), InClip.FrameCount)
                   || !ValidKeyCount(Track.Scales.size(), InClip.FrameCount))
                {
                    return "clip: a track has neither one key nor one per frame";
                }
            }
            return nullptr;
        }

        template<typename T, typename F>
        void SerializeKeys(EngineCore::Archive& Ar, std::vector<T>& InOutKeys, const uint32_t InFrames, const size_t InKeyBytes, F&& InEach)
        {
            uint32_t Count = uint32_t(InOutKeys.size());
            Ar << Count;
            if(Ar.IsLoading())
            {
                if(Count != 1 && Count != InFrames)
                {
                    Ar.SetError("clip: a track has neither one key nor one per frame");
                    return;
                }
                if(!CanHoldElements(Ar, Count, InKeyBytes, "clip: key count is past the end of the file")) { return; }
                InOutKeys.assign(Count, T());
            }
            for(T& Key : InOutKeys) { InEach(Ar, Key); }
        }

        template<typename T>
        const T& KeyAt(const std::vector<T>& InKeys, const uint32_t InFrame)
        {
            return InKeys.size() == 1 ? InKeys[0] : InKeys[std::min<size_t>(InFrame, InKeys.size() - 1)];
        }

        TAssetCache<AnimClipData>& GetCache()
        {
            static TAssetCache<AnimClipData> Cache;
            return Cache;
        }
    }

    int32_t AnimClipData::FindTrack(const std::string& InBone) const
    {
        for(size_t Index = 0; Index < Tracks.size(); ++Index)
        {
            if(Tracks[Index].Bone == InBone) { return int32_t(Index); }
        }
        return -1;
    }

    AnimTransform AnimClipData::SampleTrack(const size_t InTrack, const float InTime) const
    {
        AnimTransform Out;
        if(InTrack >= Tracks.size())
        {
            return Out;
        }
        const AnimTrack& Track = Tracks[InTrack];

        const float Frame = std::clamp(InTime, 0.0f, GetDuration()) * SampleRate;
        const uint32_t A  = uint32_t(std::floor(Frame));
        const uint32_t B  = std::min(A + 1, FrameCount - 1);
        const float    T  = Frame - float(A);

        if(!Track.Translations.empty()) { Out.Translation = AnimMath::Lerp(KeyAt(Track.Translations, A), KeyAt(Track.Translations, B), T); }
        if(!Track.Rotations.empty())    { Out.Rotation    = AnimMath::Nlerp(KeyAt(Track.Rotations, A), KeyAt(Track.Rotations, B), T); }
        if(!Track.Scales.empty())       { Out.Scale       = AnimMath::Lerp(KeyAt(Track.Scales, A), KeyAt(Track.Scales, B), T); }

        if(RootMotion == EAnimRootMotion::InPlace && !Track.Translations.empty() && Track.Bone == RootBone)
        {
            Out.Translation.X = Track.Translations[0].X;
            Out.Translation.Z = Track.Translations[0].Z;
        }
        return Out;
    }

    bool SerializeAnimClip(EngineCore::Archive& Ar, AnimClipData& InOutClip)
    {
        if(!OpenRequiredChunk(Ar, TAG_CLIP, 1, "not an animation clip (no NANM chunk)")) { return false; }

        if(Ar.IsSaving())
        {
            if(const char* Defect = FindDefect(InOutClip))
            {
                Ar.SetError(Defect);
                Ar.CloseChunk();
                return false;
            }
        }

        uint32_t TrackCount  = uint32_t(InOutClip.Tracks.size());
        uint32_t NotifyCount = uint32_t(InOutClip.Notifies.size());
        if(OpenRequiredChunk(Ar, TAG_HEADER, 1, "clip: no AHDR chunk"))
        {
            uint8_t RootMotion = uint8_t(InOutClip.RootMotion);
            Ar << InOutClip.SampleRate << InOutClip.FrameCount << InOutClip.bLoop << RootMotion;
            Ar << InOutClip.Skeleton << InOutClip.RootBone << TrackCount << NotifyCount;
            InOutClip.RootMotion = RootMotion == uint8_t(EAnimRootMotion::InPlace) ? EAnimRootMotion::InPlace : EAnimRootMotion::Keep;
            Ar.CloseChunk();
        }
        if(Ar.IsLoading() && !Ar.HasError() && (InOutClip.FrameCount == 0 || InOutClip.FrameCount > MAX_FRAMES))
        {
            Ar.SetError("clip: no frames, or implausibly many");
        }

        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_TRACKS, 1, "clip: no ATRK chunk"))
        {
            /* A track is at least a name and three one-key counts. */
            if(Ar.IsLoading() && CanHoldElements(Ar, TrackCount, 4 * sizeof(uint32_t) + 10 * sizeof(float),
                                                 "clip: track count is past the end of the file"))
            {
                InOutClip.Tracks.assign(TrackCount, AnimTrack());
            }
            for(AnimTrack& Track : InOutClip.Tracks)
            {
                if(Ar.HasError()) { break; }
                Ar << Track.Bone;
                SerializeKeys(Ar, Track.Translations, InOutClip.FrameCount, 3 * sizeof(float), &SerializeVec3);
                if(Ar.HasError()) { break; }
                SerializeKeys(Ar, Track.Rotations,    InOutClip.FrameCount, 4 * sizeof(float), &SerializeQuat);
                if(Ar.HasError()) { break; }
                SerializeKeys(Ar, Track.Scales,       InOutClip.FrameCount, 3 * sizeof(float), &SerializeVec3);
            }
            Ar.CloseChunk();
        }

        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_NOTIFIES, 1, "clip: no ANTF chunk"))
        {
            if(Ar.IsLoading() && CanHoldElements(Ar, NotifyCount, sizeof(uint32_t) + sizeof(float),
                                                 "clip: notify count is past the end of the file"))
            {
                InOutClip.Notifies.assign(NotifyCount, AnimNotify());
            }
            for(AnimNotify& Notify : InOutClip.Notifies)
            {
                Ar << Notify.Name << Notify.Time;
            }
            Ar.CloseChunk();
        }

        if(!Ar.HasError())
        {
            SerializeImportInfo(Ar, InOutClip.ImportInfo);
        }
        Ar.CloseChunk();

        if(Ar.IsLoading() && !Ar.HasError())
        {
            if(const char* Defect = FindDefect(InOutClip))
            {
                Ar.SetError(Defect);
            }
            std::stable_sort(InOutClip.Notifies.begin(), InOutClip.Notifies.end(),
                             [](const AnimNotify& InA, const AnimNotify& InB) { return InA.Time < InB.Time; });
        }
        return !Ar.HasError();
    }

    bool LoadAnimClipFile(const std::string& InPath, AnimClipData& OutClip)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        OutClip = AnimClipData();
        if(!SerializeAnimClip(Ar, OutClip))
        {
            RENDERCORE_LOG_WARN("cannot load animation clip " + InPath)
            OutClip = AnimClipData();
            return false;
        }
        return true;
    }

#if WITH_EDITOR
    bool SaveAnimClipFile(const std::string& InPath, AnimClipData& InClip)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeAnimClip(Ar, InClip))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }
#endif

    std::shared_ptr<const AnimClipData> AnimClipAssetCache::Find(const std::string& InPath)
    {
        return GetCache().Find(InPath, &LoadAnimClipFile, "animation clip");
    }

    void AnimClipAssetCache::Invalidate(const std::string& InPath) { GetCache().Invalidate(InPath); }
    uint64_t AnimClipAssetCache::GetGeneration()                    { return GetCache().GetGeneration(); }
}
