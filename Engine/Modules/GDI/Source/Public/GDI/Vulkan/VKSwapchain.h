#ifndef CORECLIENT_VKSWAPCHAIN_H
#define CORECLIENT_VKSWAPCHAIN_H

#include <vulkan/vulkan.h>
#include "Public/GDI/Vulkan/GDIVulkanPlatform.h"


#include <atomic>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.
#include <vector>
#include <string>

class VKDevice;
class VKCommandManager;
class VKRenderTarget;

/**
 * UI-thread-owned swapchain + present blit pipeline for Vulkan.
 *
 * Present path (Phase 2 — shader-quad blit):
 *   1. UI thread acquires a swapchain image (signals ImageAvailable semaphore)
 *   2. UI thread records a command buffer that:
 *        - transitions swapchain image to COLOR_ATTACHMENT_OPTIMAL
 *        - dynamic-rendering pass: bind the Screen.vsh/.fsh pipeline, bind a
 *          descriptor set sampling the offscreen RT (kept in
 *          SHADER_READ_ONLY_OPTIMAL after EndRendering), draw a full-screen quad
 *        - transitions swapchain image to PRESENT_SRC_KHR
 *   3. Submits the buffer waiting on (ImageAvailable @ COLOR_ATTACHMENT_OUTPUT,
 *      RenderFinishedSemaphore @ FRAGMENT_SHADER), signaling BlitFinished
 *   4. vkQueuePresentKHR waits on BlitFinished
 *
 * The screen-blit pipeline/sampler/quad are self-contained here (the render-
 * thread VKPipelineCache targets the RT format, not the swapchain format, and
 * is not safe to touch from the UI thread).
 */
class VKSwapchain
{
public:
    VKSwapchain() = default;
    ~VKSwapchain();

    /** Initialize surface + swapchain + UI-thread command pool + per-frame sync. */
    bool Initialize(VkInstance InInstance, VKDevice* InDevice, EngineCore::WindowHandle InWindow);

    /** Tear everything down. */
    void Shutdown();

    /** Recreate swapchain (call on resize). Caller must vkDeviceWaitIdle first. */
    bool Recreate();

    /**
     * Present the supplied offscreen RT to the window.
     * @param InMainRT Offscreen RT to copy from. Skipped (no present) unless its
     *                 tracked layout is SHADER_READ_ONLY_OPTIMAL — i.e. the render
     *                 thread is not mid-frame on it. PresentFrame never mutates the
     *                 RT's tracked layout (that state belongs to the render thread).
     * @param InRenderTimeline  Timeline semaphore signaled by the render thread's submit.
     *                          Pass VK_NULL_HANDLE to skip the render wait.
     * @param InPublishedValue  The render thread's published timeline counter. Loaded
     *                          under the queue mutex immediately before the present
     *                          submit so the wait value can't go stale against a
     *                          concurrently-submitted render frame.
     */
    void PresentFrame(VKRenderTarget* InMainRT, VkSemaphore InRenderTimeline, const std::atomic<uint64_t>* InPublishedValue);

    inline VkExtent2D GetExtent() const { return Extent; }
    inline bool       IsValid() const   { return Swapchain != VK_NULL_HANDLE; }

private:
    bool CreateSwapchain();
    void DestroySwapchain();

    bool CreateUICommandResources();
    void DestroyUICommandResources();

    // Self-contained screen-quad blit pipeline (Screen.vsh/.fsh) targeting the
    // swapchain format. Independent of swapchain extent (viewport is dynamic),
    // so it survives Recreate() and is built once in Initialize().
    bool CreateBlitPipeline();
    void DestroyBlitPipeline();
    VkShaderModule LoadShaderModule(const std::string& SpvResourcePath) const;

    void TransitionImageLayout(VkCommandBuffer Cmd,
                               VkImage Image,
                               VkImageLayout OldLayout, VkImageLayout NewLayout,
                               VkAccessFlags2 SrcAccess, VkAccessFlags2 DstAccess,
                               VkPipelineStageFlags2 SrcStage, VkPipelineStageFlags2 DstStage);

    VkInstance          Instance        = VK_NULL_HANDLE;
    VKDevice*           Device          = nullptr;
    EngineCore::WindowHandle Window;

    VkSurfaceKHR        Surface         = VK_NULL_HANDLE;
    VkSwapchainKHR      Swapchain       = VK_NULL_HANDLE;
    VkFormat            SurfaceFormat   = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR     ColorSpace      = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D          Extent          { 0, 0 };
    bool                bVSync          = false;   // GDIGetVSync as of the chain's creation: FIFO if set
    std::vector<VkImage>        SwapchainImages;
    std::vector<VkImageView>    SwapchainImageViews;

    // UI-thread command pool + buffer (one in flight in Phase 1).
    VkCommandPool       UICommandPool   = VK_NULL_HANDLE;
    VkCommandBuffer     UICommandBuffer = VK_NULL_HANDLE;

    // Per-frame sync (single frame in flight in Phase 1).
    VkSemaphore         ImageAvailable  = VK_NULL_HANDLE;
    VkFence             InFlightFence   = VK_NULL_HANDLE;

    // Present-wait semaphore, ONE PER SWAPCHAIN IMAGE. A semaphore handed to
    // vkQueuePresentKHR stays "in use by the swapchain" until that image index is
    // re-acquired, so a single shared one can't be reused across image indices.
    // Created/destroyed with the swapchain (sized to the image count).
    std::vector<VkSemaphore> BlitFinishedPerImage;

    // Screen-quad blit pipeline (Screen.vsh/.fsh) — see CreateBlitPipeline.
    VkShaderModule        BlitVS            = VK_NULL_HANDLE;
    VkShaderModule        BlitFS            = VK_NULL_HANDLE;
    VkDescriptorSetLayout BlitSetLayout     = VK_NULL_HANDLE;
    VkPipelineLayout      BlitPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            BlitPipeline      = VK_NULL_HANDLE;
    VkSampler             BlitSampler       = VK_NULL_HANDLE;
    VkDescriptorPool      BlitDescPool      = VK_NULL_HANDLE;
    VkDescriptorSet       BlitDescSet       = VK_NULL_HANDLE;
    VkBuffer              QuadVertexBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory        QuadVertexMemory  = VK_NULL_HANDLE;
};

#endif //CORECLIENT_VKSWAPCHAIN_H
