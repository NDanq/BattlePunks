#include <cstdint>

#include <SDL3/SDL.h>

#include "Private/GameWindow/GameWindowInternal.h"

/*
 * The Linux half of the one thing that is genuinely per-platform: turning an
 * SDL_Window into the native handle the rest of the engine passes around.
 *
 * X11 ONLY, ON PURPOSE. EngineCore::WindowHandle is a single void*, and an X11
 * Window id is the one Linux handle that fits in it on its own: the server
 * assigns it, so any Display connection to the same server can name the window.
 * That is what lets GDIPlatformLinux.cpp open its own connection instead of
 * borrowing SDL's -- GDI does not link SDL. A Wayland wl_surface* is only
 * meaningful alongside the wl_display* it came from, which a single pointer
 * cannot carry. On a Wayland desktop this therefore runs through XWayland.
 */

void GameWindowPlatformPreInit()
{
    /*
     * Normal priority, not SDL_HINT_OVERRIDE: an SDL_VIDEO_DRIVER environment
     * variable still wins, which is worth keeping for debugging SDL itself. A
     * window built on anything but x11 is then caught by the warning below.
     */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
}

void* GameWindowGetNativeHandle(SDL_Window* InWindow)
{
    if(!InWindow)
    {
        return nullptr;
    }

    const SDL_PropertiesID Properties = SDL_GetWindowProperties(InWindow);
    if(Properties == 0)
    {
        GW_LOG_WARN(std::string("SDL_GetWindowProperties failed: ") + SDL_GetError());
        return nullptr;
    }

    /*
     * 0 is X11's None, so a missing property and "no window" both come back as
     * a null handle, which is what WindowHandle::IsValid() expects. The usual
     * cause is SDL_VIDEO_DRIVER=wayland in the environment overriding the hint.
     */
    const Sint64 WindowId = SDL_GetNumberProperty(Properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if(WindowId == 0)
    {
        const char* Driver = SDL_GetCurrentVideoDriver();
        GW_LOG_WARN(std::string("the window has no X11 Window property (SDL video driver '")
                  + (Driver ? Driver : "none") + "'); GDI cannot present into it.");
        return nullptr;
    }

    return reinterpret_cast<void*>(static_cast<uintptr_t>(WindowId));
}
