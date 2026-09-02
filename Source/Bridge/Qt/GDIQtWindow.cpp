#include "GDIQtWindow.h"

namespace GDIQt
{
EngineCore::WindowHandle ToWindowHandle(QWindow* InWindow)
{
    if(InWindow == nullptr)
    {
        return EngineCore::WindowHandle();
    }

    // winId() returns WId, which is an integer type wide enough to hold the
    // platform handle (HWND on Windows, the NSView* on macOS). The double cast
    // through uintptr_t is what keeps that legal on both, rather than casting
    // an integer straight to a pointer.
    const WId NativeId = InWindow->winId();
    return EngineCore::WindowHandle(reinterpret_cast<void*>(static_cast<uintptr_t>(NativeId)));
}
}
