#include "Public/GameWindow/GameWindowGamepad.h"

#include <string>

#include "Private/GameWindow/GameWindowInternal.h"
#include "Public/GameWindow/GameWindowLog.h"
#include "Public/GameWindow/GameWindowSystem.h"

/*
 * SDL gamepads to GamepadDelegates: one player slot per open pad, lowest
 * free first, kept until it disconnects. GameWindowSystem.cpp routes the
 * events here and starts and stops the subsystem with SDL.
 */
namespace
{
    struct PadSlot
    {
        SDL_Gamepad*   Pad = nullptr;
        SDL_JoystickID ID  = 0;
    };

    PadSlot          GPads[GAMEPAD_PLAYERS_MAX];
    GamepadDelegates GDelegates;
    bool             bGamepadsStarted = false;

    int PlayerOf(const SDL_JoystickID InID)
    {
        for(int Player = 0; Player < GAMEPAD_PLAYERS_MAX; ++Player)
        {
            if(GPads[Player].Pad && GPads[Player].ID == InID) { return Player; }
        }
        return -1;
    }

    void Connect(const SDL_JoystickID InID)
    {
        if(PlayerOf(InID) >= 0) { return; }
        for(uint8_t Player = 0; Player < GAMEPAD_PLAYERS_MAX; ++Player)
        {
            if(GPads[Player].Pad) { continue; }
            SDL_Gamepad* Pad = SDL_OpenGamepad(InID);
            if(!Pad)
            {
                GW_LOG_WARN(std::string("a gamepad could not be opened: ") + SDL_GetError());
                return;
            }
            GPads[Player] = { Pad, InID };
            const char* Name = SDL_GetGamepadName(Pad);
            GW_LOG("gamepad connected as player " + std::to_string(Player) + ": " + (Name ? Name : "unnamed"));
            if(GDelegates.OnConnected) { GDelegates.OnConnected(Player, true, Name ? Name : "", GDelegates.UserData); }
            return;
        }
        GW_LOG_WARN("a gamepad connected, but every player slot is taken; it is ignored");
    }

    void Disconnect(const SDL_JoystickID InID)
    {
        const int Player = PlayerOf(InID);
        if(Player < 0) { return; }
        SDL_CloseGamepad(GPads[Player].Pad);
        GPads[Player] = PadSlot();
        GW_LOG("gamepad disconnected from player " + std::to_string(Player));
        if(GDelegates.OnConnected) { GDelegates.OnConnected(uint8_t(Player), false, "", GDelegates.UserData); }
    }

    /* -32768..32767 to -1..1 (a trigger's 0..32767 to 0..1). */
    float Normalise(const int16_t InValue)
    {
        const float Value = float(InValue) / 32767.0f;
        return Value < -1.0f ? -1.0f : Value;
    }
}

namespace GameWindowSystem
{
    void SetGamepadDelegates(const GamepadDelegates& InDelegates)
    {
        GDelegates = InDelegates;

        /* Pads already open were reported before this table existed. */
        for(uint8_t Player = 0; Player < GAMEPAD_PLAYERS_MAX; ++Player)
        {
            if(GPads[Player].Pad && GDelegates.OnConnected)
            {
                const char* Name = SDL_GetGamepadName(GPads[Player].Pad);
                GDelegates.OnConnected(Player, true, Name ? Name : "", GDelegates.UserData);
            }
        }
    }

    void ClearGamepadDelegates()
    {
        GDelegates = GamepadDelegates();
    }

    bool IsGamepadConnected(const uint8_t InPlayer)
    {
        return InPlayer < GAMEPAD_PLAYERS_MAX && GPads[InPlayer].Pad != nullptr;
    }

    void StartGamepads()
    {
        if(bGamepadsStarted) { return; }
        if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        {
            GW_LOG_WARN(std::string("gamepads are unavailable: ") + SDL_GetError());
            return;
        }
        bGamepadsStarted = true;   // SDL then reports the pads already present as ADDED
    }

    void StopGamepads()
    {
        if(!bGamepadsStarted) { return; }
        for(PadSlot& Each : GPads)
        {
            if(Each.Pad) { SDL_CloseGamepad(Each.Pad); }
            Each = PadSlot();
        }
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        bGamepadsStarted = false;
    }

    bool RouteGamepadEvent(const SDL_Event& InEvent)
    {
        switch(InEvent.type)
        {
        case SDL_EVENT_GAMEPAD_ADDED:
            Connect(InEvent.gdevice.which);
            return true;

        case SDL_EVENT_GAMEPAD_REMOVED:
            Disconnect(InEvent.gdevice.which);
            return true;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        {
            const int Player = PlayerOf(InEvent.gbutton.which);
            if(Player >= 0 && InEvent.gbutton.button < GB_MAX && GDelegates.OnButton)
            {
                GamepadButtonEvent Out;
                Out.Player   = uint8_t(Player);
                Out.Button   = EGamepadButton(InEvent.gbutton.button);
                Out.bPressed = InEvent.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                GDelegates.OnButton(Out, GDelegates.UserData);
            }
            return true;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        {
            const int Player = PlayerOf(InEvent.gaxis.which);
            if(Player >= 0 && InEvent.gaxis.axis < GA_MAX && GDelegates.OnAxis)
            {
                GamepadAxisEvent Out;
                Out.Player = uint8_t(Player);
                Out.Axis   = EGamepadAxis(InEvent.gaxis.axis);
                Out.Value  = Normalise(InEvent.gaxis.value);
                GDelegates.OnAxis(Out, GDelegates.UserData);
            }
            return true;
        }

        default:
            return false;
        }
    }
}
