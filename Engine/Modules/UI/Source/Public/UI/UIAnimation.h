#ifndef UI_UIANIMATION_H
#define UI_UIANIMATION_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/UI/UIAPI.h"
#include "Public/UI/UITypes.h"

/*
 * ANIMATIONS. What a document's <Animations> holds, and what plays it:
 *
 *     <Animations>
 *       <Animation Name="Intro" Duration="0.6" OnFinished="Native:Lab.IntroDone">
 *         <Track Target="Title" Property="Opacity" Easing="CubicOut">
 *           <Key Time="0" Value="0"/><Key Time="0.6" Value="1"/>
 *         </Track>
 *       </Animation>
 *     </Animations>
 *
 * A track drives one Animatable property (UPF_Animatable: Float, Color, Vec2)
 * of the widget named Target -- or of every widget whose Class names a style,
 * with Class="MenuButton" in place of Target -- between keys, each segment
 * through the track's easing. Before its first key a track holds the first
 * key's value, after its last the last's: staggering is keys that start later.
 * A key's Value "*" is what the property held when the animation started.
 *
 * Played by its document's UIAnimator (UIDocumentInstance::PlayAnimation).
 * Targets and values are resolved once, when it starts; a tick only eases and
 * writes, and allocates nothing. A document with nothing playing costs
 * nothing. When it ends, a property keeps its last key's value.
 *
 * A property two animations drive goes to the one started last. While a
 * track drives a bound property ({Bind:...}) it wins; the binding writes
 * again on the store's next change.
 */
namespace UI
{
    class UIDocumentInstance;

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
        std::string Value;         // the property's text form; "*" its value at the start
    };

    struct UIAnimationTrack
    {
        std::string                 Target;     // a widget's Name
        std::string                 Class;      // or every widget taking this style
        std::string                 Property;   // "Opacity", "Background.Tint"
        EUIEasing                   Easing = EUIEasing::Linear;
        std::vector<UIAnimationKey> Keys;
    };

    struct UIAnimationAsset
    {
        std::string                   Name;
        float                         Duration = 0.0f;   // 0: its last key's time
        bool                          bLoop    = false;
        std::string                   OnFinished;        // actions, as text; run when it ends (not when stopped)
        std::vector<UIAnimationTrack> Tracks;

        /* Duration, or its last key's time when that is 0. */
        UI_API float GetLength() const;
    };

    using UIAnimationHandle = uint32_t;
    constexpr UIAnimationHandle UI_ANIMATION_INVALID = 0;

    struct UIPlayOptions
    {
        float Speed     = 1.0f;    // 2 twice as fast
        bool  bReverse  = false;   // from the end back to the start
        float StartTime = 0.0f;    // seconds into it (from the end, reversed)
    };

    /* One property a transition moves (UIAnimator::PlayTransition). */
    struct UITransitionTarget
    {
        uint32_t Widget   = 0;
        int32_t  Property = -1;
        UIValue  To;
    };

    /*
     * Plays a document's animations. Its document owns it, and its system
     * ticks it (UISystem::Tick) with real time, so a paused game's menus move.
     */
    class UI_API UIAnimator
    {
    public:
        explicit UIAnimator(UIDocumentInstance* InDocument = nullptr) : Document(InDocument) {}

        /* InAnimation from its start (InOptions'). Playing it again restarts
         * it. UI_ANIMATION_INVALID if no track found a widget and property. */
        UIAnimationHandle Play(const UIAnimationAsset& InAnimation, const UIPlayOptions& InOptions = UIPlayOptions());

        /* Each of InTargets from where it is now to To, over InDuration
         * (0: at once) -- a visual state's transition. */
        UIAnimationHandle PlayTransition(const std::vector<UITransitionTarget>& InTargets, float InDuration, EUIEasing InEasing);

        /* Stops it where it is; its OnFinished does not run. */
        void Stop(UIAnimationHandle InHandle);
        void StopAll();
        bool IsPlaying(UIAnimationHandle InHandle) const;
        /* The newest playing animation named InName; UI_ANIMATION_INVALID none. */
        UIAnimationHandle Find(const std::string& InName) const;
        bool IsAnyPlaying() const { return !Playing.empty(); }

        /* Moves time on and writes what moved. What ended is GetFinished's
         * until the next Tick (UISystem runs their OnFinished). */
        void Tick(float InDeltaSeconds);

        /* What ended in the last Tick, with its OnFinished (cleared each Tick). */
        struct Finished
        {
            UIAnimationHandle Handle = UI_ANIMATION_INVALID;
            std::string       Name;
            const std::string* OnFinished = nullptr;   // the asset's; valid while its document lives
        };
        const std::vector<Finished>& GetFinished() const { return FinishedThisTick; }

    private:
        struct Key
        {
            float   Time = 0.0f;
            UIValue Value;
        };
        struct Track
        {
            uint32_t         Widget   = 0;
            int32_t          Property = -1;
            EUIEasing        Easing   = EUIEasing::Linear;
            std::vector<Key> Keys;
        };
        struct Instance
        {
            UIAnimationHandle  Handle = UI_ANIMATION_INVALID;
            std::string        Name;                    // "" a transition
            const std::string* OnFinished = nullptr;
            std::vector<Track> Tracks;
            float              Length   = 0.0f;
            float              Time     = 0.0f;
            float              Speed    = 1.0f;
            bool               bLoop    = false;
            bool               bReverse = false;
        };

        /* Tracks of earlier animations driving the same widget and property let go. */
        void TakeOver(const Instance& InNew);
        void Apply(const Instance& InInstance);
        UIAnimationHandle Start(Instance&& InInstance);

        UIDocumentInstance*   Document = nullptr;
        std::vector<Instance> Playing;
        std::vector<Finished> FinishedThisTick;
        UIAnimationHandle     NextHandle = 1;
    };
}

#endif //UI_UIANIMATION_H
