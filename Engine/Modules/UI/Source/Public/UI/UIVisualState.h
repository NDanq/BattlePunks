#ifndef UI_UIVISUALSTATE_H
#define UI_UIVISUALSTATE_H

#include <string>
#include <vector>

#include "Public/UI/UIAnimation.h"

/*
 * VISUAL STATES. A widget's named looks and the transitions between them:
 *
 *     <VisualStates Class="MenuButton">
 *       <State Name="Hovered"><Set Property="RenderScale" Value="1.05,1.05"/></State>
 *       <State Name="Pressed"><Set Property="RenderScale" Value="0.97,0.97"/></State>
 *       <Transition From="*" To="*" Duration="0.12" Easing="QuadOut"/>
 *     </VisualStates>
 *
 * For the widget named Target, or every widget whose Class names a style
 * (Class). An interactive widget (a Button, a CheckBox, a Slider, anything
 * focusable) goes, by itself, to:
 *
 *     Disabled  not enabled
 *     Pressed   held down with the pointer over it
 *     Hovered   under the pointer -- or focused, when there is no Focused
 *     Focused   focused (if the group has it)
 *     Normal    otherwise
 *
 * and UIWidget::GoToState goes to any state by name. A state sets its
 * properties; a property it does not set goes back to what the widget was
 * made with. A transition moves them from where they are, so a hover cut
 * short turns back smoothly. The first transition matching From and To
 * wins ("*" any); none, and the state is shown at once.
 *
 * A Button with visual states paints its Normal brush always: the states own
 * its look (animate Normal.Tint). Without, it switches brushes as before.
 */
namespace UI
{
    struct UIStateSetter
    {
        std::string Property;
        std::string Value;   // the property's text form
    };

    struct UIVisualStateDesc
    {
        std::string                Name;
        std::vector<UIStateSetter> Setters;
    };

    struct UIStateTransition
    {
        std::string From = "*";   // a state's name, or "*" any
        std::string To   = "*";
        float       Duration = 0.0f;
        EUIEasing   Easing   = EUIEasing::Linear;
    };

    struct UIVisualStateGroup
    {
        std::string                    Target;   // a widget's Name
        std::string                    Class;    // or every widget taking this style
        std::vector<UIVisualStateDesc> States;
        std::vector<UIStateTransition> Transitions;

        const UIVisualStateDesc* FindState(const std::string& InName) const
        {
            for(const UIVisualStateDesc& Each : States) { if(Each.Name == InName) return &Each; }
            return nullptr;
        }

        /* The transition from InFrom to InTo; null: at once. */
        const UIStateTransition* FindTransition(const std::string& InFrom, const std::string& InTo) const
        {
            for(const UIStateTransition& Each : Transitions)
            {
                if((Each.From == "*" || Each.From == InFrom) && (Each.To == "*" || Each.To == InTo)) return &Each;
            }
            return nullptr;
        }
    };

    /* Whether InClassList (a widget's Class: names separated by spaces) names InClass. */
    UI_API bool ClassListHas(const std::string& InClassList, const std::string& InClass);
}

#endif //UI_UIVISUALSTATE_H
