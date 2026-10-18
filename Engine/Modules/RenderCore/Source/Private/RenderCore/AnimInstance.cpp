#include "Public/RenderCore/AnimInstance.h"

#include <algorithm>
#include <cmath>

namespace RenderCore
{
    namespace
    {
        /* How fast an IK chain's weight follows its parameter, per second (1 - e^(-8 t)). */
        constexpr float IK_WEIGHT_RESPONSE = 8.0f;
    }

    AnimInstance::AnimInstance()  = default;
    AnimInstance::~AnimInstance() = default;

    // === What it plays =========================================================

    void AnimInstance::SetSkeleton(std::shared_ptr<const SkeletonData> InSkeleton)
    {
        Skeleton = std::move(InSkeleton);
        Rebuild();
        Restart();
        EvaluatePose();
    }

    void AnimInstance::SetRetarget(std::shared_ptr<const RetargetData> InRetarget)
    {
        Retarget = std::move(InRetarget);
        Rebuild();
        Restart();
        EvaluatePose();
    }

    void AnimInstance::PlayClip(std::shared_ptr<const AnimClipData> InClip)
    {
        Clip  = std::move(InClip);
        State = nullptr;
        Rebuild();
        Restart();
        EvaluatePose();
    }

    void AnimInstance::SetState(std::shared_ptr<const AnimStateData> InState)
    {
        State = std::move(InState);
        Clip  = nullptr;
        Rebuild();
        Restart();
        EvaluatePose();
    }

    void AnimInstance::SetLoopOverride(const int InLoop)
    {
        LoopOverride = InLoop;
        if(!State && !Layers.empty() && !Layers[0].States.empty())
        {
            Layers[0].States[0].Loop = InLoop < 0 ? EAnimLoop::Clip : InLoop == 0 ? EAnimLoop::Once : EAnimLoop::Loop;
        }
    }

    AnimInstance::Sample AnimInstance::MakeSample(std::shared_ptr<const AnimClipData> InClip, const float InPosition, const float InPositionY) const
    {
        Sample Out;
        Out.Clip      = std::move(InClip);
        Out.Position  = InPosition;
        Out.PositionY = InPositionY;
        if(!Out.Clip || !Skeleton)
        {
            return Out;
        }
        Out.Retargeted = RetargetBindingCache::Find(Out.Clip, Retarget, Skeleton);
        if(Out.Retargeted)
        {
            /* Its root bone is the target bone the clip's root drives. */
            if(Out.Clip->RootMotion != EAnimRootMotion::Keep)
            {
                Out.RootBone  = Out.Retargeted->TargetRootBone;
                Out.RootTrack = Out.Retargeted->RootTrack;
            }
            return Out;
        }
        BindClip(*Out.Clip, *Skeleton, Out.Binding);
        if(Out.Clip->RootMotion != EAnimRootMotion::Keep)
        {
            Out.RootBone  = Skeleton->FindBone(Out.Clip->RootBone);
            Out.RootTrack = Out.Clip->FindTrack(Out.Clip->RootBone);
        }
        return Out;
    }

    void AnimInstance::SampleInto(const Sample& InSample, const float InTime, AnimLocalPose& InOutPose)
    {
        if(InSample.Retargeted) { SampleClipRetargeted(*InSample.Clip, *InSample.Retargeted, InTime, RetargetWork, InOutPose); }
        else                    { SampleClip(*InSample.Clip, InSample.Binding, InTime, InOutPose); }
    }

    void AnimInstance::Rebuild()
    {
        Layers.clear();
        Parameters.clear();
        SyncGroups.clear();
        LookBones.clear();
        LookParameter = LookWeightParameter = -1;
        LookAngle   = 0.0f;
        bLookPlaced = false;
        IKChains.clear();
        IKProbes.clear();
        IKPelvis    = -1;
        bBaseGround = false;
        bIKPlaced   = false;
        PelvisDrop  = 0.0f;
        RefPose.clear();
        if(Skeleton) { GetRefPose(*Skeleton, RefPose); }
        OverlayBoundTo  = nullptr;   // the overlay binds again, perhaps through another retarget
        OverlayBoundVia = nullptr;

        if(!State)
        {
            /* A lone clip: one layer of one state. */
            LayerRuntime Layer;
            StateRuntime Only;
            if(Clip) { Only.Samples.push_back(MakeSample(Clip, 0.0f, 0.0f)); }
            Only.Loop = LoopOverride < 0 ? EAnimLoop::Clip : LoopOverride == 0 ? EAnimLoop::Once : EAnimLoop::Loop;
            Layer.States.push_back(std::move(Only));
            Layers.push_back(std::move(Layer));
            return;
        }

        for(const AnimStateParameter& Parameter : State->Parameters)
        {
            Parameters.push_back(Parameter.Default);
        }
        for(const AnimLayer& Source : State->Layers)
        {
            LayerRuntime Layer;
            Layer.Weight          = Source.Weight;
            Layer.WeightParameter = Source.WeightParameter.empty() ? -1 : State->FindParameter(Source.WeightParameter);
            Layer.bAdditive       = Source.Blend == EAnimLayerBlend::Additive;
            Layer.AdditiveBase    = Source.AdditiveBase;
            Layer.Entry           = Source.EntryState;
            if(Skeleton && !Source.MaskBones.empty())
            {
                MakeBoneMask(*Skeleton, Source.MaskBones, Layer.Mask);
            }
            for(const AnimStateNode& Node : Source.States)
            {
                StateRuntime Runtime;
                Runtime.Name           = Node.Name;
                Runtime.BlendParameter = Node.BlendParameter.empty() ? -1 : State->FindParameter(Node.BlendParameter);
                Runtime.BlendParameterY = Node.BlendParameterY.empty() || Runtime.BlendParameter < 0 ? -1 : State->FindParameter(Node.BlendParameterY);
                Runtime.Speed          = Node.Speed;
                Runtime.Loop           = Node.Loop;
                if(!Node.SyncGroup.empty())
                {
                    const auto Found = std::find(SyncGroups.begin(), SyncGroups.end(), Node.SyncGroup);
                    Runtime.SyncGroup = int32_t(Found - SyncGroups.begin());
                    if(Found == SyncGroups.end()) { SyncGroups.push_back(Node.SyncGroup); }
                }
                for(const AnimBlendSample& Each : Node.Samples)
                {
                    Runtime.Samples.push_back(MakeSample(AnimClipAssetCache::Find(Each.Clip), Each.Position, Each.PositionY));
                }
                std::stable_sort(Runtime.Samples.begin(), Runtime.Samples.end(),
                                 [](const Sample& InA, const Sample& InB) { return InA.Position < InB.Position; });
                Layer.States.push_back(std::move(Runtime));
            }
            for(const AnimTransition& Transition : Source.Transitions)
            {
                TransitionRuntime Runtime;
                Runtime.From     = Transition.From;
                Runtime.To       = Transition.To;
                Runtime.Duration = std::max(0.0f, Transition.Duration);
                Runtime.bAtEnd   = Transition.bAtEnd;
                for(const AnimCondition& Condition : Transition.Conditions)
                {
                    Runtime.Conditions.push_back({ State->FindParameter(Condition.Parameter), Condition.Compare, Condition.Value });
                }
                Layer.Transitions.push_back(std::move(Runtime));
            }
            Layers.push_back(std::move(Layer));
        }

        const AnimLookAt& Look = State->LookAt;
        LookParameter = Look.IsSet() ? State->FindParameter(Look.Parameter) : -1;
        if(Skeleton && LookParameter >= 0)
        {
            for(const AnimLookBone& Each : Look.Bones)
            {
                const int32_t Bone = Skeleton->FindBone(Each.Bone);
                if(Bone >= 0) { LookBones.push_back({ Bone, Each.Share }); }
            }
            /* Root first, whatever order the asset lists them in: each turns about its turned parent. */
            std::stable_sort(LookBones.begin(), LookBones.end(), [](const LookBoneRuntime& InA, const LookBoneRuntime& InB) { return InA.Bone < InB.Bone; });
            LookWeightParameter = Look.WeightParameter.empty() ? -1 : State->FindParameter(Look.WeightParameter);
            LookMin      = Look.MinAngle;
            LookMax      = Look.MaxAngle;
            LookResponse = Look.Response;
            const float Length = std::sqrt(Look.Axis[0] * Look.Axis[0] + Look.Axis[1] * Look.Axis[1] + Look.Axis[2] * Look.Axis[2]);
            LookAxis = Length > 1e-6f ? AnimVec3{ Look.Axis[0] / Length, Look.Axis[1] / Length, Look.Axis[2] / Length } : AnimVec3{ 1.0f, 0.0f, 0.0f };
        }
        if(LookBones.empty()) { LookParameter = -1; }

        /* IK: the chains whose bones the skeleton has. */
        const AnimIK& IK = State->IK;
        IKPelvis    = Skeleton && !IK.PelvisBone.empty() ? Skeleton->FindBone(IK.PelvisBone) : -1;
        IKTraceUp   = IK.TraceUp;
        IKTraceDown = IK.TraceDown;
        IKMaxStep   = IK.MaxStep;
        IKResponse  = IK.Response;
        IKMaxTilt   = IK.MaxTilt;
        for(const AnimIKChain& Chain : IK.Chains)
        {
            if(!Skeleton) { break; }
            IKChainRuntime Runtime;
            Runtime.Name      = Chain.Name;
            Runtime.Root      = Skeleton->FindBone(Chain.Root);
            Runtime.Mid       = Skeleton->FindBone(Chain.Mid);
            Runtime.End       = Skeleton->FindBone(Chain.End);
            Runtime.Goal      = Chain.Goal;
            Runtime.GoalBone  = Chain.GoalBone.empty() ? -1 : Skeleton->FindBone(Chain.GoalBone);
            Runtime.GoalOffset = Chain.GoalOffset;
            Runtime.Weight    = Chain.Weight;
            Runtime.WeightParameter = Chain.WeightParameter.empty() ? -1 : State->FindParameter(Chain.WeightParameter);
            Runtime.Pole      = { Chain.Pole[0], Chain.Pole[1], Chain.Pole[2] };
            Runtime.bAlignEnd = Chain.bAlignEnd;
            if(Runtime.Root < 0 || Runtime.Mid < 0 || Runtime.End < 0) { continue; }
            if(Runtime.Goal == EAnimIKGoal::Bone && Runtime.GoalBone < 0) { continue; }
            IKChains.push_back(std::move(Runtime));
        }
    }

    void AnimInstance::Restart()
    {
        for(LayerRuntime& Layer : Layers)
        {
            Layer.Current  = Playhead();
            Layer.Current.State = Layer.States.empty() ? -1 : std::clamp(Layer.Entry, 0, int32_t(Layer.States.size()) - 1);
            Layer.Previous = Playhead();
            Layer.FadeTime = Layer.FadeDuration = 0.0f;
        }
    }

    // === Parameters ============================================================

    bool AnimInstance::SetParameter(const std::string& InName, const float InValue)
    {
        const int32_t Index = State ? State->FindParameter(InName) : -1;
        if(Index < 0 || size_t(Index) >= Parameters.size())
        {
            return false;
        }
        Parameters[size_t(Index)] = InValue;
        return true;
    }

    float AnimInstance::GetParameter(const std::string& InName, const float InDefault) const
    {
        const int32_t Index = State ? State->FindParameter(InName) : -1;
        return Index >= 0 && size_t(Index) < Parameters.size() ? Parameters[size_t(Index)] : InDefault;
    }

    // === States ================================================================

    void AnimInstance::SampleWeights(const StateRuntime& InState, std::vector<float>& OutWeights, size_t& OutDominant) const
    {
        const size_t Count = InState.Samples.size();
        OutWeights.assign(Count, 0.0f);
        OutDominant = 0;
        if(Count == 0)
        {
            return;
        }
        if(Count == 1 || InState.BlendParameter < 0 || size_t(InState.BlendParameter) >= Parameters.size())
        {
            OutWeights[0] = 1.0f;
            return;
        }
        const float X = Parameters[size_t(InState.BlendParameter)];
        if(InState.BlendParameterY >= 0 && size_t(InState.BlendParameterY) < Parameters.size())
        {
            SampleWeights2D(InState, X, Parameters[size_t(InState.BlendParameterY)], OutWeights, OutDominant);
            return;
        }

        /* A 1D blend space: the two samples either side of the parameter, by distance. */
        if(X <= InState.Samples.front().Position) { OutWeights.front() = 1.0f; return; }
        if(X >= InState.Samples.back().Position)  { OutWeights.back() = 1.0f; OutDominant = Count - 1; return; }
        for(size_t Index = 0; Index + 1 < Count; ++Index)
        {
            const float A = InState.Samples[Index].Position, B = InState.Samples[Index + 1].Position;
            if(X >= A && X <= B)
            {
                const float T = B - A > 1e-6f ? (X - A) / (B - A) : 0.0f;
                OutWeights[Index]     = 1.0f - T;
                OutWeights[Index + 1] = T;
                OutDominant = T > 0.5f ? Index + 1 : Index;
                return;
            }
        }
    }

    void AnimInstance::SampleWeights2D(const StateRuntime& InState, const float InX, const float InY,
                                       std::vector<float>& OutWeights, size_t& OutDominant)
    {
        /* Gradient band interpolation (Johansen, "Automated Semi-Procedural
         * Animation", 2009): each sample's influence falls off toward every
         * other sample along the line between them, the least of those its
         * weight. Any layout works -- a cross, a grid, a ring -- each sample
         * is whole at its own point, and the weights move smoothly between. */
        const size_t Count = InState.Samples.size();
        float Total = 0.0f;
        for(size_t I = 0; I < Count; ++I)
        {
            const float PX = InX - InState.Samples[I].Position, PY = InY - InState.Samples[I].PositionY;
            float Weight = 1.0f;
            for(size_t J = 0; J < Count && Weight > 0.0f; ++J)
            {
                if(J == I) { continue; }
                const float DX = InState.Samples[J].Position - InState.Samples[I].Position;
                const float DY = InState.Samples[J].PositionY - InState.Samples[I].PositionY;
                const float Length2 = DX * DX + DY * DY;
                if(Length2 < 1e-8f) { continue; }   // two samples at one point: they share it
                Weight = std::min(Weight, std::clamp(1.0f - (PX * DX + PY * DY) / Length2, 0.0f, 1.0f));
            }
            OutWeights[I] = Weight;
            Total += Weight;
        }
        if(Total <= 0.0f)
        {
            OutWeights.assign(Count, 0.0f);
            OutWeights[0] = 1.0f;
            return;
        }
        for(size_t I = 0; I < Count; ++I)
        {
            OutWeights[I] /= Total;
            if(OutWeights[I] > OutWeights[OutDominant]) { OutDominant = I; }
        }
    }

    float AnimInstance::StateDuration(const StateRuntime& InState) const
    {
        /* Blended samples share one phase, so the state lasts their weighted
         * length: a walk and a run blended step in time. */
        std::vector<float> Weights;
        size_t Dominant = 0;
        SampleWeights(InState, Weights, Dominant);
        float Duration = 0.0f;
        for(size_t Index = 0; Index < InState.Samples.size(); ++Index)
        {
            if(InState.Samples[Index].Clip) { Duration += Weights[Index] * InState.Samples[Index].Clip->GetDuration(); }
        }
        return Duration;
    }

    bool AnimInstance::StateLoops(const StateRuntime& InState) const
    {
        if(InState.Loop != EAnimLoop::Clip)
        {
            return InState.Loop == EAnimLoop::Loop;
        }
        std::vector<float> Weights;
        size_t Dominant = 0;
        SampleWeights(InState, Weights, Dominant);
        return Dominant < InState.Samples.size() && InState.Samples[Dominant].Clip && InState.Samples[Dominant].Clip->bLoop;
    }

    AnimMatrix AnimInstance::ParentModel(const AnimLocalPose& InPose, const int32_t InBone) const
    {
        AnimMatrix Model;
        if(!Skeleton || InBone < 0) { return Model; }
        /* Up the chain, then composed root-first. */
        int32_t Chain[SKELETON_MAX_BONES];
        int32_t Depth = 0;
        for(int32_t At = Skeleton->Bones[size_t(InBone)].Parent; At >= 0 && Depth < int32_t(SKELETON_MAX_BONES); At = Skeleton->Bones[size_t(At)].Parent)
        {
            Chain[Depth++] = At;
        }
        for(int32_t Index = Depth - 1; Index >= 0; --Index)
        {
            Model = AnimMath::Mul(Model, AnimMath::ToMatrix(InPose[size_t(Chain[Index])]));
        }
        return Model;
    }

    float AnimInstance::LookTarget() const
    {
        if(LookParameter < 0 || size_t(LookParameter) >= Parameters.size()) { return 0.0f; }
        float Angle = std::clamp(Parameters[size_t(LookParameter)], LookMin, LookMax);
        if(LookWeightParameter >= 0 && size_t(LookWeightParameter) < Parameters.size())
        {
            Angle *= std::clamp(Parameters[size_t(LookWeightParameter)], 0.0f, 1.0f);
        }
        return Angle;
    }

    void AnimInstance::ApplyLookAt(AnimLocalPose& InOutPose) const
    {
        /* Not placed yet (evaluated before any update): where it is heading. */
        const float Angle = bLookPlaced ? LookAngle : LookTarget();
        if(LookParameter < 0 || std::fabs(Angle) < 1e-4f) { return; }
        constexpr float DegreesToRadians = 3.14159265358979f / 180.0f;
        for(const LookBoneRuntime& Each : LookBones)
        {
            if(size_t(Each.Bone) >= InOutPose.size() || Each.Share == 0.0f) { continue; }
            /* Its share about the model axis, said in its parent's space -- the parent
             * already turned, so the turns add up along the chain. */
            const AnimQuat ParentRotation = AnimMath::ToTransform(ParentModel(InOutPose, Each.Bone)).Rotation;
            const AnimVec3 Axis = AnimMath::Rotate(AnimMath::Conjugate(ParentRotation), LookAxis);
            const float    Half = 0.5f * Angle * Each.Share * DegreesToRadians;
            const float    Sin  = std::sin(Half);
            const AnimQuat Turn { Axis.X * Sin, Axis.Y * Sin, Axis.Z * Sin, std::cos(Half) };
            AnimQuat& Rotation = InOutPose[size_t(Each.Bone)].Rotation;
            Rotation = AnimMath::Normalize(AnimMath::Mul(Turn, Rotation));
        }
    }

    void AnimInstance::HoldRoot(const Sample& InSample, AnimLocalPose& InOutPose) const
    {
        if(InSample.RootBone < 0 || InSample.RootTrack < 0 || size_t(InSample.RootBone) >= InOutPose.size()) { return; }
        const std::vector<AnimVec3>& Keys = InSample.Clip->Tracks[size_t(InSample.RootTrack)].Translations;
        if(Keys.empty()) { return; }
        /* Retargeted, the first frame is the target root's, as the binding worked it out. */
        const AnimVec3& FirstLocal = InSample.Retargeted ? InSample.Retargeted->FirstRootTranslation : Keys.front();
        const AnimMatrix Parent = ParentModel(InOutPose, InSample.RootBone);
        AnimVec3& Local = InOutPose[size_t(InSample.RootBone)].Translation;
        const AnimVec3 Now   = AnimMath::TransformPoint(Parent, Local);
        const AnimVec3 First = AnimMath::TransformPoint(Parent, FirstLocal);
        Local = AnimMath::TransformPoint(AnimMath::Inverse(Parent), { First.X, Now.Y, First.Z });
    }

    AnimVec3 AnimInstance::AdvancePlayhead(const LayerRuntime& InLayer, Playhead& InOutHead, const float InDelta, const bool bInNotify,
                                           const float InSyncPhase)
    {
        InOutHead.bEnded = false;
        const bool bFollowing = InSyncPhase >= 0.0f;
        const bool bLockingOn = bFollowing && !InOutHead.bSynced;
        InOutHead.bSynced = bFollowing;
        if(InOutHead.State < 0 || size_t(InOutHead.State) >= InLayer.States.size())
        {
            return AnimVec3();
        }
        const StateRuntime& State = InLayer.States[size_t(InOutHead.State)];
        const float Duration = StateDuration(State);
        if(Duration <= 0.0f)
        {
            InOutHead.bEnded = true;   // nothing to play: as good as finished
            return AnimVec3();
        }

        const float From  = InOutHead.Phase;
        float       To    = From + InDelta * State.Speed / Duration;
        bool        bWrap = false;
        if(bFollowing)
        {
            /* Where the leader is: forward, round the end if it is behind.
             * Locking on jumps there, passing nothing. */
            To    = std::clamp(InSyncPhase, 0.0f, 1.0f);
            bWrap = !bLockingOn && To < From && StateLoops(State);
            InOutHead.bEnded = StateLoops(State) ? bWrap : To >= 1.0f;
        }
        else if(StateLoops(State))
        {
            if(To >= 1.0f || To < 0.0f)
            {
                bWrap = To >= 1.0f;
                To = To - std::floor(To);
                InOutHead.bEnded = bWrap;
            }
        }
        else
        {
            /* A one-shot holds its last frame; its end is a span, not a wrap. */
            To = std::clamp(To, 0.0f, 1.0f);
            InOutHead.bEnded = To >= 1.0f;
        }
        InOutHead.Phase = To;

        std::vector<float> Weights;
        size_t Dominant = 0;
        SampleWeights(State, Weights, Dominant);

        /* The dominant clip's notifies, moving forward. */
        const bool bForward = bFollowing ? !bLockingOn && (To > From || bWrap) : InDelta * State.Speed > 0.0f;
        if(bInNotify && To != From && bForward)
        {
            if(Dominant < State.Samples.size() && State.Samples[Dominant].Clip)
            {
                const AnimClipData& Notifying = *State.Samples[Dominant].Clip;
                const float Length = Notifying.GetDuration();
                const size_t Before = Fired.size();
                CollectNotifies(Notifying, From * Length, To * Length, bWrap, Fired);
                if(State.Samples[Dominant].Retargeted) { RemapNotifies(*State.Samples[Dominant].Retargeted, Fired, Before); }
            }
        }

        /* Each Extract clip's travel, by its weight, from its parent's space
         * into the model's (the reference pose's parents); only X and Z move it. */
        AnimVec3 Travel;
        for(size_t Index = 0; Index < State.Samples.size(); ++Index)
        {
            const Sample& Each = State.Samples[Index];
            if(Weights[Index] <= 0.0f || Each.RootBone < 0 || Each.Clip->RootMotion != EAnimRootMotion::Extract) { continue; }
            const float Length = Each.Clip->GetDuration();
            const AnimVec3 Local = Each.Retargeted ? GetRootTravelRetargeted(*Each.Clip, *Each.Retargeted, From * Length, To * Length, bWrap)
                                                   : Each.Clip->GetRootTravel(From * Length, To * Length, bWrap);
            const AnimMatrix Parent = ParentModel(RefPose, Each.RootBone);
            const AnimVec3 Model{ Parent.M[0][0] * Local.X + Parent.M[0][1] * Local.Y + Parent.M[0][2] * Local.Z,
                                  0.0f,
                                  Parent.M[2][0] * Local.X + Parent.M[2][1] * Local.Y + Parent.M[2][2] * Local.Z };
            Travel = AnimMath::Add(Travel, AnimMath::Scale(Model, Weights[Index]));
        }
        return Travel;
    }

    void AnimInstance::EvaluateState(const LayerRuntime& InLayer, const StateRuntime& InState, const float InPhase, AnimLocalPose& OutPose)
    {
        GetRefPose(*Skeleton, OutPose);
        std::vector<float> Weights;
        size_t Dominant = 0;
        SampleWeights(InState, Weights, Dominant);

        /* One sample's pose: an additive layer's relative to its base, and
         * with no root held -- root motion is the first layer's. */
        const auto Pose = [this, &InLayer](const Sample& InSample, const float InTime, AnimLocalPose& OutSample)
        {
            GetRefPose(*Skeleton, OutSample);
            SampleInto(InSample, InTime, OutSample);
            if(!InLayer.bAdditive)
            {
                HoldRoot(InSample, OutSample);
                return;
            }
            GetRefPose(*Skeleton, Scratch[3]);
            if(InLayer.AdditiveBase == EAnimAdditiveBase::FirstFrame)
            {
                SampleInto(InSample, 0.0f, Scratch[3]);
            }
            MakeAdditivePose(OutSample, Scratch[3]);
        };
        if(InLayer.bAdditive)
        {
            OutPose.assign(OutPose.size(), AnimTransform());   // nothing added, until a sample adds it
        }

        /* Incremental: each sample blended in by its share of the weight so far. */
        float Total = 0.0f;
        for(size_t Index = 0; Index < InState.Samples.size(); ++Index)
        {
            const Sample& Each = InState.Samples[Index];
            if(Weights[Index] <= 0.0f || !Each.Clip)
            {
                continue;
            }
            const float Time = InPhase * Each.Clip->GetDuration();
            if(Total <= 0.0f)
            {
                Pose(Each, Time, OutPose);
                Total = Weights[Index];
                continue;
            }
            Pose(Each, Time, Scratch[2]);
            Total += Weights[Index];
            BlendPoses(OutPose, Scratch[2], Weights[Index] / Total);
        }
    }

    bool AnimInstance::ConditionsHold(const TransitionRuntime& InTransition, const LayerRuntime& InLayer) const
    {
        if(InTransition.bAtEnd)
        {
            const Playhead& Head = InLayer.Current;
            const float Duration = StateDuration(InLayer.States[size_t(Head.State)]);
            const float Left     = (1.0f - Head.Phase) * Duration;
            if(!Head.bEnded && Left > std::max(InTransition.Duration, 1e-4f))
            {
                return false;
            }
        }
        for(const TransitionRuntime::Condition& Condition : InTransition.Conditions)
        {
            if(Condition.Parameter < 0 || size_t(Condition.Parameter) >= Parameters.size())
            {
                return false;   // a parameter the anim state lacks never holds
            }
            const float Value = Parameters[size_t(Condition.Parameter)];
            bool bHolds = false;
            switch(Condition.Compare)
            {
                case EAnimCompare::Greater:   bHolds = Value > Condition.Value; break;
                case EAnimCompare::Less:      bHolds = Value < Condition.Value; break;
                case EAnimCompare::Equal:     bHolds = std::fabs(Value - Condition.Value) < 1e-4f; break;
                case EAnimCompare::NotEqual:  bHolds = std::fabs(Value - Condition.Value) >= 1e-4f; break;
                case EAnimCompare::IsTrue:    bHolds = Value != 0.0f; break;
                case EAnimCompare::IsFalse:   bHolds = Value == 0.0f; break;
                case EAnimCompare::Triggered: bHolds = Value != 0.0f; break;
            }
            if(!bHolds)
            {
                return false;
            }
        }
        return true;
    }

    void AnimInstance::UpdateTransitions(LayerRuntime& InOutLayer)
    {
        if(InOutLayer.Current.State < 0)
        {
            return;
        }
        /* The first that holds, in order; one a layer an update. */
        for(const TransitionRuntime& Transition : InOutLayer.Transitions)
        {
            const bool bFrom = Transition.From == InOutLayer.Current.State
                            || (Transition.From < 0 && Transition.To != InOutLayer.Current.State);
            if(!bFrom || !ConditionsHold(Transition, InOutLayer))
            {
                continue;
            }
            for(const TransitionRuntime::Condition& Condition : Transition.Conditions)
            {
                if(Condition.Compare == EAnimCompare::Triggered)
                {
                    Parameters[size_t(Condition.Parameter)] = 0.0f;
                    Consumed.push_back(State->Parameters[size_t(Condition.Parameter)].Name);
                }
            }
            /* A one-shot left at its end (within the crossfade of it) has ended,
             * though its last moments play out under the fade. */
            const StateRuntime& Leaving = InOutLayer.States[size_t(InOutLayer.Current.State)];
            if(Transition.bAtEnd && !StateLoops(Leaving) && InOutLayer.Current.Phase < 1.0f)
            {
                Finished.push_back({ size_t(&InOutLayer - Layers.data()), Leaving.Name });
            }
            InOutLayer.Previous     = InOutLayer.Current;
            InOutLayer.Current      = Playhead();
            InOutLayer.Current.State = Transition.To;
            /* Within a sync group it goes on from where the state it leaves was. */
            const int32_t Group = InOutLayer.States[size_t(Transition.To)].SyncGroup;
            if(Group >= 0 && Group == Leaving.SyncGroup)
            {
                InOutLayer.Current.Phase   = InOutLayer.Previous.Phase;
                InOutLayer.Current.bSynced = InOutLayer.Previous.bSynced;
            }
            InOutLayer.FadeTime     = 0.0f;
            InOutLayer.FadeDuration = Transition.Duration;
            if(Transition.Duration <= 0.0f) { InOutLayer.Previous.State = -1; }
            return;
        }
    }

    // === Playback ==============================================================

    void AnimInstance::Update(const float InDeltaSeconds, const bool bInEvaluate)
    {
        Fired.clear();
        Finished.clear();
        Consumed.clear();
        RootMotion = AnimVec3();
        IKDelta = std::max(InDeltaSeconds, 0.0f);   // IK eases in real time, as the look-at does
        if(Skeleton)
        {
            const float Delta = InDeltaSeconds * PlayRate;
            LeaderPhase.assign(SyncGroups.size(), -1.0f);
            for(size_t Index = 0; Index < Layers.size(); ++Index)
            {
                LayerRuntime& Layer = Layers[Index];
                const bool bWasEnded = Layer.Current.State >= 0 && Layer.Current.Phase >= 1.0f;

                /* The first layer playing a state of a sync group leads it;
                 * a later one in a state of that group goes where it went. */
                const int32_t Group = Layer.Current.State >= 0 ? Layer.States[size_t(Layer.Current.State)].SyncGroup : -1;
                const float   Lead  = Group >= 0 ? LeaderPhase[size_t(Group)] : -1.0f;
                AnimVec3 Travel = AdvancePlayhead(Layer, Layer.Current, Delta, /*bInNotify*/ true, Lead);
                if(Group >= 0 && Lead < 0.0f) { LeaderPhase[size_t(Group)] = Layer.Current.Phase; }

                /* A one-shot reaching its end: once, as it gets there. */
                if(Layer.Current.State >= 0 && !bWasEnded && Layer.Current.Phase >= 1.0f
                   && !StateLoops(Layer.States[size_t(Layer.Current.State)]))
                {
                    Finished.push_back({ Index, Layer.States[size_t(Layer.Current.State)].Name });
                }
                if(Layer.Previous.State >= 0)
                {
                    const AnimVec3 Fading = AdvancePlayhead(Layer, Layer.Previous, Delta, /*bInNotify*/ false);
                    Layer.FadeTime += std::fabs(Delta);
                    const float Alpha = Layer.FadeDuration > 0.0f ? std::clamp(Layer.FadeTime / Layer.FadeDuration, 0.0f, 1.0f) : 1.0f;
                    Travel = AnimMath::Lerp(Fading, Travel, Alpha);
                    if(Layer.FadeTime >= Layer.FadeDuration) { Layer.Previous.State = -1; }
                }
                /* The first layer is the body's: its travel moves the entity. */
                if(Index == 0) { RootMotion = Travel; }
                UpdateTransitions(Layer);
            }

            /* The look-at eases toward its target, in real time: a play rate of 0 still looks. */
            if(LookParameter >= 0)
            {
                const float Target = LookTarget();
                if(!bLookPlaced || LookResponse <= 0.0f) { LookAngle = Target; }
                else { LookAngle += (Target - LookAngle) * (1.0f - std::exp(-LookResponse * std::max(InDeltaSeconds, 0.0f))); }
                bLookPlaced = true;
            }
        }
        if(bInEvaluate) { EvaluatePose(); }
    }

    void AnimInstance::Evaluate()
    {
        /* A change or a seek: the look-at, and the IK, go straight to their goals. */
        bIKPlaced = false;
        if(LookParameter >= 0)
        {
            LookAngle   = LookTarget();
            bLookPlaced = true;
        }
        EvaluatePose();
    }

    void AnimInstance::EvaluatePose()
    {
        if(!Skeleton)
        {
            LocalPose.clear();
            ModelPose.clear();
            return;
        }
        const size_t Bones = Skeleton->Bones.size();
        const bool bOverride = OverrideWeight > 0.0f && OverrideModel.size() == Bones && OverrideMask.size() == Bones;
        if(bOverride && OverrideWeight >= 1.0f && LocalPose.size() == Bones)
        {
            /* Held: physics has the pose, the rest keeps its last animated one. */
            ApplyPoseOverride();
            return;
        }
        GetRefPose(*Skeleton, LocalPose);
        for(const LayerRuntime& Layer : Layers)
        {
            if(Layer.Current.State < 0)
            {
                continue;
            }
            float Weight = Layer.Weight;
            if(Layer.WeightParameter >= 0 && size_t(Layer.WeightParameter) < Parameters.size())
            {
                Weight *= std::clamp(Parameters[size_t(Layer.WeightParameter)], 0.0f, 1.0f);
            }
            if(Weight <= 0.0f)
            {
                continue;
            }

            /* The layer's pose: its state, or a crossfade into it. */
            EvaluateState(Layer, Layer.States[size_t(Layer.Current.State)], Layer.Current.Phase, Scratch[0]);
            const AnimLocalPose* LayerPose = &Scratch[0];
            if(Layer.Previous.State >= 0 && Layer.FadeDuration > 0.0f)
            {
                EvaluateState(Layer, Layer.States[size_t(Layer.Previous.State)], Layer.Previous.Phase, Scratch[1]);
                BlendPoses(Scratch[1], Scratch[0], std::clamp(Layer.FadeTime / Layer.FadeDuration, 0.0f, 1.0f));
                LayerPose = &Scratch[1];
            }
            const std::vector<float>* Mask = Layer.Mask.empty() ? nullptr : &Layer.Mask;
            if(Layer.bAdditive) { ApplyAdditivePose(LocalPose, *LayerPose, Weight, Mask); }
            else                { BlendPoses(LocalPose, *LayerPose, Weight, Mask); }
        }
        if(Overlay && OverlayWeight > 0.0f)
        {
            if(OverlayBoundTo != Skeleton.get() || OverlayBoundVia != Retarget.get())
            {
                OverlayBinding  = MakeSample(Overlay, 0.0f, 0.0f);
                OverlayBoundTo  = Skeleton.get();
                OverlayBoundVia = Retarget.get();
            }
            GetRefPose(*Skeleton, Scratch[2]);
            SampleInto(OverlayBinding, OverlayTime, Scratch[2]);
            BlendPoses(LocalPose, Scratch[2], std::min(OverlayWeight, 1.0f));
        }
        ApplyLookAt(LocalPose);
        ApplyIK(LocalPose);
        if(bOverride)
        {
            ApplyPoseOverride();
            return;
        }
        LocalToModel(*Skeleton, LocalPose, ModelPose);
    }

    // === IK ====================================================================

    void AnimInstance::SetModelToWorld(const AnimMatrix& InModelToWorld)
    {
        ModelToWorld = InModelToWorld;
        WorldToModel = AnimMath::Inverse(InModelToWorld);
        bHasWorld    = true;
    }

    void AnimInstance::SetIKGround(const int32_t InChain, const bool bInHit, const AnimVec3& InWorldPoint, const AnimVec3& InWorldNormal)
    {
        if(InChain < 0)
        {
            bBaseGround = bInHit;
            BasePoint   = InWorldPoint;
            BaseNormal  = InWorldNormal;
            return;
        }
        if(size_t(InChain) >= IKChains.size()) { return; }
        IKChainRuntime& Chain = IKChains[size_t(InChain)];
        Chain.bGround      = bInHit;
        Chain.GroundPoint  = InWorldPoint;
        Chain.GroundNormal = InWorldNormal;
    }

    bool AnimInstance::SetIKGoal(const std::string& InChain, const AnimVec3& InWorldPosition, const float InWeight)
    {
        for(IKChainRuntime& Chain : IKChains)
        {
            if(Chain.Name != InChain) { continue; }
            Chain.bScript      = InWeight > 0.0f;
            Chain.ScriptGoal   = InWorldPosition;
            Chain.ScriptWeight = std::clamp(InWeight, 0.0f, 1.0f);
            return true;
        }
        return false;
    }

    void AnimInstance::ApplyIK(AnimLocalPose& InOutPose)
    {
        IKProbes.clear();
        if(IKChains.empty() || !Skeleton)
        {
            PelvisDrop = 0.0f;
            return;
        }
        namespace AM = AnimMath;
        const auto Direction = [](const AnimMatrix& InMatrix, const AnimVec3& InV)
        {
            return AnimVec3{ InMatrix.M[0][0] * InV.X + InMatrix.M[0][1] * InV.Y + InMatrix.M[0][2] * InV.Z,
                             InMatrix.M[1][0] * InV.X + InMatrix.M[1][1] * InV.Y + InMatrix.M[1][2] * InV.Z,
                             InMatrix.M[2][0] * InV.X + InMatrix.M[2][1] * InV.Y + InMatrix.M[2][2] * InV.Z };
        };
        const auto Position = [](const AnimMatrix& InModel) { return AnimVec3{ InModel.M[0][3], InModel.M[1][3], InModel.M[2][3] }; };
        /* The height of a world plane at (X, Z); false on a wall. */
        const auto HeightOn = [](const AnimVec3& InPoint, const AnimVec3& InNormal, const float InX, const float InZ, float& OutY)
        {
            if(std::fabs(InNormal.Y) < 0.2f) { return false; }
            OutY = InPoint.Y - (InNormal.X * (InX - InPoint.X) + InNormal.Z * (InZ - InPoint.Z)) / InNormal.Y;
            return true;
        };
        const float Ease = !bIKPlaced || IKResponse <= 0.0f ? 1.0f : 1.0f - std::exp(-IKResponse * IKDelta);
        bIKPlaced = true;

        /* The pose as animated, and model up (and how many model units a world one is). */
        LocalToModel(*Skeleton, InOutPose, IKModel);
        AnimVec3 Up{ 0.0f, 1.0f, 0.0f };
        float ModelPerWorld = 1.0f;
        if(bHasWorld)
        {
            const AnimVec3 WorldUp = Direction(WorldToModel, { 0.0f, 1.0f, 0.0f });
            ModelPerWorld = std::max(AM::Length(WorldUp), 1e-6f);
            Up = AM::Scale(WorldUp, 1.0f / ModelPerWorld);
        }
        const AnimVec3 Origin = bHasWorld ? AM::TransformPoint(ModelToWorld, AnimVec3()) : AnimVec3();
        float BaseY = Origin.Y;
        if(bHasWorld && bBaseGround) { HeightOn(BasePoint, BaseNormal, Origin.X, Origin.Z, BaseY); }

        /* Each chain's weight, its parameter's share eased in and out over about
         * a quarter second: a Bool (Armed) switching does not snap the limb. */
        const float WeightEase = Ease >= 1.0f ? 1.0f : 1.0f - std::exp(-IK_WEIGHT_RESPONSE * IKDelta);
        for(IKChainRuntime& Chain : IKChains)
        {
            float Weight = Chain.Weight;
            if(Chain.WeightParameter >= 0 && size_t(Chain.WeightParameter) < Parameters.size())
            {
                Weight *= std::clamp(Parameters[size_t(Chain.WeightParameter)], 0.0f, 1.0f);
            }
            Weight = std::clamp(Weight, 0.0f, 1.0f);
            Chain.Blend = Chain.Blend < 0.0f ? Weight : Chain.Blend + (Weight - Chain.Blend) * WeightEase;
        }
        const auto WeightOf = [](const IKChainRuntime& InChain) { return InChain.Blend; };

        /* Feet: each moved along up by how far its ground is above (or below)
         * the ground the entity stands on, eased; turned to its slope. */
        float Lowest = 0.0f;
        for(IKChainRuntime& Chain : IKChains)
        {
            if(Chain.Goal != EAnimIKGoal::Ground || Chain.bScript) { continue; }
            const AnimVec3 Foot = Position(IKModel[size_t(Chain.End)]);
            float Wanted = 0.0f;
            AnimQuat Tilt;
            if(bHasWorld && Chain.bGround)
            {
                const AnimVec3 FootWorld = AM::TransformPoint(ModelToWorld, Foot);
                float GroundY = 0.0f;
                if(HeightOn(Chain.GroundPoint, Chain.GroundNormal, FootWorld.X, FootWorld.Z, GroundY))
                {
                    Wanted = std::clamp(GroundY - BaseY, -IKMaxStep, IKMaxStep) * ModelPerWorld;
                    if(Chain.bAlignEnd)
                    {
                        /* Turned with the slope, by MaxTilt at most. */
                        Tilt = AM::FromTo(Up, Direction(WorldToModel, Chain.GroundNormal));
                        const float Angle = 2.0f * std::acos(std::min(std::fabs(Tilt.W), 1.0f)) * 180.0f / 3.14159265f;
                        if(Angle > IKMaxTilt) { Tilt = AM::Nlerp(AnimQuat(), Tilt, Angle > 1e-4f ? IKMaxTilt / Angle : 0.0f); }
                    }
                }
            }
            Chain.Offset += (Wanted - Chain.Offset) * Ease;
            Chain.Tilt    = AM::Nlerp(Chain.Tilt, Tilt, Ease);
            Lowest = std::min(Lowest, Chain.Offset * WeightOf(Chain));
        }

        /* Probes for the next evaluation: down from above each animated foot, and where the entity stands. */
        if(bHasWorld)
        {
            const AnimVec3 Above{ 0.0f, IKTraceUp, 0.0f };
            IKProbes.push_back({ -1, AM::Add(Origin, Above), IKTraceUp + IKTraceDown });
            for(size_t Index = 0; Index < IKChains.size(); ++Index)
            {
                if(IKChains[Index].Goal != EAnimIKGoal::Ground) { continue; }
                const AnimVec3 FootWorld = AM::TransformPoint(ModelToWorld, Position(IKModel[size_t(IKChains[Index].End)]));
                IKProbes.push_back({ int32_t(Index), AM::Add(FootWorld, Above), IKTraceUp + IKTraceDown });
            }
        }

        /* The pelvis lowered so the lowest foot reaches its ground (never raised: the legs could not follow). */
        PelvisDrop = std::min(-Lowest, IKMaxStep * ModelPerWorld);
        if(IKPelvis >= 0 && PelvisDrop > 1e-4f)
        {
            const int32_t Parent = Skeleton->Bones[size_t(IKPelvis)].Parent;
            const AnimVec3 Down = AM::Scale(Up, -PelvisDrop);
            const AnimVec3 Local = Parent >= 0 ? Direction(AM::Inverse(IKModel[size_t(Parent)]), Down) : Down;
            InOutPose[size_t(IKPelvis)].Translation = AM::Add(InOutPose[size_t(IKPelvis)].Translation, Local);
        }

        /* Each chain to its goal, in order (a hand on a bone another chain moved goes after it). */
        for(const IKChainRuntime& Chain : IKChains)
        {
            AnimTwoBoneIK Solve;
            Solve.Root   = Chain.Root;
            Solve.Mid    = Chain.Mid;
            Solve.End    = Chain.End;
            Solve.Pole   = Chain.Pole;
            Solve.Weight = WeightOf(Chain);
            if(Chain.bScript)
            {
                if(!bHasWorld) { continue; }
                Solve.Target  = AM::TransformPoint(WorldToModel, Chain.ScriptGoal);
                Solve.Weight *= Chain.ScriptWeight;
            }
            else if(Chain.Goal == EAnimIKGoal::Ground)
            {
                /* On level ground, with the pelvis where it was: nothing to do. */
                if(std::fabs(Chain.Offset) < 1e-3f && PelvisDrop <= 1e-4f && std::fabs(Chain.Tilt.W) > 0.999999f) { continue; }
                const AnimMatrix& Foot = IKModel[size_t(Chain.End)];
                Solve.Target    = AM::Add(Position(Foot), AM::Scale(Up, Chain.Offset));
                Solve.bRotation = Chain.bAlignEnd;
                Solve.Rotation  = AM::Normalize(AM::Mul(Chain.Tilt, AM::ToTransform(Foot).Rotation));
            }
            else if(Chain.Goal == EAnimIKGoal::Bone)
            {
                const AnimMatrix Goal = AM::Mul(ComputeBoneModel(*Skeleton, InOutPose, Chain.GoalBone), AM::ToMatrix(Chain.GoalOffset));
                Solve.Target    = Position(Goal);
                Solve.bRotation = Chain.bAlignEnd;
                Solve.Rotation  = AM::ToTransform(Goal).Rotation;
            }
            else
            {
                continue;   // a Script chain no goal is set for
            }
            SolveTwoBoneIK(*Skeleton, InOutPose, Solve);
        }
    }

    void AnimInstance::ApplyPoseOverride()
    {
        LocalToModel(*Skeleton, LocalPose, OverrideAnimated);
        const size_t Bones  = Skeleton->Bones.size();
        const float  Weight = std::min(OverrideWeight, 1.0f);
        ModelPose.resize(Bones);
        for(size_t Bone = 0; Bone < Bones; ++Bone)
        {
            const int32_t Parent = Skeleton->Bones[Bone].Parent;
            if(!OverrideMask[Bone])
            {
                /* Its animated self, under wherever its parent now is. */
                ModelPose[Bone] = Parent >= 0 ? AnimMath::Mul(ModelPose[size_t(Parent)], AnimMath::ToMatrix(LocalPose[Bone]))
                                              : OverrideAnimated[Bone];
                continue;
            }
            const AnimTransform Animated = AnimMath::ToTransform(OverrideAnimated[Bone]);
            AnimTransform Placed = AnimMath::ToTransform(OverrideModel[Bone]);
            Placed.Scale = Animated.Scale;
            ModelPose[Bone] = AnimMath::ToMatrix(Weight >= 1.0f ? Placed : AnimMath::Lerp(Animated, Placed, Weight));
        }
    }

    void AnimInstance::SetPoseOverride(const std::vector<AnimMatrix>& InModel, const std::vector<uint8_t>& InMask, const float InWeight)
    {
        if(InWeight <= 0.0f || InModel.empty() || InModel.size() != InMask.size()
           || (Skeleton && InModel.size() != Skeleton->Bones.size()))
        {
            ClearPoseOverride();
            return;
        }
        OverrideModel  = InModel;
        OverrideMask   = InMask;
        OverrideWeight = InWeight;
    }

    void AnimInstance::ClearPoseOverride()
    {
        OverrideModel.clear();
        OverrideMask.clear();
        OverrideWeight = 0.0f;
    }

    void AnimInstance::SetOverlayClip(std::shared_ptr<const AnimClipData> InClip, const float InSeconds, const float InWeight)
    {
        if(!InClip || InWeight <= 0.0f)
        {
            Overlay.reset();
            OverlayBoundTo = nullptr;
            OverlayWeight  = 0.0f;
            return;
        }
        if(InClip != Overlay)
        {
            Overlay        = std::move(InClip);
            OverlayBoundTo = nullptr;
        }
        OverlayTime   = std::clamp(InSeconds, 0.0f, Overlay->GetDuration());
        OverlayWeight = InWeight;
    }

    // === Where it is ===========================================================

    float AnimInstance::GetTime() const
    {
        return GetDuration() > 0.0f ? Layers[0].Current.Phase * GetDuration() : 0.0f;
    }

    float AnimInstance::GetDuration() const
    {
        if(Layers.empty() || Layers[0].Current.State < 0)
        {
            return 0.0f;
        }
        const StateRuntime& Current = Layers[0].States[size_t(Layers[0].Current.State)];
        std::vector<float> Weights;
        size_t Dominant = 0;
        SampleWeights(Current, Weights, Dominant);
        return Dominant < Current.Samples.size() && Current.Samples[Dominant].Clip ? Current.Samples[Dominant].Clip->GetDuration() : 0.0f;
    }

    void AnimInstance::SetTime(const float InSeconds)
    {
        const float Duration = GetDuration();
        if(Duration > 0.0f)
        {
            Layers[0].Current.Phase    = std::clamp(InSeconds / Duration, 0.0f, 1.0f);
            Layers[0].Previous.State   = -1;
        }
        Evaluate();
    }

    const char* AnimInstance::GetCurrentStateName(const size_t InLayer) const
    {
        const int32_t Current = GetCurrentState(InLayer);
        return Current >= 0 ? Layers[InLayer].States[size_t(Current)].Name.c_str() : "";
    }

    int32_t AnimInstance::GetCurrentState(const size_t InLayer) const
    {
        return InLayer < Layers.size() ? Layers[InLayer].Current.State : -1;
    }

    bool AnimInstance::IsInTransition(const size_t InLayer) const
    {
        return InLayer < Layers.size() && Layers[InLayer].Previous.State >= 0;
    }
}
