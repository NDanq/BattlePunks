#ifndef GAMEWINDOW_GAMEWINDOWTYPES_H
#define GAMEWINDOW_GAMEWINDOWTYPES_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/GameWindow/GameWindowAPI.h"

/*
 * Window description types.
 *
 * Everything here is a plain enum or a POD aggregate, deliberately: a host
 * fills a WindowDesc on the stack, and a hot-reloadable module can memcpy one
 * into a ModuleReloadState blob without a lifetime question.
 */

/* WM_ prefix, as EModuleState uses MS_ and EConfigType uses CT_. */
enum EWindowMode : uint8_t
{
    WM_Windowed = 0,
    WM_FullscreenDesktop,     // borderless, at the desktop's current mode
    WM_FullscreenExclusive,   // a real mode switch
    WM_MAX
};

enum EWindowFlags : uint32_t
{
    WF_None        = 0,
    WF_Resizable   = 1u << 0,
    WF_Borderless  = 1u << 1,
    WF_HighDPI     = 1u << 2,
    WF_Hidden      = 1u << 3,   // created hidden; Show() makes it visible
    WF_AlwaysOnTop = 1u << 4
};

/*
 * Which GDI backend will present into this window.
 *
 * A DELIBERATE DUPLICATE of GDI's ERenderPlatform, not an include of
 * GDIDefinitions.h. Including GDI's header here would make GameWindow depend on
 * the whole GDI target for one enum and drag vulkan.h into every host
 * translation unit -- and GameWindow does not link GDI at all, which is the
 * property that keeps SDL out of GDI's backends.
 *
 * The host maps between the two in one line. GameWindow's smoke test asserts
 * the mapping still holds, so a reordering of either enum is caught by a test
 * rather than by a wrong backend at runtime.
 */
enum ERenderPlatformHint : uint8_t
{
    RPH_None = 0,   // no GPU backend; a plain window
    RPH_OpenGL,
    RPH_Vulkan,
    RPH_Metal,
    RPH_MAX
};

struct WindowDesc
{
    const char* Title  = "NimbleEngine";
    int         Width  = 1280;
    int         Height = 720;

    /* -1 centers on the primary display. */
    int         X      = -1;
    int         Y      = -1;

    uint32_t    Flags  = WF_Resizable | WF_HighDPI;
    EWindowMode Mode   = WM_Windowed;

    /*
     * MUST be correct at creation. SDL_WINDOW_OPENGL and SDL_WINDOW_VULKAN
     * cannot be added to a window that already exists, so a wrong value here
     * does not fail at CreateGameWindow -- it fails much later, inside
     * wglCreateContext or vkCreateWin32SurfaceKHR, with nothing pointing back
     * at this field.
     */
    ERenderPlatformHint Backend = RPH_None;
};

#endif //GAMEWINDOW_GAMEWINDOWTYPES_H
