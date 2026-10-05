#ifndef CORECLIENT_VKDEVICE_H
#define CORECLIENT_VKDEVICE_H

#include <vulkan/vulkan.h>
#include <mutex>


// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.


/**
 * Owns the core Vulkan device-level objects:
 *   - VkInstance (borrowed from QVulkanInstance)
 *   - VkPhysicalDevice
 *   - VkDevice
 *   - graphics+present queue (single queue family in Phase 1)
 *
 * Submission to the queue is mutex-locked because both the render thread (graphics)
 * and the UI thread (present blit) submit work on the same queue.
 *
 * Phase 1: raw vkAllocateMemory is used for image allocation.
 * Phase 2: migrate allocations to VMA (vk_mem_alloc.h).
 */
class VKDevice
{
public:
    VKDevice();
    ~VKDevice();

    /**
     * Initialize: pick physical device, create logical device + graphics queue.
     * @param InInstance The VkInstance to use. Borrowed, not owned.
     * @return true on success.
     */
    bool Initialize(VkInstance InInstance);

    /** Destroy logical device and clear all handles. */
    void Shutdown();

    /** Submit a command buffer with optional wait/signal sync. Mutex-protected. */
    VkResult QueueSubmit(uint32_t SubmitCount, const VkSubmitInfo* Submits, VkFence Fence);

    /**
     * Submit WITHOUT taking the queue mutex — the caller must already hold
     * GetQueueMutex(). Used when a submit must be atomic with other CPU-side state
     * (e.g. the render thread publishing its timeline value, or the UI-thread present
     * reading it) so submission order and the published value can't interleave.
     */
    VkResult QueueSubmitLocked(uint32_t SubmitCount, const VkSubmitInfo* Submits, VkFence Fence);

    /** The mutex guarding the shared graphics/present queue (see QueueSubmitLocked). */
    inline std::mutex& GetQueueMutex() { return QueueMutex; }

    /** Present a swapchain image. Mutex-protected (shares the queue). */
    VkResult QueuePresent(const VkPresentInfoKHR* PresentInfo);

    /** Wait for the device to finish all submitted work (used at shutdown / resize). */
    void WaitIdle();

    /**
     * Find a memory type matching the requirements.
     * @return memory type index, or UINT32_MAX if none match.
     */
    uint32_t FindMemoryType(uint32_t TypeBits, VkMemoryPropertyFlags Properties) const;

    inline VkInstance       GetInstance() const         { return Instance; }
    inline VkPhysicalDevice GetPhysicalDevice() const   { return PhysicalDevice; }
    inline VkDevice         GetDevice() const           { return Device; }
    inline uint32_t         GetGraphicsQueueFamily() const { return GraphicsQueueFamilyIndex; }
    inline VkQueue          GetGraphicsQueue() const    { return GraphicsQueue; }
    inline bool             IsValid() const             { return Device != VK_NULL_HANDLE; }

    /** Whether InFormat (a block-compressed one) can be sampled: the
     *  textureCompressionBC feature, enabled when the GPU has it, and the
     *  format's own sampled-image support. */
    bool SupportsSampledFormat(VkFormat InFormat) const;

    /** Whether the graphics queue also runs compute (GDISupportsCompute): every
     *  desktop GPU's does, which is what lets one command buffer hold both. */
    inline bool GraphicsQueueSupportsCompute() const { return bGraphicsQueueCompute; }

private:
    bool PickPhysicalDevice();
    bool CreateLogicalDevice();

    VkInstance          Instance                = VK_NULL_HANDLE;   // borrowed; owned by VulkanGDIContext
    VkPhysicalDevice    PhysicalDevice          = VK_NULL_HANDLE;
    VkDevice            Device                  = VK_NULL_HANDLE;
    uint32_t            GraphicsQueueFamilyIndex = UINT32_MAX;
    VkQueue             GraphicsQueue           = VK_NULL_HANDLE;
    bool                bTextureCompressionBC   = false;   // the feature, enabled
    bool                bGraphicsQueueCompute   = false;   // GraphicsQueueFamilyIndex has VK_QUEUE_COMPUTE_BIT

    VkPhysicalDeviceMemoryProperties MemoryProperties{};

    std::mutex              QueueMutex;
};

#endif //CORECLIENT_VKDEVICE_H
