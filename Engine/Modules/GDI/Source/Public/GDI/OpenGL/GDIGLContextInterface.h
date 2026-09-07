#ifndef CORECLIENT_GDIGLCONTEXTINTERFACE_H
#define CORECLIENT_GDIGLCONTEXTINTERFACE_H

#include "Public/Core/CoreTypes.h"

/*
 * The host-supplied OpenGL context/drawable.
 *
 * WHY GDI DOES NOT DO THIS ITSELF
 *
 * GDI has a complete platform implementation of context creation and
 * make-current (GDIPlatform::appGL*, backed by a hidden window). It works --
 * Release builds render correctly on it. It is not what Odessa uses, because
 * RenderDoc's runtime-injected wglMakeCurrent hook faults on a GL drawable it
 * never saw created, which breaks every Debug build and with it the frame-
 * capture workflow.
 *
 * So the drawable became the host's business. Odessa's Qt bridge implements
 * this over QOpenGLContext + QOffscreenSurface -- byte-for-byte the behaviour
 * that RenderDoc already understands -- while an SDL host implements the same
 * three calls over appGL*. GDI itself stays Qt-free either way, which was the
 * point.
 *
 * THREADING
 *
 * MakeCurrent/DoneCurrent/SwapBuffers are all called on the render thread.
 * Implementations must be prepared for that: the Qt one relies on the host
 * having moved its QOpenGLContext and QOffscreenSurface to that thread first.
 */

class IGDIGLContext
{
public:
    virtual ~IGDIGLContext() = default;

    /* Makes the context current on the calling thread. False on failure. */
    virtual bool MakeCurrent() = 0;

    /* Releases the context from the calling thread. */
    virtual void DoneCurrent() = 0;

    /*
     * Swaps the drawable's buffers.
     *
     * Note this targets the OFFSCREEN drawable, not the visible window -- the
     * host presents the finished frame separately (Odessa hands the UI thread
     * a framebuffer texture handle and blits there). An implementation that
     * swapped the visible window would fight the host for it.
     */
    virtual void SwapBuffers() = 0;

    /* Native context handle (HGLRC / GLXContext), for tooling that needs it.
     * May be null if the implementation does not expose one. */
    virtual void* GetNativeHandle() const { return nullptr; }
};

#endif //CORECLIENT_GDIGLCONTEXTINTERFACE_H
