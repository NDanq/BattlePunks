#include "Public/GDI/GDIPlatform.h"

#include "Public/GDI/GDILog.h"

#if WITH_VULKAN_GDI
// Before the X11 headers: Xlib #defines None, Bool, Status and Success, and
// those have to land after vulkan.h, not inside it.
#include "Public/GDI/Vulkan/GDIVulkanPlatform.h"
#endif

/*
 * Linux implementation of GDI's graphics-window seam.
 *
 * The only file in GDI permitted to call Xlib directly. A port reimplements
 * this surface and nothing else.
 *
 * X11 ONLY. WindowHandle::Native holds an X11 Window id (an XID, stored as an
 * integer in the pointer), which is what GameWindowNativeLinux.cpp produces. On
 * a Wayland desktop that window lives on XWayland. GameWindowNativeLinux.cpp
 * has the reason a native Wayland surface does not fit in a WindowHandle.
 *
 * OpenGL is stubbed: GDI_WITH_OPENGL is refused on non-Windows by CMakeLists,
 * so the stubs below exist only so that lifting that refusal produces a loud
 * runtime failure instead of a link error. A real port wants EGL.
 */

#include <X11/Xlib.h>

namespace GDIPlatform
{
namespace
{
    ::Window ToXWindow(const EngineCore::WindowHandle& InWindow)
    {
        return InWindow.IsValid() ? static_cast<::Window>(reinterpret_cast<uintptr_t>(InWindow.Native)) : 0;
    }

    /*
     * GDI's own connection to the X server, opened on first use and kept for
     * the life of the process.
     *
     * Not SDL's connection: GDI does not link SDL, and does not need to. An X11
     * Window id is server-global, so any connection to the same server can
     * query it and build a Vulkan surface on it.
     *
     * Thread safety: libX11 1.8+ calls XInitThreads itself inside XOpenDisplay,
     * so this one Display is safe to use from the render thread and the UI
     * thread alike. The function-local static makes the open itself race-free.
     */
    ::Display* GetDisplay()
    {
        static ::Display* const Display = []
        {
            ::Display* Opened = ::XOpenDisplay(nullptr);
            if(Opened == nullptr)
            {
                GDI_LOG_ERROR("[GDIPlatform] XOpenDisplay failed; is DISPLAY set? (Wayland needs XWayland running.)")
            }
            return Opened;
        }();
        return Display;
    }
}

bool appGetWindowPixelSize(EngineCore::WindowHandle InWindow, uint32_t& OutWidth, uint32_t& OutHeight)
{
    const ::Window Window  = ToXWindow(InWindow);
    ::Display*     Display = GetDisplay();
    if(Window == 0 || Display == nullptr)
    {
        return false;
    }

    ::XWindowAttributes Attributes{};
    if(::XGetWindowAttributes(Display, Window, &Attributes) == 0)
    {
        return false;
    }

    // X11 sizes are already physical pixels; any desktop scaling is applied
    // by the client toolkit, not the server, so there is no factor to undo.
    OutWidth  = uint32_t(Attributes.width);
    OutHeight = uint32_t(Attributes.height);
    return true;
}
}


// ===========================================================================
// OpenGL -- stubbed
// ===========================================================================
#if WITH_OPENGL_GDI

namespace GDIPlatform
{
namespace
{
    void LogGLUnsupported()
    {
        GDI_LOG_ERROR("[GDIPlatform] OpenGL is not implemented on Linux; GDIPlatformLinux.cpp needs an EGL path.")
    }
}

void* appGLGetProcAddress(const char*)
{
    LogGLUnsupported();
    return nullptr;
}

bool appGLAdoptPixelFormat(EngineCore::WindowHandle)
{
    LogGLUnsupported();
    return false;
}

GLContextHandle appGLCreateContext(EngineCore::WindowHandle, const GLContextDesc&)
{
    LogGLUnsupported();
    return nullptr;
}

bool appGLMakeCurrent(GLContextHandle, EngineCore::WindowHandle)
{
    LogGLUnsupported();
    return false;
}

void appGLDoneCurrent()
{
}

void appGLSwapBuffers(EngineCore::WindowHandle)
{
}

void appGLDestroyContext(GLContextHandle)
{
}

EngineCore::WindowHandle appGLOffscreenWindow()
{
    LogGLUnsupported();
    return EngineCore::WindowHandle();
}
}

#endif // WITH_OPENGL_GDI


// ===========================================================================
// Vulkan
// ===========================================================================
#if WITH_VULKAN_GDI

// VK_USE_PLATFORM_XLIB_KHR is what exposes vkCreateXlibSurfaceKHR and the
// VkXlibSurfaceCreateInfoKHR struct; vulkan.h omits both without it.
#ifndef VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif
#include <vulkan/vulkan_xlib.h>

namespace GDIPlatform
{
std::vector<const char*> appVulkanRequiredInstanceExtensions()
{
    return { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
}

VkResult appVulkanCreateSurface(VkInstance InInstance,
                                EngineCore::WindowHandle InWindow,
                                VkSurfaceKHR* OutSurface)
{
    if(InInstance == VK_NULL_HANDLE || OutSurface == nullptr)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const ::Window Window  = ToXWindow(InWindow);
    ::Display*     Display = GetDisplay();
    if(Window == 0 || Display == nullptr)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkXlibSurfaceCreateInfoKHR Info{};
    Info.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    Info.dpy    = Display;
    Info.window = Window;

    return ::vkCreateXlibSurfaceKHR(InInstance, &Info, nullptr, OutSurface);
}
}

#endif // WITH_VULKAN_GDI
