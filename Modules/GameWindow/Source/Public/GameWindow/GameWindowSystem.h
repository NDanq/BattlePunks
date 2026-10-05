#ifndef GAMEWINDOW_GAMEWINDOWSYSTEM_H
#define GAMEWINDOW_GAMEWINDOWSYSTEM_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/GameWindow/GameWindowAPI.h"
#include "Public/GameWindow/GameWindowGamepad.h"
#include "Public/GameWindow/GameWindowTypes.h"

class GameWindow;

/*
 * SDL lifetime, the window registry, and the event pump.
 *
 * ============================ THREAD CONTRACT =============================
 *
 * SDL requires that windows be created and events pumped on the SAME thread,
 * and on macOS that thread must be the process's main thread. Initialize()
 * records EngineCore::appThreadID(), and every entry point here and on
 * GameWindow asserts against it with CORE_CHECK.
 *
 * The consequence that matters to a host: DELEGATES ARE INVOKED ON THIS
 * THREAD, INSIDE PumpEvents. A callback that blocks stalls the window. Hand
 * expensive work to an EngineCore::ThreadSafeQueue -- already in Core, already
 * how GDI's GRenderCommands crosses threads -- and drain it wherever the work
 * belongs.
 * ==========================================================================
 */
namespace GameWindowSystem
{
    /*
     * SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS).
     *
     * Refcounted and idempotent: two subsystems may each Initialize/Shutdown
     * without either one pulling SDL out from under the other. False if SDL
     * refused, with SDL_GetError() already logged.
     */
    GAMEWINDOW_API bool Initialize();

    /* Destroys every live window, then drops the refcount. SDL is only really
     * torn down when the last holder shuts down. */
    GAMEWINDOW_API void Shutdown();

    GAMEWINDOW_API bool IsInitialized();

    /*
     * For a host whose own loop dispatches platform messages (Qt). Call before
     * Initialize(). SDL's pump then only drains already-queued events instead
     * of running a second Win32 loop; wrapped windows still feed SDL through
     * the chained window procedure.
     */
    GAMEWINDOW_API void SetHostOwnsMessageLoop(bool bInHostOwnsLoop);

    /* Null on failure, with the reason logged. */
    GAMEWINDOW_API GameWindow* CreateGameWindow(const WindowDesc& InDesc);

    /*
     * A GameWindow over a native window owned by someone else (an editor
     * viewport). SDL chains its window procedure ahead of the owner's, so
     * input reaches the usual GameWindowDelegates. DestroyGameWindow()
     * restores the owner's procedure without destroying the window; call it
     * first. Windows only. Null on failure, logged.
     */
    GAMEWINDOW_API GameWindow* WrapNativeWindow(EngineCore::WindowHandle InNativeWindow);

    /*
     * Queues InUtf8 as text input, delivered by the next PumpEvents() via
     * OnText. For hosts that swallow WM_CHAR (Qt does). Ignored unless text
     * input is active on InWindow, and for control characters.
     */
    GAMEWINDOW_API void InjectTextInput(GameWindow* InWindow, const char* InUtf8);

    /* The system clipboard's text (UTF-8): "" when it holds none. */
    GAMEWINDOW_API std::string GetClipboardText();
    GAMEWINDOW_API void        SetClipboardText(const std::string& InUtf8);

    /*
     * Gamepads (GameWindowGamepad.h). One process-level table -- a pad is a
     * player's, not a window's. Setting it reports the pads already connected.
     * Clear it before whatever UserData points at goes away.
     */
    GAMEWINDOW_API void SetGamepadDelegates(const GamepadDelegates& InDelegates);
    GAMEWINDOW_API void ClearGamepadDelegates();
    GAMEWINDOW_API bool IsGamepadConnected(uint8_t InPlayer);

    /*
     * Immediate destruction. Safe from host code between frames; NOT safe from
     * inside a delegate -- use GameWindow::RequestClose() there, which defers
     * to the reap at the end of PumpEvents.
     */
    GAMEWINDOW_API void DestroyGameWindow(GameWindow* InWindow);

    /*
     * Drain SDL's queue, route each event to its window's delegates, then reap
     * the windows RequestClose() marked. Call once per frame.
     *
     * Returns false when the application should exit -- OnQuit fired, or the
     * last window closed. That return value IS the game loop's condition;
     * there is no separate ShouldQuit() to forget to check:
     *
     *     while(GameWindowSystem::PumpEvents()) { Render(); }
     *
     * Calling this before any window exists returns true, not false: "the last
     * window closed" cannot be true before there was a first one, and the
     * alternative makes a host that pumps during startup exit instantly.
     */
    GAMEWINDOW_API bool PumpEvents();

    /*
     * PumpEvents, but blocks up to InTimeoutMilliseconds for the first event
     * rather than spinning. For a tool window or a paused editor that should
     * not burn a core to sit still. A zero timeout is exactly PumpEvents.
     */
    GAMEWINDOW_API bool WaitEvents(int InTimeoutMilliseconds);

    /* Null if no live window has that id. */
    GAMEWINDOW_API GameWindow* FindWindow(uint32_t InWindowID);

    /* The first window created and still alive, or null. The single-window case
     * without making single-window an assumption in the design. */
    GAMEWINDOW_API GameWindow* GetPrimaryWindow();

    GAMEWINDOW_API size_t GetWindowCount();

    /* The executable's directory with a trailing separator; empty if unknown.
     * Valid before Initialize(). */
    GAMEWINDOW_API std::string GetExecutableDir();
}

#endif //GAMEWINDOW_GAMEWINDOWSYSTEM_H
