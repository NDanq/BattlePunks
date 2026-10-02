#include "Public/NimbleEngine/InputRouter.h"

#include "Private/NimbleEngine/InputInternal.h"
#include "Public/GameWindow/GameWindow.h"
#include "Public/GameWindow/GameWindowDelegates.h"
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
        if(InEvent.bRepeat) { return; }   // OS auto-repeat is not a press
        const InputKey Key = InputKeyFromKeyCode(InEvent.Key);
        InEvent.bPressed ? PendingOf(InUserData).Press(Key) : PendingOf(InUserData).Release(Key);
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
    Delegates.OnMouseButton  = &OnMouseButton;
    Delegates.OnMouseMove    = &OnMouseMove;
    Delegates.OnMouseWheel   = &OnMouseWheel;
    Delegates.OnFocusChanged = &OnFocusChanged;
    InWindow->SetDelegates(Delegates);
}

void InputRouter::Detach()
{
    if(GameWindow* Window = GetWindow()) { Window->ClearDelegates(); }
    WindowID = 0;
}

void InputRouter::Latch(const uint64_t InFrameNumber)
{
    Current = Pending;
    Current.FrameNumber = InFrameNumber;
    Pending.BeginFrame(InFrameNumber + 1);
}
}
