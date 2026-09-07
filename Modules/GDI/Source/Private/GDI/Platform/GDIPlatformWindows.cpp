#include "Public/GDI/GDIPlatform.h"

#include "Public/GDI/GDILog.h"

/*
 * Windows implementation of GDI's graphics-window seam.
 *
 * The only file in GDI permitted to call WGL or a Win32 API directly. A port
 * reimplements this surface and nothing else.
 */

#include <Windows.h>
// wglext.h uses GLenum/GLint/GLushort and does not include them itself,
// so the GL 1.1 header has to come first.
#include <gl/GL.h>
#include <wglext.h>

namespace GDIPlatform
{
namespace
{
    HWND GOffscreenWindow = nullptr;
    HDC  GOffscreenDC     = nullptr;
    int  GPixelFormat     = 0;

    HWND ToHWND(const EngineCore::WindowHandle& InWindow)
    {
        return InWindow.IsValid() ? reinterpret_cast<HWND>(InWindow.Native) : nullptr;
    }

    /*
     * Pixel-format handling, and why it works the way it does.
     *
     * TWO HARD CONSTRAINTS:
     *
     * 1. A window's pixel format can be set exactly ONCE. Qt has already set
     *    one on the visible window, so this code must ADOPT that format rather
     *    than choose its own -- calling SetPixelFormat on it again fails.
     *
     * 2. The render-thread context SHARES objects with the UI-thread context:
     *    the renderer hands the UI thread a raw framebuffer texture handle
     *    (OGL_BackBufferHandleChanged) which the UI context then samples.
     *    Sharing requires compatible pixel formats, so the hidden offscreen
     *    window must use the SAME format as the visible one.
     *
     * Both fall out of one rule: read the visible window's format, remember
     * it, and apply that same format to the hidden window. Choosing
     * independently would satisfy neither -- and the symptom of getting it
     * wrong is a black canvas, not an error.
     */
    int AdoptPixelFormatFrom(HDC InDC)
    {
        if(GPixelFormat != 0)
        {
            return GPixelFormat;
        }

        // Qt has already chosen and set this window's format.
        const int Existing = ::GetPixelFormat(InDC);
        if(Existing != 0)
        {
            GPixelFormat = Existing;
            return GPixelFormat;
        }

        // No format yet (a bare window we own): choose one and set it.
        PIXELFORMATDESCRIPTOR Desc{};
        Desc.nSize        = sizeof(PIXELFORMATDESCRIPTOR);
        Desc.nVersion     = 1;
        Desc.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        Desc.iPixelType   = PFD_TYPE_RGBA;
        Desc.cColorBits   = 32;
        Desc.cDepthBits   = 24;
        Desc.cStencilBits = 8;
        Desc.iLayerType   = PFD_MAIN_PLANE;

        GPixelFormat = ::ChoosePixelFormat(InDC, &Desc);
        if(GPixelFormat == 0)
        {
            GDI_LOG_ERROR("[GDIPlatform] ChoosePixelFormat failed.")
            return 0;
        }
        ::SetPixelFormat(InDC, GPixelFormat, &Desc);
        return GPixelFormat;
    }

    /* Applies the already-adopted format to a DC we own (the hidden window). */
    bool ApplyAdoptedPixelFormat(HDC InDC)
    {
        if(GPixelFormat == 0)
        {
            return false;
        }
        if(::GetPixelFormat(InDC) == GPixelFormat)
        {
            return true;    // already applied
        }

        PIXELFORMATDESCRIPTOR Desc{};
        ::DescribePixelFormat(InDC, GPixelFormat, sizeof(Desc), &Desc);
        return ::SetPixelFormat(InDC, GPixelFormat, &Desc) != FALSE;
    }

    /*
     * The render thread's offscreen drawable.
     *
     * Replaces the QOffscreenSurface the GL backend used to render into. A
     * plain hidden window is what GLFW and SDL use for the same job: it needs
     * no extension, it is a real double-buffered drawable, and it costs one
     * never-shown HWND.
     *
     * CS_OWNDC so the DC is created once and stays valid for the window's
     * lifetime -- otherwise every make-current would need a GetDC/ReleaseDC
     * pair on the render thread.
     */
    bool EnsureOffscreenWindow()
    {
        if(GOffscreenWindow != nullptr && GOffscreenDC != nullptr)
        {
            return true;
        }

        static const wchar_t* kClassName = L"GDIOffscreenGLWindow";
        static bool bClassRegistered = false;

        HINSTANCE Instance = ::GetModuleHandleW(nullptr);

        if(!bClassRegistered)
        {
            WNDCLASSEXW Class{};
            Class.cbSize        = sizeof(WNDCLASSEXW);
            Class.style         = CS_OWNDC;
            Class.lpfnWndProc   = ::DefWindowProcW;
            Class.hInstance     = Instance;
            Class.lpszClassName = kClassName;

            if(::RegisterClassExW(&Class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                GDI_LOG_ERROR("[GDIPlatform] Failed to register the offscreen GL window class.")
                return false;
            }
            bClassRegistered = true;
        }

        GOffscreenWindow = ::CreateWindowExW(0, kClassName, L"", WS_POPUP,
                                             0, 0, 1, 1,
                                             nullptr, nullptr, Instance, nullptr);
        if(GOffscreenWindow == nullptr)
        {
            GDI_LOG_ERROR("[GDIPlatform] Failed to create the offscreen GL window.")
            return false;
        }
        // Deliberately never shown.

        GOffscreenDC = ::GetDC(GOffscreenWindow);
        if(GOffscreenDC == nullptr)
        {
            GDI_LOG_ERROR("[GDIPlatform] Failed to acquire the offscreen window DC.")
            ::DestroyWindow(GOffscreenWindow);
            GOffscreenWindow = nullptr;
            return false;
        }

        if(!ApplyAdoptedPixelFormat(GOffscreenDC))
        {
            GDI_LOG_ERROR("[GDIPlatform] Could not apply the adopted pixel format to the offscreen window."
                          " Create the window-backed context first so there is a format to adopt.")
            ::ReleaseDC(GOffscreenWindow, GOffscreenDC);
            ::DestroyWindow(GOffscreenWindow);
            GOffscreenWindow = nullptr;
            GOffscreenDC = nullptr;
            return false;
        }
        return true;
    }

    /* Resolves the DC to draw against: the given window, or the hidden one. */
    HDC ResolveDC(const EngineCore::WindowHandle& InWindow)
    {
        if(HWND Window = ToHWND(InWindow))
        {
            return ::GetDC(Window);
        }
        return EnsureOffscreenWindow() ? GOffscreenDC : nullptr;
    }

    void ReleaseResolvedDC(const EngineCore::WindowHandle& InWindow, HDC InDC)
    {
        // The offscreen DC is CS_OWNDC and must not be released.
        if(HWND Window = ToHWND(InWindow))
        {
            ::ReleaseDC(Window, InDC);
        }
    }
}

bool appGetWindowPixelSize(EngineCore::WindowHandle InWindow, uint32_t& OutWidth, uint32_t& OutHeight)
{
    HWND Window = ToHWND(InWindow);
    if(Window == nullptr)
    {
        return false;
    }

    RECT Client{};
    if(!::GetClientRect(Window, &Client))
    {
        return false;
    }

    // GetClientRect is already in physical pixels, so there is no DPI factor
    // to apply -- unlike QWindow::width(), which is logical.
    OutWidth  = uint32_t(Client.right  - Client.left);
    OutHeight = uint32_t(Client.bottom - Client.top);
    return true;
}

void* appGLGetProcAddress(const char* InName)
{
    if(InName == nullptr)
    {
        return nullptr;
    }

    /*
     * wglGetProcAddress resolves extension and core-profile entry points but
     * returns NULL for the GL 1.1 functions opengl32.dll exports directly. A
     * loader that only calls wglGetProcAddress silently half-works: the modern
     * calls resolve and the old ones do not.
     *
     * It also has a documented quirk of returning 1, 2, 3 or -1 on some
     * drivers to mean failure rather than NULL, hence the range check.
     */
    PROC Address = ::wglGetProcAddress(InName);

    if(Address == nullptr
       || Address == reinterpret_cast<PROC>(1)
       || Address == reinterpret_cast<PROC>(2)
       || Address == reinterpret_cast<PROC>(3)
       || Address == reinterpret_cast<PROC>(-1))
    {
        // GetModuleHandle, not LoadLibrary: opengl32 is already loaded (a
        // current context implies it), and this must not take a reference.
        static HMODULE OpenGL32 = ::GetModuleHandleW(L"opengl32.dll");
        if(OpenGL32 != nullptr)
        {
            Address = ::GetProcAddress(OpenGL32, InName);
        }
    }

    return reinterpret_cast<void*>(Address);
}

bool appGLAdoptPixelFormat(EngineCore::WindowHandle InWindow)
{
    HWND Window = ToHWND(InWindow);
    if(Window == nullptr)
    {
        return false;
    }

    HDC DC = ::GetDC(Window);
    if(DC == nullptr)
    {
        return false;
    }

    const int Format = AdoptPixelFormatFrom(DC);
    ::ReleaseDC(Window, DC);
    return Format != 0;
}

GLContextHandle appGLCreateContext(EngineCore::WindowHandle InWindow, const GLContextDesc& InDesc)
{
    HWND Window = ToHWND(InWindow);
    if(Window == nullptr)
    {
        if(!EnsureOffscreenWindow())
        {
            return nullptr;
        }
        Window = GOffscreenWindow;
    }

    HDC DC = (Window == GOffscreenWindow) ? GOffscreenDC : ::GetDC(Window);
    if(DC == nullptr)
    {
        GDI_LOG_ERROR("[GDIPlatform] No device context for GL context creation.")
        return nullptr;
    }

    AdoptPixelFormatFrom(DC);

    /*
     * The bootstrap dance: wglCreateContextAttribsARB is itself an extension,
     * so a legacy context has to exist and be current before it can be
     * resolved. The temporary is deleted as soon as the real context is built.
     */
    HGLRC Temp = ::wglCreateContext(DC);
    if(Temp == nullptr)
    {
        GDI_LOG_ERROR("[GDIPlatform] wglCreateContext (bootstrap) failed.")
        if(Window != GOffscreenWindow) ::ReleaseDC(Window, DC);
        return nullptr;
    }
    ::wglMakeCurrent(DC, Temp);

    auto CreateContextAttribs =
        reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(::wglGetProcAddress("wglCreateContextAttribsARB"));

    HGLRC Result = nullptr;
    if(CreateContextAttribs != nullptr)
    {
        const int Attribs[] =
        {
            WGL_CONTEXT_MAJOR_VERSION_ARB, InDesc.MajorVersion,
            WGL_CONTEXT_MINOR_VERSION_ARB, InDesc.MinorVersion,
            WGL_CONTEXT_FLAGS_ARB,         0,
            WGL_CONTEXT_PROFILE_MASK_ARB,  InDesc.bCoreProfile ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB
                                                               : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
            0
        };
        Result = CreateContextAttribs(DC, reinterpret_cast<HGLRC>(InDesc.ShareContext), Attribs);
    }
    else
    {
        GDI_LOG_ERROR("[GDIPlatform] wglCreateContextAttribsARB unavailable; cannot create a core-profile context.")
    }

    ::wglMakeCurrent(nullptr, nullptr);
    ::wglDeleteContext(Temp);

    if(Result == nullptr)
    {
        GDI_LOG_ERROR("[GDIPlatform] Failed to create the requested OpenGL context.")
    }

    if(Window != GOffscreenWindow) ::ReleaseDC(Window, DC);
    return reinterpret_cast<GLContextHandle>(Result);
}

bool appGLMakeCurrent(GLContextHandle InContext, EngineCore::WindowHandle InWindow)
{
    HDC DC = ResolveDC(InWindow);
    if(DC == nullptr)
    {
        return false;
    }

    const BOOL bOk = ::wglMakeCurrent(DC, reinterpret_cast<HGLRC>(InContext));
    ReleaseResolvedDC(InWindow, DC);
    return bOk != FALSE;
}

void appGLDoneCurrent()
{
    ::wglMakeCurrent(nullptr, nullptr);
}

void appGLSwapBuffers(EngineCore::WindowHandle InWindow)
{
    HDC DC = ResolveDC(InWindow);
    if(DC == nullptr)
    {
        return;
    }

    ::SwapBuffers(DC);
    ReleaseResolvedDC(InWindow, DC);
}

void appGLDestroyContext(GLContextHandle InContext)
{
    if(InContext == nullptr)
    {
        return;
    }
    ::wglDeleteContext(reinterpret_cast<HGLRC>(InContext));
}

EngineCore::WindowHandle appGLOffscreenWindow()
{
    if(!EnsureOffscreenWindow())
    {
        return EngineCore::WindowHandle();
    }
    return EngineCore::WindowHandle(reinterpret_cast<void*>(GOffscreenWindow));
}
}


// ===========================================================================
// Vulkan
// ===========================================================================
#if WITH_VULKAN_GDI

#include "Public/GDI/Vulkan/GDIVulkanPlatform.h"

// VK_USE_PLATFORM_WIN32_KHR is what exposes vkCreateWin32SurfaceKHR and the
// VkWin32SurfaceCreateInfoKHR struct; vulkan.h omits both without it.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan_win32.h>

namespace GDIPlatform
{
std::vector<const char*> appVulkanRequiredInstanceExtensions()
{
    return { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
}

VkResult appVulkanCreateSurface(VkInstance InInstance,
                                EngineCore::WindowHandle InWindow,
                                VkSurfaceKHR* OutSurface)
{
    if(InInstance == VK_NULL_HANDLE || OutSurface == nullptr)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    HWND Window = ToHWND(InWindow);
    if(Window == nullptr)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkWin32SurfaceCreateInfoKHR Info{};
    Info.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    Info.hinstance = ::GetModuleHandleW(nullptr);
    Info.hwnd      = Window;

    return ::vkCreateWin32SurfaceKHR(InInstance, &Info, nullptr, OutSurface);
}
}

#endif // WITH_VULKAN_GDI
