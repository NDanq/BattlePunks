#include <deque>
#include <string>
#include <vector>

#include "Private/GameWindow/GameWindowInternal.h"
#include "Public/GameWindow/GameWindowSystem.h"

/*
 * SDL lifetime, the window registry, and the pump.
 */

namespace
{
    /*
     * WHY A FILE-SCOPE std::vector IS FINE HERE, WHEN CoreConfig.h SAYS IT IS NOT
     *
     * CoreConfig.h uses an intrusive list because a ConfigVar's constructor runs
     * during dynamic initialisation, before main() and during LoadLibrary, and
     * would push into a vector whose own constructor may not have run yet.
     *
     * Nothing touches this vector during dynamic initialisation. It is only
     * reached from function bodies, all of them after Initialize() returned, so
     * the ordering question CoreConfig.h is answering does not arise. If a
     * GameWindow ever self-registers from a file-scope constructor, this has to
     * become an intrusive list for exactly the reasons that header gives.
     */
    std::vector<GameWindow*> GWindows;

    int                  GInitRefCount = 0;
    EngineCore::ThreadID GPumpThread   = 0;

    bool GQuitRequested     = false;

    /* PumpEvents returns false on "the last window closed", which cannot be
     * true before there was a first one. Without this, a host that pumps once
     * during startup before creating its window exits immediately. */
    bool GEverCreatedWindow = false;

    bool HasAnyDelegate(const GameWindowDelegates& InDelegates)
    {
        /* Written out rather than memcmp'd against a default table: this struct
         * has padding, and a padding byte differing would produce a warning
         * about a module that had in fact cleaned up properly. */
        return InDelegates.OnCloseRequested      != nullptr
            || InDelegates.OnQuit                != nullptr
            || InDelegates.OnResized             != nullptr
            || InDelegates.OnMoved               != nullptr
            || InDelegates.OnMinimized           != nullptr
            || InDelegates.OnMaximized           != nullptr
            || InDelegates.OnRestored            != nullptr
            || InDelegates.OnFocusChanged        != nullptr
            || InDelegates.OnMouseFocusChanged   != nullptr
            || InDelegates.OnWindowModeChanged   != nullptr
            || InDelegates.OnDisplayScaleChanged != nullptr
            || InDelegates.OnKey                 != nullptr
            || InDelegates.OnText                != nullptr
            || InDelegates.OnMouseButton         != nullptr
            || InDelegates.OnMouseMove           != nullptr
            || InDelegates.OnMouseWheel          != nullptr;
    }

    SDL_WindowFlags ToSDLWindowFlags(const WindowDesc& InDesc)
    {
        SDL_WindowFlags Flags = 0;

        if(InDesc.Flags & WF_Resizable)   { Flags |= SDL_WINDOW_RESIZABLE;          }
        if(InDesc.Flags & WF_Borderless)  { Flags |= SDL_WINDOW_BORDERLESS;         }
        if(InDesc.Flags & WF_HighDPI)     { Flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY; }
        if(InDesc.Flags & WF_Hidden)      { Flags |= SDL_WINDOW_HIDDEN;             }
        if(InDesc.Flags & WF_AlwaysOnTop) { Flags |= SDL_WINDOW_ALWAYS_ON_TOP;      }

        if(InDesc.Mode != WM_Windowed)
        {
            Flags |= SDL_WINDOW_FULLSCREEN;
        }

        /*
         * The backend flag MUST be right at creation: SDL cannot add
         * SDL_WINDOW_OPENGL or SDL_WINDOW_VULKAN to a window that already
         * exists. Getting it wrong here does not fail here -- it fails later,
         * inside wglCreateContext or vkCreateWin32SurfaceKHR.
         */
        switch(InDesc.Backend)
        {
            case RPH_OpenGL: Flags |= SDL_WINDOW_OPENGL; break;
            case RPH_Vulkan: Flags |= SDL_WINDOW_VULKAN; break;
            case RPH_Metal:  Flags |= SDL_WINDOW_METAL;  break;
            case RPH_None:
            default:                                     break;
        }

        return Flags;
    }

    /* Fires the app-level quit. OnQuit lives in the per-window table but is not
     * a per-window event, so it fires ONCE, on the primary window's table --
     * firing it on every window would call a host's single quit handler N
     * times. A host with several windows puts its quit handling on the first
     * one it created. */
    void DispatchQuit()
    {
        if(GWindows.empty())
        {
            return;
        }

        const GameWindowDelegates& D = GWindows.front()->GetDelegates();
        if(D.OnQuit)
        {
            D.OnQuit(D.UserData);
        }
    }

    /* InjectTextInput's strings, alive until the pump that delivers them. A
     * deque: appending never relocates an earlier element, whose c_str() an
     * already-queued event points at. */
    std::deque<std::string> GInjectedText;

    void RouteEvent(const SDL_Event& InEvent)
    {
        if(InEvent.type == SDL_EVENT_QUIT)
        {
            GQuitRequested = true;
            DispatchQuit();
            return;
        }

        if(InEvent.type >= SDL_EVENT_WINDOW_FIRST && InEvent.type <= SDL_EVENT_WINDOW_LAST)
        {
            if(GameWindow* Window = GameWindowSystem::FindWindow(InEvent.window.windowID))
            {
                GameWindowSystem_Impl::DispatchWindowEvent(*Window, InEvent.window);
            }
            return;
        }

        /*
         * Input. Each event carries its own windowID in its own struct -- they
         * are NOT interchangeable with event.window, which is why each case
         * reads through its own member rather than a shared one.
         *
         * An event whose window is not in the registry is dropped. That is the
         * normal case for keystrokes arriving after a window was reaped in this
         * same pump: SDL queued them before the close, and there is no longer
         * anything to deliver them to.
         */
        switch(InEvent.type)
        {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if(GameWindow* Window = GameWindowSystem::FindWindow(InEvent.key.windowID))
                {
                    GameWindowSystem_Impl::DispatchKeyEvent(*Window, InEvent.key);
                }
                return;

            case SDL_EVENT_TEXT_INPUT:
                if(GameWindow* Window = GameWindowSystem::FindWindow(InEvent.text.windowID))
                {
                    GameWindowSystem_Impl::DispatchTextEvent(*Window, InEvent.text);
                }
                return;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if(GameWindow* Window = GameWindowSystem::FindWindow(InEvent.button.windowID))
                {
                    GameWindowSystem_Impl::DispatchMouseButtonEvent(*Window, InEvent.button);
                }
                return;

            case SDL_EVENT_MOUSE_MOTION:
                if(GameWindow* Window = GameWindowSystem::FindWindow(InEvent.motion.windowID))
                {
                    GameWindowSystem_Impl::DispatchMouseMotionEvent(*Window, InEvent.motion);
                }
                return;

            case SDL_EVENT_MOUSE_WHEEL:
                if(GameWindow* Window = GameWindowSystem::FindWindow(InEvent.wheel.windowID))
                {
                    GameWindowSystem_Impl::DispatchMouseWheelEvent(*Window, InEvent.wheel);
                }
                return;

            default:
                /* Joystick, gamepad, touch, pen, audio-device, clipboard and
                 * the rest. Deliberately not modelled here -- a gamepad belongs
                 * to an input module, not a window module. */
                return;
        }
    }

    /*
     * Destroys the windows RequestClose() marked.
     *
     * Deferred to here on purpose: a window destroyed inside its own callback
     * would be freed while the dispatch loop still holds the pointer. This runs
     * after the queue has drained, which is the only point at which nothing is
     * standing on any of them.
     */
    void ReapClosedWindows()
    {
        for(size_t Index = 0; Index < GWindows.size(); )
        {
            GameWindow* Window = GWindows[Index];

            if(Window->IsCloseRequested())
            {
                GWindows.erase(GWindows.begin() + static_cast<ptrdiff_t>(Index));
                GameWindowSystem_Impl::Destruct(Window);
            }
            else
            {
                ++Index;
            }
        }
    }

    bool ShouldKeepRunning()
    {
        return !GQuitRequested && !(GEverCreatedWindow && GWindows.empty());
    }
}

// === Thread contract ========================================================

EngineCore::ThreadID GameWindowSystem::GetPumpThread()
{
    return GPumpThread;
}

void GameWindowSystem::CheckPumpThread(const char* InWhat)
{
    if(GInitRefCount == 0)
    {
        /* Not initialised yet -- the caller's own "before Initialize" check is
         * the useful diagnostic, not a thread mismatch against a null id. */
        return;
    }

    CORE_CHECK(EngineCore::appThreadID() == GPumpThread,
               std::string("[GameWindow] ") + InWhat
             + " was called from a thread other than the one that called Initialize(). "
               "SDL requires window calls and event pumping on one thread, and the "
               "cached window state is written by the pump with no synchronisation.")
}

// === Lifetime ===============================================================

std::string GameWindowSystem::GetExecutableDir()
{
    /* SDL owns the returned string (SDL3); copied out, never freed here. */
    const char* BasePath = SDL_GetBasePath();
    return BasePath ? std::string(BasePath) : std::string();
}

bool GameWindowSystem::Initialize()
{
    if(GInitRefCount > 0)
    {
        /* Already up. Refcounted so two subsystems can each Initialize and
         * Shutdown without either pulling SDL out from under the other. */
        ++GInitRefCount;
        return true;
    }

    if(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        GW_LOG_ERROR(std::string("SDL_Init failed: ") + SDL_GetError());
        return false;
    }

    GPumpThread        = EngineCore::appThreadID();
    GQuitRequested     = false;
    GEverCreatedWindow = false;
    GInitRefCount      = 1;

    GW_LOG(std::string("initialised on SDL video driver '")
         + (SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none") + "'");

    return true;
}

void GameWindowSystem::Shutdown()
{
    if(GInitRefCount == 0)
    {
        return;
    }

    if(--GInitRefCount > 0)
    {
        return;
    }

    CheckPumpThread("GameWindowSystem::Shutdown");

    for(GameWindow* Window : GWindows)
    {
        /*
         * The manager cannot enforce the reload rule, but it can say that it
         * was broken -- the same position CoreModuleManager is in with
         * GLogHandler. A table still bound at teardown is very often a hot
         * module that returned from OnShutdown without calling ClearDelegates,
         * which means these pointers are about to point into unmapped memory.
         */
        if(HasAnyDelegate(Window->GetDelegates()))
        {
            GW_LOG_WARN("a window still had delegates bound at Shutdown. If they point "
                        "into a hot-reloadable module, OnShutdown failed to call "
                        "ClearDelegates() -- see the reload hazard in GameWindowDelegates.h.");
        }

        GameWindowSystem_Impl::Destruct(Window);
    }

    GWindows.clear();

    SDL_Quit();
    GInjectedText.clear();   // any still-queued injected text went with the queue

    GPumpThread        = 0;
    GQuitRequested     = false;
    GEverCreatedWindow = false;
}

bool GameWindowSystem::IsInitialized()
{
    return GInitRefCount > 0;
}

// === Windows ================================================================

void GameWindowSystem::SetHostOwnsMessageLoop(const bool bInHostOwnsLoop)
{
    /* A hint, so it may be set before SDL_Init -- and must be, for the very
     * first pump to respect it. Harmless on platforms that ignore it. */
    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, bInHostOwnsLoop ? "0" : "1");
}

GameWindow* GameWindowSystem::WrapNativeWindow(const EngineCore::WindowHandle InNativeWindow)
{
    if(!IsInitialized())
    {
        GW_LOG_ERROR("WrapNativeWindow called before Initialize().");
        return nullptr;
    }
    if(!InNativeWindow.IsValid())
    {
        GW_LOG_ERROR("WrapNativeWindow given an invalid native handle.");
        return nullptr;
    }

    CheckPumpThread("GameWindowSystem::WrapNativeWindow");

#if defined(_WIN32)
    const SDL_PropertiesID Props = SDL_CreateProperties();
    SDL_SetPointerProperty(Props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, InNativeWindow.Native);
    SDL_Window* SdlWindow = SDL_CreateWindowWithProperties(Props);
    SDL_DestroyProperties(Props);
#else
    SDL_Window* SdlWindow = nullptr;
    SDL_SetError("wrapping a native window is only implemented on Windows");
#endif

    if(!SdlWindow)
    {
        GW_LOG_ERROR(std::string("WrapNativeWindow: SDL could not adopt the window: ") + SDL_GetError());
        return nullptr;
    }

    GameWindow* Window = GameWindowSystem_Impl::Construct();
    auto& I  = GameWindowSystem_Impl::Get(*Window);
    I.Window = SdlWindow;
    I.ID     = SDL_GetWindowID(SdlWindow);

    GameWindowSystem_Impl::RefreshCachedState(*Window);

    /* Not GEverCreatedWindow: "the last window closed" is the host's business
     * when the host owns the window, and must not end a loop it does not run. */
    GWindows.push_back(Window);
    return Window;
}

void GameWindowSystem::InjectTextInput(GameWindow* InWindow, const char* InUtf8)
{
    if(!InWindow || !InUtf8 || InUtf8[0] == '\0')
    {
        return;
    }
    CheckPumpThread("GameWindowSystem::InjectTextInput");

    SDL_Window* SdlWindow = static_cast<SDL_Window*>(InWindow->GetSDLWindow());
    if(!SdlWindow || !SDL_TextInputActive(SdlWindow) || static_cast<unsigned char>(InUtf8[0]) < 0x20)
    {
        return;
    }

    /* SDL owns the text only of events it made itself (its internal temporary
     * strings); a pushed event's pointer is the pusher's to keep alive until
     * the event is consumed. The next PumpEvents/WaitEvents drains the whole
     * queue on this same thread, so each string lives exactly until then. */
    GInjectedText.emplace_back(InUtf8);

    SDL_Event Event;
    SDL_zero(Event);
    Event.type          = SDL_EVENT_TEXT_INPUT;
    Event.text.windowID = SDL_GetWindowID(SdlWindow);
    Event.text.text     = GInjectedText.back().c_str();
    if(!SDL_PushEvent(&Event))
    {
        GInjectedText.pop_back();
    }
}

GameWindow* GameWindowSystem::CreateGameWindow(const WindowDesc& InDesc)
{
    if(!IsInitialized())
    {
        GW_LOG_ERROR("CreateGameWindow called before Initialize().");
        return nullptr;
    }

    CheckPumpThread("GameWindowSystem::CreateGameWindow");

    int Width  = InDesc.Width;
    int Height = InDesc.Height;

    if(Width <= 0 || Height <= 0)
    {
        GW_LOG_WARN("WindowDesc had a non-positive size; falling back to 1280x720.");
        Width  = 1280;
        Height = 720;
    }

    const char* Title = InDesc.Title ? InDesc.Title : "NimbleEngine";

    SDL_Window* SdlWindow = SDL_CreateWindow(Title, Width, Height, ToSDLWindowFlags(InDesc));
    if(!SdlWindow)
    {
        GW_LOG_ERROR(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return nullptr;
    }

    /* SDL3's SDL_CreateWindow takes no position, so an explicit one is applied
     * after the fact. -1 in either axis means "centre that axis". */
    if(InDesc.X != -1 || InDesc.Y != -1)
    {
        SDL_SetWindowPosition(SdlWindow,
                              InDesc.X != -1 ? InDesc.X : SDL_WINDOWPOS_CENTERED,
                              InDesc.Y != -1 ? InDesc.Y : SDL_WINDOWPOS_CENTERED);
    }

    GameWindow* Window = GameWindowSystem_Impl::Construct();

    /* auto&, not GameWindow::Impl& -- Impl is a private member type and this is
     * a plain function in namespace GameWindowSystem, not the friend class.
     * GameWindowSystem_Impl::Get() is reachable here; SPELLING the type is not. */
    auto& I = GameWindowSystem_Impl::Get(*Window);

    I.Window = SdlWindow;
    I.ID     = SDL_GetWindowID(SdlWindow);

    /* So a window created fullscreen toggles back into the kind it started as. */
    if(InDesc.Mode != WM_Windowed)
    {
        I.LastFullscreenMode = InDesc.Mode;
        I.Mode               = InDesc.Mode;
    }

    GameWindowSystem_Impl::RefreshCachedState(*Window);

    GWindows.push_back(Window);
    GEverCreatedWindow = true;

    return Window;
}

void GameWindowSystem::DestroyGameWindow(GameWindow* InWindow)
{
    if(!InWindow)
    {
        return;
    }

    CheckPumpThread("GameWindowSystem::DestroyGameWindow");

    for(size_t Index = 0; Index < GWindows.size(); ++Index)
    {
        if(GWindows[Index] == InWindow)
        {
            GWindows.erase(GWindows.begin() + static_cast<ptrdiff_t>(Index));
            GameWindowSystem_Impl::Destruct(InWindow);
            return;
        }
    }

    GW_LOG_WARN("DestroyGameWindow was given a window this system does not own; ignoring.");
}

// === Pump ===================================================================

bool GameWindowSystem::PumpEvents()
{
    if(!IsInitialized())
    {
        return false;
    }

    CheckPumpThread("GameWindowSystem::PumpEvents");

    SDL_Event Event;
    while(SDL_PollEvent(&Event))
    {
        RouteEvent(Event);
    }
    GInjectedText.clear();   // every event pointing at them has been routed

    ReapClosedWindows();

    return ShouldKeepRunning();
}

bool GameWindowSystem::WaitEvents(const int InTimeoutMilliseconds)
{
    if(!IsInitialized())
    {
        return false;
    }

    CheckPumpThread("GameWindowSystem::WaitEvents");

    SDL_Event Event;

    /* Block for the first event, then drain whatever else arrived with it --
     * otherwise a burst of motion events would take one wait apiece. */
    if(SDL_WaitEventTimeout(&Event, InTimeoutMilliseconds))
    {
        RouteEvent(Event);

        while(SDL_PollEvent(&Event))
        {
            RouteEvent(Event);
        }
        GInjectedText.clear();
    }

    ReapClosedWindows();

    return ShouldKeepRunning();
}

// === Registry ===============================================================

GameWindow* GameWindowSystem::FindWindow(const uint32_t InWindowID)
{
    /* Linear, and correct at the sizes this holds -- the same call CoreConfig.h
     * makes about its intrusive list. This runs per event, never per pixel. */
    for(GameWindow* Window : GWindows)
    {
        if(Window->GetID() == InWindowID)
        {
            return Window;
        }
    }

    return nullptr;
}

GameWindow* GameWindowSystem::GetPrimaryWindow()
{
    return GWindows.empty() ? nullptr : GWindows.front();
}

size_t GameWindowSystem::GetWindowCount()
{
    return GWindows.size();
}
