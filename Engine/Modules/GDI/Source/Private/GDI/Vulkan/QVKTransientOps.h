#ifndef CORECLIENT_QVKTRANSIENTOPS_H
#define CORECLIENT_QVKTRANSIENTOPS_H

#include <vulkan/vulkan.h>

#include "Public/GDI/Vulkan/QVKDevice.h"
#include "Public/GDI/GDILog.h"

// ---------------------------------------------------------------------------
//  Shared transient-command / staging helpers for the Vulkan backend.
//
//  Used by synchronous one-shot GPU operations (texture upload/readback,
//  render-target readback, render-target resolve) on the render thread.
//  Each helper builds its own short-lived resources and submits synchronously.
// ---------------------------------------------------------------------------

namespace VKOps
{

/** Allocate a host-coherent buffer of the given size.  Returns true on success. */
inline bool AllocStagingBuffer(QVKDevice*         Device,
                               VkDeviceSize       Size,
                               VkBufferUsageFlags Usage,
                               VkBuffer&          OutBuf,
                               VkDeviceMemory&    OutMem)
{
    VkBufferCreateInfo BufInfo{};
    BufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BufInfo.size        = Size;
    BufInfo.usage       = Usage;
    BufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if(vkCreateBuffer(Device->GetDevice(), &BufInfo, nullptr, &OutBuf) != VK_SUCCESS)
    {
        GDI_LOG("[VKOps] vkCreateBuffer failed for staging buffer.")
        return false;
    }

    VkMemoryRequirements MemReq{};
    vkGetBufferMemoryRequirements(Device->GetDevice(), OutBuf, &MemReq);

    const uint32_t MemType = Device->FindMemoryType(
        MemReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if(MemType == UINT32_MAX)
    {
        GDI_LOG("[VKOps] No host-visible memory type for staging buffer.")
        vkDestroyBuffer(Device->GetDevice(), OutBuf, nullptr);
        OutBuf = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo AllocInfo{};
    AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AllocInfo.allocationSize  = MemReq.size;
    AllocInfo.memoryTypeIndex = MemType;

    if(vkAllocateMemory(Device->GetDevice(), &AllocInfo, nullptr, &OutMem) != VK_SUCCESS)
    {
        GDI_LOG("[VKOps] vkAllocateMemory failed for staging buffer.")
        vkDestroyBuffer(Device->GetDevice(), OutBuf, nullptr);
        OutBuf = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(Device->GetDevice(), OutBuf, OutMem, 0);
    return true;
}

/** Create a transient pool + one-shot command buffer and begin recording. */
inline bool BeginTransientCmd(QVKDevice*       Device,
                              VkCommandPool&   OutPool,
                              VkCommandBuffer& OutCmd)
{
    VkCommandPoolCreateInfo PoolInfo{};
    PoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    PoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    PoolInfo.queueFamilyIndex = Device->GetGraphicsQueueFamily();

    if(vkCreateCommandPool(Device->GetDevice(), &PoolInfo, nullptr, &OutPool) != VK_SUCCESS)
    {
        GDI_LOG("[VKOps] vkCreateCommandPool failed for transient cmd.")
        return false;
    }

    VkCommandBufferAllocateInfo CBInfo{};
    CBInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    CBInfo.commandPool        = OutPool;
    CBInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    CBInfo.commandBufferCount = 1;

    vkAllocateCommandBuffers(Device->GetDevice(), &CBInfo, &OutCmd);

    VkCommandBufferBeginInfo BeginInfo{};
    BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(OutCmd, &BeginInfo);
    return true;
}

/** End recording, submit synchronously (WaitIdle), and destroy the pool. */
inline void EndAndSubmitTransientCmd(QVKDevice*      Device,
                                     VkCommandPool   Pool,
                                     VkCommandBuffer Cmd)
{
    vkEndCommandBuffer(Cmd);

    VkSubmitInfo Submit{};
    Submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    Submit.commandBufferCount = 1;
    Submit.pCommandBuffers    = &Cmd;
    Device->QueueSubmit(1, &Submit, VK_NULL_HANDLE);
    Device->WaitIdle();

    // Pool destruction frees the command buffer automatically.
    vkDestroyCommandPool(Device->GetDevice(), Pool, nullptr);
}

/** Build a synchronization2 VkImageMemoryBarrier2 for a layout transition. */
inline VkImageMemoryBarrier2 MakeImageBarrier(VkImage               InImage,
                                              VkImageLayout         OldLayout,
                                              VkImageLayout         NewLayout,
                                              VkPipelineStageFlags2 SrcStage,
                                              VkAccessFlags2        SrcAccess,
                                              VkPipelineStageFlags2 DstStage,
                                              VkAccessFlags2        DstAccess)
{
    VkImageMemoryBarrier2 B{};
    B.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    B.srcStageMask                    = SrcStage;
    B.srcAccessMask                   = SrcAccess;
    B.dstStageMask                    = DstStage;
    B.dstAccessMask                   = DstAccess;
    B.oldLayout                       = OldLayout;
    B.newLayout                       = NewLayout;
    B.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    B.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    B.image                           = InImage;
    B.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    B.subresourceRange.baseMipLevel   = 0;
    B.subresourceRange.levelCount     = 1;
    B.subresourceRange.baseArrayLayer = 0;
    B.subresourceRange.layerCount     = 1;
    return B;
}

inline void RecordBarrier(VkCommandBuffer Cmd, const VkImageMemoryBarrier2& Barrier)
{
    VkDependencyInfo Dep{};
    Dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    Dep.imageMemoryBarrierCount  = 1;
    Dep.pImageMemoryBarriers     = &Barrier;
    vkCmdPipelineBarrier2(Cmd, &Dep);
}

} // namespace VKOps

#endif // CORECLIENT_QVKTRANSIENTOPS_H
