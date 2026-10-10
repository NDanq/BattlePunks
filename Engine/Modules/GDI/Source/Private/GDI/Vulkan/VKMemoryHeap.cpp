#include "Public/GDI/Vulkan/VKMemoryHeap.h"

#include <algorithm>
#include <string>

#include "Public/GDI/GDILog.h"
#include "Public/GDI/Vulkan/VKDevice.h"

namespace
{
    VkDeviceSize AlignUp(const VkDeviceSize InValue, const VkDeviceSize InAlign)
    {
        return InAlign <= 1 ? InValue : (InValue + InAlign - 1) / InAlign * InAlign;
    }
}

// === VKImageHeap ============================================================

void VKImageHeap::Shutdown()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if(Device && Device->IsValid())
    {
        for(Block& Each : Blocks)
        {
            if(Each.Memory != VK_NULL_HANDLE) { vkFreeMemory(Device->GetDevice(), Each.Memory, nullptr); }
        }
    }
    Blocks.clear();
}

bool VKImageHeap::Carve(const uint32_t InBlock, const VkDeviceSize InSize, const VkDeviceSize InAlign, VKAllocation& OutAllocation)
{
    Block& Owner = Blocks[InBlock];
    for(size_t Index = 0; Index < Owner.Free.size(); ++Index)
    {
        const Range  Each  = Owner.Free[Index];
        const VkDeviceSize Start = AlignUp(Each.Offset, InAlign);
        if(Start + InSize > Each.Offset + Each.Size) { continue; }

        /* What is left before and after it stays free. */
        std::vector<Range> Left;
        if(Start > Each.Offset)                         { Left.push_back({ Each.Offset, Start - Each.Offset }); }
        if(Start + InSize < Each.Offset + Each.Size)    { Left.push_back({ Start + InSize, Each.Offset + Each.Size - Start - InSize }); }
        Owner.Free.erase(Owner.Free.begin() + std::ptrdiff_t(Index));
        Owner.Free.insert(Owner.Free.begin() + std::ptrdiff_t(Index), Left.begin(), Left.end());

        Owner.Used += InSize;
        ++Owner.Live;
        OutAllocation.Memory    = Owner.Memory;
        OutAllocation.Offset    = Start;
        OutAllocation.Size      = InSize;
        OutAllocation.Block     = InBlock;
        OutAllocation.TypeIndex = Owner.TypeIndex;
        return true;
    }
    return false;
}

bool VKImageHeap::Allocate(const VkMemoryRequirements& InRequirements, const VkMemoryPropertyFlags InProperties,
                           VKAllocation& OutAllocation)
{
    OutAllocation = VKAllocation();
    if(!Device || !Device->IsValid()) { return false; }
    const uint32_t TypeIndex = Device->FindMemoryType(InRequirements.memoryTypeBits, InProperties);
    if(TypeIndex == UINT32_MAX) { return false; }

    const auto Dedicated = [&]() -> bool
    {
        VkMemoryAllocateInfo Info{};
        Info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        Info.allocationSize  = InRequirements.size;
        Info.memoryTypeIndex = TypeIndex;
        if(vkAllocateMemory(Device->GetDevice(), &Info, nullptr, &OutAllocation.Memory) != VK_SUCCESS)
        {
            OutAllocation.Memory = VK_NULL_HANDLE;
            return false;
        }
        OutAllocation.Size      = InRequirements.size;
        OutAllocation.TypeIndex = TypeIndex;
        std::lock_guard<std::mutex> Lock(Mutex);
        DedicatedBytes += InRequirements.size;
        ++DedicatedCount;
        return true;
    };

    if(InRequirements.size >= SMALL_LIMIT) { return Dedicated(); }

    const VkDeviceSize Kind = BLOCK_BYTES;
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        for(uint32_t Index = 0; Index < uint32_t(Blocks.size()); ++Index)
        {
            if(Blocks[Index].Memory != VK_NULL_HANDLE && Blocks[Index].TypeIndex == TypeIndex && Blocks[Index].Bytes == Kind
               && Carve(Index, InRequirements.size, InRequirements.alignment, OutAllocation))
            {
                return true;
            }
        }
    }

    /* A new block, outside the lock; a device that refuses one gets a dedicated allocation. */
    VkMemoryAllocateInfo Info{};
    Info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    Info.allocationSize  = Kind;
    Info.memoryTypeIndex = TypeIndex;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    if(vkAllocateMemory(Device->GetDevice(), &Info, nullptr, &Memory) != VK_SUCCESS)
    {
        GDI_LOG("[VKImageHeap] a " + std::to_string(Kind >> 20) + " MB block was refused; allocating the image on its own.")
        return Dedicated();
    }

    std::lock_guard<std::mutex> Lock(Mutex);
    uint32_t Slot = 0;
    while(Slot < Blocks.size() && Blocks[Slot].Memory != VK_NULL_HANDLE) { ++Slot; }
    if(Slot == Blocks.size()) { Blocks.emplace_back(); }
    Block& Made = Blocks[Slot];
    Made = Block();
    Made.Memory    = Memory;
    Made.TypeIndex = TypeIndex;
    Made.Bytes     = Kind;
    Made.Free.push_back({ 0, Kind });
    GDI_LOG("[VKImageHeap] block " + std::to_string(Slot) + " made: " + std::to_string(Kind >> 20) + " MB of memory type " + std::to_string(TypeIndex))
    return Carve(Slot, InRequirements.size, InRequirements.alignment, OutAllocation);
}

void VKImageHeap::Free(const VKAllocation& InAllocation)
{
    if(!InAllocation.IsValid()) { return; }
    if(InAllocation.Block == UINT32_MAX)
    {
        if(Device && Device->IsValid()) { vkFreeMemory(Device->GetDevice(), InAllocation.Memory, nullptr); }
        std::lock_guard<std::mutex> Lock(Mutex);
        DedicatedBytes -= (std::min<uint64_t>)(DedicatedBytes, InAllocation.Size);
        if(DedicatedCount > 0) { --DedicatedCount; }
        return;
    }

    std::lock_guard<std::mutex> Lock(Mutex);
    if(InAllocation.Block >= Blocks.size() || Blocks[InAllocation.Block].Memory != InAllocation.Memory)
    {
        return;   // its block was freed by Shutdown: nothing to give back
    }
    Block& Owner = Blocks[InAllocation.Block];

    /* Back in offset order, merged with whatever free range it touches. */
    const auto At = std::lower_bound(Owner.Free.begin(), Owner.Free.end(), InAllocation.Offset,
                                     [](const Range& InRange, const VkDeviceSize InOffset) { return InRange.Offset < InOffset; });
    auto Inserted = Owner.Free.insert(At, Range{ InAllocation.Offset, InAllocation.Size });
    if(Inserted + 1 != Owner.Free.end() && Inserted->Offset + Inserted->Size == (Inserted + 1)->Offset)
    {
        Inserted->Size += (Inserted + 1)->Size;
        Owner.Free.erase(Inserted + 1);
    }
    if(Inserted != Owner.Free.begin() && (Inserted - 1)->Offset + (Inserted - 1)->Size == Inserted->Offset)
    {
        (Inserted - 1)->Size += Inserted->Size;
        Owner.Free.erase(Inserted);
    }
    Owner.Used -= (std::min)(Owner.Used, InAllocation.Size);
    if(Owner.Live > 0) { --Owner.Live; }

    /* Empty: freed, unless it is the one empty block of its type kept for the next texture. */
    if(Owner.Live == 0)
    {
        const bool bOtherEmpty = std::any_of(Blocks.begin(), Blocks.end(), [&](const Block& InOther)
        {
            return &InOther != &Owner && InOther.Memory != VK_NULL_HANDLE && InOther.TypeIndex == Owner.TypeIndex
                && InOther.Bytes == Owner.Bytes && InOther.Live == 0;
        });
        if(bOtherEmpty && Device && Device->IsValid())
        {
            vkFreeMemory(Device->GetDevice(), Owner.Memory, nullptr);
            Owner = Block();
        }
    }
}

VKImageHeap::Stats VKImageHeap::GetStats() const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    Stats Out;
    for(const Block& Each : Blocks)
    {
        if(Each.Memory == VK_NULL_HANDLE) { continue; }
        ++Out.Blocks;
        Out.BlockBytes  += Each.Bytes;
        Out.UsedBytes   += Each.Used;
        Out.Allocations += Each.Live;
    }
    Out.DedicatedBytes = DedicatedBytes;
    Out.Dedicated      = DedicatedCount;
    return Out;
}

// === VKStagingRing ==========================================================

bool VKStagingRing::Create()
{
    bTried = true;
    if(!Device || !Device->IsValid()) { return false; }
    VkDevice Dev = Device->GetDevice();

    VkBufferCreateInfo BufferInfo{};
    BufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BufferInfo.size        = RING_BYTES;
    BufferInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    BufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(Dev, &BufferInfo, nullptr, &Buffer) != VK_SUCCESS) { Buffer = VK_NULL_HANDLE; return false; }

    VkMemoryRequirements Requirements{};
    vkGetBufferMemoryRequirements(Dev, Buffer, &Requirements);
    const uint32_t TypeIndex = Device->FindMemoryType(Requirements.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo Info{};
    Info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    Info.allocationSize  = Requirements.size;
    Info.memoryTypeIndex = TypeIndex;
    void* Address = nullptr;
    if(TypeIndex == UINT32_MAX || vkAllocateMemory(Dev, &Info, nullptr, &Memory) != VK_SUCCESS
       || vkBindBufferMemory(Dev, Buffer, Memory, 0) != VK_SUCCESS
       || vkMapMemory(Dev, Memory, 0, RING_BYTES, 0, &Address) != VK_SUCCESS)
    {
        GDI_LOG("[VKStagingRing] could not make the 16 MB upload ring; uploads stage on their own.")
        if(Memory != VK_NULL_HANDLE) { vkFreeMemory(Dev, Memory, nullptr); Memory = VK_NULL_HANDLE; }
        vkDestroyBuffer(Dev, Buffer, nullptr);
        Buffer = VK_NULL_HANDLE;
        return false;
    }
    Mapped = static_cast<uint8_t*>(Address);
    return true;
}

void VKStagingRing::Shutdown()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if(Device && Device->IsValid())
    {
        VkDevice Dev = Device->GetDevice();
        if(Memory != VK_NULL_HANDLE) { vkUnmapMemory(Dev, Memory); vkFreeMemory(Dev, Memory, nullptr); }
        if(Buffer != VK_NULL_HANDLE) { vkDestroyBuffer(Dev, Buffer, nullptr); }
    }
    Buffer = VK_NULL_HANDLE;
    Memory = VK_NULL_HANDLE;
    Mapped = nullptr;
    Regions.clear();
    Head   = 0;
    bTried = false;
}

bool VKStagingRing::Allocate(const VkDeviceSize InBytes, const VkDeviceSize InAlign, const uint64_t InDoneAt,
                             VkBuffer& OutBuffer, VkDeviceSize& OutOffset, uint8_t*& OutMapped)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if(InBytes == 0 || InBytes > RING_BYTES) { return false; }
    if(!Mapped && (bTried || !Create())) { return false; }

    /* Live bytes run from the oldest region's start to Head, wrapping. */
    VkDeviceSize Start = 0;
    if(Regions.empty())
    {
        Head  = 0;
        Start = 0;
    }
    else
    {
        const VkDeviceSize Tail = Regions.front().Start;
        const VkDeviceSize At   = AlignUp(Head, InAlign);
        if(Head > Tail)
        {
            if(At + InBytes <= RING_BYTES)  { Start = At; }
            else if(InBytes < Tail)         { Start = 0; }   // wraps to the front
            else                            { return false; }
        }
        else
        {
            if(At + InBytes < Tail) { Start = At; }
            else                    { return false; }
        }
    }

    Head = Start + InBytes;
    Regions.push_back({ Start, Head, InDoneAt });
    OutBuffer = Buffer;
    OutOffset = Start;
    OutMapped = Mapped + Start;
    return true;
}

void VKStagingRing::Reclaim(const uint64_t InCompleted)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    while(!Regions.empty() && Regions.front().DoneAt <= InCompleted) { Regions.pop_front(); }
}

VkDeviceSize VKStagingRing::GetCapacity() const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    return Mapped ? RING_BYTES : 0;
}

VkDeviceSize VKStagingRing::GetBytesInUse() const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    VkDeviceSize Bytes = 0;
    for(const Region& Each : Regions) { Bytes += Each.End - Each.Start; }
    return Bytes;
}
