#include "Public/RenderCore/AnimPose.h"

#include <algorithm>
#include <cmath>

namespace RenderCore
{
    void BindClip(const AnimClipData& InClip, const SkeletonData& InSkeleton, AnimClipBinding& OutBinding)
    {
        OutBinding.TrackBone.resize(InClip.Tracks.size());
        for(size_t Track = 0; Track < InClip.Tracks.size(); ++Track)
        {
            OutBinding.TrackBone[Track] = InSkeleton.FindBone(InClip.Tracks[Track].Bone);
        }
    }

    void GetRefPose(const SkeletonData& InSkeleton, AnimLocalPose& OutPose)
    {
        OutPose.resize(InSkeleton.Bones.size());
        for(size_t Bone = 0; Bone < InSkeleton.Bones.size(); ++Bone)
        {
            OutPose[Bone] = InSkeleton.Bones[Bone].RefPose;
        }
    }

    void SampleClip(const AnimClipData& InClip, const AnimClipBinding& InBinding, const float InTime, AnimLocalPose& InOutPose)
    {
        const size_t Tracks = std::min(InClip.Tracks.size(), InBinding.TrackBone.size());
        for(size_t Track = 0; Track < Tracks; ++Track)
        {
            const int32_t Bone = InBinding.TrackBone[Track];
            if(Bone >= 0 && size_t(Bone) < InOutPose.size())
            {
                InOutPose[size_t(Bone)] = InClip.SampleTrack(Track, InTime);
            }
        }
    }

    void BlendPoses(AnimLocalPose& InOutPose, const AnimLocalPose& InOther, const float InWeight, const std::vector<float>* InMask)
    {
        if(InWeight <= 0.0f)
        {
            return;
        }
        const size_t Bones = std::min(InOutPose.size(), InOther.size());
        for(size_t Bone = 0; Bone < Bones; ++Bone)
        {
            float Weight = InWeight;
            if(InMask) { Weight *= Bone < InMask->size() ? (*InMask)[Bone] : 0.0f; }
            if(Weight <= 0.0f)      { continue; }
            if(Weight >= 1.0f)      { InOutPose[Bone] = InOther[Bone]; continue; }
            InOutPose[Bone] = AnimMath::Lerp(InOutPose[Bone], InOther[Bone], Weight);
        }
    }

    void LocalToModel(const SkeletonData& InSkeleton, const AnimLocalPose& InPose, std::vector<AnimMatrix>& OutModel)
    {
        const size_t Bones = std::min(InSkeleton.Bones.size(), InPose.size());
        OutModel.resize(Bones);
        for(size_t Bone = 0; Bone < Bones; ++Bone)
        {
            const AnimMatrix Local  = AnimMath::ToMatrix(InPose[Bone]);
            const int32_t    Parent = InSkeleton.Bones[Bone].Parent;
            OutModel[Bone] = Parent >= 0 ? AnimMath::Mul(OutModel[size_t(Parent)], Local) : Local;
        }
    }

    void MakeBoneMask(const SkeletonData& InSkeleton, const std::vector<std::string>& InRoots, std::vector<float>& OutMask)
    {
        const size_t Bones = InSkeleton.Bones.size();
        std::vector<int32_t> Roots;
        for(const std::string& Root : InRoots)
        {
            const int32_t Bone = Root.empty() ? -1 : InSkeleton.FindBone(Root);
            if(Bone >= 0) { Roots.push_back(Bone); }
        }
        if(Roots.empty())
        {
            OutMask.assign(Bones, 1.0f);
            return;
        }
        /* Parents come first, so one pass marks every bone under a root. */
        OutMask.assign(Bones, 0.0f);
        for(const int32_t Root : Roots) { OutMask[size_t(Root)] = 1.0f; }
        for(size_t Bone = 0; Bone < Bones; ++Bone)
        {
            const int32_t Parent = InSkeleton.Bones[Bone].Parent;
            if(Parent >= 0 && OutMask[size_t(Parent)] > 0.0f) { OutMask[Bone] = 1.0f; }
        }
    }

    float AdvanceClipTime(const AnimClipData& InClip, const bool bInLoop, const float InFrom, const float InDelta, bool& OutWrapped)
    {
        OutWrapped = false;
        const float Duration = InClip.GetDuration();
        if(Duration <= 0.0f)
        {
            return 0.0f;
        }
        float Time = InFrom + InDelta;
        if(!bInLoop)
        {
            if(Time >= Duration) { OutWrapped = InFrom < Duration; }
            return std::clamp(Time, 0.0f, Duration);
        }
        if(Time >= Duration || Time < 0.0f)
        {
            OutWrapped = true;
            Time = std::fmod(Time, Duration);
            if(Time < 0.0f) { Time += Duration; }
        }
        return Time;
    }

    void CollectNotifies(const AnimClipData& InClip, const float InFrom, const float InTo, const bool bInWrapped,
                         std::vector<std::string>& OutNames)
    {
        /* (From, To], so a notify at 0 fires on the wrap and not again at once. */
        const auto Span = [&](const float InA, const float InB, const bool bInclusiveStart)
        {
            for(const AnimNotify& Notify : InClip.Notifies)
            {
                const bool bAfter = bInclusiveStart ? Notify.Time >= InA : Notify.Time > InA;
                if(bAfter && Notify.Time <= InB) { OutNames.push_back(Notify.Name); }
            }
        };
        if(!bInWrapped)
        {
            if(InTo > InFrom) { Span(InFrom, InTo, false); }
            return;
        }
        /* A looping wrap: to the end, then from the start. A clip that stopped at its end: only the first span. */
        Span(InFrom, InClip.GetDuration(), false);
        if(InTo < InFrom) { Span(0.0f, InTo, true); }
    }
}
