#ifndef CORECLIENT_QMTLCOMMANDMANAGER_H
#define CORECLIENT_QMTLCOMMANDMANAGER_H

#include "Public/GDI/Metal/MTLForward.h"

class QMTLDevice;

/**
 * Render-thread command-buffer lifecycle -- the Metal counterpart of
 * QVKCommandManager.
 *
 * Metal has no command pool and no reusable command buffers: every frame gets a
 * fresh MTLCommandBuffer from the queue, and the buffer releases itself once the
 * GPU is done with it. What survives from the Vulkan design is the *pacing*:
 * BeginFrame blocks until the previously committed buffer has completed, giving
 * exactly one frame in flight (the analog of waiting QVKCommandManager's
 * FrameFence) so the render thread can never run ahead of the GPU.
 *
 * Encoders are NOT owned here -- QMetalDynamicGDI opens and closes them as render
 * targets are bound, because only it knows the pass load/store actions.
 */
class QMTLCommandManager
{
public:
    QMTLCommandManager() = default;
    ~QMTLCommandManager();

    bool Initialize(QMTLDevice* InDevice);
    void Shutdown();

    /**
     * Wait for the previously committed buffer to finish, then start a new one.
     * Returns nullptr if the manager is not initialized.
     */
    MTLCommandBufferRef BeginFrame();

    /** Mark recording finished (the driver has already ended any open encoder). */
    void EndFrame();

    /** Commit the current buffer and keep it as the one BeginFrame/WaitIdle wait on. */
    void Commit();

    /** Block until the last committed buffer has completed. */
    void WaitIdle();

    MTLCommandBufferRef GetCommandBuffer() const { return CommandBuffer; }
    bool                IsRecording()      const { return bRecording; }

private:
    void ReleaseLastCommitted();

    QMTLDevice*         Device        = nullptr;
    MTLCommandBufferRef CommandBuffer = nullptr;   // retained while recording
    MTLCommandBufferRef LastCommitted = nullptr;   // retained until it completes
    bool                bRecording    = false;
};

#endif //CORECLIENT_QMTLCOMMANDMANAGER_H
