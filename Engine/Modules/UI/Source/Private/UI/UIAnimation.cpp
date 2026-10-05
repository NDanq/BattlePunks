#include "Public/UI/UIAnimation.h"

#include <algorithm>
#include <cmath>

namespace UI
{
    namespace
    {
        const char* const EASING_NAMES[] = {
            "Linear", "QuadIn", "QuadOut", "QuadInOut", "CubicIn", "CubicOut", "CubicInOut", "BackOut", "ElasticOut", "Step"
        };
        constexpr float PI = 3.14159265358979f;
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

    // RESERVED: the player's API, with nothing behind it yet.

    UIAnimationHandle UIAnimator::Play(const UIAnimationAsset& /*InAnimation*/)
    {
        return UI_ANIMATION_INVALID;
    }

    void UIAnimator::Stop(UIAnimationHandle /*InHandle*/)
    {
    }

    bool UIAnimator::IsPlaying(UIAnimationHandle /*InHandle*/) const
    {
        return false;
    }

    void UIAnimator::Tick(float /*InDeltaSeconds*/)
    {
    }
}
