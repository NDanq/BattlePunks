#ifndef GAMEWINDOW_GAMEWINDOWGAMEPAD_H
#define GAMEWINDOW_GAMEWINDOWGAMEPAD_H

#include <cstdint>

/*
 * Gamepads, through SDL's gamepad API (the standard layout: SDL maps any
 * supported controller onto it). NOT per window: a pad belongs to a player,
 * not to whichever window has focus, so its events go to one process-level
 * table, GameWindowSystem::SetGamepadDelegates -- the input layer's.
 *
 * PLAYERS. A pad is given the lowest free player slot when it connects and
 * keeps it until it disconnects, so unplugging pad 0 does not renumber pad 1.
 *
 * Engine-owned POD, like GameWindowInput.h: no SDL type crosses this header.
 */

enum : uint8_t { GAMEPAD_PLAYERS_MAX = 4 };

/* SDL_GamepadButton's order, which the input layer's names follow. */
enum EGamepadButton : uint8_t
{
    GB_South = 0, GB_East, GB_West, GB_North,   // A B X Y on an Xbox pad
    GB_Back, GB_Guide, GB_Start,
    GB_LeftStick, GB_RightStick,
    GB_LeftShoulder, GB_RightShoulder,
    GB_DPadUp, GB_DPadDown, GB_DPadLeft, GB_DPadRight,
    GB_MAX
};

/* SDL_GamepadAxis's order. */
enum EGamepadAxis : uint8_t
{
    GA_LeftX = 0, GA_LeftY, GA_RightX, GA_RightY,
    GA_LeftTrigger, GA_RightTrigger,
    GA_MAX
};

struct GamepadButtonEvent
{
    uint8_t        Player   = 0;
    EGamepadButton Button   = GB_South;
    bool           bPressed = false;
};

struct GamepadAxisEvent
{
    uint8_t      Player = 0;
    EGamepadAxis Axis   = GA_LeftX;
    float        Value  = 0.0f;   // sticks -1..1 (+Y is down, as SDL reports); triggers 0..1
};

/* Invoked inside PumpEvents, on the pump thread, like the window delegates. */
struct GamepadDelegates
{
    void* UserData = nullptr;

    /* InName is valid for the call only. Pads present when SDL starts are
     * reported as they are found. */
    void (*OnConnected)(uint8_t InPlayer, bool bInConnected, const char* InName, void*) = nullptr;
    void (*OnButton)(const GamepadButtonEvent&, void*)                                  = nullptr;
    void (*OnAxis)(const GamepadAxisEvent&, void*)                                      = nullptr;
};

#endif //GAMEWINDOW_GAMEWINDOWGAMEPAD_H
