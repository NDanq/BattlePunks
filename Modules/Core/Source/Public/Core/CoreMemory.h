#ifndef ENGINECORE_COREMEMORY_H
#define ENGINECORE_COREMEMORY_H

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreMemoryPool.h"

/*
 * Explicit typed allocation: Alloc<T> / AllocIn<T> / Dealloc<T>, and the
 * per-allocation header that makes them work.
 *
 * This is the MIDDLE layer. CoreMemoryPool.h beneath it deals in raw bytes and
 * knows nothing about types; CorePointer.h above it wraps what is here in
 * TUnique / TRef / TWeakRef, and CoreGC.h adds the collector. Read in that
 * order.
 *
 *      Foo* F = Alloc<Foo>(1, 2);          // default pool
 *      Foo* G = AllocIn<Foo>(MyPool, 1, 2);  // an explicit pool
 *      Dealloc(F);                          // no pool argument, ever
 *
 * WHY Dealloc TAKES NO POOL ARGUMENT
 *
 * Because a pool override that you have to remember at the free site is a
 * pool override that will eventually be freed into the wrong pool, and that
 * failure is silent -- the bytes go back to a pool that never owned them and
 * the corruption surfaces somewhere else entirely. So the pool travels WITH
 * the allocation, in a header stored immediately in front of the payload.
 * Every call site that can get it wrong has been removed rather than
 * documented.
 *
 * ================================================================
 * THE ONE STRUCTURAL RULE: TEMPLATES COMPUTE, EngineCore ALLOCATES
 * ================================================================
 *
 * Everything below is either an out-of-line CORE_API function or a template
 * that does nothing but compute sizes and call one. That is not a style
 * choice -- it is the same rule CoreModule.h:43 states for
 * ModuleReloadState::Write:
 *
 *     "If Write() were inline in this header, the copy compiled INTO the
 *      module would call operator new from the module's image. Under /MD that
 *      lands in the same CRT heap and looks fine right up until someone builds
 *      a module with /MT, at which point it is a silent cross-heap free."
 *
 * A template necessarily instantiates in whatever image includes it. So
 * Alloc<T> instantiates in the module -- and immediately calls
 * Memory::Allocate, of which there is exactly ONE copy, in EngineCore.dll.
 * The template's own body contains no allocation, no free, and no access to
 * any registry. Keep it that way.
 *
 * CoreAPI.h's rule falls out of the same argument: the templates here are NOT
 * annotated CORE_API, the free functions are.
 */

namespace EngineCore
{
    /*
     * Which image an allocation came from.
     *
     * This exists for one reason, and it is the reason CoreModule.h:37 states
     * as a rule the codebase could not previously enforce: "Nothing a module
     * allocates outlives the module." An object allocated by a hot-reloadable
     * module has a DESTRUCTOR LIVING IN THAT MODULE'S IMAGE. When the DLL
     * unmaps, the Destruct pointer in its header aims at unmapped memory, and
     * the next sweep that touches it jumps into nothing.
     *
     * Tagging every allocation lets ModuleManager destroy a module's objects
     * while its code is still mapped. See CoreGC.h's OnImageUnloading.
     */
    using ImageID = uint16_t;

    enum : ImageID
    {
        IMAGE_Host    = 0,          // the executable and EngineCore.dll itself
        IMAGE_Unknown = 0xFFFF      // a module built before image tagging existed
    };

    /*
     * ONE PER IMAGE. Read that again -- it is the entire trick.
     *
     * An `inline` variable is merged across the translation units of ONE
     * linked image, and a DLL is a separate link, so the host and every module
     * each get their own copy. That is exactly what is wanted: each image
     * reports its own identity with no lookup and no indirection.
     *
     * CORE_API_LOCAL IS LOAD-BEARING ON ELF. Without it the variable has
     * default visibility, the dynamic linker merges every image's copy into
     * one, and every module reports IMAGE_Host -- the barrier then protects
     * nothing and nothing says so. On Windows the macro is empty and DLL
     * symbol scoping already gives the per-image copy.
     *
     * Constant-initialised, so it is valid before any dynamic initialiser
     * anywhere in the process runs. An image that never claims an id simply
     * allocates as IMAGE_Host, which is correct for the host and degrades to
     * today's behaviour for an old module.
     */
    CORE_API_LOCAL inline ImageID GImageID = IMAGE_Host;

    /* Type-erased destructor. Points INTO the image that called Alloc<T>. */
    using DestructFn = void (*)(void*);

    enum EAllocFlags : uint8_t
    {
        AF_None      = 0,

        /* Linked into the collector's object table. Set by MakeRef, not by
         * plain Alloc -- a TUnique or a raw Alloc is not garbage-collected and
         * has no business costing a list insert. */
        AF_GCTracked = 1u << 0,

        /* Mark bit. Cleared at the start of every collect; meaningless
         * outside one. */
        AF_Marked    = 1u << 1,

        /* The payload's destructor has run. The block may still be alive
         * because a TWeakRef is holding it -- see the lifetime note below. */
        AF_Destroyed = 1u << 2,

        /* Never collected, never destroyed by the image barrier. For an object
         * that must outlive everything, set deliberately and rarely. */
        AF_Pinned    = 1u << 3,

        /*
         * The collector has claimed this block and will release its bytes
         * itself. Set for the whole sweep set BEFORE any destructor runs.
         *
         * Without it the sweep is a double free waiting to happen: destroying
         * one member of a cycle drops its TRefs to the others, whose counts
         * reach zero, whose ReleaseStrongRef would then free blocks the sweep
         * is still holding a pointer to. The flag makes the refcount path
         * decrement and stop, leaving the release to the phase that owns it.
         */
        AF_Sweeping  = 1u << 4,

        /*
         * This block was counted against its image's live-allocation total,
         * so Free must uncount it.
         *
         * Not every block is. An allocation from a pool that cannot free
         * individual blocks -- a frame arena -- is reclaimed by Reset() and
         * never passes through Free at all, so counting it would ratchet the
         * total up forever and report a module using a frame arena as leaking
         * every frame's scratch. Those blocks are necessarily
         * trivially-destructible (Allocate enforces it), which is exactly the
         * category the barrier's report does not care about.
         */
        AF_ImageCounted = 1u << 5
    };

    /*
     * The 48 bytes in front of every payload.
     *
     * LAYOUT IS FROZEN ABI, in the strongest sense in this codebase. The
     * accessors below are inline and read members directly, so a module
     * compiled against one layout and an EngineCore.dll compiled against
     * another disagree about where the refcount is -- and the symptom is
     * memory corruption, not a link error. Changing anything here means
     * bumping MODULE_ABI_REVISION in CoreModule.h.
     *
     * THE 48 BYTES ARE THE HONEST COST OF THIS DESIGN, and they are not
     * negotiable down without giving something up: the pool id is what lets
     * Dealloc take no pool argument, the Destruct pointer is what lets the
     * collector destroy a type it has never heard of, and the list pointers are
     * what let it enumerate objects at all. For a 16-byte object that is 75%
     * overhead -- which is exactly why FixedBlockPool exists, and why
     * FixedBlockBytesFor<T>() below computes its stride rather than leaving it
     * to a call site that will forget the header.
     */
    struct AllocHeader
    {
        uint32_t     Magic;          // ALLOC_HEADER_MAGIC; catches a foreign pointer
        uint32_t     SizeBytes;      // payload bytes, excluding this header

        DestructFn   Destruct;       // nullptr when T is trivially destructible
        AllocHeader* NextTracked;    // object table; null unless AF_GCTracked
        AllocHeader* PrevTracked;

        std::atomic<uint32_t> StrongCount;
        std::atomic<uint32_t> WeakCount;

        ImageID      OwningImage;
        uint16_t     Alignment;
        uint16_t     BlockOffset;    // payload minus the raw block base
        PoolID       Pool;
        uint8_t      Flags;
    };

    enum : uint32_t { ALLOC_HEADER_MAGIC = 0x414C4844u };   // 'ALHD'

    /* The largest alignment Alloc can satisfy. Bounded because BlockOffset is
     * a uint16_t, and bounded at a page because nothing in an engine needs a
     * heap object aligned harder than that -- the SIMD types in Math/Vector.h
     * want 16. */
    enum : size_t { ALLOC_MAX_ALIGN = 4096 };

    static_assert(sizeof(AllocHeader) == 48, "AllocHeader layout changed -- bump MODULE_ABI_REVISION");
    static_assert(alignof(AllocHeader) == 8, "AllocHeader alignment changed -- bump MODULE_ABI_REVISION");

namespace Memory
{
    // === Image identity =====================================================

    /* Claims a slot for the calling image and returns the id to store in
     * GImageID. Emitted by NIMBLE_IMPLEMENT_MODULE; a module author never
     * calls these directly. IMAGE_Unknown if the table is full, which costs
     * that module the unload barrier and says so in the log. */
    CORE_API ImageID     ClaimImageID  (const char* InName);
    CORE_API void        ReleaseImageID(ImageID InImage);

    /*
     * The id a claimed image is using, by the name it claimed.
     *
     * This exists because the barrier has to run AFTER the module instance is
     * destroyed and BEFORE the library is closed -- at which point the module
     * cannot be asked anything, since NimbleModuleDestroy has already
     * returned. The manager knows the module's NAME, so that is what it looks
     * up. IMAGE_Unknown if nothing claimed that name.
     */
    CORE_API ImageID     FindImageID(const char* InName);

    /* Never null -- "Host" for slot 0, "Unknown" for an unclaimed or
     * out-of-range id. Diagnostic text; the pointer is into a fixed buffer and
     * is valid until that slot is claimed again. */
    CORE_API const char* GetImageName(ImageID InImage);

    // === The seam ===========================================================
    // Out-of-line, CORE_API, exactly one copy in EngineCore.dll. See the
    // structural rule at the top of this file.

    /*
     * Allocates InBytes with InAlign from InPool and returns the PAYLOAD
     * pointer, with a fully-initialised header in front of it. nullptr on
     * failure -- Core does not use exceptions anywhere and CORE_CHECK is the
     * assert idiom, so this reports failure the way appFileOpenRead does.
     *
     * InDestruct doubles as the "this type has a destructor" signal, which is
     * what lets this refuse a non-trivially-destructible type in a pool whose
     * Deallocate is a no-op. A LinearMemoryPool cannot run destructors on
     * Reset(), and silently not running them is precisely the class of failure
     * this codebase writes paragraphs to prevent.
     *
     * StrongCount starts at 1 when AF_GCTracked is set, 0 otherwise.
     *
     * ==========================================================
     * WHY InImage IS A PARAMETER AND NOT READ FROM GImageID HERE
     * ==========================================================
     *
     * Because this function lives in EngineCore.dll, and GImageID is ONE PER
     * IMAGE. A read of GImageID from inside this function binds to
     * EngineCore's own copy -- which is IMAGE_Host, always, no matter who
     * called. Every module's allocation would be stamped as the host's, the
     * unload barrier would find nothing to destroy, and NOTHING WOULD SAY SO:
     * the module's own static destructors would clean up on the way out often
     * enough to look correct, right up until one of them did not.
     *
     * The callers that can see the right answer are the TEMPLATES -- Alloc,
     * AllocIn, MakeRefIn -- because they instantiate in the calling image and
     * their GImageID is that image's. So they read it and pass it down. That
     * is the only place in this design where a template does something other
     * than compute a size, and this is why.
     *
     * A direct caller of this function passes GImageID. It will be correct for
     * the same reason: the read happens in the caller's image.
     */
    CORE_API void* Allocate(size_t     InBytes,
                            size_t     InAlign,
                            PoolID     InPool,
                            DestructFn InDestruct,
                            uint8_t    InFlags,
                            ImageID    InImage);

    /*
     * Releases the bytes. DOES NOT RUN A DESTRUCTOR -- the caller has already
     * destroyed the object, which is true of both Dealloc<T> (which calls ~T()
     * directly, typed, with no indirect call) and the refcount path.
     *
     * Unlinks from the object table if tracked. Ignores the reference counts
     * entirely: this is the unconditional release, and calling it on an object
     * a TRef still holds is a use-after-free that this function cannot see.
     */
    CORE_API void Free(void* InPtr);

    /* Runs the header's Destruct exactly once, sets AF_Destroyed, frees
     * nothing. The collector's sweep and the image barrier use this; ordinary
     * code should not need it. */
    CORE_API void DestroyObject(void* InPtr);

    /* nullptr unless InPtr is a live Core allocation -- checks the magic, so a
     * stack address or a plain new'd pointer reads back null rather than
     * garbage. */
    CORE_API const AllocHeader* GetHeader(const void* InPtr);

    /* POOL_Default has no IMemoryPool object, so its stats live here. Works for
     * every id, default included. */
    CORE_API bool GetPoolStats(PoolID InPool, MemoryPoolStats& OutStats);

    // === Reference counting =================================================
    /*
     * Out-of-line for the reason the whole file is: there must be exactly ONE
     * copy of the "last reference dropped, destroy and maybe free" decision in
     * the process. Inline it and a module and the host could each run half of
     * it on the same object.
     *
     * LIFETIME MODEL -- read before using TWeakRef.
     *
     * The header and the payload are ONE allocation, so a weak reference keeps
     * the WHOLE BLOCK alive, not just the 48-byte header. This is the same
     * trade std::make_shared makes and it has the same consequence: a large
     * object with a long-lived TWeakRef pointing at it holds its full size
     * until that weak reference goes away, even though the object itself was
     * destroyed long before. The alternative -- a separately allocated control
     * block -- costs a second allocation and a second cache line on every
     * object, to optimise a case that is rare. If it ever stops being rare,
     * that is the change to make.
     *
     * THE IMPLICIT WEAK REFERENCE -- the part that is not obvious
     *
     * A tracked object is born with StrongCount 1 AND WeakCount 1. That second
     * one belongs to nobody: the strong count holds it collectively, and gives
     * it up only after the destructor has finished running.
     *
     * Without it this is a use-after-free, and a very ordinary one. A parent
     * node holding a strong reference to a child, and the child holding a
     * TWeakRef back at the parent, is the most natural shape there is. Drop
     * the last handle to the parent and:
     *
     *   parent's StrongCount hits 0  -> ~Parent() starts
     *     ~Parent() destroys its child reference
     *       child's count hits 0     -> ~Child() runs
     *         ~Child() drops its TWeakRef at the parent
     *           parent's WeakCount hits 0 -> THE PARENT'S BLOCK IS FREED
     *     ...and ~Parent() carries on destroying members of freed memory.
     *
     * The implicit reference makes that count bottom out at 1 instead of 0, so
     * the block cannot go away while the destructor that owns it is still on
     * the stack. std::shared_ptr does exactly this, for exactly this reason.
     *
     * GetWeakCount() subtracts it, so callers see the number of TWeakRefs they
     * actually created.
     *
     *   StrongCount 1 -> 0 : run the destructor, set AF_Destroyed, then
     *                        release the implicit weak reference
     *   WeakCount   1 -> 0 : free the block -- reaching zero already implies
     *                        the payload was destroyed, because the strong
     *                        count would otherwise still be holding one
     */
    CORE_API void AddStrongRef    (void* InPtr);
    CORE_API void ReleaseStrongRef(void* InPtr);
    CORE_API void AddWeakRef      (void* InPtr);
    CORE_API void ReleaseWeakRef  (void* InPtr);

    /*
     * The TWeakRef::Pin primitive. Raises the strong count only if it is not
     * already zero, atomically -- a plain "is it alive? then AddStrongRef" is
     * check-then-act and loses the race against the last TRef dying in
     * between.
     */
    CORE_API bool TryAddStrongRef(void* InPtr);

    CORE_API uint32_t GetStrongCount(const void* InPtr);
    CORE_API uint32_t GetWeakCount  (const void* InPtr);

    // === Typed entry points =================================================
    // Templates. They compute and forward; they never allocate. See the
    // structural rule at the top of this file.

    /*
     * The type-erased destructor Alloc installs. Its ADDRESS is what lands in
     * the header, and it is an address in whatever image instantiated it --
     * which is the whole reason OwningImage exists.
     */
    template<typename T>
    void DestructorThunk(void* InPtr)
    {
        static_cast<T*>(InPtr)->~T();
    }

    /* nullptr for a trivially-destructible T, so the collector skips an
     * indirect call it does not need -- and so such an object is exempt from
     * the image barrier, there being no code for it to jump into. */
    template<typename T>
    constexpr DestructFn DestructorFor()
    {
        return std::is_trivially_destructible<T>::value ? nullptr : &DestructorThunk<T>;
    }

    template<typename T, typename... TArgs>
    T* AllocIn(const PoolID InPool, TArgs&&... InArgs)
    {
        static_assert(alignof(T) <= ALLOC_MAX_ALIGN, "alignof(T) exceeds ALLOC_MAX_ALIGN");

        // GImageID read HERE, in the caller's image -- see the note on Allocate.
        void* Block = Allocate(sizeof(T), alignof(T), InPool, DestructorFor<T>(), AF_None, GImageID);
        if(!Block)
        {
            return nullptr;
        }
        return ::new (Block) T(std::forward<TArgs>(InArgs)...);
    }

    template<typename T, typename... TArgs>
    T* Alloc(TArgs&&... InArgs)
    {
        return AllocIn<T>(POOL_Default, std::forward<TArgs>(InArgs)...);
    }

    /*
     * Runs ~T() directly rather than through the header's Destruct pointer:
     * the type is known here, so the call is direct and the compiler elides it
     * entirely for a trivially-destructible T.
     *
     * Safe on nullptr, like delete.
     */
    template<typename T>
    void Dealloc(T* InPtr)
    {
        if(!InPtr)
        {
            return;
        }
        InPtr->~T();
        Free(InPtr);
    }

    /*
     * Arrays.
     *
     * The count is NOT stored in the header and DeallocArray requires it back.
     * That is deliberate: storing it would need either a fourth size field in
     * a header that is already the expensive part of this design, or the
     * cookie-before-the-array trick that makes the payload pointer no longer
     * the allocation pointer and breaks GetHeader for every array.
     *
     * If you find yourself tracking the count by hand at each call site, you
     * want a container, not this.
     */
    template<typename T>
    T* AllocArrayIn(const PoolID InPool, const size_t InCount)
    {
        static_assert(alignof(T) <= ALLOC_MAX_ALIGN, "alignof(T) exceeds ALLOC_MAX_ALIGN");

        if(InCount == 0)
        {
            return nullptr;
        }

        void* Block = Allocate(sizeof(T) * InCount, alignof(T), InPool, DestructorFor<T>(), AF_None, GImageID);
        if(!Block)
        {
            return nullptr;
        }

        T* Items = static_cast<T*>(Block);
        for(size_t Index = 0; Index < InCount; ++Index)
        {
            ::new (Items + Index) T();
        }
        return Items;
    }

    template<typename T>
    T* AllocArray(const size_t InCount)
    {
        return AllocArrayIn<T>(POOL_Default, InCount);
    }

    template<typename T>
    void DeallocArray(T* InPtr, const size_t InCount)
    {
        if(!InPtr)
        {
            return;
        }

        // Reverse order, matching what the compiler does for a real array --
        // later elements may hold references to earlier ones.
        for(size_t Index = InCount; Index > 0; --Index)
        {
            InPtr[Index - 1].~T();
        }
        Free(InPtr);
    }

    // === Sizing a FixedBlockPool ============================================

    /*
     * USE THESE. A FixedBlockPool sized sizeof(T) rejects every request for a
     * T, because every allocation carries the header in front of the payload --
     * and Allocate reports that rejection as an out-of-memory, which looks
     * nothing like the sizing mistake it actually is.
     *
     *      FixedBlockPool Pool("Particles",
     *                          FixedBlockBytesFor<Particle>(),
     *                          FixedBlockAlignFor<Particle>(),
     *                          MaxParticles);
     */
    template<typename T>
    constexpr size_t FixedBlockAlignFor()
    {
        return (alignof(T) < alignof(AllocHeader)) ? alignof(AllocHeader) : alignof(T);
    }

    template<typename T>
    constexpr size_t FixedBlockBytesFor()
    {
        // Mirrors exactly what Allocate does: the header is padded out to the
        // payload's alignment so the payload lands aligned inside the block.
        return ((sizeof(AllocHeader) + FixedBlockAlignFor<T>() - 1)
                    & ~(FixedBlockAlignFor<T>() - 1))
               + sizeof(T);
    }
}
}

#endif //ENGINECORE_COREMEMORY_H
