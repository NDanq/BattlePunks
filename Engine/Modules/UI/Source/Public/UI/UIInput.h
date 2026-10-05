#ifndef UI_UIINPUT_H
#define UI_UIINPUT_H

#include "Public/UI/UITypes.h"

/*
 * One frame of input as the UI takes it, and what it took. The host fills
 * the frame from its own input state (NimbleEngine's GameSession from
 * InputState, the editor's preview from Qt) and clears from its game input
 * what the result says was consumed -- so a click on a button never fires
 * the weapon underneath it. The UI module itself knows no input system.
 */
namespace UI
{
    struct UIInputFrame
    {
        UIVec2 MousePixels;                  // from the viewport's top-left
        bool   bHasMouse = true;             // false: the pointer is elsewhere (or captured)
        bool   MouseDown[3]     = {};        // left, right, middle
        bool   MousePressed[3]  = {};
        bool   MouseReleased[3] = {};
        float  Wheel = 0.0f;

        // Navigation, pressed this frame: keys or a gamepad, as the host maps them.
        bool bNavUp = false, bNavDown = false, bNavLeft = false, bNavRight = false;
        bool bAccept = false, bBack = false;
        bool bNext = false, bPrevious = false;   // Tab, Shift+Tab
        bool bNavFromGamepad = false;            // which device the navigation came from

        // Text editing, for a focused text field (UIWidget::WantsTextInput).
        char    Text[64] = {};                   // typed this frame, UTF-8 (the window's text input)
        uint8_t Backspace = 0, Delete = 0;       // presses and repeats this frame
        uint8_t Left = 0, Right = 0, Home = 0, End = 0;
        bool    bReturn = false;                 // Return / Enter pressed
        bool    bShift = false, bCtrl = false;   // held
        bool    bSelectAll = false, bCopy = false, bCut = false, bPaste = false;   // Ctrl+A / C / X / V
    };

    struct UIInputResult
    {
        bool bConsumeMouse    = false;   // buttons and wheel
        bool bConsumeKeyboard = false;
        bool bConsumeGamepad  = false;
        bool bShowCursor      = false;   // a UIOnly document is open
        bool bWantsTextInput  = false;   // a text field has focus: the host turns the window's text input on
        bool bOverWidget      = false;   // the pointer is over a widget that takes hits
    };
}

#endif //UI_UIINPUT_H
