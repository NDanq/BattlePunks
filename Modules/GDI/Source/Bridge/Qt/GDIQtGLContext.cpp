#include "GDIQtGLContext.h"

#include <QThread>

namespace GDIQt
{
GDIQtGLContext::~GDIQtGLContext()
{
    delete Surface;
    Surface = nullptr;

    // The QOpenGLContext wrappers do not own the native HGLRCs -- GDI's
    // QOpenGLGDIContext created them and deletes them in its own destructor.
    delete SharedContext;
    delete MainContext;
    SharedContext = nullptr;
    MainContext   = nullptr;
}

QOpenGLContext* GDIQtGLContext::Initialize(QWindow* InWindow)
{
    if(Context == nullptr || InWindow == nullptr)
    {
        return nullptr;
    }

    const QSurfaceFormat SurfaceFormat = InWindow->requestedFormat();
    HWND NativeWindow = reinterpret_cast<HWND>(InWindow->winId());

    /*
     * GDI creates both native contexts. The second shares with the first,
     * which is mandatory rather than an optimization: the renderer hands the
     * host a framebuffer texture handle that the host's context samples.
     */
    void* MainNative = Context->InitMainContext(NativeWindow,
                                                SurfaceFormat.majorVersion(),
                                                SurfaceFormat.minorVersion());
    if(MainNative == nullptr)
    {
        return nullptr;
    }

    MainContext = QNativeInterface::QWGLContext::fromNative(
            static_cast<HGLRC>(MainNative), NativeWindow, nullptr);

    void* SharedNative = Context->InitSharedContext(NativeWindow,
                                                    SurfaceFormat.majorVersion(),
                                                    SurfaceFormat.minorVersion());
    if(SharedNative != nullptr)
    {
        SharedContext = QNativeInterface::QWGLContext::fromNative(
                static_cast<HGLRC>(SharedNative), NativeWindow, MainContext);
        if(SharedContext)
        {
            SharedContext->setShareContext(MainContext);
        }
    }

    // The render thread's drawable. Format must match the contexts'.
    Surface = new QOffscreenSurface;
    Surface->setFormat(SurfaceFormat);
    Surface->create();

    // GDI asks this object for make-current from now on.
    Context->SetGLContextInterface(this);

    return MainContext;
}

void GDIQtGLContext::MoveToThread(QThread* InThread)
{
    if(InThread == nullptr)
    {
        return;
    }

    // Qt requires the context and its surface to have the affinity of the
    // thread that makes them current.
    if(SharedContext) SharedContext->moveToThread(InThread);
    if(Surface)       Surface->moveToThread(InThread);
}

bool GDIQtGLContext::MakeCurrent()
{
    if(SharedContext == nullptr || Surface == nullptr)
    {
        return false;
    }
    return SharedContext->makeCurrent(Surface);
}

void GDIQtGLContext::DoneCurrent()
{
    if(SharedContext)
    {
        SharedContext->doneCurrent();
    }
}

void GDIQtGLContext::SwapBuffers()
{
    if(SharedContext && Surface)
    {
        // The offscreen surface, not the window: the host presents separately.
        SharedContext->swapBuffers(Surface);
    }
}

void* GDIQtGLContext::GetNativeHandle() const
{
    return Context ? Context->GetThreadedNativeContext() : nullptr;
}
}
