#include <SDL3/SDL.h>

#include "Private/GameWindow/GameWindowInternal.h"

/*
 * The Windows half of the one thing that is genuinely per-platform: turning an
 * SDL_Window into the native handle the rest of the engine passes around.
 *
 * Exactly one of these files compiles per configure -- the arrangement GDI's
 * Source/Private/GDI/Platform/ uses -- so a port adds a file here and changes
 * nothing else in the module.
 *
 * WHY THIS EXISTS AT ALL, rather than storing the SDL_Window*:
 * GDIPlatformWindows.cpp's ToHWND() does an unguarded
 * reinterpret_cast<HWND>(WindowHandle::Native), and every GL context and
 * Vulkan surface call in that file routes through it. CoreTypes.h says the same
 * thing from the other side. An SDL_Window* stored in a WindowHandle compiles
 * clean, passes IsValid(), and dies inside wglCreateContext.
 */

void GameWindowPlatformPreInit()
{
    /* Nothing to choose: SDL has exactly one video driver on Windows. */
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
     * Null here is not "no window" -- it is a window SDL built on a non-Win32
     * backend, which on Windows means something is badly wrong rather than
     * merely unsupported. Worth a warning: the caller's next move is handing
     * this to InitializeGDI, and a null HWND fails there with a much less
     * specific message.
     */
    void* const Handle = SDL_GetPointerProperty(Properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if(!Handle)
    {
        GW_LOG_WARN("the window has no Win32 HWND property; GDI cannot present into it.");
    }

    return Handle;
}
