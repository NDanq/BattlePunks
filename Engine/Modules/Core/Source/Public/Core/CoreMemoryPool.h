#ifndef ENGINECORE_COREMEMORYPOOL_H
#define ENGINECORE_COREMEMORYPOOL_H

#include <cstdint>
#include <cstddef>
#include <mutex>

#include "Public/Core/CoreAPI.h"

/*
 * The memory-pool handler: an interface, three implementations Core ships, and
 * a fixed registry that hands out a small integer name for each.
 *
 * This is the LOWER half of the memory subsystem. It deals in raw bytes and
 * knows nothing about types, headers, destructors or the collector.
 * CoreMemory.h sits on top of it and adds all of that; CorePointer.h sits on
 * CoreMemory.h. Read them in that order.
 *
 * WHY POOLS ARE NAMED BY A uint8_t AND NOT BY A POINTER
 *
 * Every allocation stores the pool it came from so that Dealloc works from a
 * bare T* (see CoreMemory.h). Storing an IMemoryPool* there would put a
 * pointer INTO A POSSIBLY-UNLOADED IMAGE inside every allocation header -- a
 * module may implement a pool, and CoreModule.h's rule is that nothing a
 * module owns outlives it. A one-byte index into a table EngineCore.dll owns
 * has no such problem: the table entry can be cleared on unload and the header
 * still reads back something valid and diagnosable.
 *
 * It also keeps the header small, which matters more than it sounds like it
 * should -- see the note on the 48 bytes in CoreMemory.h.
 *
 * THREADING
 *
 * Allocate/Deallocate on any pool must be safe from any thread; a pool that
 * is not is a bug in that pool. The REGISTRY is the opposite: registering or
 * unregistering while another thread allocates is a data race, and the fix is
 * to register every pool during startup and leave the table alone afterwards.
 * That is the same stance CoreModuleManager.h takes ("THREADING: none") and
 * for the same reason -- the alternative is a lock on the allocation hot path
 * to protect a table that changes a handful of times per process.
 */

namespace EngineCore
{
    /*
     * PoolID 0 is ALWAYS the default pool, and it is deliberately NOT an
     * IMemoryPool -- it is a direct CRT path inside Memory::Allocate.
     *
     * This is the argument CoreConfig.h:31 makes about GConfigHead, applied
     * here. The table below is
     *
     *      IMemoryPool* GPools[POOL_MAX] = {};
     *
     * which has no constructor and is constant-initialised into .bss before any
     * dynamic initialiser in any image runs. A default pool OBJECT would have a
     * vtable, therefore a constructor, therefore an ordering question -- and
     * the first allocation from a file-scope constructor anywhere in the
     * process would find it half-built.
     *
     * Slot 0 is never filled and never virtual-dispatched, which as a bonus
     * keeps the common allocation path free of an indirect call.
     */
    using PoolID = uint8_t;

    enum : PoolID
    {
        POOL_Default = 0,
        POOL_MAX     = 64,      // slots 1..63 are registerable
        POOL_Invalid = 0xFF
    };

    /*
     * What a pool can do. Checked at RUNTIME by CoreMemory.h's AllocIn, not at
     * compile time: a PoolID is a runtime value, so there is no static_assert
     * to be had here however much the call site would prefer one.
     */
    enum EPoolCapability : uint8_t
    {
        PC_None           = 0,

        /* Deallocate actually reclaims the block. FALSE for a bump allocator,
         * whose Deallocate is a no-op -- which is exactly why AllocIn refuses
         * to put a non-trivially-destructible type in one. */
        PC_IndividualFree = 1u << 0
    };

    struct MemoryPoolStats
    {
        uint64_t BytesInUse      = 0;   // payload+overhead currently handed out
        uint64_t BytesReserved   = 0;   // address space held (0 for CRT-backed pools)
        uint64_t BytesCommitted  = 0;   // physical pages backed
        uint64_t PeakBytesInUse  = 0;
        uint64_t LiveAllocations = 0;
        uint64_t AllocationCount = 0;   // lifetime totals, never decremented
        uint64_t FreeCount       = 0;
    };

    /*
     * DELIBERATELY NOT CORE_API -- the same call CoreModule.h makes for
     * IModule, for the same reason.
     *
     * Core DECLARES this interface but a module may IMPLEMENT it. dllimport on
     * a class a foreign image derives from is wrong, and the concrete pools
     * below (which Core both declares and implements) are annotated instead.
     *
     * ITS LAYOUT IS FROZEN ABI. Adding, removing or reordering a virtual here
     * shifts every vtable slot, and a module built against the old order calls
     * GetStats where the host expects Deallocate -- with no diagnostic. If this
     * ever has to change, MODULE_ABI_REVISION in CoreModule.h moves with it.
     */
    class IMemoryPool
    {
    public:
        virtual ~IMemoryPool() = default;

        /* nullptr on failure -- exhaustion, or a request this pool cannot
         * satisfy (too large, over-aligned). Never throws; Core does not use
         * exceptions anywhere. */
        virtual void* Allocate(size_t InBytes, size_t InAlign) = 0;

        /*
         * InBytes and InAlign are the SAME values that were passed to
         * Allocate. They are handed back rather than looked up because a pool
         * that does not need them (fixed-block, linear) should not be forced
         * to store them per block, and CoreMemory.h's header has them anyway.
         */
        virtual void Deallocate(void* InPtr, size_t InBytes, size_t InAlign) = 0;

        /* Stable identity for logs and FindMemoryPool. Must not allocate --
         * same contract as IModule::GetName. */
        virtual const char* GetName() const = 0;

        virtual uint8_t GetCapabilities() const = 0;

        virtual void GetStats(MemoryPoolStats& OutStats) const = 0;
    };

    // === Registry ===========================================================
    // Out-of-line and CORE_API so the table lives in EngineCore.dll and every
    // image -- host and modules alike -- sees the same one. Inline it and each
    // module would build a private table, a module's PoolID 3 would mean
    // something different from the host's PoolID 3, and every cross-image
    // Dealloc would free into the wrong pool with nothing to say so. That is
    // the failure CoreConfig.h:44 describes for the config list.

    /* Returns POOL_Invalid if the table is full or InPool is null. Does NOT
     * take ownership -- the caller keeps the pool alive until it unregisters. */
    CORE_API PoolID RegisterMemoryPool(IMemoryPool* InPool);

    /*
     * Clears the slot. Allocations still outstanding from this pool become
     * unfreeable -- their headers name a slot that is now empty -- so this
     * logs an error and refuses if the pool still reports live allocations.
     * Unregistering a pool out from under live memory is not a recoverable
     * situation and pretending otherwise just moves the crash later.
     */
    CORE_API void UnregisterMemoryPool(PoolID InPool);

    /* nullptr for POOL_Default (which has no object), for an empty slot, and
     * for an out-of-range id. A null return is NOT an error by itself -- see
     * how Memory::Allocate uses it. */
    CORE_API IMemoryPool* GetMemoryPool(PoolID InPool);

    /* Linear walk over 63 slots comparing names. Core scans linearly
     * everywhere (there is no std::map in this module) and this runs at
     * startup, not in a frame. POOL_Invalid if no pool has that name. */
    CORE_API PoolID FindMemoryPool(const char* InName);

    // === Raw system allocation ==============================================

    /*
     * Aligned malloc/free, straight to the CRT. This is what POOL_Default and
     * HeapMemoryPool are built on, and the floor everything else falls back to.
     *
     * NOT NAMED app* AND NOT IN CorePlatform.h, deliberately. The app* seam is
     * for OS mechanisms; this is CRT divergence -- _aligned_malloc on MSVC
     * against std::aligned_alloc elsewhere -- and CoreModule.cpp already calls
     * std::realloc directly without anyone considering that a seam violation.
     * Putting it behind the platform seam would imply a new platform has to
     * implement it, which it does not.
     *
     * InAlign must be a power of two. SystemAlignedFree MUST be paired with
     * SystemAlignedAlloc: on MSVC the block came from _aligned_malloc and
     * passing it to plain free() corrupts the heap.
     */
    CORE_API void* SystemAlignedAlloc(size_t InBytes, size_t InAlign);
    CORE_API void  SystemAlignedFree (void* InPtr);

    // === VirtualRegion ======================================================

    /*
     * A reserve-once, commit-as-you-grow address range. The shared backing for
     * FixedBlockPool and LinearMemoryPool.
     *
     * This is why appMemReserve/appMemCommit exist rather than a malloc: the
     * whole point is that GetBase() never moves as the region grows. A realloc
     * is free to relocate, and relocating would invalidate every pointer the
     * pool has already handed out.
     *
     * LAYOUT IS FROZEN for the same reason ModuleReloadState's is: the getters
     * below are inline and read members directly, so they compile into whatever
     * image includes this header.
     */
    class CORE_API VirtualRegion
    {
    public:
        VirtualRegion() = default;
        ~VirtualRegion();

        VirtualRegion(const VirtualRegion&)            = delete;
        VirtualRegion& operator=(const VirtualRegion&) = delete;

        /* Reserves address space. False if already reserved or the OS refused. */
        bool Reserve(size_t InBytes);

        /* Commits from the base up to at least InBytes, in page-rounded
         * chunks. Cheap and idempotent when the range is already committed,
         * which is what lets a pool call it on every grow without tracking
         * page boundaries itself. False if InBytes exceeds the reservation. */
        bool EnsureCommitted(size_t InBytes);

        void Release();

        uint8_t* GetBase()           const { return Base; }
        size_t   GetReservedBytes()  const { return ReservedBytes; }
        size_t   GetCommittedBytes() const { return CommittedBytes; }
        bool     IsValid()           const { return Base != nullptr; }

    private:
        uint8_t* Base           = nullptr;
        size_t   ReservedBytes  = 0;
        size_t   CommittedBytes = 0;
    };

    // === The pools Core ships ===============================================
    // These ARE CORE_API: Core both declares and implements them, and they have
    // out-of-line virtuals. See CoreAPI.h.

    /*
     * A CRT-backed pool that counts.
     *
     * Allocates exactly as POOL_Default does; the only thing it adds is
     * accounting. Register one of these when a subsystem wants to know what it
     * is costing without changing how it allocates -- which is usually the
     * first thing you want, and usually enough.
     */
    class CORE_API HeapMemoryPool final : public IMemoryPool
    {
    public:
        explicit HeapMemoryPool(const char* InName);
        ~HeapMemoryPool() override;

        void*       Allocate  (size_t InBytes, size_t InAlign) override;
        void        Deallocate(void* InPtr, size_t InBytes, size_t InAlign) override;
        const char* GetName   () const override { return Name; }
        uint8_t     GetCapabilities() const override { return PC_IndividualFree; }
        void        GetStats  (MemoryPoolStats& OutStats) const override;

    private:
        static constexpr size_t NAME_MAX = 32;

        /* A fixed char[] and not a std::string, for the reason CoreConfig.h:74
         * spells out: a std::string member would allocate in whichever image
         * constructed the pool and be freed by code in EngineCore.dll. */
        char Name[NAME_MAX] = {};

        mutable std::mutex Mutex;
        MemoryPoolStats    Stats;
    };

    /*
     * One size class, O(1) both ways.
     *
     * Every block is the same stride, so allocation is a free-list pop and
     * deallocation is a push. The free list threads THROUGH THE FREE BLOCKS
     * THEMSELVES -- a freed block's first 8 bytes hold the next free block --
     * which is why BlockBytes must be at least sizeof(void*) and why the
     * constructor rounds it up if it is not.
     *
     * SIZE THIS WITH MakeFixedBlockPoolFor<T>() FROM CoreMemory.h, not by
     * hand. Every allocation carries a 48-byte AllocHeader in front of the
     * payload, so a pool sized sizeof(T) is a pool that rejects every request
     * for a T -- and the rejection looks like an out-of-memory, not like a
     * sizing mistake.
     */
    class CORE_API FixedBlockPool final : public IMemoryPool
    {
    public:
        /*
         * Reserves InMaxBlocks * stride of address space up front and commits
         * nothing. Physical pages are committed as the high-water mark
         * advances, so InMaxBlocks may be generous without costing memory.
         */
        FixedBlockPool(const char* InName, size_t InBlockBytes, size_t InBlockAlign, size_t InMaxBlocks);
        ~FixedBlockPool() override;

        void*       Allocate  (size_t InBytes, size_t InAlign) override;
        void        Deallocate(void* InPtr, size_t InBytes, size_t InAlign) override;
        const char* GetName   () const override { return Name; }
        uint8_t     GetCapabilities() const override { return PC_IndividualFree; }
        void        GetStats  (MemoryPoolStats& OutStats) const override;

        size_t GetBlockBytes() const { return BlockBytes; }
        size_t GetStride()     const { return Stride; }
        size_t GetMaxBlocks()  const { return MaxBlocks; }

        /* True if InPtr is a block address inside this pool's region. Used by
         * Deallocate's guard and worth having in a test. */
        bool Owns(const void* InPtr) const;

    private:
        static constexpr size_t NAME_MAX = 32;

        char   Name[NAME_MAX] = {};

        size_t BlockBytes = 0;
        size_t BlockAlign = 0;
        size_t Stride     = 0;   // BlockBytes rounded up to BlockAlign
        size_t MaxBlocks  = 0;

        VirtualRegion      Region;
        mutable std::mutex Mutex;

        void*  FreeHead  = nullptr;   // intrusive, threaded through free blocks
        size_t HighWater = 0;         // blocks ever handed out from the region

        MemoryPoolStats Stats;
    };

    /*
     * A bump allocator. Allocation is a pointer add; Deallocate DOES NOTHING.
     *
     * Reclaim happens only in Reset(), all at once.
     *
     * RESET DOES NOT RUN DESTRUCTORS. It cannot -- it has no idea what is in
     * the region. That makes this pool correct for POD scratch and wrong for
     * anything with a destructor, and rather than leave that to a comment
     * nobody reads, it reports PC_None and CoreMemory.h's AllocIn refuses at
     * runtime to put a non-trivially-destructible type here.
     *
     * bThreadSafe is a constructor argument rather than always-on because the
     * intended use is a PER-THREAD frame arena, where a lock protects nothing
     * and costs an atomic per allocation in the one place allocation is meant
     * to be nearly free. The default is ON: an unsynchronised allocator handed
     * to a second thread by accident corrupts silently.
     */
    class CORE_API LinearMemoryPool final : public IMemoryPool
    {
    public:
        LinearMemoryPool(const char* InName, size_t InReserveBytes, bool bInThreadSafe = true);
        ~LinearMemoryPool() override;

        void*       Allocate  (size_t InBytes, size_t InAlign) override;
        void        Deallocate(void* InPtr, size_t InBytes, size_t InAlign) override;
        const char* GetName   () const override { return Name; }
        uint8_t     GetCapabilities() const override { return PC_None; }
        void        GetStats  (MemoryPoolStats& OutStats) const override;

        /* Rewinds to empty WITHOUT running a single destructor. Every pointer
         * this pool has handed out dangles the instant this returns. */
        void Reset();

        size_t GetBytesUsed() const;

    private:
        static constexpr size_t NAME_MAX = 32;

        char Name[NAME_MAX] = {};

        VirtualRegion      Region;
        mutable std::mutex Mutex;
        bool               bThreadSafe = true;

        size_t          Offset = 0;
        MemoryPoolStats Stats;
    };
}

#endif //ENGINECORE_COREMEMORYPOOL_H
