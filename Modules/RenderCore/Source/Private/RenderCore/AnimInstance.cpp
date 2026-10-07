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

        const auto MakeSample = [this](std::shared_ptr<const AnimClipData> InClip, const float InPosition)
        {
            Sample Out;
            Out.Clip     = std::move(InClip);
            Out.Position = InPosition;
            if(Out.Clip && Skeleton) { BindClip(*Out.Clip, *Skeleton, Out.Binding); }
            return Out;
        };

        if(!State)
        {
            /* A lone clip: one layer of one state. */
            LayerRuntime Layer;
            StateRuntime Only;
            if(Clip) { Only.Samples.push_back(MakeSample(Clip, 0.0f)); }
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
                Runtime.Speed          = Node.Speed;
                Runtime.Loop           = Node.Loop;
                for(const AnimBlendSample& Each : Node.Samples)
                {
                    Runtime.Samples.push_back(MakeSample(AnimClipAssetCache::Find(Each.Clip), Each.Position));
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
        /* A 1D blend space: the two samples either side of the parameter, by distance. */
        const float X = Parameters[size_t(InState.BlendParameter)];
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

    void AnimInstance::AdvancePlayhead(const LayerRuntime& InLayer, Playhead& InOutHead, const float InDelta, const bool bInNotify)
    {
        InOutHead.bEnded = false;
        if(InOutHead.State < 0 || size_t(InOutHead.State) >= InLayer.States.size())
        {
            return;
        }
        const StateRuntime& State = InLayer.States[size_t(InOutHead.State)];
        const float Duration = StateDuration(State);
        if(Duration <= 0.0f)
        {
            InOutHead.bEnded = true;   // nothing to play: as good as finished
            return;
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

        /* The dominant clip's notifies, moving forward. */
        if(bInNotify && To != From && InDelta * State.Speed > 0.0f)
        {
            std::vector<float> Weights;
            size_t Dominant = 0;
            SampleWeights(State, Weights, Dominant);
            if(Dominant < State.Samples.size() && State.Samples[Dominant].Clip)
            {
                const AnimClipData& Notifying = *State.Samples[Dominant].Clip;
                const float Length = Notifying.GetDuration();
                CollectNotifies(Notifying, From * Length, To * Length, bWrap, Fired);
            }
        }
    }

    void AnimInstance::EvaluateState(const StateRuntime& InState, const float InPhase, AnimLocalPose& OutPose)
    {
        GetRefPose(*Skeleton, OutPose);
        std::vector<float> Weights;
        size_t Dominant = 0;
        SampleWeights(InState, Weights, Dominant);

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
                SampleClip(*Each.Clip, Each.Binding, Time, OutPose);
                Total = Weights[Index];
                continue;
            }
            GetRefPose(*Skeleton, Scratch[2]);
            SampleClip(*Each.Clip, Each.Binding, Time, Scratch[2]);
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

    void AnimInstance::Update(const float InDeltaSeconds)
    {
        Fired.clear();
        Consumed.clear();
        if(Skeleton)
        {
            const float Delta = InDeltaSeconds * PlayRate;
            for(LayerRuntime& Layer : Layers)
            {
                AdvancePlayhead(Layer, Layer.Current, Delta, /*bInNotify*/ true);
                if(Layer.Previous.State >= 0)
                {
                    AdvancePlayhead(Layer, Layer.Previous, Delta, /*bInNotify*/ false);
                    Layer.FadeTime += std::fabs(Delta);
                    if(Layer.FadeTime >= Layer.FadeDuration) { Layer.Previous.State = -1; }
                }
                UpdateTransitions(Layer);
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
            EvaluateState(Layer.States[size_t(Layer.Current.State)], Layer.Current.Phase, Scratch[0]);
            const AnimLocalPose* LayerPose = &Scratch[0];
            if(Layer.Previous.State >= 0 && Layer.FadeDuration > 0.0f)
            {
                EvaluateState(Layer.States[size_t(Layer.Previous.State)], Layer.Previous.Phase, Scratch[1]);
                BlendPoses(Scratch[1], Scratch[0], std::clamp(Layer.FadeTime / Layer.FadeDuration, 0.0f, 1.0f));
                LayerPose = &Scratch[1];
            }
            BlendPoses(LocalPose, *LayerPose, Weight, Layer.Mask.empty() ? nullptr : &Layer.Mask);
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
