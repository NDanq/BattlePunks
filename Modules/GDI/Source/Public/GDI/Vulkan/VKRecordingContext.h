#ifndef CORECLIENT_VKRECORDINGCONTEXT_H
#define CORECLIENT_VKRECORDINGCONTEXT_H

#include <vulkan/vulkan.h>
#include <vector>

#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"

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
 * Kept by the driver across frames (VulkanDynamicGDI::RecordingContexts). Each
 * pass takes a fresh secondary from Pool; ResetRecordingContexts (BeginRendering,
 * once the frame's fence is waited) resets the pool and the allocator.
 */
class VKRecordingContext : public GDIRecordingContext
{
public:
    VkCommandPool                Pool       = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> Buffers;           // allocated from Pool: one a pass, reused each frame
    size_t                       NextBuffer = 0;
    VkCommandBuffer              Cmd        = VK_NULL_HANDLE;   // the open pass's
    VKDescriptorAllocator*       DescAlloc  = nullptr;
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
