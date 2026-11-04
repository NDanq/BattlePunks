#include "Public/GDI/Vulkan/VKBuffer.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/Vulkan/VulkanDynamicGDI.h"
#include "Private/GDI/Vulkan/VKTransientOps.h"

#include <cstring>
#include "Public/GDI/GDILog.h"

using namespace VKOps;

extern GDIDynamicDriver* GDynamicGDI;

VKBuffer::VKBuffer(VKDevice* InDevice, const uint32_t InBytes, const uint8_t InUsage, const void* InInitialData)
    : GDIBuffer(InBytes, InUsage)
    , Device(InDevice)
{
    if(!Device || !Device->IsValid() || InBytes == 0) return;
    VkDevice Dev = Device->GetDevice();

    VkBufferCreateInfo Info{};
    Info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    Info.size        = InBytes;
    Info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if(InUsage & GDI_BU_Storage)  { Info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; }
    if(InUsage & GDI_BU_Instance) { Info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; }
    if(InUsage & GDI_BU_Indirect) { Info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT; }
    if(InUsage & GDI_BU_Index)    { Info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT; }
    Info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(Dev, &Info, nullptr, &Buffer) != VK_SUCCESS)
    {
        GDI_LOG("[VKBuffer] vkCreateBuffer failed.")
        Buffer = VK_NULL_HANDLE;
        return;
    }

    VkMemoryRequirements Req{};
    vkGetBufferMemoryRequirements(Dev, Buffer, &Req);
    const uint32_t Type = Device->FindMemoryType(Req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo Alloc{};
    Alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    Alloc.allocationSize  = Req.size;
    Alloc.memoryTypeIndex = Type;
    if(Type == UINT32_MAX || vkAllocateMemory(Dev, &Alloc, nullptr, &Memory) != VK_SUCCESS)
    {
        GDI_LOG("[VKBuffer] no device-local memory for the buffer.")
        vkDestroyBuffer(Dev, Buffer, nullptr);
        Buffer = VK_NULL_HANDLE;
        Memory = VK_NULL_HANDLE;
        return;
    }
    vkBindBufferMemory(Dev, Buffer, Memory, 0);

    /* Its first contents, synchronously: the data through a staging copy, or zeros. */
    VkBuffer       Staging    = VK_NULL_HANDLE;
    VkDeviceMemory StagingMem = VK_NULL_HANDLE;
    if(InInitialData)
    {
        if(!AllocStagingBuffer(Device, InBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Staging, StagingMem)) return;
        void* Mapped = nullptr;
        vkMapMemory(Dev, StagingMem, 0, InBytes, 0, &Mapped);
        std::memcpy(Mapped, InInitialData, InBytes);
        vkUnmapMemory(Dev, StagingMem);
    }

    VkCommandPool   Pool = VK_NULL_HANDLE;
    VkCommandBuffer Cmd  = VK_NULL_HANDLE;
    if(BeginTransientCmd(Device, Pool, Cmd))
    {
        if(Staging != VK_NULL_HANDLE)
        {
            VkBufferCopy Region{};
            Region.size = InBytes;
            vkCmdCopyBuffer(Cmd, Staging, Buffer, 1, &Region);
        }
        else
        {
            vkCmdFillBuffer(Cmd, Buffer, 0, VK_WHOLE_SIZE, 0);
        }
        EndAndSubmitTransientCmd(Device, Pool, Cmd);
    }
    if(Staging != VK_NULL_HANDLE) { vkDestroyBuffer(Dev, Staging, nullptr); }
    if(StagingMem != VK_NULL_HANDLE) { vkFreeMemory(Dev, StagingMem, nullptr); }
}

VKBuffer::~VKBuffer()
{
    if(Device && Device->IsValid())
    {
        if(Buffer != VK_NULL_HANDLE) vkDestroyBuffer(Device->GetDevice(), Buffer, nullptr);
        if(Memory != VK_NULL_HANDLE) vkFreeMemory(Device->GetDevice(), Memory, nullptr);
    }
}

void VKBuffer::Release()
{
    /* The open frame, or one in flight, may still read it: freed after the
     * next frame-fence wait, as a released texture is. */
    if(auto* GDI = dynamic_cast<VulkanDynamicGDI*>(GDynamicGDI))
    {
        GDI->DeferDestroyBuffer(Buffer, Memory);
        Buffer = VK_NULL_HANDLE;
        Memory = VK_NULL_HANDLE;
    }
    delete this;
}

void VKBuffer::SetDebugName(const std::string& InName)
{
    GDIBuffer::SetDebugName(InName);
    if(Device) { Device->SetObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(Buffer), InName); }
}
