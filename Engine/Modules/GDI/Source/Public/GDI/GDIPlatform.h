#ifndef CORECLIENT_GDIPLATFORM_H
#define CORECLIENT_GDIPLATFORM_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/Core/CoreTypes.h"

/*
 * GDI's graphics-window-system seam.
 *
 * Distinct from EngineCore's CorePlatform.h on purpose. That one is the
 * general OS-mechanism seam -- files, threads -- and EngineCore must never
 * learn what a VkInstance or an HGLRC is. Anything needing a graphics API or a
 * window system belongs here instead.
 *
 * Implemented once per platform under Source/Private/GDI/Platform/. An SDL
 * host reimplements exactly this and touches nothing else in GDI.
 */

namespace GDIPlatform
{
    // === Windows ============================================================

    /*
     * Client-area size of InWindow in PHYSICAL pixels.
     *
     * Physical, not logical: this replaces `Window->width() * devicePixelRatio()`
     * in the Vulkan swapchain, and a swapchain sized in logical pixels is
     * blurry on a scaled display. Win32's GetClientRect is already physical,
     * so no scale factor is involved. False if the window is invalid.
     */
    bool appGetWindowPixelSize(EngineCore::WindowHandle InWindow, uint32_t& OutWidth, uint32_t& OutHeight);

    // === OpenGL =============================================================

    /* Opaque native GL context -- an HGLRC on Windows. */
    using GLContextHandle = void*;

    struct GLContextDesc
    {
        int  MajorVersion  = 4;
        int  MinorVersion  = 5;   // glClipControl: render targets stored top-down (GDI.h)
        bool bCoreProfile  = true;

        /* Context to share objects (textures, buffers) with, or null. */
        GLContextHandle ShareContext = nullptr;
    };

    /*
     * Resolves a GL entry point.
     *
     * On Windows this is wglGetProcAddress with an opengl32.dll fallback --
     * wglGetProcAddress returns null for the GL 1.1 functions the DLL exports
     * directly, which is the most common reason a hand-written loader
     * half-works. Requires a current context.
     */
    void* appGLGetProcAddress(const char* InName);

    /*
     * Adopts the pixel format already set on InWindow.
     *
     * Must be called before the offscreen drawable is first used. The
     * render-thread context shares objects with the UI-thread context, and
     * sharing requires compatible pixel formats -- so the hidden offscreen
     * window has to match the visible one rather than choose for itself.
     */
    bool appGLAdoptPixelFormat(EngineCore::WindowHandle InWindow);

    /* Creates a context against InWindow's pixel format. Null on failure. */
    GLContextHandle appGLCreateContext(EngineCore::WindowHandle InWindow, const GLContextDesc& InDesc);

    /*
     * Makes InContext current.
     *
     * THE OFFSCREEN CASE: passing an invalid WindowHandle makes the context
     * current against a hidden, never-shown window owned by the platform layer.
     *
     * That hidden window is what replaces the QOffscreenSurface the render
     * thread used to draw into. It is not a detail -- the render thread must
     * NOT make itself current on the visible window's DC, because the UI
     * thread presents there. Rendering happens offscreen and the host blits
     * the result, which is exactly the arrangement Qt's offscreen surface
     * provided.
     */
    bool appGLMakeCurrent(GLContextHandle InContext, EngineCore::WindowHandle InWindow);

    /* Releases any current context on the calling thread. */
    void appGLDoneCurrent();

    /* Swaps the drawable's buffers. An invalid WindowHandle targets the same
     * hidden offscreen window appGLMakeCurrent uses. */
    void appGLSwapBuffers(EngineCore::WindowHandle InWindow);

    void appGLDestroyContext(GLContextHandle InContext);

    /* Native handle of the hidden offscreen window, for a host that needs to
     * hand the same drawable to something else. Invalid until first use. */
    EngineCore::WindowHandle appGLOffscreenWindow();
}

#endif //CORECLIENT_GDIPLATFORM_H
