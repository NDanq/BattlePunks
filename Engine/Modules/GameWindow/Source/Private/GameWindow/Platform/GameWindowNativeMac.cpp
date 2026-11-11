#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include "Private/GameWindow/GameWindowInternal.h"

/*
 * The macOS half of turning an SDL_Window into the native handle the rest of
 * the engine passes around.
 *
 * The handle is an NSView* whose backing layer is a CAMetalLayer -- what
 * MTLSwapchain::Initialize expects (it reads -layer off the view). SDL makes
 * that view for us with SDL_Metal_CreateView, so this file needs no
 * Objective-C and GDI never sees SDL.
 *
 * The view is made once per window, on first request, and kept in the
 * window's own property set with a cleanup callback, so it goes when the
 * window does and a second request returns the same view (and the same layer
 * the swapchain already holds). The window must carry SDL_WINDOW_METAL, which
 * GameWindowSystem.cpp sets for RPH_Metal.
 */

namespace
{
    constexpr const char* MetalViewProperty = "Nimble.MetalView";

    void DestroyMetalView(void* /*InUserData*/, void* InValue)
    {
        if(InValue)
        {
            SDL_Metal_DestroyView(static_cast<SDL_MetalView>(InValue));
        }
    }
}

void GameWindowPlatformPreInit()
{
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

    if(void* Existing = SDL_GetPointerProperty(Properties, MetalViewProperty, nullptr))
    {
        return Existing;
    }

    if((SDL_GetWindowFlags(InWindow) & SDL_WINDOW_METAL) == 0)
    {
        GW_LOG_WARN("the window was not created with SDL_WINDOW_METAL (backend not RPH_Metal?); GDI cannot present into it.");
        return nullptr;
    }

    SDL_MetalView View = SDL_Metal_CreateView(InWindow);
    if(View == nullptr)
    {
        GW_LOG_WARN(std::string("SDL_Metal_CreateView failed: ") + SDL_GetError());
        return nullptr;
    }

    SDL_SetPointerPropertyWithCleanup(Properties, MetalViewProperty, View, &DestroyMetalView, nullptr);
    return View;
}
