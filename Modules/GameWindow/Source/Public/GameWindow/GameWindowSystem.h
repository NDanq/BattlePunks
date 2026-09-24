#ifndef GAMEWINDOW_GAMEWINDOWSYSTEM_H
#define GAMEWINDOW_GAMEWINDOWSYSTEM_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/GameWindow/GameWindowAPI.h"
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
     * For a host whose OWN event loop dispatches the platform's messages -- a
     * Qt editor. Call before Initialize(). On Windows, SDL_PumpEvents()
     * normally runs the Win32 message loop itself; two loops on one thread
     * would each steal the other's messages, so with this set SDL's pump only
     * drains events SDL has already queued (SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP
     * = 0). Input still arrives: a window from WrapNativeWindow() has SDL's
     * window procedure chained in front of the host's, which turns each
     * message into an SDL event as the host's loop dispatches it.
     */
    GAMEWINDOW_API void SetHostOwnsMessageLoop(bool bInHostOwnsLoop);

    /* Null on failure, with the reason logged. */
    GAMEWINDOW_API GameWindow* CreateGameWindow(const WindowDesc& InDesc);

    /*
     * A GameWindow over a native window someone else created and still owns --
     * an editor viewport. SDL chains its window procedure in front of the
     * owner's (SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER; the owner's procedure
     * still sees every message after SDL), so the window's input reaches the
     * same GameWindowDelegates a window from CreateGameWindow would fire.
     *
     * DestroyGameWindow() detaches SDL and restores the owner's procedure; it
     * never destroys the native window. Call it before the owner does.
     *
     * Windows only for now (null, with an error logged, elsewhere). Null on
     * failure, with the reason logged.
     */
    GAMEWINDOW_API GameWindow* WrapNativeWindow(EngineCore::WindowHandle InNativeWindow);

    /*
     * Queues InUtf8 as text input on InWindow, delivered by the next
     * PumpEvents() through OnText exactly like typed text. For a host whose
     * toolkit CONSUMES character messages before they reach the wrapped
     * window: Qt pulls each WM_CHAR off the queue while translating the
     * WM_KEYDOWN before it and never dispatches it, so SDL's window procedure
     * never sees one. The host forwards the characters it saw; nothing else
     * about text changes. Ignored unless text input is active on InWindow
     * (SetTextInputActive), and for control characters -- SDL's own rules.
     */
    GAMEWINDOW_API void InjectTextInput(GameWindow* InWindow, const char* InUtf8);

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

    /*
     * The directory holding the running executable, with a trailing separator
     * -- SDL_GetBasePath(). Empty if the platform cannot say. Valid before
     * Initialize(): SDL answers this without SDL_Init, which is what lets a
     * host fix its working directory before reading any config.
     */
    GAMEWINDOW_API std::string GetExecutableDir();
}

#endif //GAMEWINDOW_GAMEWINDOWSYSTEM_H
