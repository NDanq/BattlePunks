#include "Public/Core/CoreJob.h"

#include "Public/Core/CoreMemoryPool.h"   // SystemAlignedAlloc for the ring

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string>

/*
 * The job system.
 *
 * Read CoreJob.h first -- all of the policy is there, and this file is the
 * mechanism that implements it. The four things worth knowing before reading
 * any of the code below:
 *
 *   1. A slot's PendingCount is "this job plus its live children". It starts at
 *      one and the job is complete at zero. EVERYTHING here is that counter.
 *      Parenting adds a reference, finishing releases one, and a group is
 *      nothing but a slot whose one reference belongs to the dispatcher.
 *
 *   2. ONE MUTEX guards the free list, the ready queue and the main-thread
 *      queue. That is the topology the header promises not to expose, and it
 *      buys something a lock-free version would have to rebuild by hand: the
 *      dispatcher writes the payload, then pushes the index under the mutex, and
 *      the worker pops it under the same mutex -- so PAYLOAD PUBLICATION NEEDS
 *      NO FENCES AT ALL. The release/acquire edge is the lock.
 *
 *      The one place ordering does matter is completion, where a waiter that
 *      observes zero must see everything the job wrote. That is the acq_rel on
 *      the PendingCount decrement, and it is commented where it happens.
 *
 *   3. Wait RUNS JOBS. It never blocks on the thing it is waiting for. See the
 *      header's block on why, and the reentrancy that buys.
 *
 *   4. Slots are recycled, so a handle can name a job that no longer exists.
 *      The generation counter is what makes that detectable rather than
 *      arbitrary, and reading it correctly takes two loads -- see IsComplete.
 */

namespace EngineCore
{
namespace
{
    enum : uint32_t { INVALID_SLOT = 0xFFFFFFFFu };

    /*
     * Wait depth, per thread and not per pool: the stack being protected is the
     * thread's, and a thread waiting inside two different pools is nesting on
     * the same stack.
     */
    thread_local uint32_t GWaitDepth = 0;

    /*
     * Which pool owns the calling thread, so IsWorkerThread can answer without
     * scanning. A thread_local in EngineCore.dll is one variable per THREAD per
     * process -- not per image -- because the functions that touch it are all
     * compiled here. That is the opposite of GImageID's situation and is the
     * reason this does not need the defaulted-parameter treatment.
     */
    thread_local JobSystem* GOwningPool = nullptr;
}

/*
 * One slot, exactly one cache line.
 *
 * THE LAYOUT IS WHY JOB_PAYLOAD_CAPACITY IS 40. A worker claiming a job touches
 * the function pointer, the counter and the payload; if those straddle two lines
 * it pays for two. 64 minus the 24 bytes of bookkeeping below leaves 40, and the
 * static_assert under this struct is what keeps that arithmetic honest.
 *
 * Parent is an INDEX, not a JobHandle. A parent cannot outlive its children (its
 * count includes them), so there is no recycling window to protect against and
 * the generation would be dead weight.
 */
struct JobSystem::Impl
{
    struct alignas(64) Job
    {
        JobFn                 Fn              = nullptr;        //  8   @ 0
        std::atomic<uint32_t> Generation      {0};              //  4   @ 8
        std::atomic<int32_t>  PendingCount    {0};              //  4   @ 12
        uint32_t              Parent          = INVALID_SLOT;   //  4   @ 16
        ImageID               OwningImage     = IMAGE_Host;     //  2   @ 20
        bool                  bOpenGroup      = false;          //  1   @ 22
        bool                  bMainThreadOnly = false;          //  1   @ 23
        unsigned char         Payload[JOB_PAYLOAD_CAPACITY] = {};// 40  @ 24
    };                                                          // -----------
                                                                // 64
    /*
     * THIS ASSERT IS THE POINT, not decoration.
     *
     * It is what caught JOB_PAYLOAD_CAPACITY being 48 in the first draft of the
     * header: 48 plus 24 of bookkeeping is 72, which alignas(64) then rounds to
     * 128 -- silently doubling the ring's footprint and putting two cache lines
     * under every job claim, while every comment in the header went on claiming
     * one. Nothing else would have complained.
     *
     * If this fires, shrink JOB_PAYLOAD_CAPACITY. Do not relax the assert.
     */
    static_assert(sizeof(Job) == 64, "A job slot must be exactly one cache line.");

    Job*     Slots = nullptr;

    /* Guards all three containers below, and nothing else. */
    std::mutex               Mutex;
    std::deque<uint32_t>     Ready;        // indices, not pointers
    std::deque<uint32_t>     MainQueue;
    std::deque<uint32_t>     FreeSlots;

    /* Wakes idle workers. Signalled once per queued job; see WorkerMain for why
     * the worker side is a bounded wait rather than Wait(). */
    Semaphore                WorkSignal{0};

    std::atomic<bool>        bShuttingDown{false};

    /* Jobs claimed by a worker or a waiter and not yet finished. IsQuiescent is
     * "Ready empty AND this zero"; either alone is not enough -- a queue can be
     * empty while eight workers are mid-job. */
    std::atomic<uint32_t>    Executing{0};

    Thread*                  Workers     = nullptr;
    uint32_t                 WorkerCount = 0;

    /* Relaxed throughout: they are counters for a profile, and no decision is
     * made on them. */
    std::atomic<uint64_t>    Dispatched     {0};
    std::atomic<uint64_t>    Executed       {0};
    std::atomic<uint64_t>    ExecutedInline {0};
    std::atomic<uint64_t>    StolenByWaiters{0};
    std::atomic<uint64_t>    WorkerSleeps   {0};
    std::atomic<uint32_t>    PeakQueueDepth {0};
};

// === Construction ===========================================================

JobSystem::JobSystem(const uint32_t InWorkerCount)
{
    Pimpl = new Impl();

    /*
     * The ring is allocated ONCE and never grows. SystemAlignedAlloc rather than
     * Memory::AllocArray because these slots are not tracked objects, must not
     * appear in ReportLeaks, and want a 64-byte alignment the allocation header
     * would push around.
     */
    void* Block = SystemAlignedAlloc(sizeof(Impl::Job) * MAX_PENDING_JOBS, 64);
    CORE_CHECK(Block != nullptr, "Jobs: could not allocate the job ring.")
    if(!Block)
    {
        return;
    }

    /* Placement-new rather than memset: Generation and PendingCount are
     * std::atomic, and zeroing their storage is not the same as constructing
     * them. Same reasoning CoreMemory.cpp:265 gives for its stats block. */
    Pimpl->Slots = static_cast<Impl::Job*>(Block);
    for(uint32_t Slot = 0; Slot < MAX_PENDING_JOBS; ++Slot)
    {
        new (&Pimpl->Slots[Slot]) Impl::Job();
        Pimpl->FreeSlots.push_back(Slot);
    }

    /*
     * PHYSICAL cores MINUS ONE, clamped to at least one worker.
     *
     * Minus one because the main thread is the one that waits, and a pool sized
     * to every core makes the OS preempt a worker in order to run the thread
     * that is waiting for it. Physical rather than logical because this work is
     * compute-bound -- see appThreadHardwareCount's note on why both numbers
     * exist.
     */
    uint32_t Count = InWorkerCount;
    if(Count == 0)
    {
        const uint32_t Physical = Threads::GetPhysicalCoreCount();
        Count = Physical > 1 ? Physical - 1 : 1;
    }

    Pimpl->Workers     = new Thread[Count];
    Pimpl->WorkerCount = Count;

    for(uint32_t Index = 0; Index < Count; ++Index)
    {
        /* "Job/0", "Job/1" ... -- short on purpose. appThreadSetName truncates
         * to 15 characters on Linux, and these names exist to be READ in a
         * profiler's thread list. */
        char Name[16];
        std::snprintf(Name, sizeof(Name), "Job/%u", Index);

        if(!Pimpl->Workers[Index].Start(Name, [this] { WorkerMain(); }))
        {
            CORE_LOG_ERROR(std::string("Jobs: worker ") + std::to_string(Index) +
                           " failed to start; the pool will run with fewer workers than "
                           "requested. Dispatch still works -- Wait executes jobs itself.")
        }
    }
}

JobSystem::~JobSystem()
{
    if(!Pimpl)
    {
        return;
    }

    /*
     * Release, paired with the acquire in WorkerMain's loop condition: a worker
     * must not observe the flag before the writes that preceded it.
     */
    Pimpl->bShuttingDown.store(true, std::memory_order_release);

    /*
     * Signal generously. A worker blocked in TryWaitFor needs a permit or a
     * timeout to notice the flag, and over-signalling a semaphore is harmless
     * because the count simply accumulates. Under-signalling costs up to the
     * timeout per worker, which is the one thing teardown should not gamble on.
     */
    Pimpl->WorkSignal.Signal(Pimpl->WorkerCount * 2);

    for(uint32_t Index = 0; Index < Pimpl->WorkerCount; ++Index)
    {
        Pimpl->Workers[Index].Join();
    }
    delete[] Pimpl->Workers;

    /*
     * DROP what is still queued, and say how much.
     *
     * Draining instead would be worse: a queued job may have been dispatched by
     * something already torn down, so running it here is how a clean exit turns
     * into a crash in the last millisecond of the process. A caller who wants
     * everything finished calls WaitForIdle first, which is a visible statement
     * of that intent.
     *
     * Every dropped job is still COMPLETED, so anything waiting on it is
     * released. Leaving a waiter blocked forever on work that can never run is
     * not an improvement.
     */
    std::deque<uint32_t> ToRelease;
    {
        std::lock_guard<std::mutex> Lock(Pimpl->Mutex);

        ToRelease.insert(ToRelease.end(), Pimpl->Ready.begin(), Pimpl->Ready.end());
        ToRelease.insert(ToRelease.end(), Pimpl->MainQueue.begin(), Pimpl->MainQueue.end());
        Pimpl->Ready.clear();
        Pimpl->MainQueue.clear();
    }

    /* COMPLETED OUTSIDE THE LOCK. CompleteSlot takes the mutex in order to
     * recycle, so doing this in the block above would self-deadlock on a
     * non-recursive std::mutex -- and would do so only on the teardown path,
     * which is the worst possible place to hide a hang. */
    const uint32_t Dropped = uint32_t(ToRelease.size());
    for(const uint32_t Slot : ToRelease)
    {
        CompleteSlot(Slot);
    }

    if(Dropped > 0)
    {
        CORE_LOG_WARN(std::string("Jobs: the pool was destroyed with ") +
                      std::to_string(Dropped) + " job(s) still queued; they were dropped "
                      "without running. Call WaitForIdle() before destruction if their "
                      "results were needed.")
    }

    if(Pimpl->Slots)
    {
        for(uint32_t Slot = 0; Slot < MAX_PENDING_JOBS; ++Slot)
        {
            Pimpl->Slots[Slot].~Job();
        }
        SystemAlignedFree(Pimpl->Slots);
    }

    delete Pimpl;
    Pimpl = nullptr;
}

// === Slots ==================================================================

/*
 * Caller must hold the mutex. INVALID_SLOT when the ring is exhausted, which is
 * the condition Dispatch turns into inline execution and OpenGroup turns into an
 * invalid handle.
 */
uint32_t JobSystem::AcquireSlot()
{
    if(Pimpl->FreeSlots.empty())
    {
        return INVALID_SLOT;
    }

    const uint32_t Slot = Pimpl->FreeSlots.front();
    Pimpl->FreeSlots.pop_front();

    Impl::Job& J = Pimpl->Slots[Slot];

    /*
     * Bump the generation on ACQUIRE, not on release, and be clear about what
     * that costs: THERE IS A WINDOW where a slot sits on the free list still
     * carrying the generation of the job that just finished, so an old handle
     * to that job still resolves. Bumping on release would close that window
     * instead. This is a choice, not an oversight.
     *
     * It is safe because ReleaseSlot leaves the slot in a state where every
     * path through the window reaches the right answer on its own:
     *
     *      PendingCount is 0  -> IsComplete says complete, which is TRUE
     *                         -> Wait returns immediately, which is CORRECT
     *                         -> TryAdopt refuses, which is CORRECT
     *      bOpenGroup is false -> CloseGroup refuses a second close
     *
     * and the one genuinely dangerous interleaving -- the slot being reacquired
     * BETWEEN a caller's ResolveHandle and its TryAdopt -- is caught by the
     * generation re-check inside TryAdopt, not by this bump.
     *
     * What the window buys is a better diagnostic. A caller who parents a job to
     * something that has already finished gets "already complete", naming the
     * actual mistake, instead of "recycled", which describes an implementation
     * detail they did not cause and cannot act on.
     *
     * Zero is skipped because JobHandle uses Generation == 0 to mean "invalid",
     * and a wrapped counter must not manufacture a handle that reads as invalid.
     */
    uint32_t NextGen = J.Generation.load(std::memory_order_relaxed) + 1;
    if(NextGen == 0)
    {
        NextGen = 1;
    }
    J.Generation.store(NextGen, std::memory_order_relaxed);

    return Slot;
}

/* Caller must hold the mutex. */
void JobSystem::ReleaseSlot(const uint32_t InSlot)
{
    Impl::Job& J = Pimpl->Slots[InSlot];

    J.Fn              = nullptr;
    J.Parent          = INVALID_SLOT;
    J.bOpenGroup      = false;
    J.bMainThreadOnly = false;

    Pimpl->FreeSlots.push_back(InSlot);
}

/*
 * Releases one reference, and keeps going while that empties a parent.
 *
 * ITERATIVE, NOT RECURSIVE, on purpose: the chain length is the depth of the job
 * tree, which is a number this class does not control. A recursive version would
 * put a caller's data structure in charge of this thread's stack.
 *
 * Takes the mutex only to recycle, so completing a deep chain does not hold the
 * lock across the whole walk.
 */
void JobSystem::CompleteSlot(uint32_t InSlot)
{
    while(InSlot != INVALID_SLOT)
    {
        Impl::Job& J = Pimpl->Slots[InSlot];

        /*
         * acq_rel IS LOAD-BEARING AND IS THE ONE ORDERING THAT MATTERS HERE.
         *
         * Release half: a waiter that observes zero must also see every write
         * the job made. Acquire half: the thread that drives the count to zero
         * must see the writes of the children that decremented before it.
         *
         * Everything else in this file can be relaxed because the mutex orders
         * it. This cannot, because completion is deliberately lock-free.
         */
        if(J.PendingCount.fetch_sub(1, std::memory_order_acq_rel) != 1)
        {
            return;     // children outstanding, or a group still held open
        }

        const uint32_t Parent = J.Parent;

        {
            std::lock_guard<std::mutex> Lock(Pimpl->Mutex);
            ReleaseSlot(InSlot);
        }

        InSlot = Parent;
    }
}

/*
 * Adds a child's reference to a parent, refusing if the parent is already
 * complete.
 *
 * A CAS LOOP AND NOT A LOAD-THEN-INCREMENT. The check and the increment have to
 * be one operation: between a load that reads 1 and an increment, the parent's
 * own reference can be released and the count reach zero -- and then the child
 * is attached to a job whose waiters have already been let go, which is exactly
 * the "child of a completed parent" the header refuses.
 */
bool JobSystem::TryAdopt(const uint32_t InParentSlot, const uint32_t InExpectedGeneration)
{
    Impl::Job& Parent = Pimpl->Slots[InParentSlot];

    int32_t Count = Parent.PendingCount.load(std::memory_order_relaxed);
    while(Count > 0)
    {
        if(Parent.PendingCount.compare_exchange_weak(Count, Count + 1,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed))
        {
            /*
             * CONFIRM THE SLOT DID NOT RECYCLE UNDER US.
             *
             * The caller resolved the handle a moment ago, but resolving and
             * adopting are two steps: in between, the parent can complete and
             * its slot be handed to an unrelated job. Without this check the
             * child is attached to a STRANGER -- so an unrelated Wait blocks on
             * our child, and our completion decrements somebody else's counter.
             * Not memory-unsafe, and almost impossible to attribute afterwards.
             *
             * Giving the reference back cannot complete the stranger by
             * accident: the CAS only fired with Count > 0, so the count is at
             * least 2 here and at least 1 after the release.
             */
            if(Parent.Generation.load(std::memory_order_acquire) != InExpectedGeneration)
            {
                CompleteSlot(InParentSlot);
                return false;
            }
            return true;
        }
        // compare_exchange_weak reloaded Count; loop unless it reached zero.
    }

    return false;
}

/*
 * Resolves a handle to a slot, or INVALID_SLOT if it is stale.
 *
 * Caller must NOT hold the mutex (this takes none) and must treat a stale result
 * as "completed long ago", which is what a recycled slot means.
 */
uint32_t JobSystem::ResolveHandle(const JobHandle InHandle) const
{
    if(!InHandle.IsValid() || InHandle.Index >= MAX_PENDING_JOBS)
    {
        return INVALID_SLOT;
    }

    const Impl::Job& J = Pimpl->Slots[InHandle.Index];
    if(J.Generation.load(std::memory_order_acquire) != InHandle.Generation)
    {
        return INVALID_SLOT;
    }

    return InHandle.Index;
}

// === Dispatch ===============================================================

JobHandle JobSystem::Dispatch(const JobFn     InFn,
                              const void*     InPayload,
                              const size_t    InPayloadBytes,
                              const JobHandle InParent,
                              const ImageID   InOwningImage)
{
    if(!InFn)
    {
        CORE_LOG_ERROR("Jobs: Dispatch called with a null function.")
        return JobHandle{};
    }

    if(InPayloadBytes > JOB_PAYLOAD_CAPACITY)
    {
        CORE_LOG_ERROR(std::string("Jobs: payload of ") + std::to_string(InPayloadBytes) +
                       " bytes exceeds JOB_PAYLOAD_CAPACITY; the job was not dispatched. "
                       "Capture a pointer to the state instead.")
        return JobHandle{};
    }

    /*
     * Adopt the parent BEFORE taking a slot.
     *
     * If this fails the dispatch is refused outright, and doing it first means
     * there is no slot to give back on that path. The reverse order would have
     * to unwind, and unwinding a slot means retaking the mutex.
     */
    uint32_t ParentSlot = INVALID_SLOT;
    if(InParent.IsValid())
    {
        ParentSlot = ResolveHandle(InParent);
        if(ParentSlot == INVALID_SLOT)
        {
            CORE_LOG_ERROR("Jobs: Dispatch given a stale parent handle; the parent completed "
                           "and was recycled, so this child would run unobserved. Refused.")
            return JobHandle{};
        }

        if(!TryAdopt(ParentSlot, InParent.Generation))
        {
            CORE_LOG_ERROR("Jobs: Dispatch given a parent that is already complete. Anything "
                           "waiting on it has been released, so this child would run "
                           "unobserved. Refused -- if this is a fan-out, the parent should be "
                           "an OpenGroup.")
            return JobHandle{};
        }
    }

    uint32_t  Slot = INVALID_SLOT;
    JobHandle Handle{};

    {
        std::lock_guard<std::mutex> Lock(Pimpl->Mutex);

        Slot = AcquireSlot();
        if(Slot != INVALID_SLOT)
        {
            Impl::Job& J = Pimpl->Slots[Slot];

            J.Fn              = InFn;
            J.Parent          = ParentSlot;
            J.OwningImage     = InOwningImage;
            J.bOpenGroup      = false;
            J.bMainThreadOnly = false;
            J.PendingCount.store(1, std::memory_order_relaxed);

            if(InPayload && InPayloadBytes > 0)
            {
                std::memcpy(J.Payload, InPayload, InPayloadBytes);
            }

            Handle.Index      = Slot;
            Handle.Generation = J.Generation.load(std::memory_order_relaxed);

            /* Pushed under the same lock the worker pops under, which is what
             * publishes the payload above with no fence of its own. */
            Pimpl->Ready.push_back(Slot);

            const uint32_t Depth = uint32_t(Pimpl->Ready.size());
            if(Depth > Pimpl->PeakQueueDepth.load(std::memory_order_relaxed))
            {
                Pimpl->PeakQueueDepth.store(Depth, std::memory_order_relaxed);
            }
        }
    }

    if(Slot == INVALID_SLOT)
    {
        /*
         * THE RING IS FULL, SO RUN IT HERE -- outside the lock, which matters:
         * the payload may dispatch, and dispatching under our own mutex is an
         * immediate self-deadlock.
         *
         * This is the backpressure the header describes. The handle returned is
         * invalid because the work is already done and there is no slot to name;
         * a Wait on it returns at once, which is correct.
         */
        Pimpl->ExecutedInline.fetch_add(1, std::memory_order_relaxed);
        Pimpl->Executed.fetch_add(1, std::memory_order_relaxed);

        unsigned char Local[JOB_PAYLOAD_CAPACITY];
        if(InPayload && InPayloadBytes > 0)
        {
            std::memcpy(Local, InPayload, InPayloadBytes);
        }
        InFn(Local);

        /* The parent was adopted above and this child is now finished, so its
         * reference has to come back off -- otherwise the parent never completes
         * and a Wait on it hangs forever. */
        if(ParentSlot != INVALID_SLOT)
        {
            CompleteSlot(ParentSlot);
        }

        return JobHandle{};
    }

    Pimpl->Dispatched.fetch_add(1, std::memory_order_relaxed);
    Pimpl->WorkSignal.Signal();

    return Handle;
}

JobHandle JobSystem::DispatchOnMain(const JobFn   InFn,
                                    const void*   InPayload,
                                    const size_t  InPayloadBytes,
                                    const ImageID InOwningImage)
{
    if(!InFn || InPayloadBytes > JOB_PAYLOAD_CAPACITY)
    {
        CORE_LOG_ERROR("Jobs: DispatchOnMain called with a null function or an oversized payload.")
        return JobHandle{};
    }

    JobHandle Handle{};

    {
        std::lock_guard<std::mutex> Lock(Pimpl->Mutex);

        const uint32_t Slot = AcquireSlot();
        if(Slot == INVALID_SLOT)
        {
            /*
             * NO INLINE FALLBACK HERE, unlike Dispatch. The entire point of this
             * queue is that the work must run ON THE MAIN THREAD; running it
             * here, on whatever worker called, would do the exact thing the
             * caller was avoiding -- touch ModuleManager or a Qt file provider
             * from a worker. Refusing loudly is the only honest option.
             */
            CORE_LOG_ERROR("Jobs: the ring is full, so a main-thread job was REFUSED rather "
                           "than run inline -- running it here would defeat the purpose of "
                           "the queue. Call TickMainThread more often.")
            return JobHandle{};
        }

        Impl::Job& J = Pimpl->Slots[Slot];

        J.Fn              = InFn;
        J.Parent          = INVALID_SLOT;
        J.OwningImage     = InOwningImage;
        J.bOpenGroup      = false;
        J.bMainThreadOnly = true;
        J.PendingCount.store(1, std::memory_order_relaxed);

        if(InPayload && InPayloadBytes > 0)
        {
            std::memcpy(J.Payload, InPayload, InPayloadBytes);
        }

        Handle.Index      = Slot;
        Handle.Generation = J.Generation.load(std::memory_order_relaxed);

        Pimpl->MainQueue.push_back(Slot);
    }

    /* Deliberately NOT signalled: no worker may take this, and TickMainThread
     * polls rather than waits. */
    Pimpl->Dispatched.fetch_add(1, std::memory_order_relaxed);

    return Handle;
}

// === Groups =================================================================

JobHandle JobSystem::OpenGroup()
{
    std::lock_guard<std::mutex> Lock(Pimpl->Mutex);

    const uint32_t Slot = AcquireSlot();
    if(Slot == INVALID_SLOT)
    {
        CORE_LOG_ERROR("Jobs: the ring is full, so no group could be opened. A group holds no "
                       "work, so there is nothing to run inline instead -- the caller must "
                       "fall back to doing the work serially.")
        return JobHandle{};
    }

    Impl::Job& J = Pimpl->Slots[Slot];

    J.Fn              = nullptr;    // never executed, and never queued
    J.Parent          = INVALID_SLOT;
    J.bOpenGroup      = true;
    J.bMainThreadOnly = false;

    /* The one reference belongs to the caller and is released by CloseGroup. It
     * is what makes the counter unable to reach zero while children are still
     * being added. */
    J.PendingCount.store(1, std::memory_order_relaxed);

    JobHandle Handle;
    Handle.Index      = Slot;
    Handle.Generation = J.Generation.load(std::memory_order_relaxed);
    return Handle;
}

void JobSystem::CloseGroup(const JobHandle InGroup)
{
    const uint32_t Slot = ResolveHandle(InGroup);
    if(Slot == INVALID_SLOT)
    {
        CORE_LOG_ERROR("Jobs: CloseGroup on an invalid or already-recycled handle. Closing a "
                       "group twice releases a reference that is not yours and can complete "
                       "the group out from under its children.")
        return;
    }

    if(!Pimpl->Slots[Slot].bOpenGroup)
    {
        CORE_LOG_ERROR("Jobs: CloseGroup called on a handle that is not an open group.")
        return;
    }

    /* Cleared before the reference is released: after CompleteSlot the slot may
     * already belong to somebody else, and writing to it then would corrupt
     * their job. */
    Pimpl->Slots[Slot].bOpenGroup = false;

    CompleteSlot(Slot);
}

// === Execution ==============================================================

/*
 * Runs one ready job if there is one. False if the queue was empty.
 *
 * The single place a job is ever executed, so it is also the single place the
 * Executing counter and the completion path are driven from. Called by workers
 * and by Wait, which is what makes a wait productive instead of idle.
 */
bool JobSystem::TryRunOneJob()
{
    uint32_t Slot = INVALID_SLOT;

    {
        std::lock_guard<std::mutex> Lock(Pimpl->Mutex);
        if(Pimpl->Ready.empty())
        {
            return false;
        }
        Slot = Pimpl->Ready.front();
        Pimpl->Ready.pop_front();

        /*
         * Incremented INSIDE the lock, together with the pop.
         *
         * If it were bumped after releasing the lock there would be an instant
         * where the job is in neither the queue nor the executing count, and
         * IsQuiescent would report an idle pool with a job in flight -- which is
         * precisely the window in which CollectGarbage would then run against a
         * live worker.
         */
        Pimpl->Executing.fetch_add(1, std::memory_order_relaxed);
    }

    Impl::Job& J = Pimpl->Slots[Slot];

    /* Copied out before running. The payload lives in a slot that CompleteSlot
     * may recycle the moment the job finishes, and a job that dispatches more
     * work can cause exactly that recycling while its own frame is still live. */
    const JobFn   Fn = J.Fn;
    unsigned char Local[JOB_PAYLOAD_CAPACITY];
    std::memcpy(Local, J.Payload, JOB_PAYLOAD_CAPACITY);

    if(Fn)
    {
        Fn(Local);
    }

    Pimpl->Executed.fetch_add(1, std::memory_order_relaxed);

    /* Completion before the Executing decrement, so the pool never reports
     * itself quiescent while a completion chain is still unwinding. */
    CompleteSlot(Slot);
    Pimpl->Executing.fetch_sub(1, std::memory_order_relaxed);

    return true;
}

void JobSystem::WorkerMain()
{
    GOwningPool = this;

    /*
     * THE SHUTDOWN CHECK IS FIRST, AND THAT IS THE POLICY, NOT A STYLE CHOICE.
     * The destructor documents that queued-but-unstarted jobs are DROPPED;
     * draining before checking would instead run them during teardown, which is
     * the behaviour that turns a clean exit into a crash.
     */
    while(!Pimpl->bShuttingDown.load(std::memory_order_acquire))
    {
        if(TryRunOneJob())
        {
            continue;
        }

        Pimpl->WorkerSleeps.fetch_add(1, std::memory_order_relaxed);

        /*
         * BOUNDED, not Wait().
         *
         * A permit can be consumed by a thread sitting in Wait() rather than by
         * a worker, so the count and the queue drift apart: there can be work
         * ready with no permit left to announce it. An unbounded wait would sleep
         * a worker on a non-empty queue -- the lost-wakeup symptom, arrived at
         * from the other direction. The timeout makes that a 2ms latency instead
         * of a hang, and it is also how a worker notices shutdown.
         */
        Pimpl->WorkSignal.TryWaitFor(2);
    }

    GOwningPool = nullptr;
}

uint32_t JobSystem::TickMainThread(const uint32_t InMaxJobs)
{
    CORE_CHECK_MAIN_THREAD("JobSystem::TickMainThread")

    /*
     * The batch is snapshotted by COUNT, not drained until empty: a main-thread
     * job may itself call DispatchOnMain, and draining "until empty" would then
     * run the new work in the same tick and, in the pathological case, never
     * return. Taking the queue's size once bounds the tick to the work that was
     * already pending.
     */
    uint32_t Budget = InMaxJobs;
    if(Budget == 0)
    {
        std::lock_guard<std::mutex> Lock(Pimpl->Mutex);
        Budget = uint32_t(Pimpl->MainQueue.size());
    }

    uint32_t Ran = 0;
    while(Ran < Budget)
    {
        uint32_t Slot = INVALID_SLOT;
        {
            std::lock_guard<std::mutex> Lock(Pimpl->Mutex);
            if(Pimpl->MainQueue.empty())
            {
                break;
            }
            Slot = Pimpl->MainQueue.front();
            Pimpl->MainQueue.pop_front();
            Pimpl->Executing.fetch_add(1, std::memory_order_relaxed);
        }

        Impl::Job&    J  = Pimpl->Slots[Slot];
        const JobFn   Fn = J.Fn;
        unsigned char Local[JOB_PAYLOAD_CAPACITY];
        std::memcpy(Local, J.Payload, JOB_PAYLOAD_CAPACITY);

        if(Fn)
        {
            Fn(Local);
        }

        Pimpl->Executed.fetch_add(1, std::memory_order_relaxed);
        CompleteSlot(Slot);
        Pimpl->Executing.fetch_sub(1, std::memory_order_relaxed);
        ++Ran;
    }

    return Ran;
}

// === Waiting ================================================================

bool JobSystem::IsComplete(const JobHandle InHandle) const
{
    if(!InHandle.IsValid() || InHandle.Index >= MAX_PENDING_JOBS)
    {
        return true;        // nothing to wait for
    }

    const Impl::Job& J = Pimpl->Slots[InHandle.Index];

    /*
     * TWO GENERATION READS, AROUND THE COUNT. This is the recycling race, and it
     * is the subtlest thing in the file.
     *
     * A single read is not enough: the generation can match, the job can then
     * complete and its slot be handed to an unrelated job, and the count this
     * reads belongs to that stranger. The waiter would then wait on somebody
     * else's work -- not a crash, but an arbitrarily long stall with no
     * explanation.
     *
     * Reading the generation again afterwards closes it. If it moved, the slot
     * was recycled underneath us, which means our job finished: complete. If it
     * held, the count was genuinely ours.
     */
    const uint32_t Before = J.Generation.load(std::memory_order_acquire);
    if(Before != InHandle.Generation)
    {
        return true;        // recycled, therefore finished
    }

    const int32_t Count = J.PendingCount.load(std::memory_order_acquire);

    const uint32_t After = J.Generation.load(std::memory_order_acquire);
    if(After != InHandle.Generation)
    {
        return true;
    }

    return Count <= 0;
}

void JobSystem::Wait(const JobHandle InHandle)
{
    if(!InHandle.IsValid())
    {
        return;             // an inline-executed or refused dispatch: already done
    }

    const uint32_t Slot = ResolveHandle(InHandle);
    if(Slot == INVALID_SLOT)
    {
        CORE_LOG_WARN("Jobs: Wait on a recycled handle. The job completed earlier and its slot "
                      "has been reused; treating it as complete. A handle does not survive "
                      "across frames.")
        return;
    }

    /*
     * An open group can never complete, because the reference keeping it open is
     * held by this caller. So this is not a wait, it is a hang.
     *
     * THE RETURN AFTER CORE_CHECK IS LOAD-BEARING. Per CoreLog.h's fatal
     * semantics, a host with a handler installed logs and RETURNS -- so without
     * the explicit bail the "fatal" check would be followed by the very hang it
     * exists to prevent, and the log line would be the last thing the process
     * ever printed.
     */
    if(Pimpl->Slots[Slot].bOpenGroup)
    {
        CORE_CHECK(false, "Jobs: Wait on a group that was never closed. Call CloseGroup first "
                          "-- this wait could never return.")
        return;
    }

    /*
     * Depth is capped because Wait runs jobs, so a wait chain nests real stack
     * frames. Exceeding the cap is a diagnosable refusal; the alternative is a
     * stack overflow in a worker thread, which is neither.
     */
    if(GWaitDepth >= MAX_WAIT_DEPTH)
    {
        CORE_CHECK(false, "Jobs: wait chain deeper than MAX_WAIT_DEPTH. A job is waiting on a "
                          "job that is waiting on a job, 32 deep -- restructure the fan-in.")
        return;
    }

    ++GWaitDepth;

    uint32_t Spins = 0;
    while(!IsComplete(InHandle))
    {
        if(TryRunOneJob())
        {
            Pimpl->StolenByWaiters.fetch_add(1, std::memory_order_relaxed);
            Spins = 0;
            continue;
        }

        /*
         * Nothing runnable, so what we are waiting on is executing on another
         * thread. Yield first -- the common case is that it finishes almost
         * immediately -- then fall back to sleeping, because burning a core to
         * watch another core work helps nobody.
         *
         * THIS IS THE CRUDE PART OF THE FILE, and knowingly so. The principled
         * fix is a per-job completion signal, which means a semaphore per slot
         * and 4096 of them; the fix engines actually reach for is fibers, which
         * the header rules out and explains why. Revisit with a profile showing
         * time spent here, not before.
         */
        if(++Spins < 64)
        {
            Threads::Yield();
        }
        else
        {
            Threads::SleepMs(1);
        }
    }

    --GWaitDepth;
}

void JobSystem::WaitForIdle()
{
    /* Same shape as Wait, against "nothing queued and nothing running" instead
     * of one handle -- so a caller on the main thread helps drain rather than
     * watching workers do it. */
    uint32_t Spins = 0;
    while(!IsQuiescent())
    {
        if(TryRunOneJob())
        {
            Spins = 0;
            continue;
        }

        if(++Spins < 64)
        {
            Threads::Yield();
        }
        else
        {
            Threads::SleepMs(1);
        }
    }
}

bool JobSystem::IsQuiescent() const
{
    /*
     * BOTH CONDITIONS, and the queue is checked first only because it is the
     * cheaper one to fail on. Neither alone is sufficient: a queue can be empty
     * while every worker is mid-job, and the executing count can be zero for an
     * instant while the queue is still full.
     *
     * Racy the moment anything else is still dispatching, and the header says so
     * -- it can only promise the pool was idle when it returned. That is enough
     * for the frame-boundary use it exists for.
     */
    {
        std::lock_guard<std::mutex> Lock(Pimpl->Mutex);
        if(!Pimpl->Ready.empty())
        {
            return false;
        }
    }

    return Pimpl->Executing.load(std::memory_order_acquire) == 0;
}

// === Introspection ==========================================================

uint32_t JobSystem::GetWorkerCount() const
{
    return Pimpl->WorkerCount;
}

bool JobSystem::IsWorkerThread() const
{
    return GOwningPool == this;
}

void JobSystem::GetStats(JobSystemStats& OutStats) const
{
    OutStats.Dispatched      = Pimpl->Dispatched.load(std::memory_order_relaxed);
    OutStats.Executed        = Pimpl->Executed.load(std::memory_order_relaxed);
    OutStats.ExecutedInline  = Pimpl->ExecutedInline.load(std::memory_order_relaxed);
    OutStats.StolenByWaiters = Pimpl->StolenByWaiters.load(std::memory_order_relaxed);
    OutStats.WorkerSleeps    = Pimpl->WorkerSleeps.load(std::memory_order_relaxed);
    OutStats.WorkerCount     = Pimpl->WorkerCount;
    OutStats.PeakQueueDepth  = Pimpl->PeakQueueDepth.load(std::memory_order_relaxed);
}

// === The image barrier ======================================================

uint32_t JobSystem::OnImageUnloading(const ImageID InImage)
{
    /*
     * Two phases, and the order is forced.
     *
     * First remove what is QUEUED, because a queued job in a doomed image is a
     * function pointer into memory about to be unmapped and the whole point is
     * that nothing can pick it up. Then wait out what is ALREADY RUNNING, which
     * cannot be cancelled -- Thread::Join's comment says why -- so the only
     * option is to let it finish while its code is still mapped.
     *
     * Doing it the other way round would let a worker pull a queued job from the
     * dying image while we were waiting for a different one to finish.
     */
    std::deque<uint32_t> ToRelease;

    {
        std::lock_guard<std::mutex> Lock(Pimpl->Mutex);

        for(std::deque<uint32_t>* Queue : { &Pimpl->Ready, &Pimpl->MainQueue })
        {
            std::deque<uint32_t> Survivors;

            for(const uint32_t Slot : *Queue)
            {
                if(Pimpl->Slots[Slot].OwningImage == InImage)
                {
                    /* Cleared so that nothing can call into the dying image even
                     * if this slot is somehow reached again before it recycles. */
                    Pimpl->Slots[Slot].Fn = nullptr;
                    ToRelease.push_back(Slot);
                }
                else
                {
                    Survivors.push_back(Slot);
                }
            }

            Queue->swap(Survivors);     // survivors keep their original order
        }
    }

    /* Released outside the lock, for the reason CompleteSlot's comment gives. */
    const uint32_t Dropped = uint32_t(ToRelease.size());
    for(const uint32_t Slot : ToRelease)
    {
        CompleteSlot(Slot);
    }

    if(Dropped > 0)
    {
        /*
         * Logged loudly and per-image, because a dropped job means a module was
         * reloaded with work outstanding and SOMEBODY'S RESULT IS NOW MISSING.
         * That is CoreModule.h's RELOAD HAZARDS list, not something this can fix
         * from here -- but it must not be silent, or the missing result surfaces
         * as a rendering glitch three reloads later.
         */
        CORE_LOG_WARN(std::string("Jobs: image '") + Memory::GetImageName(InImage) +
                      "' is unloading; dropped " + std::to_string(Dropped) +
                      " queued job(s) that would have called into it. Anything waiting on "
                      "them has been released without their work being done.")
    }

    /* Anything already running is waited out, including jobs from other images:
     * there is no way to wait for "only that image's" running jobs without a
     * per-job image check the executing path does not keep. Waiting for all of
     * them is stricter than needed and correct. */
    WaitForIdle();

    return Dropped;
}

// === The locator ============================================================

namespace
{
    /*
     * Non-owning, no locking, set once by the host -- the same shape
     * CoreModuleManager.h and CoreFileProvider.h use, and constant-initialised
     * into .bss for the reason CoreConfig.h:31 gives.
     */
    JobSystem* GJobSystem = nullptr;
}

void SetJobSystem(JobSystem* const InSystem)
{
    GJobSystem = InSystem;
}

JobSystem* GetJobSystem()
{
    return GJobSystem;
}
}
