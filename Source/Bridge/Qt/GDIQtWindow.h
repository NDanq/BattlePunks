#ifndef CORECLIENT_GDIQTWINDOW_H
#define CORECLIENT_GDIQTWINDOW_H

#include <QWindow>

#include "Public/Core/CoreTypes.h"

/*
 * QWindow -> EngineCore::WindowHandle.
 *
 * GDI's backends need a window to create a GL context against or to make a
 * Vulkan surface from, but they must not know what a QWindow is. This is the
 * one place that translation happens.
 *
 * WHICH HANDLE IS CARRIED, AND WHY IT IS THE NATIVE ONE
 *
 * WindowHandle carries the platform's own handle -- an HWND on Windows, the
 * NSView* on macOS -- not the QWindow pointer. The alternative (smuggling the
 * QWindow* through as an opaque void*) would work today because both current
 * backends happen to run under Qt, but it would mean the platform layer held
 * a Qt type in disguise, and an SDL host could not produce one. Carrying the
 * native handle is what lets appGLCreateContext and appVulkanCreateSurface be
 * implemented against SDL later without touching GDI: the Windows
 * implementation feeds the HWND straight to wglCreateContextAttribsARB and
 * vkCreateWin32SurfaceKHR.
 *
 * Consequence worth knowing: Qt creates the native window lazily. winId()
 * forces creation, so a handle taken from a window that has not been shown is
 * still valid -- but the call is not free and is not const.
 *
 * The conversion is deliberately ONE-WAY. There is no ToQWindow: mapping a
 * native handle back would have to go through QWindow::fromWinId, which
 * fabricates a foreign wrapper rather than returning the original window. A
 * round trip that silently returns a different object is worse than no round
 * trip, and nothing needs one -- the application already holds its QWindow.
 */

namespace GDIQt
{
    /* Forces native window creation if it has not happened yet.
     * A null window yields an invalid handle. */
    EngineCore::WindowHandle ToWindowHandle(QWindow* InWindow);
}

#endif //CORECLIENT_GDIQTWINDOW_H
