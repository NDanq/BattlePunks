#ifndef CORECLIENT_VKDEVICE_H
#define CORECLIENT_VKDEVICE_H

#include <string>

#include <vulkan/vulkan.h>

#include "Public/GDI/Vulkan/VKMemoryHeap.h"
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

    /** Names InHandle (of InType) for capture tools and validation messages,
     *  through VK_EXT_debug_utils; nothing when the instance lacks it, the
     *  handle or name is empty, or [Shaders] ShaderDebug is off. */
    void SetObjectName(VkObjectType InType, uint64_t InHandle, const std::string& InName) const;

    /** Attaches InTag's bytes to InHandle under InTagName (VK_EXT_debug_utils
     *  object tags), for a tool that looks for that name; nothing when the
     *  instance lacks the extension or the handle or tag is empty. */
    void SetObjectTag(VkObjectType InType, uint64_t InHandle, uint64_t InTagName, const std::string& InTag) const;

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
    /* InFormat as a render target: an attachment that blends, sampled with linear filtering, copied to and from. */
    bool SupportsRenderTargetFormat(VkFormat InFormat) const;

    /* Anisotropic filtering: 1 if the device has no samplerAnisotropy, else its limit (maxSamplerAnisotropy). */
    inline float GetMaxSamplerAnisotropy() const { return MaxSamplerAnisotropy; }

    /** Whether the graphics queue also runs compute (GDISupportsCompute): every
     *  desktop GPU's does, which is what lets one command buffer hold both. */
    inline bool GraphicsQueueSupportsCompute() const { return bGraphicsQueueCompute; }

    /** VK_EXT_conditional_rendering and its feature, enabled when the GPU has
     *  them: what lets an occlusion query skip draws (GDISetOcclusionPredicate). */
    inline bool SupportsConditionalRendering() const { return bConditionalRendering; }

    /** Secondary command buffers' inheritance features, enabled when the GPU
     *  has them (parallel recording's gate, GDIParallelRecordingSupport):
     *  an occlusion query, or conditional rendering, begun on the primary and
     *  still in effect inside the secondaries it executes. */
    inline bool SupportsInheritedQueries() const              { return bInheritedQueries; }
    inline bool SupportsInheritedConditionalRendering() const { return bInheritedConditionalRendering; }

    /** Sampled images' memory, sub-allocated from blocks, and the upload ring
     *  (VKMemoryHeap.h). Freed at Shutdown, before the device. */
    inline VKImageHeap&   GetImageHeap()   { return ImageHeap; }
    inline VKStagingRing& GetStagingRing() { return StagingRing; }

private:
    bool PickPhysicalDevice();
    bool CreateLogicalDevice();

    VkInstance          Instance                = VK_NULL_HANDLE;   // borrowed; owned by VulkanGDIContext
    VkPhysicalDevice    PhysicalDevice          = VK_NULL_HANDLE;
    VkDevice            Device                  = VK_NULL_HANDLE;
    uint32_t            GraphicsQueueFamilyIndex = UINT32_MAX;
    VkQueue             GraphicsQueue           = VK_NULL_HANDLE;
    bool                bTextureCompressionBC   = false;   // the feature, enabled
    float               MaxSamplerAnisotropy    = 1.0f;    // samplerAnisotropy enabled: the device's limit
    bool                bGraphicsQueueCompute   = false;   // GraphicsQueueFamilyIndex has VK_QUEUE_COMPUTE_BIT
    bool                bConditionalRendering   = false;   // VK_EXT_conditional_rendering, enabled
    bool                bInheritedQueries       = false;   // inheritedQueries, enabled
    bool                bInheritedConditionalRendering = false;   // inheritedConditionalRendering, enabled

    VkPhysicalDeviceMemoryProperties MemoryProperties{};

    std::mutex              QueueMutex;

    VKImageHeap             ImageHeap;
    VKStagingRing           StagingRing;
};

#endif //CORECLIENT_VKDEVICE_H
