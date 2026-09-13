#ifndef CORECLIENT_GDIQTGLCONTEXT_H
#define CORECLIENT_GDIQTGLCONTEXT_H

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QWindow>

#include "Public/GDI/OpenGL/GDIGLContextInterface.h"
#include "Public/GDI/OpenGL/OpenGLDynamicGDI.h"

/*
 * Qt's implementation of GDI's OpenGL drawable.
 *
 * GDI creates the native contexts (raw WGL, as it always did) and asks this
 * for make-current / done-current / swap. Everything Qt-shaped lives here: the
 * QOpenGLContext wrappers and the QOffscreenSurface the render thread draws
 * into.
 *
 * WHY THE SURFACE IS HERE RATHER THAN IN GDI
 *
 * GDI has a working platform drawable of its own (a hidden window, behind
 * GDIPlatform::appGL*), and Release builds render correctly on it. RenderDoc
 * does not survive it: its runtime-injected wglMakeCurrent hook faults on a GL
 * drawable it never saw created, taking out every Debug build and the frame-
 * capture workflow with it. Keeping Qt's surface here preserves exactly the
 * behaviour RenderDoc already understands, and costs GDI nothing -- GDI still
 * has no idea what a QOffscreenSurface is.
 *
 * An SDL host implements IGDIGLContext over appGL* instead and never compiles
 * this file.
 *
 * THREADING: MoveToThread() must be called before the render thread starts.
 * Qt requires a QOpenGLContext and its surface to have the affinity of the
 * thread that makes them current.
 */

namespace GDIQt
{
    class GDIQtGLContext : public IGDIGLContext
    {
    public:
        explicit GDIQtGLContext(OpenGLGDIContext* InContext) : Context(InContext) {}
        ~GDIQtGLContext() override;

        /*
         * Creates both native contexts through GDI, then wraps them as
         * QOpenGLContext for the host's own present path, and builds the
         * offscreen surface the render thread draws into.
         *
         * Returns the MAIN context: the host needs it for format queries and
         * for RenderDoc's native-handle lookup.
         */
        QOpenGLContext* Initialize(QWindow* InWindow);

        /* Hands the render-thread objects to InThread. Qt requires this before
         * that thread makes the context current. */
        void MoveToThread(QThread* InThread);

        // --- IGDIGLContext ---------------------------------------------------
        bool  MakeCurrent() override;
        void  DoneCurrent() override;
        void  SwapBuffers() override;
        void* GetNativeHandle() const override;

        QOpenGLContext* GetMainContext()   const { return MainContext; }
        QOpenGLContext* GetSharedContext() const { return SharedContext; }

    private:
        OpenGLGDIContext*  Context       = nullptr;   // not owned
        QOpenGLContext*    MainContext   = nullptr;
        QOpenGLContext*    SharedContext = nullptr;
        QOffscreenSurface* Surface       = nullptr;
    };
}

#endif //CORECLIENT_GDIQTGLCONTEXT_H
