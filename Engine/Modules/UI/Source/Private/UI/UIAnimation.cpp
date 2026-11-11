#include "Public/UI/UIAnimation.h"

#include <algorithm>
#include <cmath>

#include "Public/UI/UIDocument.h"
#include "Public/UI/UILog.h"
#include "Public/UI/UIProperty.h"
#include "Public/UI/UIVisualState.h"

namespace UI
{
    namespace
    {
        const char* const EASING_NAMES[] = {
            "Linear", "QuadIn", "QuadOut", "QuadInOut", "CubicIn", "CubicOut", "CubicInOut", "BackOut", "ElasticOut", "Step"
        };
        constexpr float PI = 3.14159265358979f;

        bool IsAnimatableType(const EUIPropertyType InType)
        {
            return InType == EUIPropertyType::Float || InType == EUIPropertyType::Color || InType == EUIPropertyType::Vec2;
        }

        /* InA to InB by InT, component by component; InA's type. No allocation. */
        UIValue Lerp(const UIValue& InA, const UIValue& InB, const float InT)
        {
            UIValue Out;
            Out.Type = InA.Type;
            for(int At = 0; At < 4; ++At) { Out.F[At] = InA.F[At] + (InB.F[At] - InA.F[At]) * InT; }
            return Out;
        }
    }

    float Ease(const EUIEasing InEasing, float InT)
    {
        InT = std::clamp(InT, 0.0f, 1.0f);
        switch(InEasing)
        {
            case EUIEasing::Linear:     return InT;
            case EUIEasing::QuadIn:     return InT * InT;
            case EUIEasing::QuadOut:    return 1.0f - (1.0f - InT) * (1.0f - InT);
            case EUIEasing::QuadInOut:  return InT < 0.5f ? 2.0f * InT * InT : 1.0f - std::pow(-2.0f * InT + 2.0f, 2.0f) * 0.5f;
            case EUIEasing::CubicIn:    return InT * InT * InT;
            case EUIEasing::CubicOut:   return 1.0f - std::pow(1.0f - InT, 3.0f);
            case EUIEasing::CubicInOut: return InT < 0.5f ? 4.0f * InT * InT * InT : 1.0f - std::pow(-2.0f * InT + 2.0f, 3.0f) * 0.5f;
            case EUIEasing::BackOut:
            {
                const float C1 = 1.70158f, C3 = C1 + 1.0f;
                return 1.0f + C3 * std::pow(InT - 1.0f, 3.0f) + C1 * std::pow(InT - 1.0f, 2.0f);
            }
            case EUIEasing::ElasticOut:
                if(InT <= 0.0f || InT >= 1.0f) return InT;
                return std::pow(2.0f, -10.0f * InT) * std::sin((InT * 10.0f - 0.75f) * (2.0f * PI / 3.0f)) + 1.0f;
            case EUIEasing::Step:       return InT < 1.0f ? 0.0f : 1.0f;
        }
        return InT;
    }

    EUIEasing ParseEasing(const std::string& InName)
    {
        for(size_t At = 0; At < sizeof(EASING_NAMES) / sizeof(EASING_NAMES[0]); ++At)
        {
            if(InName == EASING_NAMES[At]) return EUIEasing(At);
        }
        return EUIEasing::Linear;
    }

    const char* GetEasingName(const EUIEasing InEasing)
    {
        const size_t Index = size_t(InEasing);
        return Index < sizeof(EASING_NAMES) / sizeof(EASING_NAMES[0]) ? EASING_NAMES[Index] : "Linear";
    }

    float UIAnimationAsset::GetLength() const
    {
        if(Duration > 0.0f) return Duration;
        float Last = 0.0f;
        for(const UIAnimationTrack& Track : Tracks)
        {
            for(const UIAnimationKey& Key : Track.Keys) { Last = std::max(Last, Key.Time); }
        }
        return Last;
    }

    bool ClassListHas(const std::string& InClassList, const std::string& InClass)
    {
        if(InClass.empty()) return false;
        size_t At = 0;
        while(At < InClassList.size())
        {
            while(At < InClassList.size() && InClassList[At] == ' ') ++At;
            size_t End = At;
            while(End < InClassList.size() && InClassList[End] != ' ') ++End;
            if(End - At == InClass.size() && InClassList.compare(At, End - At, InClass) == 0) return true;
            At = End;
        }
        return false;
    }

    // === UIAnimator =================================================================

    UIAnimationHandle UIAnimator::Play(const UIAnimationAsset& InAnimation, const UIPlayOptions& InOptions)
    {
        if(!Document) return UI_ANIMATION_INVALID;

        Instance Made;
        Made.Name       = InAnimation.Name;
        Made.OnFinished = &InAnimation.OnFinished;
        Made.Length     = InAnimation.GetLength();
        Made.Speed      = InOptions.Speed > 0.0f ? InOptions.Speed : 1.0f;
        Made.bLoop      = InAnimation.bLoop;
        Made.bReverse   = InOptions.bReverse;
        Made.Time       = std::clamp(InOptions.StartTime, 0.0f, Made.Length);

        /* Each track, for each widget it names: its property found and its
         * keys parsed now, so a tick only eases between numbers. */
        for(const UIAnimationTrack& Authored : InAnimation.Tracks)
        {
            if(Authored.Keys.empty()) continue;
            for(uint32_t At = 0; At < Document->GetWidgetCount(); ++At)
            {
                UIWidget& W = *Document->GetWidget(At);
                const bool bNamed = !Authored.Target.empty() && W.Name == Authored.Target;
                if(!bNamed && !ClassListHas(W.Class, Authored.Class)) continue;

                const int32_t Property = W.GetType()->FindProperty(Authored.Property.c_str());
                const UIPropertyDesc* Desc = Property >= 0 ? &W.GetType()->Properties[size_t(Property)] : nullptr;
                if(!Desc || !IsAnimatableType(Desc->Type))
                {
                    UI_LOG_WARN("'" + Document->GetPath() + "': animation '" + InAnimation.Name + "' cannot drive "
                                + Authored.Property + " of '" + W.Name + "' (" + W.GetType()->Name + ")")
                    continue;
                }

                Track Bound;
                Bound.Widget   = At;
                Bound.Property = Property;
                Bound.Easing   = Authored.Easing;
                const UIValue Now = Desc->Get(W);
                for(const UIAnimationKey& Key : Authored.Keys)
                {
                    UIValue Value;
                    if(Key.Value == "*")                                  { Value = Now; }
                    else if(!ParsePropertyValue(*Desc, Key.Value, Value)) { continue; }
                    Bound.Keys.push_back({ Key.Time, Value });
                }
                if(Bound.Keys.empty()) continue;
                std::stable_sort(Bound.Keys.begin(), Bound.Keys.end(), [](const Key& InA, const Key& InB) { return InA.Time < InB.Time; });
                Made.Tracks.push_back(std::move(Bound));
            }
        }
        if(Made.Tracks.empty()) return UI_ANIMATION_INVALID;

        /* Playing it again starts it over. */
        if(!Made.Name.empty())
        {
            Playing.erase(std::remove_if(Playing.begin(), Playing.end(), [&](const Instance& InEach) { return InEach.Name == Made.Name; }),
                          Playing.end());
        }
        return Start(std::move(Made));
    }

    UIAnimationHandle UIAnimator::PlayTransition(const std::vector<UITransitionTarget>& InTargets, const float InDuration, const EUIEasing InEasing)
    {
        if(!Document || InTargets.empty()) return UI_ANIMATION_INVALID;

        Instance Made;
        Made.Length = std::max(InDuration, 0.0f);
        for(const UITransitionTarget& Target : InTargets)
        {
            UIWidget* W = Document->GetWidget(Target.Widget);
            if(!W || Target.Property < 0 || size_t(Target.Property) >= W->GetType()->Properties.size()) continue;
            const UIPropertyDesc& Desc = W->GetType()->Properties[size_t(Target.Property)];
            Track Bound;
            Bound.Widget   = Target.Widget;
            Bound.Property = Target.Property;
            Bound.Easing   = InEasing;
            Bound.Keys.push_back({ 0.0f, Desc.Get(*W) });
            Bound.Keys.push_back({ Made.Length, Target.To });
            Made.Tracks.push_back(std::move(Bound));
        }
        if(Made.Tracks.empty()) return UI_ANIMATION_INVALID;
        return Start(std::move(Made));
    }

    UIAnimationHandle UIAnimator::Start(Instance&& InInstance)
    {
        TakeOver(InInstance);
        InInstance.Handle = NextHandle++;
        if(NextHandle == UI_ANIMATION_INVALID) ++NextHandle;
        const UIAnimationHandle Handle = InInstance.Handle;

        /* Its first frame now, so nothing shows a frame of what it was. */
        Apply(InInstance);
        if(InInstance.Length <= 0.0f && !InInstance.bLoop)
        {
            /* At once: shown, and over. A named one still finishes on the next tick. */
            if(InInstance.Name.empty()) return Handle;
        }
        Playing.push_back(std::move(InInstance));
        return Handle;
    }

    void UIAnimator::TakeOver(const Instance& InNew)
    {
        for(size_t At = 0; At < Playing.size();)
        {
            std::vector<Track>& Tracks = Playing[At].Tracks;
            Tracks.erase(std::remove_if(Tracks.begin(), Tracks.end(), [&](const Track& InOld)
            {
                for(const Track& New : InNew.Tracks)
                {
                    if(New.Widget == InOld.Widget && New.Property == InOld.Property) return true;
                }
                return false;
            }), Tracks.end());
            /* Nothing left to drive: gone, quietly. */
            if(Tracks.empty()) { Playing.erase(Playing.begin() + std::ptrdiff_t(At)); }
            else               { ++At; }
        }
    }

    void UIAnimator::Apply(const Instance& InInstance)
    {
        const float Time = InInstance.bReverse ? InInstance.Length - InInstance.Time : InInstance.Time;
        for(const Track& T : InInstance.Tracks)
        {
            const std::vector<Key>& Keys = T.Keys;
            UIValue Value;
            /* At or past the last key first: a transition of no length is its end. */
            if(Time >= Keys.back().Time)       { Value = Keys.back().Value; }
            else if(Time <= Keys.front().Time) { Value = Keys.front().Value; }
            else
            {
                size_t Next = 1;
                while(Next < Keys.size() && Keys[Next].Time < Time) ++Next;
                const Key& A = Keys[Next - 1];
                const Key& B = Keys[Next];
                const float Span = B.Time - A.Time;
                const float T01  = Span > 0.0f ? (Time - A.Time) / Span : 1.0f;
                Value = Lerp(A.Value, B.Value, Ease(T.Easing, T01));
            }
            Document->ApplyAnimatedValue(T.Widget, T.Property, Value);
        }
    }

    void UIAnimator::Stop(const UIAnimationHandle InHandle)
    {
        Playing.erase(std::remove_if(Playing.begin(), Playing.end(), [&](const Instance& InEach) { return InEach.Handle == InHandle; }),
                      Playing.end());
    }

    void UIAnimator::StopAll()
    {
        Playing.clear();
    }

    bool UIAnimator::IsPlaying(const UIAnimationHandle InHandle) const
    {
        if(InHandle == UI_ANIMATION_INVALID) return false;
        for(const Instance& Each : Playing)
        {
            if(Each.Handle == InHandle) return true;
        }
        return false;
    }

    UIAnimationHandle UIAnimator::Find(const std::string& InName) const
    {
        if(InName.empty()) return UI_ANIMATION_INVALID;
        for(size_t At = Playing.size(); At-- > 0;)
        {
            if(Playing[At].Name == InName) return Playing[At].Handle;
        }
        return UI_ANIMATION_INVALID;
    }

    void UIAnimator::Tick(const float InDeltaSeconds)
    {
        FinishedThisTick.clear();
        if(Playing.empty() || !Document) return;

        for(size_t At = 0; At < Playing.size();)
        {
            Instance& Each = Playing[At];
            Each.Time += std::max(InDeltaSeconds, 0.0f) * Each.Speed;
            bool bDone = false;
            if(Each.Time >= Each.Length)
            {
                if(Each.bLoop && Each.Length > 0.0f) { Each.Time = std::fmod(Each.Time, Each.Length); }
                else                                 { Each.Time = Each.Length; bDone = true; }
            }
            Apply(Each);
            if(bDone)
            {
                if(!Each.Name.empty()) { FinishedThisTick.push_back({ Each.Handle, Each.Name, Each.OnFinished }); }
                Playing.erase(Playing.begin() + std::ptrdiff_t(At));
            }
            else
            {
                ++At;
            }
        }
    }
}
