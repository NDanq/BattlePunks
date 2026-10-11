#include "Public/Core/CoreMemoryPool.h"

#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"

#include <cstdlib>
#include <cstring>
#include <string>

/*
 * The pool registry and the three pools Core ships.
 *
 * Everything here deals in raw bytes. Nothing in this file knows what a type,
 * a destructor or a GC object is -- that all lives one layer up in
 * CoreMemory.cpp.
 */

namespace EngineCore
{
namespace
{
    /*
     * CONSTANT-INITIALISED into .bss. No constructor, no ordering question, and
     * therefore valid from the first instruction of the first dynamic
     * initialiser in the process -- which matters because a file-scope object
     * anywhere in any image may allocate. This is the same argument
     * CoreConfig.h makes for GConfigHead, and it is the reason POOL_Default is
     * a code path rather than an entry in this table.
     */
    IMemoryPool* GPools[POOL_MAX] = {};

    /* std::mutex has a constexpr default constructor, so this is
     * constant-initialised too and does not reintroduce the ordering problem
     * the raw table above exists to avoid. */
    std::mutex GRegistryMutex;

    constexpr bool IsPowerOfTwo(const size_t InValue)
    {
        return InValue != 0 && (InValue & (InValue - 1)) == 0;
    }

    constexpr size_t AlignUp(const size_t InValue, const size_t InAlign)
    {
        return (InValue + InAlign - 1) & ~(InAlign - 1);
    }

    uint8_t* AlignUpPtr(uint8_t* InPtr, const size_t InAlign)
    {
        const uintptr_t Raw = reinterpret_cast<uintptr_t>(InPtr);
        const uintptr_t Up  = (Raw + InAlign - 1) & ~uintptr_t(InAlign - 1);
        return reinterpret_cast<uint8_t*>(Up);
    }

    /* Copies into a fixed buffer, always NUL-terminating. A pool name that is
     * too long is truncated rather than rejected -- it is diagnostic text, and
     * failing to construct a pool over a long name would be absurd. */
    void CopyName(char* OutDst, const size_t InCapacity, const char* InName)
    {
        if(!InName)
        {
            OutDst[0] = '\0';
            return;
        }

        size_t Index = 0;
        while(Index + 1 < InCapacity && InName[Index] != '\0')
        {
            OutDst[Index] = InName[Index];
            ++Index;
        }
        OutDst[Index] = '\0';
    }

    void RecordAllocation(MemoryPoolStats& InOutStats, const size_t InBytes)
    {
        InOutStats.BytesInUse += InBytes;
        ++InOutStats.LiveAllocations;
        ++InOutStats.AllocationCount;

        if(InOutStats.BytesInUse > InOutStats.PeakBytesInUse)
        {
            InOutStats.PeakBytesInUse = InOutStats.BytesInUse;
        }
    }

    void RecordFree(MemoryPoolStats& InOutStats, const size_t InBytes)
    {
        // Saturate rather than wrap. A mismatched free is already a bug; a
        // BytesInUse of 18446744073709551000 in the log makes it look like a
        // different, much more alarming bug.
        InOutStats.BytesInUse = (InOutStats.BytesInUse >= InBytes)
                              ? (InOutStats.BytesInUse - InBytes) : 0;

        if(InOutStats.LiveAllocations > 0)
        {
            --InOutStats.LiveAllocations;
        }
        ++InOutStats.FreeCount;
    }
}

// === Raw system allocation ==================================================

void* SystemAlignedAlloc(const size_t InBytes, const size_t InAlign)
{
    if(InBytes == 0 || !IsPowerOfTwo(InAlign))
    {
        return nullptr;
    }

#if defined(_MSC_VER)
    return ::_aligned_malloc(InBytes, InAlign);
#else
    /*
     * std::aligned_alloc requires the SIZE to be a multiple of the alignment.
     * glibc tolerates a violation, the standard does not, and musl does not --
     * so round rather than find out per-libc. Rounding up is always safe: the
     * caller asked for InBytes and gets at least that.
     */
    return std::aligned_alloc(InAlign, AlignUp(InBytes, InAlign));
#endif
}

void SystemAlignedFree(void* InPtr)
{
    if(!InPtr)
    {
        return;
    }

#if defined(_MSC_VER)
    // NOT free(). _aligned_malloc offsets the block and stashes the real base
    // in front of it; plain free() on that pointer corrupts the CRT heap.
    ::_aligned_free(InPtr);
#else
    std::free(InPtr);
#endif
}

// === Registry ===============================================================

PoolID RegisterMemoryPool(IMemoryPool* InPool)
{
    if(!InPool)
    {
        return POOL_Invalid;
    }

    std::lock_guard<std::mutex> Lock(GRegistryMutex);

    // Slot 0 is POOL_Default and is never filled -- see CoreMemoryPool.h.
    for(PoolID Index = 1; Index < POOL_MAX; ++Index)
    {
        if(GPools[Index] == nullptr)
        {
            GPools[Index] = InPool;
            return Index;
        }
    }

    CORE_LOG_ERROR(std::string("Memory: pool table full (") + std::to_string(int(POOL_MAX) - 1) +
                   " slots); cannot register '" + InPool->GetName() + "'")
    return POOL_Invalid;
}

void UnregisterMemoryPool(const PoolID InPool)
{
    if(InPool == POOL_Default || InPool >= POOL_MAX)
    {
        return;
    }

    std::lock_guard<std::mutex> Lock(GRegistryMutex);

    IMemoryPool* Pool = GPools[InPool];
    if(!Pool)
    {
        return;
    }

    /*
     * REFUSE while memory is still out.
     *
     * Every live allocation's header names this slot. Clearing it turns each
     * of them into a block that Memory::Free cannot route anywhere -- the
     * pointer is valid, the pool it belongs to is gone, and the failure
     * surfaces at some unrelated later Dealloc. Refusing here keeps the
     * diagnostic attached to the actual mistake.
     */
    MemoryPoolStats Stats;
    Pool->GetStats(Stats);
    if(Stats.LiveAllocations > 0)
    {
        CORE_LOG_ERROR(std::string("Memory: refusing to unregister pool '") + Pool->GetName() +
                       "' with " + std::to_string(Stats.LiveAllocations) +
                       " live allocation(s); free them first")
        return;
    }

    GPools[InPool] = nullptr;
}

IMemoryPool* GetMemoryPool(const PoolID InPool)
{
    // POOL_Default has no object by design, so slot 0 reads back null and the
    // caller falls through to the CRT path. That is not an error.
    if(InPool >= POOL_MAX)
    {
        return nullptr;
    }
    return GPools[InPool];
}

PoolID FindMemoryPool(const char* InName)
{
    if(!InName)
    {
        return POOL_Invalid;
    }

    std::lock_guard<std::mutex> Lock(GRegistryMutex);

    for(PoolID Index = 1; Index < POOL_MAX; ++Index)
    {
        if(GPools[Index] && std::strcmp(GPools[Index]->GetName(), InName) == 0)
        {
            return Index;
        }
    }
    return POOL_Invalid;
}

// === VirtualRegion ==========================================================

VirtualRegion::~VirtualRegion()
{
    Release();
}

bool VirtualRegion::Reserve(const size_t InBytes)
{
    if(Base || InBytes == 0)
    {
        return false;
    }

    /*
     * Round the bookkeeping to whole pages BEFORE reserving. Both platforms
     * round the reservation up internally, so the range really is this big;
     * recording the un-rounded number would make EnsureCommitted refuse a
     * commit of the last partial page that would in fact have succeeded.
     */
    const size_t Page   = appMemPageSize();
    const size_t Rounded = AlignUp(InBytes, Page);

    void* Reserved = appMemReserve(Rounded);
    if(!Reserved)
    {
        return false;
    }

    Base           = static_cast<uint8_t*>(Reserved);
    ReservedBytes  = Rounded;
    CommittedBytes = 0;
    return true;
}

bool VirtualRegion::EnsureCommitted(const size_t InBytes)
{
    if(!Base)
    {
        return false;
    }
    if(InBytes <= CommittedBytes)
    {
        return true;
    }
    if(InBytes > ReservedBytes)
    {
        return false;
    }

    /*
     * Commit in chunks, not exactly what was asked for.
     *
     * A pool that grows an object at a time would otherwise issue one
     * VirtualAlloc/mprotect syscall per page, which is a system call every
     * 4KB of a sequential fill. 64KB granules cut that by 16x and waste at
     * most 64KB per pool.
     */
    const size_t Page  = appMemPageSize();
    const size_t Chunk = (Page > (64u * 1024u)) ? Page : (64u * 1024u);

    size_t Target = AlignUp(InBytes, Chunk);
    if(Target > ReservedBytes)
    {
        Target = ReservedBytes;     // already page-aligned; see Reserve
    }

    if(!appMemCommit(Base + CommittedBytes, Target - CommittedBytes))
    {
        return false;
    }

    CommittedBytes = Target;
    return true;
}

void VirtualRegion::Release()
{
    if(Base)
    {
        appMemRelease(Base, ReservedBytes);
    }
    Base           = nullptr;
    ReservedBytes  = 0;
    CommittedBytes = 0;
}

// === HeapMemoryPool =========================================================

HeapMemoryPool::HeapMemoryPool(const char* InName)
{
    CopyName(Name, NAME_CAPACITY, InName);
}

HeapMemoryPool::~HeapMemoryPool()
{
    if(Stats.LiveAllocations > 0)
    {
        CORE_LOG_ERROR(std::string("Memory: HeapMemoryPool '") + Name + "' destroyed with " +
                       std::to_string(Stats.LiveAllocations) + " live allocation(s), " +
                       std::to_string(Stats.BytesInUse) + " bytes leaked")
    }
}

void* HeapMemoryPool::Allocate(const size_t InBytes, const size_t InAlign)
{
    void* Block = SystemAlignedAlloc(InBytes, InAlign);
    if(!Block)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> Lock(Mutex);
    RecordAllocation(Stats, InBytes);
    return Block;
}

void HeapMemoryPool::Deallocate(void* InPtr, const size_t InBytes, const size_t /*InAlign*/)
{
    if(!InPtr)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> Lock(Mutex);
        RecordFree(Stats, InBytes);
    }

    SystemAlignedFree(InPtr);
}

void HeapMemoryPool::GetStats(MemoryPoolStats& OutStats) const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    OutStats = Stats;
}

// === FixedBlockPool =========================================================

FixedBlockPool::FixedBlockPool(const char*  InName,
                               const size_t InBlockBytes,
                               const size_t InBlockAlign,
                               const size_t InMaxBlocks)
{
    CopyName(Name, NAME_CAPACITY, InName);

    /*
     * A free block stores the next free block's address in its own first
     * bytes, so a block smaller than a pointer cannot hold the free list. Round
     * up rather than reject: the caller asked for a pool of small blocks and
     * gets one, very slightly larger than requested.
     */
    BlockBytes = (InBlockBytes < sizeof(void*)) ? sizeof(void*) : InBlockBytes;

    BlockAlign = (IsPowerOfTwo(InBlockAlign) && InBlockAlign >= alignof(void*))
               ? InBlockAlign : alignof(void*);

    Stride    = AlignUp(BlockBytes, BlockAlign);
    MaxBlocks = InMaxBlocks;

    if(MaxBlocks == 0)
    {
        CORE_LOG_ERROR(std::string("Memory: FixedBlockPool '") + Name + "' created with 0 blocks")
        return;
    }

    // + BlockAlign so the first block can be aligned inside the region even if
    // the reservation base happens not to satisfy an unusually large
    // BlockAlign. In practice the base is page-aligned and this pads nothing,
    // but "in practice" is not a guarantee the platform seam makes.
    if(!Region.Reserve(Stride * MaxBlocks + BlockAlign))
    {
        CORE_LOG_ERROR(std::string("Memory: FixedBlockPool '") + Name + "' could not reserve " +
                       std::to_string(Stride * MaxBlocks) + " bytes of address space")
    }
}

FixedBlockPool::~FixedBlockPool()
{
    if(Stats.LiveAllocations > 0)
    {
        CORE_LOG_ERROR(std::string("Memory: FixedBlockPool '") + Name + "' destroyed with " +
                       std::to_string(Stats.LiveAllocations) + " live allocation(s)")
    }
}

bool FixedBlockPool::Owns(const void* InPtr) const
{
    if(!InPtr || !Region.IsValid())
    {
        return false;
    }

    const uint8_t* Blocks = AlignUpPtr(Region.GetBase(), BlockAlign);
    const uint8_t* Ptr    = static_cast<const uint8_t*>(InPtr);

    if(Ptr < Blocks || Ptr >= Blocks + (Stride * MaxBlocks))
    {
        return false;
    }

    // Inside the range but not ON a block boundary means the caller is handing
    // back an interior pointer, which is a different bug from a foreign one.
    return (size_t(Ptr - Blocks) % Stride) == 0;
}

void* FixedBlockPool::Allocate(const size_t InBytes, const size_t InAlign)
{
    if(InBytes > BlockBytes || InAlign > BlockAlign || !Region.IsValid())
    {
        /*
         * Deliberately silent. A pool that is a poor fit for one request is a
         * normal thing for a caller to discover by asking, and Memory::Allocate
         * logs the failure once with the type and size in hand -- which is far
         * more useful than a message from here that knows neither.
         */
        return nullptr;
    }

    std::lock_guard<std::mutex> Lock(Mutex);

    uint8_t* Blocks = AlignUpPtr(Region.GetBase(), BlockAlign);
    void*    Block  = nullptr;

    if(FreeHead)
    {
        Block = FreeHead;
        // Read the next pointer out of the block BEFORE handing the block over.
        std::memcpy(&FreeHead, Block, sizeof(void*));
    }
    else if(HighWater < MaxBlocks)
    {
        const size_t NeedBytes = size_t(Blocks - Region.GetBase()) + ((HighWater + 1) * Stride);
        if(!Region.EnsureCommitted(NeedBytes))
        {
            return nullptr;
        }

        Block = Blocks + (HighWater * Stride);
        ++HighWater;
    }
    else
    {
        return nullptr;     // genuinely exhausted
    }

    RecordAllocation(Stats, Stride);
    return Block;
}

void FixedBlockPool::Deallocate(void* InPtr, const size_t /*InBytes*/, const size_t /*InAlign*/)
{
    if(!InPtr)
    {
        return;
    }

    if(!Owns(InPtr))
    {
        CORE_LOG_ERROR(std::string("Memory: FixedBlockPool '") + Name +
                       "' asked to free a pointer it does not own; ignoring")
        return;
    }

    std::lock_guard<std::mutex> Lock(Mutex);

    // Push onto the free list by writing the old head into the block itself.
    std::memcpy(InPtr, &FreeHead, sizeof(void*));
    FreeHead = InPtr;

    RecordFree(Stats, Stride);
}

void FixedBlockPool::GetStats(MemoryPoolStats& OutStats) const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    OutStats                = Stats;
    OutStats.BytesReserved  = Region.GetReservedBytes();
    OutStats.BytesCommitted = Region.GetCommittedBytes();
}

// === LinearMemoryPool =======================================================

LinearMemoryPool::LinearMemoryPool(const char*  InName,
                                   const size_t InReserveBytes,
                                   const bool   bInThreadSafe)
    : bThreadSafe(bInThreadSafe)
{
    CopyName(Name, NAME_CAPACITY, InName);

    if(!Region.Reserve(InReserveBytes))
    {
        CORE_LOG_ERROR(std::string("Memory: LinearMemoryPool '") + Name + "' could not reserve " +
                       std::to_string(InReserveBytes) + " bytes of address space")
    }
}

LinearMemoryPool::~LinearMemoryPool() = default;

void* LinearMemoryPool::Allocate(const size_t InBytes, const size_t InAlign)
{
    if(InBytes == 0 || !IsPowerOfTwo(InAlign) || !Region.IsValid())
    {
        return nullptr;
    }

    /*
     * std::unique_lock rather than lock_guard because the lock is CONDITIONAL:
     * a single-threaded frame arena skips it entirely, and that is the whole
     * reason bThreadSafe exists.
     */
    std::unique_lock<std::mutex> Lock(Mutex, std::defer_lock);
    if(bThreadSafe)
    {
        Lock.lock();
    }

    uint8_t* const Base    = Region.GetBase();
    uint8_t* const Aligned = AlignUpPtr(Base + Offset, InAlign);
    const size_t   Start   = size_t(Aligned - Base);
    const size_t   End     = Start + InBytes;

    if(End > Region.GetReservedBytes() || !Region.EnsureCommitted(End))
    {
        return nullptr;
    }

    Offset = End;
    RecordAllocation(Stats, InBytes);
    return Aligned;
}

void LinearMemoryPool::Deallocate(void* /*InPtr*/, const size_t /*InBytes*/, const size_t /*InAlign*/)
{
    /*
     * INTENTIONALLY EMPTY -- this is what PC_None means.
     *
     * A bump allocator cannot reclaim an individual block; that is the trade
     * it makes for an allocation that is a pointer add. Reclaim happens in
     * Reset(), all at once. Stats are NOT decremented here, because they are
     * not: the bytes really are still in use until the reset.
     */
}

void LinearMemoryPool::Reset()
{
    std::unique_lock<std::mutex> Lock(Mutex, std::defer_lock);
    if(bThreadSafe)
    {
        Lock.lock();
    }

    /*
     * The pages stay COMMITTED. Decommitting here would hand them back to the
     * OS and re-fault every one of them in on the next frame -- for an arena
     * that is reset every frame, that is the entire cost of the arena, paid
     * every frame, to save memory that is about to be used again anyway.
     */
    Offset                = 0;
    Stats.BytesInUse      = 0;
    Stats.LiveAllocations = 0;
}

size_t LinearMemoryPool::GetBytesUsed() const
{
    std::unique_lock<std::mutex> Lock(Mutex, std::defer_lock);
    if(bThreadSafe)
    {
        Lock.lock();
    }
    return Offset;
}

void LinearMemoryPool::GetStats(MemoryPoolStats& OutStats) const
{
    std::unique_lock<std::mutex> Lock(Mutex, std::defer_lock);
    if(bThreadSafe)
    {
        Lock.lock();
    }

    OutStats                = Stats;
    OutStats.BytesReserved  = Region.GetReservedBytes();
    OutStats.BytesCommitted = Region.GetCommittedBytes();
}

// === BucketMemoryPool =======================================================

BucketMemoryPool::BucketMemoryPool(const char* InName, const size_t InMinBytes, const size_t InMaxBytes,
                                   const size_t InMaxCachedBytes, const size_t InHeaderSlack)
    : MaxCachedBytes(InMaxCachedBytes)
{
    CopyName(Name, NAME_CAPACITY, InName);

    /* 2^k and 1.5 x 2^k from the first power of two at or above InMinBytes. */
    size_t Power = 64;
    while(Power < InMinBytes) { Power *= 2; }
    while(ClassCount < CLASS_MAX && Power <= InMaxBytes)
    {
        ClassBytes[ClassCount++] = Power + InHeaderSlack;
        if(ClassCount < CLASS_MAX && Power + Power / 2 <= InMaxBytes) { ClassBytes[ClassCount++] = Power + Power / 2 + InHeaderSlack; }
        Power *= 2;
    }
}

BucketMemoryPool::~BucketMemoryPool()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    TrimTo(0);
    if(Stats.LiveAllocations > 0)
    {
        CORE_LOG_ERROR(std::string("Memory: BucketMemoryPool '") + Name + "' destroyed with " +
                       std::to_string(Stats.LiveAllocations) + " live allocation(s), " +
                       std::to_string(Stats.BytesInUse) + " bytes leaked")
    }
}

int BucketMemoryPool::ClassOf(const size_t InBytes, const size_t InAlign) const
{
    if(InAlign > BLOCK_ALIGN || ClassCount == 0 || InBytes > ClassBytes[ClassCount - 1]) { return -1; }
    /* The classes ascend: the first that holds it. Few enough that a walk beats a search. */
    for(int Class = 0; Class < ClassCount; ++Class)
    {
        if(InBytes <= ClassBytes[Class]) { return Class; }
    }
    return -1;
}

void* BucketMemoryPool::Allocate(const size_t InBytes, const size_t InAlign)
{
    const int Class = ClassOf(InBytes, InAlign);
    if(Class < 0)
    {
        void* Block = SystemAlignedAlloc(InBytes, InAlign < BLOCK_ALIGN ? size_t(BLOCK_ALIGN) : InAlign);
        if(!Block) { return nullptr; }
        std::lock_guard<std::mutex> Lock(Mutex);
        RecordAllocation(Stats, InBytes);
        return Block;
    }

    const size_t Bytes = ClassBytes[Class];
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        if(void* Kept = FreeHead[Class])
        {
            std::memcpy(&FreeHead[Class], Kept, sizeof(void*));
            --FreeCount[Class];
            CachedBytes -= Bytes;
            RecordAllocation(Stats, Bytes);
            return Kept;
        }
    }

    /* Nothing kept: a new block, outside the lock. */
    void* Block = SystemAlignedAlloc(Bytes, BLOCK_ALIGN);
    if(!Block) { return nullptr; }
    std::lock_guard<std::mutex> Lock(Mutex);
    RecordAllocation(Stats, Bytes);
    return Block;
}

void BucketMemoryPool::Deallocate(void* InPtr, const size_t InBytes, const size_t InAlign)
{
    if(!InPtr) { return; }
    const int Class = ClassOf(InBytes, InAlign);
    if(Class >= 0)
    {
        const size_t Bytes = ClassBytes[Class];
        std::lock_guard<std::mutex> Lock(Mutex);
        RecordFree(Stats, Bytes);
        if(CachedBytes + Bytes <= MaxCachedBytes)
        {
            std::memcpy(InPtr, &FreeHead[Class], sizeof(void*));
            FreeHead[Class] = InPtr;
            ++FreeCount[Class];
            CachedBytes += Bytes;
            return;
        }
    }
    else
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        RecordFree(Stats, InBytes);
    }
    SystemAlignedFree(InPtr);
}

void BucketMemoryPool::GetStats(MemoryPoolStats& OutStats) const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    OutStats = Stats;
    OutStats.BytesCommitted = Stats.BytesInUse + CachedBytes;
    OutStats.BytesReserved  = OutStats.BytesCommitted;
}

void BucketMemoryPool::TrimTo(const size_t InBytes)
{
    /* The largest classes first: fewest frees for the most bytes. */
    for(int Class = ClassCount - 1; Class >= 0 && CachedBytes > InBytes; --Class)
    {
        while(FreeHead[Class] && CachedBytes > InBytes)
        {
            void* Block = FreeHead[Class];
            std::memcpy(&FreeHead[Class], Block, sizeof(void*));
            --FreeCount[Class];
            CachedBytes -= ClassBytes[Class];
            SystemAlignedFree(Block);
        }
    }
}

void BucketMemoryPool::Trim()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    TrimTo(0);
}

void BucketMemoryPool::SetMaxCachedBytes(const size_t InBytes)
{
    std::lock_guard<std::mutex> Lock(Mutex);
    MaxCachedBytes = InBytes;
    TrimTo(InBytes);
}

size_t BucketMemoryPool::GetCachedBytes() const
{
    std::lock_guard<std::mutex> Lock(Mutex);
    return CachedBytes;
}
}
