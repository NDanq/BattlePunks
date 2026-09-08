#include "Private/GameWindow/GameWindowInternal.h"

/*
 * SDL input codes -> the engine's own.
 *
 * This file is the entire cost of not exposing SDL's enums publicly. Every
 * other translation unit deals in EKeyCode / EMouseButton / EModifierKeys, so
 * an SDL3 -> SDL4 bump, or a second windowing backend, rewrites this file and
 * nothing else.
 *
 * SCANCODES, NOT KEYCODES. SDL_KeyboardEvent carries both: `scancode` is the
 * physical key position, `key` is what that position produces under the user's
 * current layout. EKeyCode is the physical one, so KC_W is the key where W sits
 * on a US layout and stays KC_W on AZERTY even though it types Z. That is what
 * a WASD binding wants. Anything the user is TYPING must come from OnText
 * instead, which is post-layout, post-dead-key and post-IME.
 */

namespace
{
    /*
     * CONSTANT-INITIALISED into .bss, deliberately, and the reasoning is
     * CoreConfig.h's: a file-scope object with no constructor has no
     * initialisation-order question to get wrong. KC_Unknown is 0, so the
     * zero-filled table is already correct for every key not mapped below.
     */
    EKeyCode GScancodeToKey[SDL_SCANCODE_COUNT];
    bool     GKeyTableBuilt = false;

    /* Non-contiguous keys. The contiguous runs are filled by loop below --
     * writing out 26 letters by hand is 26 chances to typo one. */
    struct ScancodePair
    {
        SDL_Scancode Scancode;
        EKeyCode     Key;
    };

    const ScancodePair GKeyPairs[] =
    {
        { SDL_SCANCODE_RETURN,       KC_Return       },
        { SDL_SCANCODE_ESCAPE,       KC_Escape       },
        { SDL_SCANCODE_BACKSPACE,    KC_Backspace    },
        { SDL_SCANCODE_TAB,          KC_Tab          },
        { SDL_SCANCODE_SPACE,        KC_Space        },

        { SDL_SCANCODE_MINUS,        KC_Minus        },
        { SDL_SCANCODE_EQUALS,       KC_Equals       },
        { SDL_SCANCODE_LEFTBRACKET,  KC_LeftBracket  },
        { SDL_SCANCODE_RIGHTBRACKET, KC_RightBracket },
        { SDL_SCANCODE_BACKSLASH,    KC_Backslash    },
        { SDL_SCANCODE_SEMICOLON,    KC_Semicolon    },
        { SDL_SCANCODE_APOSTROPHE,   KC_Apostrophe   },
        { SDL_SCANCODE_GRAVE,        KC_Grave        },
        { SDL_SCANCODE_COMMA,        KC_Comma        },
        { SDL_SCANCODE_PERIOD,       KC_Period       },
        { SDL_SCANCODE_SLASH,        KC_Slash        },

        { SDL_SCANCODE_CAPSLOCK,     KC_CapsLock     },

        { SDL_SCANCODE_PRINTSCREEN,  KC_PrintScreen  },
        { SDL_SCANCODE_SCROLLLOCK,   KC_ScrollLock   },
        { SDL_SCANCODE_PAUSE,        KC_Pause        },
        { SDL_SCANCODE_INSERT,       KC_Insert       },
        { SDL_SCANCODE_HOME,         KC_Home         },
        { SDL_SCANCODE_PAGEUP,       KC_PageUp       },
        { SDL_SCANCODE_DELETE,       KC_Delete       },
        { SDL_SCANCODE_END,          KC_End          },
        { SDL_SCANCODE_PAGEDOWN,     KC_PageDown     },

        { SDL_SCANCODE_RIGHT,        KC_Right        },
        { SDL_SCANCODE_LEFT,         KC_Left         },
        { SDL_SCANCODE_DOWN,         KC_Down         },
        { SDL_SCANCODE_UP,           KC_Up           },

        { SDL_SCANCODE_NUMLOCKCLEAR, KC_NumLockClear },
        { SDL_SCANCODE_KP_DIVIDE,    KC_KP_Divide    },
        { SDL_SCANCODE_KP_MULTIPLY,  KC_KP_Multiply  },
        { SDL_SCANCODE_KP_MINUS,     KC_KP_Minus     },
        { SDL_SCANCODE_KP_PLUS,      KC_KP_Plus      },
        { SDL_SCANCODE_KP_ENTER,     KC_KP_Enter     },
        { SDL_SCANCODE_KP_PERIOD,    KC_KP_Period    },

        { SDL_SCANCODE_LCTRL,        KC_LeftCtrl     },
        { SDL_SCANCODE_LSHIFT,       KC_LeftShift    },
        { SDL_SCANCODE_LALT,         KC_LeftAlt      },
        { SDL_SCANCODE_LGUI,         KC_LeftSuper    },
        { SDL_SCANCODE_RCTRL,        KC_RightCtrl    },
        { SDL_SCANCODE_RSHIFT,       KC_RightShift   },
        { SDL_SCANCODE_RALT,         KC_RightAlt     },
        { SDL_SCANCODE_RGUI,         KC_RightSuper   },
    };

    void BuildKeyTable()
    {
        /* Built on the pump thread, which is the only thread that may reach a
         * dispatch -- CheckPumpThread enforces that at every entry point. */
        for(int Index = 0; Index < 26; ++Index)
        {
            GScancodeToKey[SDL_SCANCODE_A + Index] = EKeyCode(KC_A + Index);
        }

        /* SDL runs 1-9 then 0; EKeyCode runs 0-9. The two disagree, which is
         * precisely why this is a loop over 1..9 plus one special case rather
         * than one tidy-looking loop that would be off by one. */
        for(int Index = 0; Index < 9; ++Index)
        {
            GScancodeToKey[SDL_SCANCODE_1 + Index] = EKeyCode(KC_1 + Index);
        }
        GScancodeToKey[SDL_SCANCODE_0] = KC_0;

        for(int Index = 0; Index < 12; ++Index)
        {
            GScancodeToKey[SDL_SCANCODE_F1 + Index] = EKeyCode(KC_F1 + Index);
        }

        /* The keypad repeats the same 1-9-then-0 disagreement. */
        for(int Index = 0; Index < 9; ++Index)
        {
            GScancodeToKey[SDL_SCANCODE_KP_1 + Index] = EKeyCode(KC_KP_1 + Index);
        }
        GScancodeToKey[SDL_SCANCODE_KP_0] = KC_KP_0;

        for(const ScancodePair& Pair : GKeyPairs)
        {
            GScancodeToKey[Pair.Scancode] = Pair.Key;
        }

        GKeyTableBuilt = true;
    }
}

EKeyCode GameWindowKeyFromScancode(const SDL_Scancode InScancode)
{
    if(!GKeyTableBuilt)
    {
        BuildKeyTable();
    }

    /* SDL_Scancode is an enum over a 512-entry space and a driver could in
     * principle hand back something outside it. Bounds-check rather than trust
     * it: the cost is one compare and the alternative is an OOB read. */
    if(InScancode < 0 || InScancode >= SDL_SCANCODE_COUNT)
    {
        return KC_Unknown;
    }

    return GScancodeToKey[InScancode];
}

uint16_t GameWindowModifiersFromSDL(const SDL_Keymod InMod)
{
    uint16_t Result = MOD_None;

    /* Side-agnostic: SDL_KMOD_SHIFT is already (LSHIFT|RSHIFT). A binding that
     * cares which shift reads the KC_LeftShift / KC_RightShift key events. */
    if(InMod & SDL_KMOD_SHIFT) { Result |= MOD_Shift;    }
    if(InMod & SDL_KMOD_CTRL)  { Result |= MOD_Ctrl;     }
    if(InMod & SDL_KMOD_ALT)   { Result |= MOD_Alt;      }
    if(InMod & SDL_KMOD_GUI)   { Result |= MOD_Super;    }
    if(InMod & SDL_KMOD_CAPS)  { Result |= MOD_CapsLock; }
    if(InMod & SDL_KMOD_NUM)   { Result |= MOD_NumLock;  }

    return Result;
}

EMouseButton GameWindowMouseButtonFromSDL(const uint8_t InButton)
{
    switch(InButton)
    {
        case SDL_BUTTON_LEFT:   return MB_Left;
        case SDL_BUTTON_MIDDLE: return MB_Middle;
        case SDL_BUTTON_RIGHT:  return MB_Right;
        case SDL_BUTTON_X1:     return MB_X1;
        case SDL_BUTTON_X2:     return MB_X2;

        /* A mouse with more than five buttons is real and reports 6, 7, ...
         * MB_MAX means "not one we model"; the dispatch drops the event rather
         * than reporting it as a left click. */
        default:                return MB_MAX;
    }
}
