#ifndef CORECLIENT_MTLSYNC_H
#define CORECLIENT_MTLSYNC_H

#include "Public/GDI/GDISync.h"
#include "Public/GDI/Metal/MTLForward.h"

class MTLDevice;

/**
 * GDISync implementation backed by an MTLSharedEvent used as a timeline semaphore
 * (the Metal analog of VKSync's VkFence, and of the RenderTimeline the Vulkan
 * backend threads render->present through).
 *
 * Usage mirrors VKSync:
 *   - AdvanceSignalValue(): the render thread's EndRendering calls this to get the
 *     next monotonic value, then does `[cmdBuf encodeSignalEvent:GetEvent() value:v]`
 *     before commit.
 *   - Fence(): mark that a signal is pending so the next WaitSync actually waits.
 *   - WaitSync(): block on the CPU until the event reaches the last advanced value.
 *
 * A monotonic timeline (rather than a binary fence) is used so the render and UI
 * threads -- driven by independent triggers -- never desync: a wait for ">= last
 * value" is idempotent.
 */
class MTLSync : public GDISync
{
public:
    MTLSync();
    ~MTLSync();

    /** Bind to the device that owns the shared event. Re-entrant. */
    void Bind(MTLDevice* InDevice);

    void Initialize() override;
    void Fence() override;
    void WaitSync() override;
    void DestroySync() override;
    bool IsFenced() override;

    MTLSharedEventRef GetEvent() const { return Event; }

    /** Return the next monotonic value the GPU should signal (render thread only). */
    uint64_t AdvanceSignalValue();

    /** Last value handed out by AdvanceSignalValue(). */
    uint64_t GetSignaledTarget() const { return SignaledTarget; }

private:
    MTLDevice*          Device         = nullptr;
    MTLSharedEventRef   Event          = nullptr;
    void*               Listener       = nullptr;   // MTLSharedEventListener*
    void*               WaitQueue      = nullptr;   // dispatch_queue_t
    uint64_t            SignaledTarget = 0;
    bool                bFenced        = false;
};

#endif //CORECLIENT_MTLSYNC_H
