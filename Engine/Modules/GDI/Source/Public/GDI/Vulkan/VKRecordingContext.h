#ifndef CORECLIENT_VKRECORDINGCONTEXT_H
#define CORECLIENT_VKRECORDINGCONTEXT_H

#include <vulkan/vulkan.h>
#include <vector>

#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include "Public/GDI/Vulkan/VKCommandManager.h"

class VKDescriptorAllocator;

/**
 * A parallel pass's recording context on Vulkan (GDIRecordingContext; RenderCore's
 * MULTITHREADED_RENDERING.md, phase 2): one thread's chunk of the pass, recorded
 * into a secondary command buffer that continues the primary's rendering.
 *
 * Everything a draw writes is its own -- the command buffer, a descriptor
 * allocator (pools and a uniform-ring slice), the draw caches, the blend and
 * depth states, the viewport, the core blocks -- so contexts record at once.
 * What they share is read-only while the pass is open: the device, the pipeline
 * cache (locked), the shaders, meshes and textures.
 *
 * Kept by the driver across frames (VulkanDynamicGDI::RecordingContexts), with
 * a command pool and allocator per frame in flight (Frames, made on first
 * need). Each pass takes a fresh secondary from the recording frame slot's
 * pool; ResetRecordingContexts (BeginRendering, once that slot's fence is
 * waited) resets the slot's pool and allocator.
 */
class VKRecordingContext : public GDIRecordingContext
{
public:
    struct FrameResources
    {
        VkCommandPool                Pool       = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> Buffers;           // allocated from Pool: one a pass, reused each time round
        size_t                       NextBuffer = 0;
        VKDescriptorAllocator*       DescAlloc  = nullptr;
    };
    FrameResources               Frames[VK_FRAMES_IN_FLIGHT_MAX];
    VkCommandBuffer              Cmd        = VK_NULL_HANDLE;   // the open pass's
    VKDescriptorAllocator*       DescAlloc  = nullptr;          // the open pass's frame slot's
    VKDrawCaches                 Caches;
    VKBlendState                 Blend;
    VKDepthStencilState          Depth;
    VkViewport                   Viewport{};
    VkRect2D                     Scissor{};
    VKShaderResource::CoreBlocks Core;
    int                          LabelDepth = 0;
    uint64_t                     PassGeneration = 0;   // the blocks' serials' high bits, a new one each pass
    bool                         bRecording = false;
};

#endif //CORECLIENT_VKRECORDINGCONTEXT_H
