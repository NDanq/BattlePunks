#ifndef ENGINECORE_COREGC_H
#define ENGINECORE_COREGC_H

#include <cstdint>
#include <cstddef>

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreMemory.h"

/*
 * The collector: a mark-and-sweep pass over everything MakeRef has allocated,
 * plus the hot-reload barrier that keeps a module's objects from outliving its
 * code.
 *
 * ===========================================================
 * WHAT THIS IS FOR, AND WHAT IT IS EMPHATICALLY NOT FOR
 * ===========================================================
 *
 * TRef is REFERENCE COUNTED. The overwhelming majority of objects die the
 * instant the last handle drops -- deterministically, with no pause, on the
 * thread that dropped it, in destructor order. That is the primary mechanism
 * and it is the one you should be thinking about.
 *
 * This file exists for the one thing reference counting structurally cannot
 * do: reclaim a CYCLE. Two objects holding TRefs to each other keep each
 * other's count at 1 forever, and no amount of care at the call site changes
 * that. CollectGarbage() finds them.
 *
 * So this is a backstop, not a memory manager. If a frame's allocations are
 * only reclaimed when the collector runs, something is wrong with the
 * ownership, not with the collector's schedule.
 *
 * ==========================
 * HOW EDGES ARE DISCOVERED
 * ==========================
 *
 * EngineCore has no reflection and no code generation, and building either
 * would dwarf this entire subsystem. So the collector does not ask a type what
 * it points at -- it asks the POINTERS THEMSELVES.
 *
 * Every TRef registers a node in a global intrusive list when it is
 * constructed and removes it when destroyed. At collect time that list is the
 * complete set of strong references in the process, and each one is classified
 * purely by ITS OWN ADDRESS:
 *
 *   * a TRef whose address lies INSIDE a tracked object's payload is that
 *     object's outgoing EDGE;
 *   * a TRef anywhere else -- stack, global, member of a plain struct, inside
 *     a std::vector's heap buffer -- is a ROOT.
 *
 * No base class, no macro, no registration by the type author, and it is
 * correct for a TRef in a container, a TRef in a lambda capture, and a TRef in
 * a type Core has never heard of. TWeakRef deliberately does NOT register:
 * being invisible to the mark phase is what makes it weak.
 *
 * THE COST IS REAL AND IT IS ON TRef, NOT HERE: copying a TRef is an atomic
 * increment plus a locked list insert. See the note in CorePointer.h.
 *
 * ==========================
 * WHAT THE COLLECTOR CANNOT SEE
 * ==========================
 *
 * A tracked object reachable only through a RAW T* is garbage as far as this
 * is concerned, and will be collected out from under that pointer. If you are
 * holding a GC-tracked object, hold a TRef. Use Get() for the raw pointer at
 * the point of use, not for storage.
 */

namespace EngineCore
{
namespace Memory
{
    /*
     * The intrusive list node every TRef embeds.
     *
     * It is declared here rather than in CorePointer.h because the collector is
     * what gives it meaning, and because CoreMemory.cpp needs the layout to
     * walk the list.
     *
     * Target is the payload pointer. The NODE'S OWN ADDRESS is the root/edge
     * discriminator described above, which is why it must be the first member
     * of TRef and why TRef must not be reordered around it.
     */
    struct GCHandleNode
    {
        GCHandleNode* Next   = nullptr;
        GCHandleNode* Prev   = nullptr;
        void*         Target = nullptr;
    };

    /* Called by TRef's constructor/destructor. Out-of-line and CORE_API so the
     * list lives in EngineCore.dll -- inline it and every module builds a
     * private list, the collector sees only its own image's handles, and it
     * frees live objects with no diagnostic anywhere. That is the failure
     * CoreConfig.h:44 describes for the config list. */
    CORE_API void RegisterHandle  (GCHandleNode* InNode);
    CORE_API void UnregisterHandle(GCHandleNode* InNode);

    struct GCStats
    {
        uint64_t ObjectsConsidered = 0;
        uint64_t ObjectsMarked     = 0;
        uint64_t ObjectsCollected  = 0;   // cycles reclaimed; usually 0
        uint64_t BytesCollected    = 0;
        uint64_t HandlesScanned    = 0;
        uint64_t RootsFound        = 0;
    };

    /*
     * One full mark-and-sweep. Explicitly driven, never automatic.
     *
     * The host calls this at a frame boundary of its choosing -- the same
     * stance CoreModuleManager.h:36 takes about UnloadAll ("the host must be
     * able to write Manager.UnloadAll() at a point of its choosing in
     * main()"). An engine that collects when it feels like it collects during
     * the frame you were profiling.
     *
     * THREADING: THE WORLD MUST BE STOPPED. No other thread may construct,
     * destroy, copy or assign a TRef while this runs. There is no read barrier
     * and no write barrier; adding them is a different collector, not a
     * parameter to this one. Re-entrancy is CORE_CHECKed, and a call from a
     * thread other than the one passed to SetCollectorThread is refused.
     */
    CORE_API void CollectGarbage(GCStats* OutStats = nullptr);

    /*
     * Names the thread allowed to call CollectGarbage, using appThreadID().
     * Defaults to whichever thread calls this first; if never called, the
     * first CollectGarbage claims it.
     */
    CORE_API void SetCollectorThread();

    /* Live tracked objects right now. Cheap -- a counter, not a walk. */
    CORE_API uint64_t GetTrackedObjectCount();

    /*
     * Logs every allocation still outstanding: tracked objects individually,
     * and a per-image count for untracked ones. Returns the total.
     *
     * An EXPLICIT call and not a static destructor, for the reason both
     * CoreConfig.h and CoreModuleManager.h give for their own teardown: a
     * destructor at process exit runs in an order nothing specifies, after
     * other translation units' globals are already gone, and would report
     * leaks that are really just teardown ordering.
     */
    CORE_API uint64_t ReportLeaks();

    // === The hot-reload barrier =============================================

    /*
     * Destroys everything InImage allocated, WHILE ITS CODE IS STILL MAPPED.
     *
     * CoreModule.h:37 states the rule this enforces -- "Nothing a module
     * allocates outlives the module" -- and until now nothing checked it. The
     * failure it prevents: a tracked object allocated by a module holds a
     * Destruct pointer into that module's image. After appDLLClose that
     * pointer aims at unmapped memory, and the next sweep to touch the object
     * jumps into nothing. The crash lands in the collector, arbitrarily far
     * from the module that caused it.
     *
     * ModuleManager::TeardownInstance calls this between destroying the module
     * instance and closing the library, alongside the existing checks that
     * restore the log handler and file provider. Anything still strongly
     * referenced from outside the module is destroyed anyway and logged
     * loudly: leaving it alive is not an option, and leaving it alive
     * SILENTLY is how this becomes an unexplainable crash three reloads later.
     *
     * Objects with a null Destruct (trivially destructible) are exempt --
     * there is no code left to jump into, so they can safely outlive the
     * module.
     *
     * Returns the number of objects destroyed.
     */
    CORE_API uint64_t OnImageUnloading(ImageID InImage);
}
}

#endif //ENGINECORE_COREGC_H
