#ifndef GAMEWINDOW_GAMEWINDOW_H
#define GAMEWINDOW_GAMEWINDOW_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameWindow/GameWindowAPI.h"
#include "Public/GameWindow/GameWindowDelegates.h"
#include "Public/GameWindow/GameWindowTypes.h"

/*
 * One operating-system window.
 *
 * Constructed only by GameWindowSystem::CreateGameWindow and destroyed only by
 * DestroyGameWindow or the reap inside PumpEvents -- the constructor is private
 * for that reason. A GameWindow* stays valid until one of those runs.
 *
 * THREAD AFFINITY. Every member here must be called on the thread that called
 * GameWindowSystem::Initialize. SDL requires it, macOS requires it be the main
 * thread, and the accessors are cached reads that the pump thread writes.
 */
class GAMEWINDOW_API GameWindow
{
public:
    ~GameWindow();

    /* Owns an SDL_Window; copying would double-free it. */
    GameWindow(const GameWindow&)            = delete;
    GameWindow& operator=(const GameWindow&) = delete;

    // === Identity ===========================================================

    /* The SDL window id -- the key events are routed by. Never 0 on a live
     * window, which is what makes 0 usable as "no window" in the registry. */
    uint32_t GetID() const;

    /*
     * The NATIVE OS handle: an HWND on Windows, an NSView* on macOS. This is
     * what goes into InitializeGDI().
     *
     * NOT the SDL_Window*. GDIPlatformWindows.cpp's ToHWND() does an unguarded
     * reinterpret_cast<HWND>(Native) and every GL context and Vulkan surface
     * call in that file routes through it, so an SDL_Window* stored here
     * compiles clean, passes IsValid(), and dies inside wglCreateContext with
     * nothing pointing back here. CoreTypes.h says the same thing from the
     * other side.
     */
    EngineCore::WindowHandle GetNativeHandle() const;

    /*
     * The SDL_Window*, for this module's own platform code and for a host that
     * genuinely needs SDL (SDL_Vulkan_CreateSurface).
     *
     * void* so that no public header here includes SDL.h -- which is what keeps
     * SDL a private link dependency of this one target. Callers that want it
     * cast it themselves and thereby opt into the dependency explicitly.
     */
    void* GetSDLWindow() const;

    // === Geometry ===========================================================
    //
    // These are CACHED, updated by the event dispatch, not live SDL queries. A
    // renderer asks for the size every frame and a syscall per frame per query
    // is what shows up in a profile six months later. The cached value is the
    // authoritative one anyway: it is what the last event reported, which is
    // what the host reacted to.

    /* PHYSICAL pixels -- size a swapchain and a viewport from this one. */
    EngineCore::Size GetPixelSize() const;

    /* Logical (scaled) units -- what SDL calls the window size. For placing UI
     * against OS metrics, not for sizing a framebuffer. */
    EngineCore::Size GetLogicalSize() const;

    float GetDisplayScale() const;

    EngineCore::Point GetPosition() const;

    void SetTitle(const char* InTitle);
    void SetSize(int InWidth, int InHeight);   // logical units
    void SetPosition(int InX, int InY);
    void Center();

    // === State ==============================================================

    bool IsMinimized() const;
    bool IsMaximized() const;
    bool HasFocus()    const;
    bool IsVisible()   const;

    void Show();
    void Hide();
    void Minimize();
    void Maximize();
    void Restore();
    void Raise();

    EWindowMode GetWindowMode() const;

    /*
     * False if the platform refused the switch. The cached mode is NOT updated
     * here -- it changes when SDL reports ENTER/LEAVE_FULLSCREEN, so
     * GetWindowMode() and OnWindowModeChanged always agree, and a mode change
     * the compositor initiated (a user hitting the OS fullscreen shortcut)
     * reads identically to one this call caused.
     */
    bool SetWindowMode(EWindowMode InMode);

    /* Windowed <-> whichever fullscreen mode was last used. */
    bool ToggleFullscreen();

    // === Text input =========================================================

    /*
     * Gates OnText and the platform IME. OFF by default.
     *
     * This is not an optimisation, it is the whole reason OnText can look
     * broken: with text input inactive SDL emits no SDL_EVENT_TEXT_INPUT at
     * all, so a bound OnText simply never fires and nothing says why. A host
     * turns it on while a field has focus and off again afterwards -- leaving
     * it on permanently also leaves the on-screen keyboard up on platforms that
     * have one.
     *
     * OnKey is unaffected and fires either way.
     */
    void SetTextInputActive(bool bInActive);
    bool IsTextInputActive() const;

    // === Exit ===============================================================

    /*
     * Marks the window for destruction. Does NOT destroy it here.
     *
     * Destroying inside a callback would free the object the dispatch loop is
     * standing on, and the loop still holds the pointer. PumpEvents reaps
     * marked windows after the event queue drains, which is the only point at
     * which nothing is standing on them.
     */
    void RequestClose();
    bool IsCloseRequested() const;

    // === Callbacks ==========================================================

    void SetDelegates(const GameWindowDelegates& InDelegates);
    const GameWindowDelegates& GetDelegates() const;

    /* Restores the empty table. A hot-reloadable module MUST call this from
     * OnShutdown -- see the reload hazard in GameWindowDelegates.h. */
    void ClearDelegates();

private:
    friend class GameWindowSystem_Impl;

    GameWindow();

    /* Pimpl: no SDL types here, and a stable layout across the DLL boundary. */
    struct Impl;
    Impl* P;
};

#endif //GAMEWINDOW_GAMEWINDOW_H
