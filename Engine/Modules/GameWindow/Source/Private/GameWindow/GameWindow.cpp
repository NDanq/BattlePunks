#include "Private/GameWindow/GameWindowInternal.h"

/*
 * GameWindow -- the per-window half. GameWindowSystem.cpp owns creation,
 * destruction and the pump; GameWindowDispatch.cpp owns event translation.
 *
 * Every accessor here reads cached state. Only the mutators talk to SDL, and
 * they are the ones that assert the pump thread: a cross-thread SetTitle is a
 * real SDL violation, while a cross-thread GetPixelSize is a plain data race
 * that an assert in a per-frame getter would not be worth catching.
 */

GameWindow::GameWindow()
    : P(new Impl())
{
}

GameWindow::~GameWindow()
{
    if(P)
    {
        /* Ownership ends here, in one place, whether the window was destroyed
         * explicitly or reaped after RequestClose(). */
        if(P->Window)
        {
            SDL_DestroyWindow(P->Window);
            P->Window = nullptr;
        }

        delete P;
        P = nullptr;
    }
}

// === Identity ===============================================================

uint32_t GameWindow::GetID() const
{
    return P->ID;
}

EngineCore::WindowHandle GameWindow::GetNativeHandle() const
{
    if(!P->Window)
    {
        return EngineCore::WindowHandle();
    }

    return EngineCore::WindowHandle(GameWindowGetNativeHandle(P->Window));
}

void* GameWindow::GetSDLWindow() const
{
    return P->Window;
}

// === Geometry ===============================================================

EngineCore::Size GameWindow::GetPixelSize() const
{
    return P->PixelSize;
}

EngineCore::Size GameWindow::GetLogicalSize() const
{
    return P->LogicalSize;
}

float GameWindow::GetDisplayScale() const
{
    return P->DisplayScale;
}

EngineCore::Point GameWindow::GetPosition() const
{
    return P->Position;
}

void GameWindow::SetTitle(const char* InTitle)
{
    GameWindowSystem::CheckPumpThread("GameWindow::SetTitle");

    if(P->Window)
    {
        SDL_SetWindowTitle(P->Window, InTitle ? InTitle : "");
    }
}

void GameWindow::SetSize(const int InWidth, const int InHeight)
{
    GameWindowSystem::CheckPumpThread("GameWindow::SetSize");

    if(P->Window)
    {
        SDL_SetWindowSize(P->Window, InWidth, InHeight);
    }
}

void GameWindow::SetPosition(const int InX, const int InY)
{
    GameWindowSystem::CheckPumpThread("GameWindow::SetPosition");

    if(P->Window)
    {
        SDL_SetWindowPosition(P->Window, InX, InY);
    }
}

void GameWindow::Center()
{
    GameWindowSystem::CheckPumpThread("GameWindow::Center");

    if(P->Window)
    {
        SDL_SetWindowPosition(P->Window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

// === State ==================================================================

bool GameWindow::IsMinimized() const { return P->bMinimized; }
bool GameWindow::IsMaximized() const { return P->bMaximized; }
bool GameWindow::HasFocus()    const { return P->bFocused;   }
bool GameWindow::IsVisible()   const { return P->bVisible;   }

void GameWindow::Show()
{
    GameWindowSystem::CheckPumpThread("GameWindow::Show");
    if(P->Window) { SDL_ShowWindow(P->Window); }
}

void GameWindow::Hide()
{
    GameWindowSystem::CheckPumpThread("GameWindow::Hide");
    if(P->Window) { SDL_HideWindow(P->Window); }
}

void GameWindow::Minimize()
{
    GameWindowSystem::CheckPumpThread("GameWindow::Minimize");
    if(P->Window) { SDL_MinimizeWindow(P->Window); }
}

void GameWindow::Maximize()
{
    GameWindowSystem::CheckPumpThread("GameWindow::Maximize");
    if(P->Window) { SDL_MaximizeWindow(P->Window); }
}

void GameWindow::Restore()
{
    GameWindowSystem::CheckPumpThread("GameWindow::Restore");
    if(P->Window) { SDL_RestoreWindow(P->Window); }
}

void GameWindow::Raise()
{
    GameWindowSystem::CheckPumpThread("GameWindow::Raise");
    if(P->Window) { SDL_RaiseWindow(P->Window); }
}

EWindowMode GameWindow::GetWindowMode() const
{
    return P->Mode;
}

bool GameWindow::SetWindowMode(const EWindowMode InMode)
{
    GameWindowSystem::CheckPumpThread("GameWindow::SetWindowMode");

    if(InMode >= WM_MAX || !P->Window)
    {
        return false;
    }

    /*
     * WM_FullscreenExclusive is accepted but behaves as WM_FullscreenDesktop
     * for now. Telling the two apart needs SDL_SetWindowFullscreenMode and a
     * chosen SDL_DisplayMode, which is build step 5 -- and silently doing
     * borderless when asked for exclusive is the honest half-implementation:
     * the window is fullscreen either way, it just has not changed the display
     * mode. The cached Mode below still records what was ASKED for, so step 5
     * has the information it needs and nothing has to be re-plumbed.
     */
    const bool bWantFullscreen = (InMode != WM_Windowed);

    if(!SDL_SetWindowFullscreen(P->Window, bWantFullscreen))
    {
        GW_LOG_WARN(std::string("SetWindowMode refused by the platform: ") + SDL_GetError());
        return false;
    }

    if(bWantFullscreen)
    {
        P->LastFullscreenMode = InMode;
    }

    /*
     * Deliberately NOT setting P->Mode here. It changes when SDL reports
     * ENTER/LEAVE_FULLSCREEN, so GetWindowMode() and OnWindowModeChanged can
     * never disagree, and a mode change the compositor initiated -- a user
     * hitting the OS fullscreen shortcut -- looks identical to one this call
     * caused. Setting it optimistically here would make a REFUSED-but-reported-
     * success switch leave the cache lying.
     */
    return true;
}

bool GameWindow::ToggleFullscreen()
{
    return SetWindowMode(P->Mode == WM_Windowed ? P->LastFullscreenMode : WM_Windowed);
}

// === Text input =============================================================

void GameWindow::SetTextInputActive(const bool bInActive)
{
    GameWindowSystem::CheckPumpThread("GameWindow::SetTextInputActive");

    if(!P->Window)
    {
        return;
    }

    const bool bOk = bInActive ? SDL_StartTextInput(P->Window)
                               : SDL_StopTextInput(P->Window);

    if(!bOk)
    {
        /* Worth a warning rather than a silent false: the symptom of a failed
         * StartTextInput is "OnText never fires", which is indistinguishable
         * from a host that forgot to bind the slot. */
        GW_LOG_WARN(std::string("text input could not be ")
                  + (bInActive ? "started: " : "stopped: ") + SDL_GetError());
    }
}

void GameWindow::SetRelativeMouseMode(const bool bInRelative)
{
    GameWindowSystem::CheckPumpThread("GameWindow::SetRelativeMouseMode");
    if(P->Window && !SDL_SetWindowRelativeMouseMode(P->Window, bInRelative))
    {
        GW_LOG_WARN(std::string("relative mouse mode could not be ") + (bInRelative ? "entered: " : "left: ") + SDL_GetError());
    }
}

bool GameWindow::IsRelativeMouseMode() const
{
    return P->Window && SDL_GetWindowRelativeMouseMode(P->Window);
}

bool GameWindow::IsTextInputActive() const
{
    /* Asked of SDL rather than cached: unlike the window-state flags there is
     * no event that reports it changing, so a cached copy would have nothing
     * keeping it honest. */
    return P->Window && SDL_TextInputActive(P->Window);
}

// === Exit ===================================================================

void GameWindow::RequestClose()
{
    P->bCloseRequested = true;
}

bool GameWindow::IsCloseRequested() const
{
    return P->bCloseRequested;
}

// === Callbacks ==============================================================

void GameWindow::SetDelegates(const GameWindowDelegates& InDelegates)
{
    P->Delegates = InDelegates;
}

const GameWindowDelegates& GameWindow::GetDelegates() const
{
    return P->Delegates;
}

void GameWindow::ClearDelegates()
{
    P->Delegates = GameWindowDelegates();
}

// === Internals ==============================================================

void GameWindowSystem_Impl::RefreshCachedState(GameWindow& InWindow)
{
    GameWindow::Impl& I = Get(InWindow);
    if(!I.Window)
    {
        return;
    }

    int W = 0;
    int H = 0;

    if(SDL_GetWindowSizeInPixels(I.Window, &W, &H))
    {
        I.PixelSize = EngineCore::Size(W, H);
    }

    if(SDL_GetWindowSize(I.Window, &W, &H))
    {
        I.LogicalSize = EngineCore::Size(W, H);
    }

    int X = 0;
    int Y = 0;
    if(SDL_GetWindowPosition(I.Window, &X, &Y))
    {
        I.Position = EngineCore::Point(X, Y);
    }

    const float Scale = SDL_GetWindowDisplayScale(I.Window);
    if(Scale > 0.0f)
    {
        I.DisplayScale = Scale;
    }

    /* Distinct from DisplayScale -- see the note on Impl::PixelDensity. Guarded
     * because a zero would silently collapse every mouse coordinate to the
     * origin, which looks like a broken mouse rather than a bad scale factor. */
    const float Density = SDL_GetWindowPixelDensity(I.Window);
    if(Density > 0.0f)
    {
        I.PixelDensity = Density;
    }

    const SDL_WindowFlags Flags = SDL_GetWindowFlags(I.Window);
    I.bMinimized  = (Flags & SDL_WINDOW_MINIMIZED)   != 0;
    I.bMaximized  = (Flags & SDL_WINDOW_MAXIMIZED)   != 0;
    I.bFocused    = (Flags & SDL_WINDOW_INPUT_FOCUS) != 0;
    I.bMouseFocus = (Flags & SDL_WINDOW_MOUSE_FOCUS) != 0;
    I.bVisible    = (Flags & SDL_WINDOW_HIDDEN)      == 0;

    /* Only windowed-vs-fullscreen is observable from the flags. Which KIND of
     * fullscreen was asked for is remembered in Mode/LastFullscreenMode and
     * must not be clobbered by this refresh. */
    if((Flags & SDL_WINDOW_FULLSCREEN) != 0)
    {
        if(I.Mode == WM_Windowed)
        {
            I.Mode = I.LastFullscreenMode;
        }
    }
    else
    {
        I.Mode = WM_Windowed;
    }
}
