#include <algorithm>
#include <cmath>
#include <cstring>

#include "Private/AICore/BTInternal.h"
#include "Public/AICore/BehaviorTree.h"

/*
 * Utility scoring: the UtilitySelector composite and its considerations'
 * response curves (Dave Mark's Infinite Axis Utility System, as in
 * Behavioral Mathematics for Game AI; Mike Lewis, "Choosing Effective
 * Utility-Based Considerations", Game AI Pro 3).
 *
 * A child's score is the product of its considerations -- each an input
 * normalised to 0..1, shaped by its curve, raised to its weight -- with
 * Mark's compensation factor so a child with many considerations is not
 * punished for having them, times the child's Weight. A child whose
 * condition decorators fail scores 0. The selector runs the best; while it
 * runs, it re-scores every Interval seconds and switches when another child
 * beats the running one by more than Hysteresis (so it does not dither). A
 * child that fails is set aside until the selector is entered again.
 *
 * Params: Interval (0.3 s; 0 scores only on entering), Hysteresis (0.15),
 * Pick (Best, or Weighted: a weighted draw among the TopN best).
 */
namespace AICore
{
    float BTConsideration::Evaluate(const float InX) const
    {
        const float X = std::clamp(InX, 0.0f, 1.0f);
        float Y = 0.0f;
        switch(Curve)
        {
            case EBTCurve::Linear:        Y = Slope * (X - XShift) + YShift; break;
            case EBTCurve::InverseLinear: Y = 1.0f - X; break;
            case EBTCurve::Quadratic:     Y = Slope * std::pow(std::max(X - XShift, 0.0f), Exponent) + YShift; break;
            case EBTCurve::Logistic:      Y = 1.0f / (1.0f + std::exp(-Slope * (X - XShift))) + YShift; break;
            case EBTCurve::Step:          Y = X >= XShift ? 1.0f : 0.0f; break;
            case EBTCurve::Constant:      Y = YShift; break;
            case EBTCurve::Points:
            {
                if(Points.size() < 2) { Y = X; break; }
                const size_t Count = Points.size() / 2;
                if(X <= Points[0]) { Y = Points[1]; break; }
                Y = Points[(Count - 1) * 2 + 1];
                for(size_t Index = 1; Index < Count; ++Index)
                {
                    const float X0 = Points[(Index - 1) * 2], Y0 = Points[(Index - 1) * 2 + 1];
                    const float X1 = Points[Index * 2],       Y1 = Points[Index * 2 + 1];
                    if(X <= X1)
                    {
                        const float T = X1 > X0 ? (X - X0) / (X1 - X0) : 1.0f;
                        Y = Y0 + (Y1 - Y0) * T;
                        break;
                    }
                }
                break;
            }
            default: Y = X; break;
        }
        return std::clamp(Y, 0.0f, 1.0f);
    }

    namespace BTDetail
    {
        namespace
        {
            float Input(BTInstance& InInstance, const BTConsideration& InConsideration)
            {
                if(InConsideration.Key < 0) { return InConsideration.Max; }
                if(InConsideration.bDistance)
                {
                    EngineCore::Vector3 Where;
                    /* Nothing to measure to is as far as it gets. */
                    if(!ResolveKeyPosition(InInstance, InConsideration.Key, Where)) { return InConsideration.Max; }
                    const EngineCore::Vector3 Self = SelfPosition(InInstance);
                    const double DX = double(Where.GetX()) - Self.GetX(), DY = double(Where.GetY()) - Self.GetY(), DZ = double(Where.GetZ()) - Self.GetZ();
                    return float(std::sqrt(DX * DX + DY * DY + DZ * DZ));
                }
                return float(InInstance.GetBlackboard().GetValue(InConsideration.Key).AsNumber());
            }

            float Score(BTInstance& InInstance, const uint32_t InChild, const float InDeltaSeconds)
            {
                const BTNodeDef& Child = InInstance.GetAsset().Nodes[InChild];
                if(!InInstance.ConditionsPass(InChild, InDeltaSeconds)) { return 0.0f; }
                float Total = 1.0f;
                const size_t Count = Child.Considerations.size();
                const float Modification = Count > 0 ? 1.0f - 1.0f / float(Count) : 0.0f;
                for(const BTConsideration& Consideration : Child.Considerations)
                {
                    const float Range = Consideration.Max - Consideration.Min;
                    const float X = Range != 0.0f ? (Input(InInstance, Consideration) - Consideration.Min) / Range : 0.0f;
                    float S = Consideration.Evaluate(X);
                    if(Consideration.Weight != 1.0f) { S = std::pow(S, std::max(Consideration.Weight, 0.0f)); }
                    /* Mark's compensation: what a factor took away, given partly back. */
                    const float MakeUp = (1.0f - S) * Modification;
                    S = S + MakeUp * S;
                    Total *= S;
                    if(Total <= 0.0f) { break; }
                }
                return std::max(Total * std::max(Child.Weight, 0.0f), 0.0f);
            }

            /* The child to run among those not set aside; BT_NO_NODE if none scores. */
            uint32_t Choose(BTInstance& InInstance, const uint32_t InNode, const BTNodeMemory& InMemory, const float InDeltaSeconds,
                            const bool bInWeighted, const int InTopN)
            {
                const BTNodeDef& Node = InInstance.GetAsset().Nodes[InNode];
                struct Candidate { uint32_t Index; float Score; };
                Candidate Ranked[32];
                int Count = 0;
                for(uint32_t Child = 0; Child < Node.Children.size(); ++Child)
                {
                    const float S = Score(InInstance, Node.Children[Child], InDeltaSeconds);
                    InInstance.SetUtilityScore(Node.Children[Child], S);
                    if((InMemory.Mask & (1u << Child)) || S <= 0.0f) { continue; }
                    Ranked[Count++] = { Child, S };
                }
                if(Count == 0) { return BT_NO_NODE; }
                std::sort(Ranked, Ranked + Count, [](const Candidate& InA, const Candidate& InB)
                {
                    return InA.Score > InB.Score || (InA.Score == InB.Score && InA.Index < InB.Index);
                });
                if(!bInWeighted || Count == 1) { return Ranked[0].Index; }
                const int Top = std::clamp(InTopN, 1, Count);
                float Sum = 0.0f;
                for(int Index = 0; Index < Top; ++Index) { Sum += Ranked[Index].Score; }
                float Roll = InInstance.NextRandom() * Sum;
                for(int Index = 0; Index < Top; ++Index)
                {
                    Roll -= Ranked[Index].Score;
                    if(Roll <= 0.0f) { return Ranked[Index].Index; }
                }
                return Ranked[0].Index;
            }
        }

        EBTStatus TickUtilitySelector(BTInstance& InInstance, const uint32_t InNode, const float InDeltaSeconds)
        {
            const BTNodeDef& Node = InInstance.GetAsset().Nodes[InNode];
            BTNodeMemory& Mem = InInstance.GetMutableNodeMemory(InNode);
            BTContext Context = InInstance.MakeContext(InNode, Node.Params, Mem, InDeltaSeconds);
            const double Interval   = Context.GetNumber("Interval", 0.3);
            const float  Hysteresis = float(Context.GetNumber("Hysteresis", 0.15));
            const bool   bWeighted  = std::strcmp(Context.GetString("Pick", "Best"), "Weighted") == 0;
            const int    TopN       = int(Context.GetNumber("TopN", 2.0));
            const double Now = InInstance.GetClock();

            if(!(Mem.Flags & 1))
            {
                Mem.Flags |= 1;
                const uint32_t Chosen = Choose(InInstance, InNode, Mem, InDeltaSeconds, bWeighted, TopN);
                if(Chosen == BT_NO_NODE) { return EBTStatus::Failure; }
                Mem.Child = Chosen;
                Mem.Time = Now + Interval;
            }
            else if(Interval > 0.0 && Now >= Mem.Time)
            {
                Mem.Time = Now + Interval;
                const uint32_t Current = Mem.Child;
                /* The running child is scored like the rest, and must lose
                 * by more than the hysteresis. */
                const uint32_t Best = Choose(InInstance, InNode, Mem, InDeltaSeconds, false, 1);
                if(Best != BT_NO_NODE && Best != Current)
                {
                    const float CurrentScore = InInstance.GetUtilityScore(Node.Children[Current]);
                    const float BestScore    = InInstance.GetUtilityScore(Node.Children[Best]);
                    if(BestScore > CurrentScore * (1.0f + Hysteresis) + 0.01f)
                    {
                        InInstance.AbortNode(Node.Children[Current]);
                        Mem.Child = Best;
                    }
                }
            }

            const EBTStatus Status = InInstance.TickNode(Node.Children[Mem.Child], InDeltaSeconds);
            if(Status != EBTStatus::Failure) { return Status; }

            /* It failed: set aside, and the next best runs from the next tick. */
            Mem.Mask |= 1u << Mem.Child;
            const uint32_t Next = Choose(InInstance, InNode, Mem, InDeltaSeconds, bWeighted, TopN);
            if(Next == BT_NO_NODE) { return EBTStatus::Failure; }
            Mem.Child = Next;
            return EBTStatus::Running;
        }
    }
}
