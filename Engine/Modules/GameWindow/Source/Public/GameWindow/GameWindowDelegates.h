#ifndef GAMEWINDOW_GAMEWINDOWDELEGATES_H
#define GAMEWINDOW_GAMEWINDOWDELEGATES_H

#include "Public/Core/CoreTypes.h"
#include "Public/GameWindow/GameWindowInput.h"
#include "Public/GameWindow/GameWindowTypes.h"

class GameWindow;

/*
 * The callback table a host installs on a window.
 *
 * WHY RAW FUNCTION POINTERS AND NOT std::function
 *
 * A std::function constructed in one image and destroyed in another is the
 * cross-heap free that CoreConfig.h and ModuleReloadState are both explicitly
 * designed around: it looks fine under /MD and becomes silent corruption the
 * moment somebody builds a module /MT. A void (*)(...) plus a void* has no
 * owner and no allocation, so the question does not arise. It is also exactly
 * the shape CoreLog.h's LogHandler already uses, so this seam reads like the
 * one next to it.
 *
 * WHY NOT A VIRTUAL IWindowListener
 *
 * A listener implemented inside a hot-reloadable module leaves a dangling
 * vtable in the window after unload -- the first hazard CoreModule.h documents.
 *
 * WHY ONE UserData FOR THE WHOLE TABLE
 *
 * A host needing a different context per callback wants a dispatcher object of
 * its own; per-slot void* would double this struct for a case nobody has.
 *
 * ======================= RELOAD HAZARD -- read this ========================
 *
 * A module that binds these in OnStartup with pointers into its OWN image
 * leaves the window holding pointers into unmapped memory after unload. The
 * next mouse move jumps into nothing, and the crash lands in the HOST's
 * PumpEvents, nowhere near the module that caused it.
 *
 *   Rule: whatever OnStartup binds, OnShutdown clears with ClearDelegates().
 *
 * GameWindowSystem::Shutdown warns about any window still holding a non-null
 * table at teardown. It cannot enforce the rule -- the same position
 * CoreModuleManager is in with GLogHandler -- but it will tell you.
 * ===========================================================================
 *
 * Every slot is nullable and null-checked at the dispatch site. Binding three
 * of these and leaving the rest at nullptr is the expected case.
 *
 * All slots are invoked on the pump thread, inside PumpEvents. A callback that
 * blocks stalls the window; hand expensive work to an
 * EngineCore::ThreadSafeQueue instead.
 */
struct GameWindowDelegates
{
    void* UserData = nullptr;

    // === Lifecycle and exit =================================================

    /*
     * The X button, Alt-F4, or the window manager asking.
     *
     * It is a REQUEST, and this is the only slot that returns a value, because
     * closing is the only event where the host's answer changes what happens
     * next:
     *
     *   false  -- veto. Nothing is marked, the window stays open. A host that
     *             wants an async "save your work?" prompt vetoes here and calls
     *             RequestClose() itself once the answer comes back.
     *   true   -- proceed. The window is marked and destroyed by the reap at
     *             the end of this PumpEvents, never inside this call.
     *
     * A null slot behaves as true: an unhandled close closes.
     */
    bool (*OnCloseRequested)(GameWindow*, void*) = nullptr;

    /*
     * Process-level quit: the last window went away, the OS is logging out, or
     * a console SIGINT arrived. Not per-window, so it carries no GameWindow*.
     */
    void (*OnQuit)(void*) = nullptr;

    // === Window state =======================================================

    /*
     * InPixelSize is PHYSICAL pixels -- what a swapchain must be sized to.
     *
     * EXPECT ONE OF THESE SHORTLY AFTER CREATION, before the user has touched
     * anything: SDL emits a pixel-size event for a newly created window, and it
     * arrives on the first PumpEvents after CreateGameWindow. That is convenient
     * if you build your swapchain lazily on first resize, and a double-create if
     * you build one eagerly and assume this only follows a user drag.
     *
     * Fires once per resize, not twice: SDL sends both RESIZED (logical) and
     * PIXEL_SIZE_CHANGED (physical), and only the physical one reaches here.
     */
    void (*OnResized)(GameWindow*, const EngineCore::Size& InPixelSize, void*) = nullptr;

    void (*OnMoved)(GameWindow*, const EngineCore::Point&, void*)     = nullptr;
    void (*OnMinimized)(GameWindow*, void*)                           = nullptr;
    void (*OnMaximized)(GameWindow*, void*)                           = nullptr;
    void (*OnRestored)(GameWindow*, void*)                            = nullptr;

    /* Keyboard focus. A game that pauses when it loses focus wants this one. */
    void (*OnFocusChanged)(GameWindow*, bool bInHasFocus, void*)      = nullptr;

    /* Mouse enter/leave, which is NOT the same as keyboard focus. */
    void (*OnMouseFocusChanged)(GameWindow*, bool bInHasMouse, void*) = nullptr;

    void (*OnWindowModeChanged)(GameWindow*, EWindowMode, void*)      = nullptr;

    /* The window moved to a display with a different scale, or the user changed
     * the scale on the current one. Physical size changes with it. */
    void (*OnDisplayScaleChanged)(GameWindow*, float InScale, void*)  = nullptr;

    // === Input ==============================================================
    // NOT YET DISPATCHED. The slots exist so the table's layout is settled, but
    // GameWindowDispatch.cpp handles window events only; input arrives at build
    // step 4. Binding these today is harmless and silent.

    void (*OnKey)(GameWindow*, const KeyEvent&, void*)                 = nullptr;

    /* InUtf8 is valid FOR THE DURATION OF THE CALL ONLY -- it points into SDL's
     * event. Copy it if you need to keep it. */
    void (*OnText)(GameWindow*, const char* InUtf8, void*)             = nullptr;

    void (*OnMouseButton)(GameWindow*, const MouseButtonEvent&, void*) = nullptr;
    void (*OnMouseMove)(GameWindow*, const MouseMoveEvent&, void*)     = nullptr;
    void (*OnMouseWheel)(GameWindow*, const MouseWheelEvent&, void*)   = nullptr;
};

#endif //GAMEWINDOW_GAMEWINDOWDELEGATES_H
