#include "Public/RenderCore/AnimRetarget.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

#include "Public/RenderCore/RenderCoreLog.h"

namespace RenderCore
{
    namespace
    {
        /* Model-space rotations of InPose, scales left out (AnimRetarget.h). */
        void ModelRotations(const std::vector<int32_t>& InParent, const AnimLocalPose& InPose, std::vector<AnimQuat>& OutModel)
        {
            OutModel.resize(InParent.size());
            for(size_t Bone = 0; Bone < InParent.size(); ++Bone)
            {
                const int32_t Parent = InParent[Bone];
                OutModel[Bone] = Parent >= 0 ? AnimMath::Normalize(AnimMath::Mul(OutModel[size_t(Parent)], InPose[Bone].Rotation))
                                             : InPose[Bone].Rotation;
            }
        }

        bool IsScaled(const ERetargetTranslation InMode)
        {
            return InMode == ERetargetTranslation::AnimationScaled || InMode == ERetargetTranslation::AnimationRelative;
        }

        bool IsAncestor(const SkeletonData& InSkeleton, const int32_t InAncestor, int32_t InBone)
        {
            for(InBone = InBone >= 0 ? InSkeleton.Bones[size_t(InBone)].Parent : -1; InBone >= 0; InBone = InSkeleton.Bones[size_t(InBone)].Parent)
            {
                if(InBone == InAncestor) { return true; }
            }
            return false;
        }

        AnimVec3 Position(const AnimMatrix& InModel) { return { InModel.M[0][3], InModel.M[1][3], InModel.M[2][3] }; }

        /* The shortest turn taking direction InFrom to InTo; identity if either is zero. */
        AnimQuat FromTo(const AnimVec3& InFrom, const AnimVec3& InTo)
        {
            const float LengthFrom = std::sqrt(InFrom.X * InFrom.X + InFrom.Y * InFrom.Y + InFrom.Z * InFrom.Z);
            const float LengthTo   = std::sqrt(InTo.X * InTo.X + InTo.Y * InTo.Y + InTo.Z * InTo.Z);
            if(LengthFrom < 1e-6f || LengthTo < 1e-6f) { return AnimQuat(); }
            const AnimVec3 A = AnimMath::Scale(InFrom, 1.0f / LengthFrom);
            const AnimVec3 B = AnimMath::Scale(InTo, 1.0f / LengthTo);
            const float    Dot = A.X * B.X + A.Y * B.Y + A.Z * B.Z;
            if(Dot < -0.999999f)
            {
                /* Opposite: half a turn about any axis square to A. */
                AnimVec3 Axis = std::fabs(A.X) < 0.9f ? AnimVec3{ 0.0f, -A.Z, A.Y } : AnimVec3{ A.Z, 0.0f, -A.X };
                const float Length = std::sqrt(Axis.X * Axis.X + Axis.Y * Axis.Y + Axis.Z * Axis.Z);
                Axis = AnimMath::Scale(Axis, 1.0f / Length);
                return { Axis.X, Axis.Y, Axis.Z, 0.0f };
            }
            const AnimVec3 Cross{ A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X };
            return AnimMath::Normalize({ Cross.X, Cross.Y, Cross.Z, 1.0f + Dot });
        }
    }

    AnimQuat ComputeRetargetPoseOffset(const SkeletonData& InSource, const SkeletonData& InTarget, const RetargetSource& InMap,
                                       const std::string& InTargetBone)
    {
        const int32_t Target = InTarget.FindBone(InTargetBone);
        const RetargetBoneMap* Map = InMap.FindBone(InTargetBone);
        const int32_t Source = Map ? InSource.FindBone(Map->Source) : -1;
        if(Target < 0 || Source < 0)
        {
            return AnimQuat();
        }
        /* The first mapped target descendant whose source lies below this bone's source. */
        for(size_t Child = size_t(Target) + 1; Child < InTarget.Bones.size(); ++Child)
        {
            if(!IsAncestor(InTarget, Target, int32_t(Child))) { continue; }
            const RetargetBoneMap* ChildMap = InMap.FindBone(InTarget.Bones[Child].Name);
            const int32_t ChildSource = ChildMap ? InSource.FindBone(ChildMap->Source) : -1;
            if(ChildSource < 0 || !IsAncestor(InSource, Source, ChildSource)) { continue; }

            std::vector<AnimMatrix> SourceModel, TargetModel;
            InSource.ComputeRefPoseModel(SourceModel);
            InTarget.ComputeRefPoseModel(TargetModel);
            const AnimVec3 SourceDirection = AnimMath::Sub(Position(SourceModel[size_t(ChildSource)]), Position(SourceModel[size_t(Source)]));
            const AnimVec3 TargetDirection = AnimMath::Sub(Position(TargetModel[Child]), Position(TargetModel[size_t(Target)]));
            return FromTo(SourceDirection, TargetDirection);
        }
        /* An end bone (a hand, a foot, the head) keeps to its nearest mapped ancestor. */
        for(int32_t Up = InTarget.Bones[size_t(Target)].Parent; Up >= 0; Up = InTarget.Bones[size_t(Up)].Parent)
        {
            const RetargetBoneMap* UpMap = InMap.FindBone(InTarget.Bones[size_t(Up)].Name);
            if(UpMap && InSource.FindBone(UpMap->Source) >= 0)
            {
                return ComputeRetargetPoseOffset(InSource, InTarget, InMap, InTarget.Bones[size_t(Up)].Name);
            }
        }
        return AnimQuat();
    }

    float ComputeRetargetRootScale(const SkeletonData& InSource, const SkeletonData& InTarget, const RetargetSource& InMap)
    {
        /* The first target bone, in hierarchy order, that is scaled and mapped. */
        for(size_t Target = 0; Target < InTarget.Bones.size(); ++Target)
        {
            const RetargetBoneMap* Map = InMap.FindBone(InTarget.Bones[Target].Name);
            if(!Map || !IsScaled(Map->Translation)) { continue; }
            const int32_t Source = InSource.FindBone(Map->Source);
            if(Source < 0) { continue; }
            std::vector<AnimMatrix> SourceModel, TargetModel;
            InSource.ComputeRefPoseModel(SourceModel);
            InTarget.ComputeRefPoseModel(TargetModel);
            const float SourceHeight = SourceModel[size_t(Source)].M[1][3];
            const float TargetHeight = TargetModel[Target].M[1][3];
            if(std::fabs(SourceHeight) < 1e-4f || std::fabs(TargetHeight) < 1e-4f) { return 1.0f; }
            return TargetHeight / SourceHeight;
        }
        return 1.0f;
    }

    bool BindClipRetargeted(const AnimClipData& InClip, const SkeletonData& InSource, const SkeletonData& InTarget,
                            const RetargetSource& InMap, RetargetBinding& OutBinding)
    {
        OutBinding = RetargetBinding();
        if(InSource.Bones.empty() || InTarget.Bones.empty())
        {
            return false;
        }
        BindClip(InClip, InSource, OutBinding.SourceTracks);
        GetRefPose(InSource, OutBinding.SourceRef);
        GetRefPose(InTarget, OutBinding.TargetRef);
        OutBinding.SourceParent.resize(InSource.Bones.size());
        for(size_t Bone = 0; Bone < InSource.Bones.size(); ++Bone) { OutBinding.SourceParent[Bone] = InSource.Bones[Bone].Parent; }
        OutBinding.TargetParent.resize(InTarget.Bones.size());
        for(size_t Bone = 0; Bone < InTarget.Bones.size(); ++Bone) { OutBinding.TargetParent[Bone] = InTarget.Bones[Bone].Parent; }

        std::vector<AnimQuat> SourceRefModel;
        ModelRotations(OutBinding.SourceParent, OutBinding.SourceRef, SourceRefModel);
        ModelRotations(OutBinding.TargetParent, OutBinding.TargetRef, OutBinding.TargetRefModel);

        const size_t Targets = InTarget.Bones.size();
        OutBinding.TargetSource.assign(Targets, -1);
        OutBinding.TargetTranslation.assign(Targets, ERetargetTranslation::Skeleton);
        OutBinding.SourceRefInverse.assign(Targets, AnimQuat());
        for(const RetargetBoneMap& Map : InMap.Bones)
        {
            const int32_t Target = InTarget.FindBone(Map.Target);
            const int32_t Source = Map.Source.empty() ? -1 : InSource.FindBone(Map.Source);
            if(Target < 0 || Source < 0) { continue; }
            OutBinding.TargetSource[size_t(Target)]      = Source;
            OutBinding.TargetTranslation[size_t(Target)] = Map.Translation;
            const AnimQuat Offset = AnimMath::Normalize(Map.PoseOffset);
            OutBinding.SourceRefInverse[size_t(Target)]  = AnimMath::Conjugate(AnimMath::Normalize(AnimMath::Mul(Offset, SourceRefModel[size_t(Source)])));
        }
        OutBinding.RootScale = InMap.RootScale > 0.0f ? InMap.RootScale : ComputeRetargetRootScale(InSource, InTarget, InMap);

        /* Root motion: the target bone the clip's root bone drives (the first, if several). */
        OutBinding.SourceRootBone = InClip.RootBone.empty() ? -1 : InSource.FindBone(InClip.RootBone);
        OutBinding.RootTrack      = InClip.FindTrack(InClip.RootBone);
        if(OutBinding.SourceRootBone >= 0)
        {
            for(size_t Target = 0; Target < Targets; ++Target)
            {
                if(OutBinding.TargetSource[Target] == OutBinding.SourceRootBone) { OutBinding.TargetRootBone = int32_t(Target); break; }
            }
        }
        if(OutBinding.TargetRootBone >= 0)
        {
            const int32_t SourceParent = OutBinding.SourceParent[size_t(OutBinding.SourceRootBone)];
            const int32_t TargetParent = OutBinding.TargetParent[size_t(OutBinding.TargetRootBone)];
            const AnimQuat SourceFrame = SourceParent >= 0 ? SourceRefModel[size_t(SourceParent)] : AnimQuat();
            const AnimQuat TargetFrame = TargetParent >= 0 ? OutBinding.TargetRefModel[size_t(TargetParent)] : AnimQuat();
            OutBinding.RootTravelRotation = AnimMath::Normalize(AnimMath::Mul(AnimMath::Conjugate(TargetFrame), SourceFrame));
            const ERetargetTranslation Mode = OutBinding.TargetTranslation[size_t(OutBinding.TargetRootBone)];
            OutBinding.RootTravelScale = IsScaled(Mode) ? OutBinding.RootScale : 1.0f;

            RetargetScratch Scratch;
            AnimLocalPose   First = OutBinding.TargetRef;
            SampleClipRetargeted(InClip, OutBinding, 0.0f, Scratch, First);
            OutBinding.FirstRootTranslation = First[size_t(OutBinding.TargetRootBone)].Translation;
        }
        else if(InClip.RootMotion != EAnimRootMotion::Keep && !InClip.RootBone.empty())
        {
            RENDERCORE_LOG_WARN("retarget: the clip's root bone '" + InClip.RootBone + "' drives no target bone; it has no root motion there")
        }

        /* Notify bones: each source bone's target, or its nearest mapped ancestor's. */
        std::vector<int32_t> SourceTarget(InSource.Bones.size(), -1);
        for(size_t Target = 0; Target < Targets; ++Target)
        {
            const int32_t Source = OutBinding.TargetSource[Target];
            if(Source >= 0 && SourceTarget[size_t(Source)] < 0) { SourceTarget[size_t(Source)] = int32_t(Target); }
        }
        for(size_t Source = 0; Source < InSource.Bones.size(); ++Source)
        {
            const int32_t Parent = InSource.Bones[Source].Parent;
            if(SourceTarget[Source] < 0 && Parent >= 0) { SourceTarget[Source] = SourceTarget[size_t(Parent)]; }
        }
        for(const AnimNotify& Notify : InClip.Notifies)
        {
            if(Notify.Bone.empty()) { continue; }
            const bool bKnown = std::any_of(OutBinding.NotifyBones.begin(), OutBinding.NotifyBones.end(),
                                            [&](const auto& InPair) { return InPair.first == Notify.Bone; });
            if(bKnown) { continue; }
            const int32_t Source = InSource.FindBone(Notify.Bone);
            const int32_t Target = Source >= 0 ? SourceTarget[size_t(Source)] : -1;
            OutBinding.NotifyBones.emplace_back(Notify.Bone, Target >= 0 ? InTarget.Bones[size_t(Target)].Name : std::string());
        }
        return true;
    }

    void SampleClipRetargeted(const AnimClipData& InClip, const RetargetBinding& InBinding, const float InTime,
                              RetargetScratch& InOutScratch, AnimLocalPose& InOutPose)
    {
        const size_t Targets = InBinding.TargetSource.size();
        if(Targets == 0 || InOutPose.size() != Targets)
        {
            return;
        }

        /* The clip on its own skeleton, and that skeleton's model rotations. */
        InOutScratch.Source = InBinding.SourceRef;
        SampleClip(InClip, InBinding.SourceTracks, InTime, InOutScratch.Source);
        ModelRotations(InBinding.SourceParent, InOutScratch.Source, InOutScratch.SourceModel);
        const std::vector<AnimQuat>& Qs = InOutScratch.SourceModel;

        /* Parents first: each target bone's parent is solved before it. */
        std::vector<AnimQuat>& Qt = InOutScratch.TargetModel;
        Qt.resize(Targets);
        for(size_t Target = 0; Target < Targets; ++Target)
        {
            const int32_t  Parent       = InBinding.TargetParent[Target];
            const AnimQuat ParentModel  = Parent >= 0 ? Qt[size_t(Parent)] : AnimQuat();
            const int32_t  Source       = InBinding.TargetSource[Target];
            AnimTransform& Out          = InOutPose[Target];
            if(Source < 0)
            {
                Qt[Target] = AnimMath::Normalize(AnimMath::Mul(ParentModel, Out.Rotation));
                continue;
            }

            /* How far the source turned from its reference, applied to the target's. */
            const AnimQuat Delta = AnimMath::Mul(Qs[size_t(Source)], InBinding.SourceRefInverse[Target]);
            Qt[Target]   = AnimMath::Normalize(AnimMath::Mul(Delta, InBinding.TargetRefModel[Target]));
            Out.Rotation = AnimMath::Normalize(AnimMath::Mul(AnimMath::Conjugate(ParentModel), Qt[Target]));
            Out.Scale    = InBinding.TargetRef[Target].Scale;

            const ERetargetTranslation Mode = InBinding.TargetTranslation[Target];
            if(Mode == ERetargetTranslation::Skeleton)
            {
                Out.Translation = InBinding.TargetRef[Target].Translation;
                continue;
            }
            /* The source's translation, from its parent's frame into model space and back into the target parent's. */
            const int32_t  SourceParent = InBinding.SourceParent[size_t(Source)];
            const AnimQuat SourceFrame  = SourceParent >= 0 ? Qs[size_t(SourceParent)] : AnimQuat();
            const AnimQuat Into         = AnimMath::Mul(AnimMath::Conjugate(ParentModel), SourceFrame);
            const AnimVec3& Animated    = InOutScratch.Source[size_t(Source)].Translation;
            switch(Mode)
            {
                case ERetargetTranslation::Animation:
                    Out.Translation = AnimMath::Rotate(Into, Animated);
                    break;
                case ERetargetTranslation::AnimationScaled:
                    Out.Translation = AnimMath::Scale(AnimMath::Rotate(Into, Animated), InBinding.RootScale);
                    break;
                case ERetargetTranslation::AnimationRelative:
                {
                    const AnimVec3 Moved = AnimMath::Sub(Animated, InBinding.SourceRef[size_t(Source)].Translation);
                    Out.Translation = AnimMath::Add(InBinding.TargetRef[Target].Translation,
                                                    AnimMath::Scale(AnimMath::Rotate(Into, Moved), InBinding.RootScale));
                    break;
                }
                case ERetargetTranslation::Skeleton:
                    break;
            }
        }
    }

    AnimVec3 GetRootTravelRetargeted(const AnimClipData& InClip, const RetargetBinding& InBinding, const float InFrom, const float InTo,
                                     const bool bInWrapped)
    {
        if(InBinding.TargetRootBone < 0)
        {
            return AnimVec3();
        }
        const AnimVec3 Travel = InClip.GetRootTravel(InFrom, InTo, bInWrapped);
        return AnimMath::Scale(AnimMath::Rotate(InBinding.RootTravelRotation, Travel), InBinding.RootTravelScale);
    }

    void BakeRetargetedClip(const AnimClipData& InClip, const RetargetBinding& InBinding, const SkeletonData& InTarget,
                            const std::string& InTargetPath, AnimClipData& OutClip)
    {
        OutClip = AnimClipData();
        OutClip.SampleRate = InClip.SampleRate;
        OutClip.FrameCount = std::max<uint32_t>(InClip.FrameCount, 1);
        OutClip.bLoop      = InClip.bLoop;
        OutClip.RootMotion = InClip.RootMotion;
        OutClip.Skeleton   = InTargetPath;
        OutClip.RootBone   = InBinding.TargetRootBone >= 0 ? InTarget.Bones[size_t(InBinding.TargetRootBone)].Name : std::string();
        OutClip.Notifies   = InClip.Notifies;
        RemapNotifies(InBinding, OutClip.Notifies, 0);
        if(InBinding.TargetSource.size() != InTarget.Bones.size())
        {
            return;
        }

        /* Every frame onto the reference pose; a track per mapped bone. */
        std::vector<size_t> Mapped;
        for(size_t Bone = 0; Bone < InBinding.TargetSource.size(); ++Bone)
        {
            if(InBinding.TargetSource[Bone] >= 0) { Mapped.push_back(Bone); }
        }
        OutClip.Tracks.resize(Mapped.size());
        for(size_t Track = 0; Track < Mapped.size(); ++Track) { OutClip.Tracks[Track].Bone = InTarget.Bones[Mapped[Track]].Name; }
        RetargetScratch Scratch;
        AnimLocalPose   Pose;
        for(uint32_t Frame = 0; Frame < OutClip.FrameCount; ++Frame)
        {
            Pose = InBinding.TargetRef;
            SampleClipRetargeted(InClip, InBinding, InClip.SampleRate > 0.0f ? float(Frame) / InClip.SampleRate : 0.0f, Scratch, Pose);
            for(size_t Track = 0; Track < Mapped.size(); ++Track)
            {
                const AnimTransform& Each = Pose[Mapped[Track]];
                OutClip.Tracks[Track].Translations.push_back(Each.Translation);
                OutClip.Tracks[Track].Rotations.push_back(Each.Rotation);
                OutClip.Tracks[Track].Scales.push_back(Each.Scale);
            }
        }

        /* Constant keys collapse to one; the root's translation never does, so it keeps its travel. */
        const auto Collapse = [](auto& InOutKeys, const auto& InSame)
        {
            for(size_t Key = 1; Key < InOutKeys.size(); ++Key)
            {
                if(!InSame(InOutKeys[0], InOutKeys[Key])) { return; }
            }
            InOutKeys.resize(std::min<size_t>(InOutKeys.size(), 1));
        };
        const auto SameVector = [](const AnimVec3& InA, const AnimVec3& InB)
        {
            return std::fabs(InA.X - InB.X) < 1e-5f && std::fabs(InA.Y - InB.Y) < 1e-5f && std::fabs(InA.Z - InB.Z) < 1e-5f;
        };
        const auto SameRotation = [](const AnimQuat& InA, const AnimQuat& InB) { return std::fabs(AnimMath::Dot(InA, InB)) > 1.0f - 1e-7f; };
        for(AnimTrack& Track : OutClip.Tracks)
        {
            if(Track.Bone != OutClip.RootBone) { Collapse(Track.Translations, SameVector); }
            Collapse(Track.Rotations, SameRotation);
            Collapse(Track.Scales, SameVector);
        }
    }

    void RemapNotifies(const RetargetBinding& InBinding, std::vector<AnimNotify>& InOutNotifies, const size_t InFirst)
    {
        for(size_t Index = InFirst; Index < InOutNotifies.size(); ++Index)
        {
            AnimNotify& Notify = InOutNotifies[Index];
            if(Notify.Bone.empty()) { continue; }
            const auto Found = std::find_if(InBinding.NotifyBones.begin(), InBinding.NotifyBones.end(),
                                            [&](const auto& InPair) { return InPair.first == Notify.Bone; });
            Notify.Bone = Found != InBinding.NotifyBones.end() ? Found->second : std::string();
        }
    }
}

namespace RenderCore
{
    namespace
    {
        struct BindingCacheState
        {
            using Key = std::tuple<const AnimClipData*, const RetargetData*, const SkeletonData*>;
            struct Item
            {
                /* Held, so the key's pointers are not reused while it is cached. */
                std::shared_ptr<const AnimClipData>    Clip;
                std::shared_ptr<const RetargetData>    Retarget;
                std::shared_ptr<const SkeletonData>    Target;
                std::shared_ptr<const SkeletonData>    Source;
                std::shared_ptr<const RetargetBinding> Binding;   // null: plays by name
            };

            std::mutex                                     Lock;
            std::map<Key, Item>                            Items;
            std::set<std::pair<std::string, std::string>>  Warned;   // (retarget target, clip skeleton)
            uint64_t ClipGeneration = 0, SkeletonGeneration = 0, RetargetGeneration = 0;
        };

        BindingCacheState& GetBindingCache()
        {
            static BindingCacheState State;
            return State;
        }
    }

    std::shared_ptr<const RetargetBinding> RetargetBindingCache::Find(const std::shared_ptr<const AnimClipData>& InClip,
                                                                      const std::shared_ptr<const RetargetData>& InRetarget,
                                                                      const std::shared_ptr<const SkeletonData>& InTarget)
    {
        if(!InClip || !InRetarget || !InTarget || InClip->Skeleton.empty() || InClip->Skeleton == InRetarget->Target)
        {
            return nullptr;
        }
        BindingCacheState& Cache = GetBindingCache();
        const RetargetSource* Map = InRetarget->FindSource(InClip->Skeleton);
        if(!Map)
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            if(Cache.Warned.emplace(InRetarget->Target, InClip->Skeleton).second)
            {
                RENDERCORE_LOG_WARN("retarget to '" + InRetarget->Target + "' has no source for skeleton '" + InClip->Skeleton
                                    + "'; its clips play by bone name")
            }
            return nullptr;
        }

        const BindingCacheState::Key Key{ InClip.get(), InRetarget.get(), InTarget.get() };
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const uint64_t Clips     = AnimClipAssetCache::GetGeneration();
            const uint64_t Skeletons = SkeletonAssetCache::GetGeneration();
            const uint64_t Retargets = RetargetAssetCache::GetGeneration();
            if(Clips != Cache.ClipGeneration || Skeletons != Cache.SkeletonGeneration || Retargets != Cache.RetargetGeneration)
            {
                Cache.Items.clear();
                Cache.ClipGeneration     = Clips;
                Cache.SkeletonGeneration = Skeletons;
                Cache.RetargetGeneration = Retargets;
            }
            const auto Found = Cache.Items.find(Key);
            if(Found != Cache.Items.end())
            {
                return Found->second.Binding;
            }
        }

        /* Bound outside the lock; if two threads race, the first stored wins. */
        BindingCacheState::Item Made;
        Made.Clip     = InClip;
        Made.Retarget = InRetarget;
        Made.Target   = InTarget;
        Made.Source   = SkeletonAssetCache::Find(InClip->Skeleton);
        if(Made.Source)
        {
            auto Binding = std::make_shared<RetargetBinding>();
            if(BindClipRetargeted(*InClip, *Made.Source, *InTarget, *Map, *Binding)) { Made.Binding = std::move(Binding); }
        }
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Items.emplace(Key, std::move(Made)).first->second.Binding;
    }

    void RetargetBindingCache::Clear()
    {
        BindingCacheState& Cache = GetBindingCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Items.clear();
        Cache.Warned.clear();
    }
}
