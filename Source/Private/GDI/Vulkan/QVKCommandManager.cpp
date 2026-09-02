#include "Public/GDI/Vulkan/QVKCommandManager.h"
#include "Public/GDI/Vulkan/QVKDevice.h"
#include "Public/GDI/GDILog.h"

QVKCommandManager::~QVKCommandManager()
{
    Shutdown();
}

bool QVKCommandManager::Initialize(QVKDevice* InDevice)
{
    Device = InDevice;
    if(!Device || !Device->IsValid())
    {
        return false;
    }

    VkCommandPoolCreateInfo PoolInfo{};
    PoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    PoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    PoolInfo.queueFamilyIndex = Device->GetGraphicsQueueFamily();

    if(vkCreateCommandPool(Device->GetDevice(), &PoolInfo, nullptr, &CommandPool) != VK_SUCCESS)
    {
        GDI_LOG("[QVKCommandManager] vkCreateCommandPool failed.")
        return false;
    }

    VkCommandBufferAllocateInfo AllocInfo{};
    AllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    AllocInfo.commandPool        = CommandPool;
    AllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    AllocInfo.commandBufferCount = 1;

    if(vkAllocateCommandBuffers(Device->GetDevice(), &AllocInfo, &CommandBuffer) != VK_SUCCESS)
    {
        GDI_LOG("[QVKCommandManager] vkAllocateCommandBuffers failed.")
        return false;
    }

    // Completion fence — created signaled so the first BeginFrame returns immediately.
    VkFenceCreateInfo FenceInfo{};
    FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if(vkCreateFence(Device->GetDevice(), &FenceInfo, nullptr, &FrameFence) != VK_SUCCESS)
    {
        GDI_LOG("[QVKCommandManager] vkCreateFence failed.")
        return false;
    }

    return true;
}

void QVKCommandManager::Shutdown()
{
    if(Device && Device->IsValid())
    {
        if(FrameFence != VK_NULL_HANDLE)
            vkDestroyFence(Device->GetDevice(), FrameFence, nullptr);
        if(CommandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(Device->GetDevice(), CommandPool, nullptr);
    }
    FrameFence    = VK_NULL_HANDLE;
    CommandPool   = VK_NULL_HANDLE;
    CommandBuffer = VK_NULL_HANDLE;
    Device        = nullptr;
    bRecording    = false;
}

VkCommandBuffer QVKCommandManager::BeginFrame()
{
    if(!Device || CommandBuffer == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    // Block until the previous submission of this buffer has completed before reusing
    // it. Every submit signals FrameFence, so this is the single authoritative gate —
    // re-recording a still-pending command buffer (the resize/re-init storm) is now safe.
    if(FrameFence != VK_NULL_HANDLE)
    {
        vkWaitForFences(Device->GetDevice(), 1, &FrameFence, VK_TRUE, UINT64_MAX);
        vkResetFences(Device->GetDevice(), 1, &FrameFence);
    }

    vkResetCommandPool(Device->GetDevice(), CommandPool, 0);

    VkCommandBufferBeginInfo BeginInfo{};
    BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(CommandBuffer, &BeginInfo);

    bRecording = true;
    return CommandBuffer;
}

void QVKCommandManager::EndFrame()
{
    if(!bRecording) return;
    vkEndCommandBuffer(CommandBuffer);
    bRecording = false;
}
