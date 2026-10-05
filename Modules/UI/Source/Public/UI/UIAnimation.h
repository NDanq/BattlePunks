#ifndef UI_UIANIMATION_H
#define UI_UIANIMATION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/UI/UIAPI.h"

/*
 * ANIMATIONS -- RESERVED. The shape of what a document's <Animations> holds
 * and of what will play it; nothing plays yet. A document keeps its
 * <Animations> through every load, save and cook (UIDocumentAsset::Reserved),
 * and UIDocumentInstance::PlayAnimation says no, once, with a warning.
 *
 *     <Animations>
 *       <Animation Name="Intro" Duration="0.6">
 *         <Track Target="Title" Property="Opacity" Easing="CubicOut">
 *           <Key Time="0" Value="0"/><Key Time="0.6" Value="1"/>
 *         </Track>
 *       </Animation>
 *     </Animations>
 *
 * A track drives one Animatable property (UPF_Animatable: Float, Color, Vec2)
 * of the widget named Target, between keys, through its easing.
 */
namespace UI
{
    enum class EUIEasing : uint8_t
    {
        Linear, QuadIn, QuadOut, QuadInOut, CubicIn, CubicOut, CubicInOut, BackOut, ElasticOut, Step
    };

    /* The curve, 0-1 in and out (BackOut and ElasticOut overshoot). */
    UI_API float Ease(EUIEasing InEasing, float InT);

    /* "CubicOut" and back; Linear for a name it does not know. */
    UI_API EUIEasing   ParseEasing(const std::string& InName);
    UI_API const char* GetEasingName(EUIEasing InEasing);

    struct UIAnimationKey
    {
        float       Time = 0.0f;   // seconds
        std::string Value;         // the property's text form
    };

    struct UIAnimationTrack
    {
        std::string                 Target;     // a widget's Name
        std::string                 Property;   // "Opacity", "Background.Tint"
        EUIEasing                   Easing = EUIEasing::Linear;
        std::vector<UIAnimationKey> Keys;
    };

    struct UIAnimationAsset
    {
        std::string                   Name;
        float                         Duration = 0.0f;
        bool                          bLoop    = false;
        std::vector<UIAnimationTrack> Tracks;
    };

    using UIAnimationHandle = uint32_t;
    constexpr UIAnimationHandle UI_ANIMATION_INVALID = 0;

    /*
     * Plays a document's animations: RESERVED. The API a document's instance
     * will call; Play refuses (UI_ANIMATION_INVALID) and Tick does nothing.
     */
    class UI_API UIAnimator
    {
    public:
        UIAnimationHandle Play(const UIAnimationAsset& InAnimation);
        void              Stop(UIAnimationHandle InHandle);
        bool              IsPlaying(UIAnimationHandle InHandle) const;
        void              Tick(float InDeltaSeconds);
    };
}

#endif //UI_UIANIMATION_H
