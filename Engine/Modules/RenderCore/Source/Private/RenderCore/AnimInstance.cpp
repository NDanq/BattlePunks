#include "Public/RenderCore/AnimInstance.h"

#include <algorithm>
#include <cmath>

namespace RenderCore
{
    AnimInstance::AnimInstance()  = default;
    AnimInstance::~AnimInstance() = default;

    // === What it plays =========================================================

    void AnimInstance::SetSkeleton(std::shared_ptr<const SkeletonData> InSkeleton)
    {
        Skeleton = std::move(InSkeleton);
        Rebuild();
        Restart();
        Evaluate();
    }

    void AnimInstance::PlayClip(std::shared_ptr<const AnimClipData> InClip)
    {
        Clip  = std::move(InClip);
        State = nullptr;
        Rebuild();
        Restart();
        Evaluate();
    }

    void AnimInstance::SetState(std::shared_ptr<const AnimStateData> InState)
    {
        State = std::move(InState);
        Clip  = nullptr;
        Rebuild();
        Restart();
        Evaluate();
    }

    void AnimInstance::SetLoopOverride(const int InLoop)
    {
        LoopOverride = InLoop;
        if(!State && !Layers.empty() && !Layers[0].States.empty())
        {
            Layers[0].States[0].Loop = InLoop < 0 ? EAnimLoop::Clip : InLoop == 0 ? EAnimLoop::Once : EAnimLoop::Loop;
        }
    }

    void AnimInstance::Rebuild()
    {
        Layers.clear();
        Parameters.clear();
        RefPose.clear();
        if(Skeleton) { GetRefPose(*Skeleton, RefPose); }

        const auto MakeSample = [this](std::shared_ptr<const AnimClipData> InClip, const float InPosition, const float InPositionY)
        {
            Sample Out;
            Out.Clip      = std::move(InClip);
            Out.Position  = InPosition;
            Out.PositionY = InPositionY;
            if(Out.Clip && Skeleton)
            {
                BindClip(*Out.Clip, *Skeleton, Out.Binding);
                if(Out.Clip->RootMotion != EAnimRootMotion::Keep)
                {
                    Out.RootBone  = Skeleton->FindBone(Out.Clip->RootBone);
                    Out.RootTrack = Out.Clip->FindTrack(Out.Clip->RootBone);
                }
            }
            return Out;
        };

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

    void AnimInstance::HoldRoot(const Sample& InSample, AnimLocalPose& InOutPose) const
    {
        if(InSample.RootBone < 0 || InSample.RootTrack < 0 || size_t(InSample.RootBone) >= InOutPose.size()) { return; }
        const std::vector<AnimVec3>& Keys = InSample.Clip->Tracks[size_t(InSample.RootTrack)].Translations;
        if(Keys.empty()) { return; }
        const AnimMatrix Parent = ParentModel(InOutPose, InSample.RootBone);
        AnimVec3& Local = InOutPose[size_t(InSample.RootBone)].Translation;
        const AnimVec3 Now   = AnimMath::TransformPoint(Parent, Local);
        const AnimVec3 First = AnimMath::TransformPoint(Parent, Keys.front());
        Local = AnimMath::TransformPoint(AnimMath::Inverse(Parent), { First.X, Now.Y, First.Z });
    }

    AnimVec3 AnimInstance::AdvancePlayhead(const LayerRuntime& InLayer, Playhead& InOutHead, const float InDelta, const bool bInNotify)
    {
        InOutHead.bEnded = false;
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
        if(StateLoops(State))
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
        if(bInNotify && To != From && InDelta * State.Speed > 0.0f)
        {
            if(Dominant < State.Samples.size() && State.Samples[Dominant].Clip)
            {
                const AnimClipData& Notifying = *State.Samples[Dominant].Clip;
                const float Length = Notifying.GetDuration();
                CollectNotifies(Notifying, From * Length, To * Length, bWrap, Fired);
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
            const AnimVec3 Local = Each.Clip->GetRootTravel(From * Length, To * Length, bWrap);
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
            SampleClip(*InSample.Clip, InSample.Binding, InTime, OutSample);
            if(!InLayer.bAdditive)
            {
                HoldRoot(InSample, OutSample);
                return;
            }
            GetRefPose(*Skeleton, Scratch[3]);
            if(InLayer.AdditiveBase == EAnimAdditiveBase::FirstFrame)
            {
                SampleClip(*InSample.Clip, InSample.Binding, 0.0f, Scratch[3]);
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
        if(Skeleton)
        {
            const float Delta = InDeltaSeconds * PlayRate;
            for(size_t Index = 0; Index < Layers.size(); ++Index)
            {
                LayerRuntime& Layer = Layers[Index];
                const bool bWasEnded = Layer.Current.State >= 0 && Layer.Current.Phase >= 1.0f;
                AnimVec3 Travel = AdvancePlayhead(Layer, Layer.Current, Delta, /*bInNotify*/ true);

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
        }
        if(bInEvaluate) { Evaluate(); }
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
        LocalToModel(*Skeleton, LocalPose, ModelPose);
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
