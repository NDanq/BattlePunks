#ifndef CORECLIENT_MTLDEVICE_H
#define CORECLIENT_MTLDEVICE_H

#include "Public/GDI/Metal/MTLForward.h"

/**
 * Owns the core Metal device-level objects (the Metal analog of VKDevice):
 *   - MTLDevice (the GPU)
 *   - the primary graphics MTLCommandQueue (render-thread submissions)
 *
 * Unlike Vulkan there is no VkInstance / physical-device pick / queue-family
 * selection: MTLCreateSystemDefaultDevice() returns the system GPU directly, and
 * MTLCommandQueue is internally thread-safe, so no submit mutex is required (the
 * Vulkan QueueMutex / QueueSubmitLocked machinery has no Metal counterpart).
 *
 * The UI-thread present uses its OWN command queue (created off this device and
 * owned by MTLSwapchain), ordered against the render queue via an MTLSharedEvent
 * timeline in MTLSync -- see the plan's present architecture.
 */
class MTLDevice
{
public:
    MTLDevice();
    ~MTLDevice();

    /** Create the system default MTLDevice and the primary graphics queue. */
    bool Initialize();

    /** Release the queue and device. */
    void Shutdown();

    /** Create an additional command queue off this device (e.g. the UI present queue). */
    MTLCommandQueueRef CreateCommandQueue() const;

    MTLDeviceRef       GetDevice() const        { return Device; }
    MTLCommandQueueRef GetGraphicsQueue() const { return GraphicsQueue; }
    bool               IsValid() const          { return Device != nullptr; }

private:
    MTLDeviceRef        Device        = nullptr;
    MTLCommandQueueRef  GraphicsQueue = nullptr;
};

#endif //CORECLIENT_MTLDEVICE_H
