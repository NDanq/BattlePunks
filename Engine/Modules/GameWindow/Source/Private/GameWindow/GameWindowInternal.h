#ifndef GAMEWINDOW_GAMEWINDOWINTERNAL_H
#define GAMEWINDOW_GAMEWINDOWINTERNAL_H

#include <SDL3/SDL.h>

#include "Public/Core/CorePlatform.h"
#include "Public/GameWindow/GameWindow.h"
#include "Public/GameWindow/GameWindowLog.h"

/*
 * The module's private half: GameWindow::Impl, and the one friend that is
 * allowed to reach into it.
 *
 * This is the only header in the module that includes SDL.h, and it lives under
 * Private/ so it cannot be reached from outside. That is what makes the
 * PRIVATE link of SDL3::SDL3 in CMakeLists.txt true rather than aspirational.
 */

struct GameWindow::Impl
{
    SDL_Window* Window = nullptr;
    uint32_t    ID     = 0;

    GameWindowDelegates Delegates;

    /* Cached state. Written by the dispatch, read by the accessors -- see the
     * note on GameWindow's geometry section for why these are not live SDL
     * queries. */
    EngineCore::Size  PixelSize;
    EngineCore::Size  LogicalSize;
    EngineCore::Point Position;
    float             DisplayScale = 1.0f;

    /*
     * Physical pixels per logical unit -- NOT the same as DisplayScale.
     *
     * DisplayScale is the user's content/UI scale (SDL_GetWindowDisplayScale);
     * this is the framebuffer-to-window ratio (SDL_GetWindowPixelDensity).
     * They are equal on a lot of setups and differ on exactly the ones that
     * matter, which is why both are cached rather than one being derived from
     * the other.
     *
     * SDL reports mouse coordinates in logical window units. The engine's mouse
     * events promise PHYSICAL pixels, matching GDIPlatform::appGetWindowPixelSize
     * and anything sized against the swapchain, so every coordinate is scaled by
     * this on the way through.
     */
    float             PixelDensity = 1.0f;

    EWindowMode Mode = WM_Windowed;

    /* What ToggleFullscreen() goes back to. Seeded from the WindowDesc so a
     * window created fullscreen toggles back into the mode it started in. */
    EWindowMode LastFullscreenMode = WM_FullscreenDesktop;

    bool bMinimized      = false;
    bool bMaximized      = false;
    bool bFocused        = false;
    bool bMouseFocus     = false;
    bool bVisible        = false;
    bool bCloseRequested = false;
};

/*
 * Named as GameWindow.h's friend declaration. Everything that needs at a
 * window's internals from outside its own member functions goes through here,
 * so the friendship is one class wide rather than one per implementation file.
 */
class GameWindowSystem_Impl
{
public:
    static GameWindow* Construct() { return new GameWindow(); }
    static void        Destruct(GameWindow* InWindow) { delete InWindow; }

    static GameWindow::Impl&       Get(GameWindow& InWindow)             { return *InWindow.P; }
    static const GameWindow::Impl& Get(const GameWindow& InWindow)       { return *InWindow.P; }

    /* Pulls every cached field back from SDL. Called after creation and after
     * any event that could have invalidated more than it reports. */
    static void RefreshCachedState(GameWindow& InWindow);

    /* GameWindowDispatch.cpp. Each routes one SDL event to the delegates. */
    static void DispatchWindowEvent(GameWindow& InWindow, const SDL_WindowEvent& InEvent);
    static void DispatchKeyEvent(GameWindow& InWindow, const SDL_KeyboardEvent& InEvent);
    static void DispatchTextEvent(GameWindow& InWindow, const SDL_TextInputEvent& InEvent);
    static void DispatchMouseButtonEvent(GameWindow& InWindow, const SDL_MouseButtonEvent& InEvent);
    static void DispatchMouseMotionEvent(GameWindow& InWindow, const SDL_MouseMotionEvent& InEvent);
    static void DispatchMouseWheelEvent(GameWindow& InWindow, const SDL_MouseWheelEvent& InEvent);
};

/*
 * SDL input codes -> the engine's own. GameWindowInputSDL.cpp.
 *
 * Free functions rather than members: they touch no window state, and keeping
 * them reachable without a GameWindow is what lets the smoke test check the
 * tables directly.
 */
EKeyCode     GameWindowKeyFromScancode(SDL_Scancode InScancode);
uint16_t     GameWindowModifiersFromSDL(SDL_Keymod InMod);

/* MB_MAX for a button this engine does not model -- a 6-button mouse reports
 * one, and the dispatch drops it rather than calling it a left click. */
EMouseButton GameWindowMouseButtonFromSDL(uint8_t InButton);

/*
 * The native OS handle behind an SDL_Window -- an HWND on Windows, an NSView*
 * on macOS. Defined by exactly one file under Private/GameWindow/Platform/,
 * chosen by CMakeLists.txt, so a port adds a file and changes nothing else.
 */
void* GameWindowGetNativeHandle(SDL_Window* InWindow);

namespace GameWindowSystem
{
    /*
     * CORE_CHECKs that the caller is on the thread that called Initialize.
     *
     * SDL requires window calls and event pumping on one thread and macOS
     * requires it be the main thread; the cached state is also written by the
     * pump with no synchronisation. InWhat names the entry point so the fatal
     * message says which call was wrong rather than just that one was.
     */
    void CheckPumpThread(const char* InWhat);

    /* Owner-thread id, for the assert above. */
    EngineCore::ThreadID GetPumpThread();

    /* GameWindowGamepad.cpp: the gamepad subsystem, started and stopped with
     * SDL, and its events (true if InEvent was one). */
    void StartGamepads();
    void StopGamepads();
    bool RouteGamepadEvent(const SDL_Event& InEvent);
}

#endif //GAMEWINDOW_GAMEWINDOWINTERNAL_H
