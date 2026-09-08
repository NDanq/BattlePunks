#include <cmath>

#include "Private/GameWindow/GameWindowInternal.h"

namespace
{
    /*
     * SDL reports mouse position in LOGICAL window units; the engine's mouse
     * events promise PHYSICAL pixels, so everything goes through here.
     *
     * On a 1x display this is the identity and the whole thing is invisible.
     * On a 2x display, skipping it means every hit test is out by a factor of
     * two against a swapchain sized in physical pixels -- and it reproduces on
     * nobody's machine but the one with the scaled monitor.
     *
     * std::lround, not a truncating cast: truncation biases every coordinate
     * toward the top-left by up to a pixel, which is visible in a paint tool
     * and infuriating to track down.
     *
     * Takes the density rather than the Impl because GameWindow::Impl is a
     * private member type and this is not the friend class -- the same access
     * rule that bites in GameWindowSystem::CreateGameWindow.
     */
    EngineCore::Point ToPhysical(const float InDensity, const float InX, const float InY)
    {
        return EngineCore::Point(int(std::lround(InX * InDensity)),
                                 int(std::lround(InY * InDensity)));
    }
}

/*
 * SDL_EVENT_WINDOW_* -> GameWindowDelegates.
 *
 * The whole switch lives here, alone, so that adding an event is a change to
 * one file and so the translation is readable as a table rather than buried in
 * the pump.
 *
 * TWO RULES HOLD THROUGHOUT:
 *
 *   1. Cached state is updated BEFORE the callback fires. A host that asks
 *      Window->GetPixelSize() from inside OnResized must see the new size, not
 *      the previous one -- that is the entire reason the callback is useful.
 *
 *   2. Every slot is null-checked. A host binds what it needs and leaves the
 *      rest nullptr; that is the expected case, not a degraded one.
 */

void GameWindowSystem_Impl::DispatchWindowEvent(GameWindow& InWindow, const SDL_WindowEvent& InEvent)
{
    GameWindow::Impl&          I = Get(InWindow);
    const GameWindowDelegates& D = I.Delegates;

    switch(InEvent.type)
    {
        // === Visibility =====================================================

        case SDL_EVENT_WINDOW_SHOWN:
            I.bVisible = true;
            break;

        case SDL_EVENT_WINDOW_HIDDEN:
            I.bVisible = false;
            break;

        // === Geometry =======================================================

        case SDL_EVENT_WINDOW_MOVED:
            I.Position = EngineCore::Point(InEvent.data1, InEvent.data2);
            if(D.OnMoved)
            {
                D.OnMoved(&InWindow, I.Position, D.UserData);
            }
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            /*
             * Logical size only. OnResized deliberately does NOT fire here:
             * PIXEL_SIZE_CHANGED is the authoritative physical-size event and
             * arrives alongside this one, so firing on both would call a host's
             * swapchain rebuild twice per resize.
             */
            I.LogicalSize = EngineCore::Size(InEvent.data1, InEvent.data2);
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            I.PixelSize = EngineCore::Size(InEvent.data1, InEvent.data2);
            if(D.OnResized)
            {
                D.OnResized(&InWindow, I.PixelSize, D.UserData);
            }
            break;

        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            /*
             * Scale is not in data1/data2, and the physical size moves with it,
             * so this is one of the cases that re-reads everything rather than
             * trusting the event's payload.
             */
            RefreshCachedState(InWindow);
            if(D.OnDisplayScaleChanged)
            {
                D.OnDisplayScaleChanged(&InWindow, I.DisplayScale, D.UserData);
            }
            break;

        // === Window state ===================================================

        case SDL_EVENT_WINDOW_MINIMIZED:
            I.bMinimized = true;
            I.bMaximized = false;
            if(D.OnMinimized)
            {
                D.OnMinimized(&InWindow, D.UserData);
            }
            break;

        case SDL_EVENT_WINDOW_MAXIMIZED:
            I.bMaximized = true;
            I.bMinimized = false;
            if(D.OnMaximized)
            {
                D.OnMaximized(&InWindow, D.UserData);
            }
            break;

        case SDL_EVENT_WINDOW_RESTORED:
            I.bMinimized = false;
            I.bMaximized = false;
            if(D.OnRestored)
            {
                D.OnRestored(&InWindow, D.UserData);
            }
            break;

        // === Focus ==========================================================

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            I.bFocused = true;
            if(D.OnFocusChanged)
            {
                D.OnFocusChanged(&InWindow, true, D.UserData);
            }
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            I.bFocused = false;
            if(D.OnFocusChanged)
            {
                D.OnFocusChanged(&InWindow, false, D.UserData);
            }
            break;

        /* Mouse focus is NOT keyboard focus: the pointer can be over a window
         * that does not have input focus, and a hover highlight wants this one
         * while a pause-on-focus-loss wants the pair above. */
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            I.bMouseFocus = true;
            if(D.OnMouseFocusChanged)
            {
                D.OnMouseFocusChanged(&InWindow, true, D.UserData);
            }
            break;

        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            I.bMouseFocus = false;
            if(D.OnMouseFocusChanged)
            {
                D.OnMouseFocusChanged(&InWindow, false, D.UserData);
            }
            break;

        // === Fullscreen =====================================================
        //
        // The cached mode changes HERE and nowhere else -- SetWindowMode
        // deliberately does not set it. So GetWindowMode() and
        // OnWindowModeChanged can never disagree, and a switch the compositor
        // initiated (the user hitting the OS fullscreen shortcut) is
        // indistinguishable from one the engine asked for, which is what a host
        // actually wants.

        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            I.Mode = I.LastFullscreenMode;
            RefreshCachedState(InWindow);
            if(D.OnWindowModeChanged)
            {
                D.OnWindowModeChanged(&InWindow, I.Mode, D.UserData);
            }
            break;

        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            I.Mode = WM_Windowed;
            RefreshCachedState(InWindow);
            if(D.OnWindowModeChanged)
            {
                D.OnWindowModeChanged(&InWindow, WM_Windowed, D.UserData);
            }
            break;

        // === Close ==========================================================

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            /*
             * Returning false vetoes: nothing is marked, the window stays.
             * Returning true, or leaving the slot null, marks it -- the reap at
             * the end of PumpEvents destroys it. A host that wants to run an
             * async "save your work?" prompt vetoes here and calls
             * RequestClose() itself when the answer comes back.
             *
             * The window is NOT destroyed inside this call. It cannot be: the
             * dispatch loop is standing on it.
             */
            if(D.OnCloseRequested && !D.OnCloseRequested(&InWindow, D.UserData))
            {
                break;
            }
            InWindow.RequestClose();
            break;

        /*
         * SDL_EVENT_WINDOW_DESTROYED is not handled: this module owns every
         * window it knows about and destroys them itself, so the only way to
         * see one would be somebody calling SDL_DestroyWindow behind the
         * registry's back.
         */
        default:
            break;
    }
}

// ===========================================================================
// Input
// ===========================================================================

void GameWindowSystem_Impl::DispatchKeyEvent(GameWindow& InWindow, const SDL_KeyboardEvent& InEvent)
{
    const GameWindowDelegates& D = Get(InWindow).Delegates;
    if(!D.OnKey)
    {
        return;
    }

    KeyEvent Out;
    Out.Key       = GameWindowKeyFromScancode(InEvent.scancode);
    Out.Modifiers = GameWindowModifiersFromSDL(InEvent.mod);
    Out.bPressed  = InEvent.down;
    Out.bRepeat   = InEvent.repeat;

    /*
     * KC_Unknown is still delivered rather than dropped. A key this build has
     * no name for is a real press the host may want to see -- media keys,
     * international keys, a vendor's extra row -- and swallowing it would make
     * "my keyboard has a key that does nothing" undebuggable from the outside.
     */
    D.OnKey(&InWindow, Out, D.UserData);
}

void GameWindowSystem_Impl::DispatchTextEvent(GameWindow& InWindow, const SDL_TextInputEvent& InEvent)
{
    const GameWindowDelegates& D = Get(InWindow).Delegates;
    if(!D.OnText || !InEvent.text)
    {
        return;
    }

    /*
     * InEvent.text points into SDL's event storage and is valid only until the
     * next pump iteration -- which is exactly the lifetime the slot documents.
     * Nothing is copied here on purpose: a host that needs to keep the text
     * copies it, and a host that appends it to a buffer (the common case) does
     * not pay for an allocation per keystroke.
     */
    D.OnText(&InWindow, InEvent.text, D.UserData);
}

void GameWindowSystem_Impl::DispatchMouseButtonEvent(GameWindow& InWindow, const SDL_MouseButtonEvent& InEvent)
{
    GameWindow::Impl&          I = Get(InWindow);
    const GameWindowDelegates& D = I.Delegates;
    if(!D.OnMouseButton)
    {
        return;
    }

    const EMouseButton Button = GameWindowMouseButtonFromSDL(InEvent.button);
    if(Button == MB_MAX)
    {
        /* A button this engine does not model. Dropped rather than reported as
         * some other button, which would be worse than silence. */
        return;
    }

    MouseButtonEvent Out;
    Out.Button     = Button;
    Out.Pos        = ToPhysical(I.PixelDensity, InEvent.x, InEvent.y);
    Out.ClickCount = InEvent.clicks;
    Out.bPressed   = InEvent.down;
    Out.Modifiers  = GameWindowModifiersFromSDL(SDL_GetModState());

    D.OnMouseButton(&InWindow, Out, D.UserData);
}

void GameWindowSystem_Impl::DispatchMouseMotionEvent(GameWindow& InWindow, const SDL_MouseMotionEvent& InEvent)
{
    GameWindow::Impl&          I = Get(InWindow);
    const GameWindowDelegates& D = I.Delegates;
    if(!D.OnMouseMove)
    {
        return;
    }

    const EngineCore::Point Delta = ToPhysical(I.PixelDensity, InEvent.xrel, InEvent.yrel);

    MouseMoveEvent Out;
    Out.Pos       = ToPhysical(I.PixelDensity, InEvent.x, InEvent.y);
    Out.DeltaX    = Delta.X;
    Out.DeltaY    = Delta.Y;
    Out.Modifiers = GameWindowModifiersFromSDL(SDL_GetModState());

    D.OnMouseMove(&InWindow, Out, D.UserData);
}

void GameWindowSystem_Impl::DispatchMouseWheelEvent(GameWindow& InWindow, const SDL_MouseWheelEvent& InEvent)
{
    GameWindow::Impl&          I = Get(InWindow);
    const GameWindowDelegates& D = I.Delegates;
    if(!D.OnMouseWheel)
    {
        return;
    }

    /*
     * SDL_MOUSEWHEEL_FLIPPED means the platform already inverted the values
     * (macOS "natural" scrolling). Undoing it here means a host always sees
     * "positive Y is away from the user" and never has to ask which convention
     * it is looking at.
     */
    const float Sign = (InEvent.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;

    MouseWheelEvent Out;
    Out.DeltaX    = InEvent.x * Sign;
    Out.DeltaY    = InEvent.y * Sign;
    Out.Pos       = ToPhysical(I.PixelDensity, InEvent.mouse_x, InEvent.mouse_y);
    Out.Modifiers = GameWindowModifiersFromSDL(SDL_GetModState());

    D.OnMouseWheel(&InWindow, Out, D.UserData);
}
