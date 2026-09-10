#ifndef GAMEWINDOW_GAMEWINDOWSYSTEM_H
#define GAMEWINDOW_GAMEWINDOWSYSTEM_H

#include <cstddef>
#include <cstdint>

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

    /* Null on failure, with the reason logged. */
    GAMEWINDOW_API GameWindow* CreateGameWindow(const WindowDesc& InDesc);

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
}

#endif //GAMEWINDOW_GAMEWINDOWSYSTEM_H
