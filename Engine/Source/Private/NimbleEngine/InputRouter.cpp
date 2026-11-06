#include "Public/NimbleEngine/InputRouter.h"

#include "Private/NimbleEngine/InputInternal.h"
#include "Public/GameWindow/GameWindow.h"
#include "Public/GameWindow/GameWindowDelegates.h"
#include "Public/GameWindow/GameWindowGamepad.h"
#include "Public/GameWindow/GameWindowInput.h"
#include "Public/GameWindow/GameWindowSystem.h"

/*
 * GameWindow's input callbacks, which run inside PumpEvents on the game
 * thread, into the pending InputState. Latch() then makes it this frame's.
 */
namespace NimbleEngine
{
namespace
{
    InputState& PendingOf(void* InUserData) { return static_cast<InputRouter*>(InUserData)->GetPending(); }

    void OnKey(GameWindow*, const KeyEvent& InEvent, void* InUserData)
    {
        const InputKey Key = InputKeyFromKeyCode(InEvent.Key);
        if(InEvent.bRepeat)
        {
            if(InEvent.bPressed) { PendingOf(InUserData).Repeat(Key); }   // OS auto-repeat is not a press
            return;
        }
        InEvent.bPressed ? PendingOf(InUserData).Press(Key) : PendingOf(InUserData).Release(Key);
    }

    void OnText(GameWindow*, const char* InUtf8, void* InUserData)
    {
        PendingOf(InUserData).AppendText(InUtf8);
    }

    void OnMouseButton(GameWindow*, const MouseButtonEvent& InEvent, void* InUserData)
    {
        const InputKey Key = InputKeyFromMouseButton(InEvent.Button);
        InEvent.bPressed ? PendingOf(InUserData).Press(Key) : PendingOf(InUserData).Release(Key);
    }

    void OnMouseMove(GameWindow*, const MouseMoveEvent& InEvent, void* InUserData)
    {
        PendingOf(InUserData).MoveMouse(InEvent.Pos.X, InEvent.Pos.Y, InEvent.DeltaX, InEvent.DeltaY);
    }

    void OnMouseWheel(GameWindow*, const MouseWheelEvent& InEvent, void* InUserData)
    {
        PendingOf(InUserData).AddWheel(InEvent.DeltaX, InEvent.DeltaY);
    }

    void OnFocusChanged(GameWindow*, const bool bInHasFocus, void* InUserData)
    {
        InputState& State = PendingOf(InUserData);
        State.SetFocus(bInHasFocus);
        if(!bInHasFocus) { State.ReleaseAll(); }   // the key-ups go to whoever has focus now
    }

    void OnPadConnected(const uint8_t InPlayer, const bool bInConnected, const char* /*InName*/, void* InUserData)
    {
        if(InPlayer < INPUT_PLAYERS_MAX) { PendingOf(InUserData).SetPadConnected(InPlayer, bInConnected); }
    }

    void OnPadButton(const GamepadButtonEvent& InEvent, void* InUserData)
    {
        if(InEvent.Player >= INPUT_PLAYERS_MAX) { return; }
        const InputKey Key = InputKeyFromPadButton(InEvent.Button);
        InEvent.bPressed ? PendingOf(InUserData).Press(Key, InEvent.Player) : PendingOf(InUserData).Release(Key, InEvent.Player);
    }

    void OnPadAxis(const GamepadAxisEvent& InEvent, void* InUserData)
    {
        if(InEvent.Player >= INPUT_PLAYERS_MAX) { return; }
        auto* Router = static_cast<InputRouter*>(InUserData);

        /* Rescaled past the dead zone, so a full push still reads 1. */
        float Value = InEvent.Value;
        const bool  bStick = InEvent.Axis <= GA_RightY;
        const float Zone   = Router->GetStickDeadZone();
        if(bStick && Zone > 0.0f && Zone < 1.0f)
        {
            const float Magnitude = Value < 0.0f ? -Value : Value;
            Value = Magnitude <= Zone ? 0.0f : (Value < 0.0f ? -1.0f : 1.0f) * (Magnitude - Zone) / (1.0f - Zone);
        }
        Router->GetPending().SetAnalog(InputKeyFromPadAxis(InEvent.Axis), Value, InEvent.Player);
    }
}

InputRouter::~InputRouter()
{
    Detach();
}

GameWindow* InputRouter::GetWindow() const
{
    return WindowID != 0 ? GameWindowSystem::FindWindow(WindowID) : nullptr;
}

void InputRouter::Attach(GameWindow* InWindow)
{
    Detach();
    if(!InWindow) { return; }
    WindowID = InWindow->GetID();

    GameWindowDelegates Delegates;
    Delegates.UserData       = this;
    Delegates.OnKey          = &OnKey;
    Delegates.OnText         = &OnText;
    Delegates.OnMouseButton  = &OnMouseButton;
    Delegates.OnMouseMove    = &OnMouseMove;
    Delegates.OnMouseWheel   = &OnMouseWheel;
    Delegates.OnFocusChanged = &OnFocusChanged;
    InWindow->SetDelegates(Delegates);
    InWindow->SetTextInputActive(bTextInput);

    GamepadDelegates Pads;
    Pads.UserData    = this;
    Pads.OnConnected = &OnPadConnected;
    Pads.OnButton    = &OnPadButton;
    Pads.OnAxis      = &OnPadAxis;
    GameWindowSystem::SetGamepadDelegates(Pads);   // reports the pads already connected
    bOwnsGamepads = true;
}

void InputRouter::Detach()
{
    if(GameWindow* Window = GetWindow())
    {
        Window->SetRelativeMouseMode(false);
        Window->SetTextInputActive(false);
        Window->ClearDelegates();
    }
    if(bOwnsGamepads && GameWindowSystem::IsInitialized())
    {
        GameWindowSystem::ClearGamepadDelegates();
    }
    bOwnsGamepads = false;
    WindowID      = 0;
}

void InputRouter::SetMouseCaptured(const bool bInCaptured)
{
    if(GameWindow* Window = GetWindow()) { Window->SetRelativeMouseMode(bInCaptured); }
}

bool InputRouter::IsMouseCaptured() const
{
    const GameWindow* Window = GetWindow();
    return Window && Window->IsRelativeMouseMode();
}

void InputRouter::SetTextInputActive(const bool bInActive)
{
    bTextInput = bInActive;
    if(GameWindow* Window = GetWindow()) { Window->SetTextInputActive(bInActive); }
}

void InputRouter::Latch(const uint64_t InFrameNumber)
{
    Current = Pending;
    Current.FrameNumber = InFrameNumber;
    Pending.BeginFrame(InFrameNumber + 1);
}
}
