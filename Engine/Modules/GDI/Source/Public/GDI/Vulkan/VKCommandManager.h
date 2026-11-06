#ifndef CORECLIENT_VKCOMMANDMANAGER_H
#define CORECLIENT_VKCOMMANDMANAGER_H

#include <vulkan/vulkan.h>
#include <cstdint>

#include "Public/GDI/GDIDefinitions.h"

class VKDevice;

/* At most this many frames in flight ([Rendering] FramesInFlight). */
constexpr uint32_t VK_FRAMES_IN_FLIGHT_MAX = GDI_FRAMES_IN_FLIGHT_MAX;

/**
 * The render thread's primary command buffers: one command pool, buffer and
 * completion fence per frame slot, so the CPU records frame N+1 while the GPU
 * still runs frame N (RenderCore's MULTITHREADED_RENDERING.md, phase 4).
 *
 * BeginFrame waits for the current slot's last submission -- the frame
 * SlotCount frames back -- then resets its pool and begins its buffer.
 * Advance moves to the next slot once a frame is submitted; a mid-frame
 * submit (FlushForReadback) stays on its slot and begins it again.
 */
class VKCommandManager
{
public:
    VKCommandManager() = default;
    ~VKCommandManager();

    bool Initialize(VKDevice* InDevice, uint32_t InSlots = 1);
    void Shutdown();

    /**
     * Wait for the current slot's previous submission to complete, then reset
     * its pool and begin recording. The wait makes re-recording safe regardless
     * of caller -- a command buffer is never re-begun while still pending.
     */
    VkCommandBuffer BeginFrame();

    /** End recording. Caller submits via VKDevice::QueueSubmit, passing GetFrameFence(). */
    void EndFrame();

    /** The submission just made from the current slot signals InTimelineValue (0: none). */
    void NoteSubmitted(uint64_t InTimelineValue);

    /** On to the next slot (EndRendering, after its submit). */
    void Advance();

    inline VkCommandBuffer GetCommandBuffer() const { return Slots[Current].Buffer; }
    inline bool IsRecording() const { return bRecording; }

    /**
     * Fence that every submission of the current slot's buffer MUST signal (pass
     * it to QueueSubmit). BeginFrame waits and resets it before reusing the buffer.
     */
    inline VkFence GetFrameFence() const { return Slots[Current].Fence; }

    inline uint32_t GetSlot()      const { return Current; }
    inline uint32_t GetSlotCount() const { return SlotCount; }

    /** The timeline value the last BeginFrame waited for (its slot's last frame): done on the GPU. */
    inline uint64_t GetWaitedValue() const { return WaitedValue; }

private:
    struct Slot
    {
        VkCommandPool   Pool           = VK_NULL_HANDLE;
        VkCommandBuffer Buffer         = VK_NULL_HANDLE;
        VkFence         Fence          = VK_NULL_HANDLE;
        uint64_t        SubmittedValue = 0;
    };

    VKDevice* Device      = nullptr;
    Slot      Slots[VK_FRAMES_IN_FLIGHT_MAX];
    uint32_t  SlotCount   = 1;
    uint32_t  Current     = 0;
    uint64_t  WaitedValue = 0;
    bool      bRecording  = false;
};

#endif //CORECLIENT_VKCOMMANDMANAGER_H
