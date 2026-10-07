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

    if(!GrowPool())        return false;
    if(!CreateRingChunk()) return false;

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

    // Destroy the UBO ring's chunks.
    for(RingChunk& Chunk : RingChunks)
    {
        if(Chunk.Mapped && Chunk.Memory != VK_NULL_HANDLE) vkUnmapMemory(Dev, Chunk.Memory);
        if(Chunk.Buffer != VK_NULL_HANDLE)                 vkDestroyBuffer(Dev, Chunk.Buffer, nullptr);
        if(Chunk.Memory != VK_NULL_HANDLE)                 vkFreeMemory(Dev, Chunk.Memory, nullptr);
    }
    RingChunks.clear();
    ActiveChunk = 0;
    RingOffset  = 0;

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
    ActiveChunk = 0;
    RingOffset  = 0;
    ++Epoch;
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
    if(RingChunks.empty() || Size > kRingCapacity)
        return VKUBOAllocation{};

    // Align offset to kUBOAlignment; past the chunk's end, the next chunk.
    VkDeviceSize Aligned = (RingOffset + kUBOAlignment - 1) & ~(kUBOAlignment - 1);
    if(Aligned + Size > kRingCapacity)
    {
        if(ActiveChunk + 1 >= RingChunks.size())
        {
            if(RingChunks.size() >= kRingChunkMax || !CreateRingChunk())
            {
                if(!bWarnedFull)
                {
                    bWarnedFull = true;
                    GDI_LOG("[VKDescriptorAllocator] UBO ring full at "
                         + ::GDILogDetail::GDINum(RingChunks.size() * (kRingCapacity / 1024))
                         + " KB this frame; draws past it are dropped.")
                }
                return VKUBOAllocation{};
            }
        }
        ++ActiveChunk;
        Aligned = 0;
    }

    const RingChunk& Chunk = RingChunks[ActiveChunk];
    VKUBOAllocation Alloc;
    Alloc.Buffer    = Chunk.Buffer;
    Alloc.Offset    = Aligned;
    Alloc.MappedPtr = static_cast<uint8_t*>(Chunk.Mapped) + Aligned;
    RingOffset = Aligned + Size;
    return Alloc;
}

// ---------------------------------------------------------------------------
// Pool growth
// ---------------------------------------------------------------------------

bool VKDescriptorAllocator::GrowPool()
{
    // Pool sizes:
    //   UNIFORM_BUFFER         — up to 6 UBO bindings per set (VP@0, Transform@1, material@2, scene@3, skin@12, previous skin@13)
    //   COMBINED_IMAGE_SAMPLER — up to 2 sampler bindings per set
    //   STORAGE_BUFFER         — a compute set's buffers (GDI_COMPUTE_MAX_STORAGE_BUFFERS)
    VkDescriptorPoolSize PoolSizes[3]{};
    PoolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    PoolSizes[0].descriptorCount = kSetsPerPool * 6;
    PoolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    PoolSizes[1].descriptorCount = kSetsPerPool * 8;   // a material set holds GDI_MATERIAL_TEXTURE_SLOTS
    PoolSizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    PoolSizes[2].descriptorCount = kSetsPerPool * 4;

    VkDescriptorPoolCreateInfo PoolCI{};
    PoolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    PoolCI.maxSets       = kSetsPerPool;
    PoolCI.poolSizeCount = 3;
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

bool VKDescriptorAllocator::CreateRingChunk()
{
    VkDevice Dev = Device->GetDevice();
    RingChunk Chunk;

    VkBufferCreateInfo BufCI{};
    BufCI.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BufCI.size        = kRingCapacity;
    BufCI.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    BufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if(vkCreateBuffer(Dev, &BufCI, nullptr, &Chunk.Buffer) != VK_SUCCESS)
    {
        GDI_LOG("[VKDescriptorAllocator] vkCreateBuffer (ring) failed.")
        return false;
    }

    /* A chunk that fails part-way leaves nothing behind. */
    const auto Discard = [Dev, &Chunk]()
    {
        if(Chunk.Buffer != VK_NULL_HANDLE) vkDestroyBuffer(Dev, Chunk.Buffer, nullptr);
        if(Chunk.Memory != VK_NULL_HANDLE) vkFreeMemory(Dev, Chunk.Memory, nullptr);
        return false;
    };

    VkMemoryRequirements MemReqs;
    vkGetBufferMemoryRequirements(Dev, Chunk.Buffer, &MemReqs);

    VkMemoryAllocateInfo AllocInfo{};
    AllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AllocInfo.allocationSize  = MemReqs.size;
    AllocInfo.memoryTypeIndex = Device->FindMemoryType(
        MemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if(AllocInfo.memoryTypeIndex == UINT32_MAX)
    {
        GDI_LOG("[VKDescriptorAllocator] No host-coherent memory type for UBO ring.")
        return Discard();
    }

    if(vkAllocateMemory(Dev, &AllocInfo, nullptr, &Chunk.Memory) != VK_SUCCESS)
    {
        GDI_LOG("[VKDescriptorAllocator] vkAllocateMemory (ring) failed.")
        return Discard();
    }

    vkBindBufferMemory(Dev, Chunk.Buffer, Chunk.Memory, 0);

    if(vkMapMemory(Dev, Chunk.Memory, 0, kRingCapacity, 0, &Chunk.Mapped) != VK_SUCCESS)
    {
        GDI_LOG("[VKDescriptorAllocator] vkMapMemory (ring) failed.")
        return Discard();
    }

    RingChunks.push_back(Chunk);
    GDI_LOG("[VKDescriptorAllocator] UBO ring chunk " + ::GDILogDetail::GDINum(RingChunks.size() - 1) + " allocated ("
         + ::GDILogDetail::GDINum(kRingCapacity / 1024) + " KB).")
    return true;
}
