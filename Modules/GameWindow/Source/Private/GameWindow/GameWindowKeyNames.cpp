#include "Public/GameWindow/GameWindowInput.h"

#include <cctype>

/*
 * Names for EKeyCode and EMouseButton: what an input binding is written in
 * ("Space", "W", "Mouse.Left"), so a game, a config file or the editor can name
 * a key without the enum. No SDL here -- the names are the engine's own.
 *
 * Indexed by the enum, which is contiguous (GameWindowInput.h): the table must
 * list every key in enum order, which the static_assert checks by count.
 */
namespace
{
    const char* const GKeyNames[] = {
        "Unknown",
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "Return", "Escape", "Backspace", "Tab", "Space",
        "Minus", "Equals", "LeftBracket", "RightBracket", "Backslash",
        "Semicolon", "Apostrophe", "Grave", "Comma", "Period", "Slash",
        "CapsLock",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
        "PrintScreen", "ScrollLock", "Pause",
        "Insert", "Home", "PageUp", "Delete", "End", "PageDown",
        "Right", "Left", "Down", "Up",
        "NumLock",
        "KP_Divide", "KP_Multiply", "KP_Minus", "KP_Plus", "KP_Enter",
        "KP_0", "KP_1", "KP_2", "KP_3", "KP_4", "KP_5", "KP_6", "KP_7", "KP_8", "KP_9",
        "KP_Period",
        "LeftCtrl", "LeftShift", "LeftAlt", "LeftSuper",
        "RightCtrl", "RightShift", "RightAlt", "RightSuper",
    };
    static_assert(sizeof(GKeyNames) / sizeof(GKeyNames[0]) == KC_MAX, "GKeyNames must name every EKeyCode, in order");

    const char* const GMouseButtonNames[] = { "Mouse.Left", "Mouse.Middle", "Mouse.Right", "Mouse.X1", "Mouse.X2" };
    static_assert(sizeof(GMouseButtonNames) / sizeof(GMouseButtonNames[0]) == MB_MAX, "name every EMouseButton");

    bool EqualsNoCase(const char* InA, const char* InB)
    {
        for(; *InA != '\0' && *InB != '\0'; ++InA, ++InB)
        {
            if(std::tolower(static_cast<unsigned char>(*InA)) != std::tolower(static_cast<unsigned char>(*InB))) { return false; }
        }
        return *InA == *InB;
    }
}

const char* GetKeyName(const EKeyCode InKey)
{
    return InKey < KC_MAX ? GKeyNames[InKey] : GKeyNames[KC_Unknown];
}

EKeyCode FindKeyByName(const char* InName)
{
    if(!InName || InName[0] == '\0') { return KC_Unknown; }
    for(uint16_t Key = KC_Unknown + 1; Key < KC_MAX; ++Key)
    {
        if(EqualsNoCase(GKeyNames[Key], InName)) { return EKeyCode(Key); }
    }
    return KC_Unknown;
}

const char* GetMouseButtonName(const EMouseButton InButton)
{
    return InButton < MB_MAX ? GMouseButtonNames[InButton] : "Mouse.Unknown";
}

bool FindMouseButtonByName(const char* InName, EMouseButton& OutButton)
{
    for(uint8_t Button = 0; InName && Button < MB_MAX; ++Button)
    {
        if(EqualsNoCase(GMouseButtonNames[Button], InName)) { OutButton = EMouseButton(Button); return true; }
    }
    return false;
}
