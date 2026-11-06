#include "Public/GDI/Vulkan/VKCommandManager.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/GDILog.h"

#include <algorithm>

VKCommandManager::~VKCommandManager()
{
    Shutdown();
}

bool VKCommandManager::Initialize(VKDevice* InDevice, const uint32_t InSlots)
{
    Device = InDevice;
    if(!Device || !Device->IsValid())
    {
        return false;
    }
    SlotCount = std::clamp<uint32_t>(InSlots, 1, VK_FRAMES_IN_FLIGHT_MAX);
    Current   = 0;

    for(uint32_t Index = 0; Index < SlotCount; ++Index)
    {
        Slot& Each = Slots[Index];

        VkCommandPoolCreateInfo PoolInfo{};
        PoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        PoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        PoolInfo.queueFamilyIndex = Device->GetGraphicsQueueFamily();
        if(vkCreateCommandPool(Device->GetDevice(), &PoolInfo, nullptr, &Each.Pool) != VK_SUCCESS)
        {
            GDI_LOG("[VKCommandManager] vkCreateCommandPool failed.")
            return false;
        }

        VkCommandBufferAllocateInfo AllocInfo{};
        AllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        AllocInfo.commandPool        = Each.Pool;
        AllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        AllocInfo.commandBufferCount = 1;
        if(vkAllocateCommandBuffers(Device->GetDevice(), &AllocInfo, &Each.Buffer) != VK_SUCCESS)
        {
            GDI_LOG("[VKCommandManager] vkAllocateCommandBuffers failed.")
            return false;
        }

        // Completion fence -- created signaled so the slot's first BeginFrame returns immediately.
        VkFenceCreateInfo FenceInfo{};
        FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if(vkCreateFence(Device->GetDevice(), &FenceInfo, nullptr, &Each.Fence) != VK_SUCCESS)
        {
            GDI_LOG("[VKCommandManager] vkCreateFence failed.")
            return false;
        }
    }
    return true;
}

void VKCommandManager::Shutdown()
{
    for(Slot& Each : Slots)
    {
        if(Device && Device->IsValid())
        {
            if(Each.Fence != VK_NULL_HANDLE) vkDestroyFence(Device->GetDevice(), Each.Fence, nullptr);
            if(Each.Pool  != VK_NULL_HANDLE) vkDestroyCommandPool(Device->GetDevice(), Each.Pool, nullptr);
        }
        Each = Slot();
    }
    Device     = nullptr;
    Current    = 0;
    bRecording = false;
}

VkCommandBuffer VKCommandManager::BeginFrame()
{
    Slot& Each = Slots[Current];
    if(!Device || Each.Buffer == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    // Block until the slot's previous submission has completed before reusing
    // it. Every submit signals its slot's fence, so this is the single
    // authoritative gate -- re-recording a still-pending command buffer (the
    // resize/re-init storm) is safe. A fence also covers every earlier
    // submission to the queue: WaitedValue and all before it are done.
    vkWaitForFences(Device->GetDevice(), 1, &Each.Fence, VK_TRUE, UINT64_MAX);
    vkResetFences(Device->GetDevice(), 1, &Each.Fence);
    WaitedValue = std::max(WaitedValue, Each.SubmittedValue);

    vkResetCommandPool(Device->GetDevice(), Each.Pool, 0);

    VkCommandBufferBeginInfo BeginInfo{};
    BeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    BeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(Each.Buffer, &BeginInfo);

    bRecording = true;
    return Each.Buffer;
}

void VKCommandManager::EndFrame()
{
    if(!bRecording) return;
    vkEndCommandBuffer(Slots[Current].Buffer);
    bRecording = false;
}

void VKCommandManager::NoteSubmitted(const uint64_t InTimelineValue)
{
    if(InTimelineValue != 0) { Slots[Current].SubmittedValue = InTimelineValue; }
}

void VKCommandManager::Advance()
{
    Current = (Current + 1) % SlotCount;
}
