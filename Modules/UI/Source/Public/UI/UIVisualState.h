#ifndef UI_UIVISUALSTATE_H
#define UI_UIVISUALSTATE_H

#include <string>
#include <vector>

#include "Public/UI/UIAnimation.h"

/*
 * VISUAL STATES -- RESERVED. A widget's named looks and the transitions
 * between them, as a document's <VisualStates> will hold them:
 *
 *     <VisualStates Target="Play">
 *       <State Name="Hovered"><Set Property="RenderScale" Value="1.05,1.05"/></State>
 *       <Transition From="*" To="Hovered" Duration="0.1" Easing="QuadOut"/>
 *     </VisualStates>
 *
 * Kept through load, save and cook, not applied. UIWidget::GoToState records
 * the state's name; a Button's Normal, Hovered, Pressed and Disabled brushes
 * are what it shows for now, and become states when these play.
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
        std::string To;
        float       Duration = 0.0f;
        EUIEasing   Easing   = EUIEasing::Linear;
    };

    struct UIVisualStateGroup
    {
        std::string                    Target;   // a widget's Name
        std::vector<UIVisualStateDesc> States;
        std::vector<UIStateTransition> Transitions;
    };
}

#endif //UI_UIVISUALSTATE_H
