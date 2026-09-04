#include "Public/GDI/Vulkan/QVKSync.h"
#include "Public/GDI/Vulkan/QVKDevice.h"

QVKSync::QVKSync() = default;

QVKSync::~QVKSync()
{
    DestroySync();
}

void QVKSync::Bind(QVKDevice* InDevice)
{
    if(Device == InDevice) return;
    DestroySync();
    Device = InDevice;
    Initialize();
}

void QVKSync::Initialize()
{
    if(!Device || !Device->IsValid())                   return;
    if(InternalFence != VK_NULL_HANDLE)                 return;

    // Created unsignaled: WaitSync() is gated on bFenced (false until the first
    // EndRendering Fence()), so the first wait is skipped regardless. Creating it
    // signaled would make the first submit pass an already-signaled fence (invalid).
    VkFenceCreateInfo FenceInfo{};
    FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    FenceInfo.flags = 0;
    vkCreateFence(Device->GetDevice(), &FenceInfo, nullptr, &InternalFence);
    bFenced = false;
}

void QVKSync::Fence()
{
    // The fence is signaled by the queue submission that the caller pairs with this object;
    // QVulkanDynamicGDI::EndRendering passes GetFence() into vkQueueSubmit. Mark as fenced so
    // the next WaitSync actually waits.
    bFenced = true;
}

void QVKSync::WaitSync()
{
    if(!Device || InternalFence == VK_NULL_HANDLE) return;
    if(!bFenced) return;

    vkWaitForFences(Device->GetDevice(), 1, &InternalFence, VK_TRUE, UINT64_MAX);
    vkResetFences(Device->GetDevice(), 1, &InternalFence);
    bFenced = false;
}

void QVKSync::DestroySync()
{
    if(Device && Device->IsValid() && InternalFence != VK_NULL_HANDLE)
    {
        vkDestroyFence(Device->GetDevice(), InternalFence, nullptr);
    }
    InternalFence = VK_NULL_HANDLE;
    bFenced       = false;
}

bool QVKSync::IsFenced()
{
    return bFenced;
}
