#ifndef GAMEWINDOW_GAMEWINDOWINPUT_H
#define GAMEWINDOW_GAMEWINDOWINPUT_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"

/*
 * Input types.
 *
 * ENGINE-OWNED, NOT SDL'S. Nothing here is an SDL_Scancode or an SDL_Keymod,
 * and that is the point: no host translation unit includes SDL.h, so SDL stays
 * a private link dependency of this one module, and an SDL3 -> SDL4 bump is one
 * translation table in GameWindowInputSDL.cpp rather than a sweep of the
 * codebase.
 *
 * PHYSICAL KEYS, NOT CHARACTERS. EKeyCode names a position on the keyboard --
 * KC_W is the key where W sits on a US layout, and it stays KC_W on AZERTY even
 * though that key types Z. This is what a WASD binding wants. Text is a
 * separate stream entirely: OnText delivers the UTF-8 the OS produced after
 * layout, dead keys and the IME, and it is the ONLY correct source for anything
 * the user is typing.
 *
 * Every struct here is trivially copyable POD -- the same constraint
 * ModuleReloadState imposes on its blob -- so a hot module can queue one
 * without a cross-image lifetime question.
 */

/*
 * KC_ prefix. Values are contiguous and KC_MAX bounds the translation table, so
 * the table is a flat array and the smoke test can prove it has no holes.
 *
 * REORDERING THIS IS AN ABI CHANGE for anything that has serialised a binding.
 * Append; do not insert.
 */
enum EKeyCode : uint16_t
{
    KC_Unknown = 0,

    // Letters, in alphabetical order rather than layout order.
    KC_A, KC_B, KC_C, KC_D, KC_E, KC_F, KC_G, KC_H, KC_I, KC_J, KC_K, KC_L,
    KC_M, KC_N, KC_O, KC_P, KC_Q, KC_R, KC_S, KC_T, KC_U, KC_V, KC_W, KC_X,
    KC_Y, KC_Z,

    // Top-row digits. NOT the keypad -- see KC_KP_* below.
    KC_0, KC_1, KC_2, KC_3, KC_4, KC_5, KC_6, KC_7, KC_8, KC_9,

    KC_Return, KC_Escape, KC_Backspace, KC_Tab, KC_Space,

    KC_Minus, KC_Equals, KC_LeftBracket, KC_RightBracket, KC_Backslash,
    KC_Semicolon, KC_Apostrophe, KC_Grave, KC_Comma, KC_Period, KC_Slash,

    KC_CapsLock,

    KC_F1, KC_F2, KC_F3, KC_F4,  KC_F5,  KC_F6,
    KC_F7, KC_F8, KC_F9, KC_F10, KC_F11, KC_F12,

    KC_PrintScreen, KC_ScrollLock, KC_Pause,
    KC_Insert, KC_Home, KC_PageUp, KC_Delete, KC_End, KC_PageDown,

    KC_Right, KC_Left, KC_Down, KC_Up,

    KC_NumLockClear,
    KC_KP_Divide, KC_KP_Multiply, KC_KP_Minus, KC_KP_Plus, KC_KP_Enter,
    KC_KP_0, KC_KP_1, KC_KP_2, KC_KP_3, KC_KP_4,
    KC_KP_5, KC_KP_6, KC_KP_7, KC_KP_8, KC_KP_9,
    KC_KP_Period,

    // Left/right are distinct because a rebindable control scheme needs them to
    // be. The MOD_ bitmask below deliberately is not: a shortcut asking "was
    // shift held" does not care which one.
    KC_LeftCtrl, KC_LeftShift, KC_LeftAlt, KC_LeftSuper,
    KC_RightCtrl, KC_RightShift, KC_RightAlt, KC_RightSuper,

    KC_MAX
};

enum EMouseButton : uint8_t
{
    MB_Left = 0,
    MB_Middle,
    MB_Right,
    MB_X1,
    MB_X2,
    MB_MAX
};

/*
 * A bitmask, and side-agnostic on purpose -- see the note on KC_LeftCtrl.
 * MOD_Super is Windows/Command/Meta.
 */
enum EModifierKeys : uint16_t
{
    MOD_None  = 0,
    MOD_Shift = 1u << 0,
    MOD_Ctrl  = 1u << 1,
    MOD_Alt   = 1u << 2,
    MOD_Super = 1u << 3,

    MOD_CapsLock = 1u << 4,
    MOD_NumLock  = 1u << 5
};

struct KeyEvent
{
    EKeyCode Key       = KC_Unknown;
    uint16_t Modifiers = MOD_None;
    bool     bPressed  = false;

    /*
     * An auto-repeat from the OS key-repeat timer, not a fresh press. A
     * movement binding ignores these; a text-ish binding (backspace held down)
     * wants them. There is no separate repeat callback precisely so that the
     * choice is the host's and is visible at the call site.
     */
    bool     bRepeat   = false;
};

struct MouseButtonEvent
{
    EMouseButton      Button     = MB_Left;
    EngineCore::Point Pos;                    // physical pixels, window-relative
    uint8_t           ClickCount = 1;         // 2 on a double click
    bool              bPressed   = false;
    uint16_t          Modifiers  = MOD_None;
};

struct MouseMoveEvent
{
    EngineCore::Point Pos;                    // physical pixels, window-relative
    int               DeltaX     = 0;
    int               DeltaY     = 0;
    uint16_t          Modifiers  = MOD_None;
};

struct MouseWheelEvent
{
    /* Float because trackpads and high-resolution wheels report fractions. */
    float             DeltaX     = 0.0f;
    float             DeltaY     = 0.0f;
    EngineCore::Point Pos;                    // physical pixels, window-relative
    uint16_t          Modifiers  = MOD_None;
};

#endif //GAMEWINDOW_GAMEWINDOWINPUT_H
