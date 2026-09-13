#ifndef CORECLIENT_VKCOMMANDMANAGER_H
#define CORECLIENT_VKCOMMANDMANAGER_H

#include <vulkan/vulkan.h>

class VKDevice;

/**
 * Single-threaded command pool + primary command buffer pair.
 * One instance per thread that records commands (render thread, UI thread).
 *
 * Phase 1: 1 frame in flight, 1 command buffer per pool. BeginFrame resets the pool.
 * Phase 2 will expand to per-frame-in-flight pools.
 */
class VKCommandManager
{
public:
    VKCommandManager() = default;
    ~VKCommandManager();

    bool Initialize(VKDevice* InDevice);
    void Shutdown();

    /**
     * Wait for the previous submission of the command buffer to complete, then reset
     * the pool and begin recording. The wait makes re-recording safe regardless of
     * caller — a command buffer is never re-begun while still pending on the GPU.
     */
    VkCommandBuffer BeginFrame();

    /** End recording. Caller submits via VKDevice::QueueSubmit, passing GetFrameFence(). */
    void EndFrame();

    inline VkCommandBuffer GetCommandBuffer() const { return CommandBuffer; }
    inline bool IsRecording() const { return bRecording; }

    /**
     * Fence that every submission of this command buffer MUST signal (pass it to
     * QueueSubmit). BeginFrame waits and resets it before reusing the buffer.
     */
    inline VkFence GetFrameFence() const { return FrameFence; }

private:
    VKDevice*       Device          = nullptr;
    VkCommandPool   CommandPool     = VK_NULL_HANDLE;
    VkCommandBuffer CommandBuffer   = VK_NULL_HANDLE;
    VkFence         FrameFence      = VK_NULL_HANDLE;
    bool            bRecording      = false;
};

#endif //CORECLIENT_VKCOMMANDMANAGER_H
