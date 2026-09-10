#include "Public/Core/CoreMemory.h"

#include "Private/Core/CoreMemoryInternal.h"
#include "Public/Core/CoreLog.h"

#include <cstring>
#include <mutex>
#include <string>

/*
 * Memory::Allocate / Free, the reference-count lifecycle, and image identity.
 *
 * EVERY FUNCTION HERE IS THE ONLY COPY IN THE PROCESS. That is the point of
 * the file -- see the structural rule at the top of CoreMemory.h. Nothing in
 * this file may become inline or move into a header.
 */

namespace EngineCore
{
namespace Memory
{
namespace
{
    constexpr bool IsPowerOfTwo(const size_t InValue)
    {
        return InValue != 0 && (InValue & (InValue - 1)) == 0;
    }

    constexpr size_t AlignUp(const size_t InValue, const size_t InAlign)
    {
        return (InValue + InAlign - 1) & ~(InAlign - 1);
    }

    /*
     * The header sits immediately below the payload, so this is a fixed
     * negative offset and not a search.
     *
     * READING BEFORE AN ARBITRARY POINTER IS THE TRICK AND ALSO THE HAZARD:
     * if InPtr did not come from Allocate, this reads 48 bytes that belong to
     * somebody else. The Magic check makes a false POSITIVE effectively
     * impossible (1 in 2^32) but cannot make the read itself safe. Never call
     * this on a pointer that has no chance of being a Core allocation.
     */
    AllocHeader* MutableHeader(void* InPtr)
    {
        if(!InPtr)
        {
            return nullptr;
        }

        AllocHeader* Header = reinterpret_cast<AllocHeader*>(InPtr) - 1;
        return (Header->Magic == ALLOC_HEADER_MAGIC) ? Header : nullptr;
    }

    // === Default-pool accounting ===========================================
    // POOL_Default is a code path, not an IMemoryPool (see CoreMemoryPool.h),
    // so its stats have nowhere else to live.

    std::mutex      GDefaultStatsMutex;
    MemoryPoolStats GDefaultStats;

    // === Image identity ====================================================

    std::mutex GImageMutex;

    /* Constant-initialised: raw arrays, no constructors, valid before any
     * dynamic initialiser in the process runs. Same argument as GPools in
     * CoreMemoryPool.cpp. */
    char                  GImageNames[Internal::MAX_IMAGES][32] = {};
    bool                  GImageUsed [Internal::MAX_IMAGES]     = {};
    std::atomic<uint64_t> GImageLive [Internal::MAX_IMAGES]     = {};
}

// === Image identity =========================================================

ImageID ClaimImageID(const char* InName)
{
    std::lock_guard<std::mutex> Lock(GImageMutex);

    // Slot 0 is IMAGE_Host and is never claimed by a module.
    for(ImageID Index = 1; Index < Internal::MAX_IMAGES; ++Index)
    {
        if(!GImageUsed[Index])
        {
            GImageUsed[Index] = true;

            const size_t Capacity = sizeof(GImageNames[Index]);
            size_t       Cursor   = 0;
            if(InName)
            {
                while(Cursor + 1 < Capacity && InName[Cursor] != '\0')
                {
                    GImageNames[Index][Cursor] = InName[Cursor];
                    ++Cursor;
                }
            }
            GImageNames[Index][Cursor] = '\0';

            GImageLive[Index].store(0, std::memory_order_relaxed);
            return Index;
        }
    }

    CORE_LOG_ERROR(std::string("Memory: image table full; '") + (InName ? InName : "?") +
                   "' will allocate as IMAGE_Unknown and is exempt from the unload barrier")
    return IMAGE_Unknown;
}

void ReleaseImageID(const ImageID InImage)
{
    if(InImage == IMAGE_Host || InImage >= Internal::MAX_IMAGES)
    {
        return;
    }

    std::lock_guard<std::mutex> Lock(GImageMutex);
    GImageUsed [InImage]    = false;
    GImageNames[InImage][0] = '\0';
}

ImageID FindImageID(const char* InName)
{
    if(!InName)
    {
        return IMAGE_Unknown;
    }

    std::lock_guard<std::mutex> Lock(GImageMutex);

    for(ImageID Index = 1; Index < Internal::MAX_IMAGES; ++Index)
    {
        if(GImageUsed[Index] && std::strcmp(GImageNames[Index], InName) == 0)
        {
            return Index;
        }
    }
    return IMAGE_Unknown;
}

const char* GetImageName(const ImageID InImage)
{
    if(InImage == IMAGE_Host)
    {
        return "Host";
    }
    if(InImage >= Internal::MAX_IMAGES)
    {
        return "Unknown";
    }
    return GImageNames[InImage][0] ? GImageNames[InImage] : "Unknown";
}

namespace Internal
{
    void NoteImageAllocation(const ImageID InImage)
    {
        if(InImage < MAX_IMAGES)
        {
            GImageLive[InImage].fetch_add(1, std::memory_order_relaxed);
        }
    }

    void NoteImageFree(const ImageID InImage)
    {
        if(InImage < MAX_IMAGES)
        {
            GImageLive[InImage].fetch_sub(1, std::memory_order_relaxed);
        }
    }

    uint64_t ImageLiveAllocations(const ImageID InImage)
    {
        return (InImage < MAX_IMAGES) ? GImageLive[InImage].load(std::memory_order_relaxed) : 0;
    }
}

// === Allocate / Free ========================================================

void* Allocate(const size_t     InBytes,
               const size_t     InAlign,
               const PoolID     InPool,
               const DestructFn InDestruct,
               const uint8_t    InFlags,
               const ImageID    InImage)
{
    if(InBytes == 0)
    {
        return nullptr;
    }

    /*
     * The header must land 8-aligned, so no payload may be aligned weaker than
     * the header itself. Raising a too-small request costs nothing: an
     * alignment stronger than asked for is always still correct.
     */
    const size_t Align = (InAlign < alignof(AllocHeader)) ? alignof(AllocHeader) : InAlign;

    if(!IsPowerOfTwo(Align) || Align > ALLOC_MAX_ALIGN)
    {
        CORE_LOG_ERROR("Memory: alignment " + std::to_string(InAlign) +
                       " is not a power of two at or below ALLOC_MAX_ALIGN")
        return nullptr;
    }

    /*
     * Pad the header out to the payload's alignment, then put the payload
     * immediately after. Because the block base is Align-aligned and HeaderPad
     * is a multiple of Align, the payload is Align-aligned too -- no
     * over-allocate-and-search, and the header always sits exactly 48 bytes
     * below the payload.
     */
    const size_t HeaderPad  = AlignUp(sizeof(AllocHeader), Align);
    const size_t TotalBytes = HeaderPad + InBytes;

    // SizeBytes is a uint32_t. An allocation that does not fit in it would
    // silently truncate and be freed with the wrong size, so refuse it here
    // where the number is still known.
    if(InBytes > 0xFFFFFFFFull)
    {
        CORE_LOG_ERROR("Memory: allocation of " + std::to_string(InBytes) +
                       " bytes exceeds the 4GB per-allocation limit")
        return nullptr;
    }

    IMemoryPool* Pool = GetMemoryPool(InPool);

    if(!Pool && InPool != POOL_Default)
    {
        CORE_LOG_ERROR("Memory: pool id " + std::to_string(int(InPool)) +
                       " is not registered; allocation refused")
        return nullptr;
    }

    /*
     * The runtime half of "you cannot put a destructible type in a bump
     * allocator".
     *
     * InDestruct doubles as the "this type has a destructor" signal, which is
     * why no extra parameter is needed. It is a RUNTIME check and not a
     * static_assert because a PoolID is a runtime value -- there is no
     * compile-time fact here to assert on, however much the call site would
     * prefer one.
     */
    if(Pool && InDestruct && (Pool->GetCapabilities() & PC_IndividualFree) == 0)
    {
        CORE_LOG_ERROR(std::string("Memory: pool '") + Pool->GetName() +
                       "' cannot free individual blocks, so it cannot hold a type with a "
                       "destructor -- its Reset() would reclaim the bytes without running one")
        return nullptr;
    }

    void* Block = Pool ? Pool->Allocate(TotalBytes, Align)
                       : SystemAlignedAlloc(TotalBytes, Align);
    if(!Block)
    {
        CORE_LOG_ERROR(std::string("Memory: out of memory allocating ") +
                       std::to_string(TotalBytes) + " bytes (align " + std::to_string(Align) +
                       ") from pool '" + (Pool ? Pool->GetName() : "Default") + "'")
        return nullptr;
    }

    uint8_t*     Payload = static_cast<uint8_t*>(Block) + HeaderPad;
    AllocHeader* Header  = reinterpret_cast<AllocHeader*>(Payload) - 1;

    // Placement-new rather than memset: the counts are std::atomic and
    // assigning over raw bytes is not how you initialise one.
    ::new (static_cast<void*>(Header)) AllocHeader();

    Header->Magic       = ALLOC_HEADER_MAGIC;
    Header->SizeBytes   = uint32_t(InBytes);
    Header->Destruct    = InDestruct;
    Header->NextTracked = nullptr;
    Header->PrevTracked = nullptr;
    Header->OwningImage = InImage;
    Header->Alignment   = uint16_t(Align);
    Header->BlockOffset = uint16_t(HeaderPad);
    Header->Pool        = InPool;
    Header->Flags       = InFlags;

    // A tracked object is born with one strong reference: the TRef that
    // MakeRef is about to return. An untracked one has no refcount at all.
    Header->StrongCount.store((InFlags & AF_GCTracked) ? 1u : 0u, std::memory_order_relaxed);
    Header->WeakCount.store(0u, std::memory_order_relaxed);

    if(InFlags & AF_GCTracked)
    {
        Internal::TrackObject(Header);
    }

    Internal::NoteImageAllocation(Header->OwningImage);

    if(!Pool)
    {
        std::lock_guard<std::mutex> Lock(GDefaultStatsMutex);
        GDefaultStats.BytesInUse += TotalBytes;
        ++GDefaultStats.LiveAllocations;
        ++GDefaultStats.AllocationCount;
        if(GDefaultStats.BytesInUse > GDefaultStats.PeakBytesInUse)
        {
            GDefaultStats.PeakBytesInUse = GDefaultStats.BytesInUse;
        }
    }

    return Payload;
}

void Free(void* InPtr)
{
    if(!InPtr)
    {
        return;
    }

    AllocHeader* Header = MutableHeader(InPtr);
    if(!Header)
    {
        /*
         * Either not a Core allocation, or a double free -- the magic is
         * poisoned below precisely so the second free lands here instead of
         * corrupting a pool's free list.
         */
        CORE_LOG_ERROR("Memory: Free() on a pointer that is not a live EngineCore allocation "
                       "(foreign pointer, or freed twice)")
        return;
    }

    if(Header->Flags & AF_GCTracked)
    {
        Internal::UntrackObject(Header);
    }

    Internal::NoteImageFree(Header->OwningImage);

    // Everything Free needs must be read out BEFORE the header is destroyed.
    const PoolID PoolId     = Header->Pool;
    const size_t Align      = Header->Alignment;
    const size_t TotalBytes = size_t(Header->BlockOffset) + size_t(Header->SizeBytes);
    void* const  Block      = static_cast<uint8_t*>(InPtr) - Header->BlockOffset;

    // Poison first, so a concurrent or repeated Free sees a dead header rather
    // than a live-looking one.
    Header->Magic = 0;
    Header->~AllocHeader();

    if(IMemoryPool* Pool = GetMemoryPool(PoolId))
    {
        Pool->Deallocate(Block, TotalBytes, Align);
    }
    else if(PoolId == POOL_Default)
    {
        SystemAlignedFree(Block);

        std::lock_guard<std::mutex> Lock(GDefaultStatsMutex);
        GDefaultStats.BytesInUse = (GDefaultStats.BytesInUse >= TotalBytes)
                                 ? (GDefaultStats.BytesInUse - TotalBytes) : 0;
        if(GDefaultStats.LiveAllocations > 0)
        {
            --GDefaultStats.LiveAllocations;
        }
        ++GDefaultStats.FreeCount;
    }
    else
    {
        /*
         * The pool slot was cleared while this block was still out. The bytes
         * are unrecoverable -- there is no allocator left to hand them to --
         * so this leaks, loudly. UnregisterMemoryPool refuses to create this
         * situation; reaching it means something bypassed it.
         */
        CORE_LOG_ERROR("Memory: block belongs to pool id " + std::to_string(int(PoolId)) +
                       " which is no longer registered; " + std::to_string(TotalBytes) +
                       " bytes leaked")
    }
}

void DestroyObject(void* InPtr)
{
    AllocHeader* Header = MutableHeader(InPtr);
    if(!Header || (Header->Flags & AF_Destroyed))
    {
        return;
    }

    // Set the flag BEFORE running the destructor. A destructor that reaches
    // back into this object's own graph -- releasing a TRef that leads back
    // here -- must not start a second destruction.
    Header->Flags |= AF_Destroyed;

    if(Header->Destruct)
    {
        Header->Destruct(InPtr);
    }
}

const AllocHeader* GetHeader(const void* InPtr)
{
    if(!InPtr)
    {
        return nullptr;
    }

    const AllocHeader* Header = reinterpret_cast<const AllocHeader*>(InPtr) - 1;
    return (Header->Magic == ALLOC_HEADER_MAGIC) ? Header : nullptr;
}

bool GetPoolStats(const PoolID InPool, MemoryPoolStats& OutStats)
{
    if(InPool == POOL_Default)
    {
        std::lock_guard<std::mutex> Lock(GDefaultStatsMutex);
        OutStats = GDefaultStats;
        return true;
    }

    if(IMemoryPool* Pool = GetMemoryPool(InPool))
    {
        Pool->GetStats(OutStats);
        return true;
    }

    OutStats = MemoryPoolStats();
    return false;
}

// === Reference counting =====================================================

void AddStrongRef(void* InPtr)
{
    if(AllocHeader* Header = MutableHeader(InPtr))
    {
        // relaxed is sufficient to ADD a reference: the caller already holds
        // one, so the object cannot die underneath this, and no other memory
        // is being published by the increment itself.
        Header->StrongCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void ReleaseStrongRef(void* InPtr)
{
    AllocHeader* Header = MutableHeader(InPtr);
    if(!Header)
    {
        return;
    }

    // acq_rel: everything every other thread did to this object before
    // dropping its reference must be visible to whoever runs the destructor.
    const uint32_t Previous = Header->StrongCount.fetch_sub(1, std::memory_order_acq_rel);

    if(Previous == 0)
    {
        CORE_LOG_ERROR("Memory: strong reference released on an object with a zero count")
        return;
    }
    if(Previous != 1)
    {
        return;
    }

    DestroyObject(InPtr);

    /*
     * AF_Sweeping means the collector owns this block's bytes and will release
     * them after every destructor in the sweep set has run. Freeing here would
     * pull a block out from under the loop that is still walking it -- see
     * the flag's note in CoreMemory.h.
     */
    if((Header->Flags & AF_Sweeping) == 0 &&
       Header->WeakCount.load(std::memory_order_acquire) == 0)
    {
        Free(InPtr);
    }
}

void AddWeakRef(void* InPtr)
{
    if(AllocHeader* Header = MutableHeader(InPtr))
    {
        Header->WeakCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void ReleaseWeakRef(void* InPtr)
{
    AllocHeader* Header = MutableHeader(InPtr);
    if(!Header)
    {
        return;
    }

    const uint32_t Previous = Header->WeakCount.fetch_sub(1, std::memory_order_acq_rel);
    if(Previous != 1)
    {
        return;
    }

    // The block outlives the payload only to keep weak references readable.
    // This was the last one, so if the payload is already gone the block has
    // no reason left to exist.
    if((Header->Flags & AF_Destroyed) != 0 &&
       (Header->Flags & AF_Sweeping)  == 0 &&
       Header->StrongCount.load(std::memory_order_acquire) == 0)
    {
        Free(InPtr);
    }
}

bool TryAddStrongRef(void* InPtr)
{
    AllocHeader* Header = MutableHeader(InPtr);
    if(!Header || (Header->Flags & AF_Destroyed))
    {
        return false;
    }

    /*
     * A compare-exchange loop, NOT "if(count) ++count".
     *
     * The check-then-act version loses a race it will eventually meet: the
     * last TRef can drop between reading a nonzero count and incrementing it,
     * and the increment then resurrects an object whose destructor has already
     * started. The CAS makes "it was alive, and now I own a reference to it"
     * a single indivisible fact.
     */
    uint32_t Current = Header->StrongCount.load(std::memory_order_acquire);
    while(Current != 0)
    {
        if(Header->StrongCount.compare_exchange_weak(Current, Current + 1,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire))
        {
            return true;
        }
        // compare_exchange_weak wrote the observed value back into Current.
    }
    return false;
}

uint32_t GetStrongCount(const void* InPtr)
{
    const AllocHeader* Header = GetHeader(InPtr);
    return Header ? Header->StrongCount.load(std::memory_order_relaxed) : 0;
}

uint32_t GetWeakCount(const void* InPtr)
{
    const AllocHeader* Header = GetHeader(InPtr);
    return Header ? Header->WeakCount.load(std::memory_order_relaxed) : 0;
}
}
}
