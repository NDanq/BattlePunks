#include "Public/GDI/Vulkan/VKDescriptorAllocator.h"
#include "Public/GDI/Vulkan/VKDevice.h"
#include "Public/GDI/GDILog.h"

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

// ---------------------------------------------------------------------------
// Life-cycle
// ---------------------------------------------------------------------------

bool VKDescriptorAllocator::Initialize(VKDevice* InDevice)
{
    if(!InDevice || !InDevice->IsValid()) return false;
    Device = InDevice;

    if(!GrowPool())       return false;
    if(!CreateRingBuffer()) return false;

    GDI_LOG("[VKDescriptorAllocator] Initialized.")
    return true;
}

void VKDescriptorAllocator::Shutdown()
{
    if(!Device || !Device->IsValid()) return;

    VkDevice Dev = Device->GetDevice();

    // Destroy descriptor pools.
    for(VkDescriptorPool Pool : Pools)
    {
        if(Pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(Dev, Pool, nullptr);
    }
    Pools.clear();
    ActivePool = 0;

    // Destroy UBO ring buffer.
    if(RingMapped && RingMemory != VK_NULL_HANDLE)
    {
        vkUnmapMemory(Dev, RingMemory);
        RingMapped = nullptr;
    }
    if(RingBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(Dev, RingBuffer, nullptr);
        RingBuffer = VK_NULL_HANDLE;
    }
    if(RingMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(Dev, RingMemory, nullptr);
        RingMemory = VK_NULL_HANDLE;
    }
    RingOffset = 0;

    Device = nullptr;
}

// ---------------------------------------------------------------------------
// Per-frame reset
// ---------------------------------------------------------------------------

void VKDescriptorAllocator::Reset()
{
    if(!Device || Pools.empty()) return;

    VkDevice Dev = Device->GetDevice();
    for(VkDescriptorPool Pool : Pools)
    {
        if(Pool != VK_NULL_HANDLE)
            vkResetDescriptorPool(Dev, Pool, 0);
    }
    ActivePool = 0;

    // Rewind UBO ring so the same memory is reused each frame.
    RingOffset = 0;
}

// ---------------------------------------------------------------------------
// Descriptor set allocation
// ---------------------------------------------------------------------------

VkDescriptorSet VKDescriptorAllocator::Allocate(VkDescriptorSetLayout Layout)
{
    if(!Device || Pools.empty()) return VK_NULL_HANDLE;

    // Try the active pool first; on exhaustion grow and retry once.
    for(int Attempt = 0; Attempt < 2; ++Attempt)
    {
        VkDescriptorSetAllocateInfo AllocInfo{};
        AllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        AllocInfo.descriptorPool     = Pools[ActivePool];
        AllocInfo.descriptorSetCount = 1;
        AllocInfo.pSetLayouts        = &Layout;

        VkDescriptorSet Set = VK_NULL_HANDLE;
        const VkResult  Res = vkAllocateDescriptorSets(Device->GetDevice(), &AllocInfo, &Set);

        if(Res == VK_SUCCESS)
            return Set;

        if(Res == VK_ERROR_OUT_OF_POOL_MEMORY || Res == VK_ERROR_FRAGMENTED_POOL)
        {
            ++ActivePool;
            if(ActivePool >= Pools.size())
            {
                if(!GrowPool())
                    return VK_NULL_HANDLE;
            }
            continue;
        }

        GDI_LOG("[VKDescriptorAllocator] vkAllocateDescriptorSets failed: " + ::GDILogDetail::GDINum(Res))
        return VK_NULL_HANDLE;
    }

    GDI_LOG("[VKDescriptorAllocator] Failed to allocate descriptor set after pool growth.")
    return VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// UBO ring buffer allocation
// ---------------------------------------------------------------------------

VKUBOAllocation VKDescriptorAllocator::AllocateUBO(VkDeviceSize Size)
{
    if(RingBuffer == VK_NULL_HANDLE || RingMapped == nullptr)
        return VKUBOAllocation{};

    // Align offset to kUBOAlignment.
    const VkDeviceSize Aligned = (RingOffset + kUBOAlignment - 1) & ~(kUBOAlignment - 1);
    if(Aligned + Size > kRingCapacity)
    {
        GDI_LOG("[VKDescriptorAllocator] UBO ring buffer full; increase kRingCapacity.")
        return VKUBOAllocation{};
    }

    VKUBOAllocation Alloc;
    Alloc.Buffer    = RingBuffer;
    Alloc.Offset    = Aligned;
    Alloc.MappedPtr = static_cast<uint8_t*>(RingMapped) + Aligned;
    RingOffset = Aligned + Size;
    return Alloc;
}

// ---------------------------------------------------------------------------
// Pool growth
// ---------------------------------------------------------------------------

bool VKDescriptorAllocator::GrowPool()
{
    // Pool sizes:
    //   UNIFORM_BUFFER         — up to 4 UBO bindings per set (VP@0, Transform@1, custom@2, spare)
    //   COMBINED_IMAGE_SAMPLER — up to 2 sampler bindings per set
    VkDescriptorPoolSize PoolSizes[2]{};
    PoolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    PoolSizes[0].descriptorCount = kSetsPerPool * 4;
    PoolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    PoolSizes[1].descriptorCount = kSetsPerPool * 8;   // a material set holds GDI_MATERIAL_TEXTURE_SLOTS

    VkDescriptorPoolCreateInfo PoolCI{};
    PoolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    PoolCI.maxSets       = kSetsPerPool;
    PoolCI.poolSizeCount = 2;
    PoolCI.pPoolSizes    = PoolSizes;
    // No FREE_DESCRIPTOR_SET_BIT — reset semantics only.

    VkDescriptorPool Pool = VK_NULL_HANDLE;
    const VkResult   Res  = vkCreateDescriptorPool(Device->GetDevice(), &PoolCI, nullptr, &Pool);
    if(Res != VK_SUCCESS)
    {
        GDI_LOG("[VKDescriptorAllocator] vkCreateDescriptorPool failed: " + ::GDILogDetail::GDINum(Res))
        return false;
    }

    Pools.push_back(Pool);
    GDI_LOG("[VKDescriptorAllocator] Pool " + ::GDILogDetail::GDINum(Pools.size() - 1)
         + " created (" + ::GDILogDetail::GDINum(kSetsPerPool) + " sets).")
    return true;
}

// ---------------------------------------------------------------------------
// UBO ring buffer creation
// ---------------------------------------------------------------------------

bool VKDescriptorAllocator::CreateRingBuffer()
{
    VkDevice Dev = Device->GetDevice();

    VkBufferCreateInfo BufCI{};
    BufCI.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BufCI.size        = kRingCapacity;
    BufCI.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    BufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if(vkCreateBuffer(Dev, &BufCI, nullptr, &RingBuffer) != VK_SUCCESS)
    {
        GDI_LOG("[VKDescriptorAllocator] vkCreateBuffer (ring) failed.")
        return false;
    }

    VkMemoryRequirements MemReqs;
    vkGetBufferMemoryRequirements(Dev, RingBuffer, &MemReqs);

    VkMemoryAllocateInfo AllocInfo{};
    AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AllocInfo.allocationSize  = MemReqs.size;
    AllocInfo.memoryTypeIndex = Device->FindMemoryType(
        MemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if(AllocInfo.memoryTypeIndex == UINT32_MAX)
    {
        GDI_LOG("[VKDescriptorAllocator] No host-coherent memory type for UBO ring.")
        return false;
    }

    if(vkAllocateMemory(Dev, &AllocInfo, nullptr, &RingMemory) != VK_SUCCESS)
    {
        GDI_LOG("[VKDescriptorAllocator] vkAllocateMemory (ring) failed.")
        return false;
    }

    vkBindBufferMemory(Dev, RingBuffer, RingMemory, 0);

    if(vkMapMemory(Dev, RingMemory, 0, kRingCapacity, 0, &RingMapped) != VK_SUCCESS)
    {
        GDI_LOG("[VKDescriptorAllocator] vkMapMemory (ring) failed.")
        return false;
    }

    GDI_LOG("[VKDescriptorAllocator] UBO ring buffer allocated ("
         + ::GDILogDetail::GDINum(kRingCapacity / 1024) + " KB).")
    return true;
}
