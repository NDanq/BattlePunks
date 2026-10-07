#include "Public/RenderCore/AnimInstance.h"

#include <algorithm>

namespace RenderCore
{
    AnimInstance::AnimInstance()  = default;
    AnimInstance::~AnimInstance() = default;

    void AnimInstance::SetSkeleton(std::shared_ptr<const SkeletonData> InSkeleton)
    {
        Skeleton = std::move(InSkeleton);
        Time = 0.0f;
        if(Clip && Skeleton) { BindClip(*Clip, *Skeleton, Binding); }
        Evaluate();
    }

    void AnimInstance::PlayClip(std::shared_ptr<const AnimClipData> InClip)
    {
        Clip = std::move(InClip);
        Time = 0.0f;
        Binding = AnimClipBinding();
        if(Clip && Skeleton) { BindClip(*Clip, *Skeleton, Binding); }
        Evaluate();
    }

    bool AnimInstance::IsLooping() const
    {
        return LoopOverride < 0 ? (Clip && Clip->bLoop) : LoopOverride != 0;
    }

    void AnimInstance::SetTime(const float InSeconds)
    {
        Time = Clip ? std::clamp(InSeconds, 0.0f, Clip->GetDuration()) : 0.0f;
        Evaluate();
    }

    void AnimInstance::Update(const float InDeltaSeconds)
    {
        Fired.clear();
        if(Clip && Skeleton)
        {
            bool bWrapped = false;
            const float From = Time;
            Time = AdvanceClipTime(*Clip, IsLooping(), Time, InDeltaSeconds * PlayRate, bWrapped);
            if(InDeltaSeconds * PlayRate > 0.0f)
            {
                CollectNotifies(*Clip, From, Time, bWrapped, Fired);
            }
        }
        Evaluate();
    }

    void AnimInstance::Evaluate()
    {
        if(!Skeleton)
        {
            LocalPose.clear();
            ModelPose.clear();
            return;
        }
        GetRefPose(*Skeleton, LocalPose);
        if(Clip)
        {
            SampleClip(*Clip, Binding, Time, LocalPose);
        }
        LocalToModel(*Skeleton, LocalPose, ModelPose);
    }
}
