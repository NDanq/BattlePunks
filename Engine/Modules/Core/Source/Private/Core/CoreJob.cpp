#include "Public/Core/CoreJob.h"

#include "Public/Core/CoreMemoryPool.h"   // SystemAlignedAlloc for the ring

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <string>

/*
 * The job system. CoreJob.h carries the policy; this is the mechanism.
 *
 * Two facts orient the whole file:
 *
 *   PendingCount is "this job plus its live children", starting at one and
 *   complete at zero. Parenting adds a reference, finishing releases one, and a
 *   group is a slot whose single reference belongs to the dispatcher.
 *
 *   ONE MUTEX guards the free list and both queues. A dispatcher writes its
 *   payload then pushes the index under that lock, and a worker pops under the
 *   same one, so payload publication needs no fences of its own. Completion is
 *   the exception: it is lock-free, and carries the one ordering that matters.
 */

namespace EngineCore
{
namespace
{
    enum : uint32_t { INVALID_SLOT = 0xFFFFFFFFu };

    /* Per thread, not per pool: the stack being protected is the thread's. */
    thread_local uint32_t GWaitDepth = 0;

    /* Which pool owns the calling thread, so IsWorkerThread need not scan. One
     * copy per thread for the whole process, not per image -- every function that
     * touches it is compiled here, which is why it needs no GImageID treatment. */
    thread_local JobSystem* GOwningPool = nullptr;
}

/*
 * One slot, exactly one cache line: 64 minus 24 of bookkeeping leaves 40 for the
 * payload, which is where JOB_PAYLOAD_CAPACITY comes from.
 *
 * Parent is an INDEX, not a handle. A parent cannot outlive its children, so
 * there is no recycling window here and a generation would be dead weight.
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
    /* Not decoration: this caught JOB_PAYLOAD_CAPACITY being 48, where 48 plus 24
     * is 72 and alignas(64) rounds to 128 -- doubling the ring and putting two
     * lines under every job claim, silently. Shrink the payload if it fires. */
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

    /* SetActiveWorkerLimit's. A worker at or past the limit waits on ParkWake;
     * ParkMutex guards nothing but that wait, so parking never contends with
     * the queue's Mutex. */
    std::atomic<uint32_t>    ActiveLimit{0};
    std::atomic<uint32_t>    IdleWaitMs {2};
    std::mutex               ParkMutex;
    std::condition_variable  ParkWake;

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

    /* Allocated once, never grown. SystemAlignedAlloc and not Memory::AllocArray:
     * these are not tracked objects, must not show up in ReportLeaks, and want an
     * alignment the allocation header would push around. */
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
     * PHYSICAL cores minus one, at least one. Minus one because the main thread
     * is the one that waits, and a pool sized to every core makes the OS preempt
     * a worker to run the thread waiting on it. Physical because this work is
     * compute-bound -- appThreadHardwareCount explains why both numbers exist.
     */
    uint32_t Count = InWorkerCount;
    if(Count == 0)
    {
        const uint32_t Physical = Threads::GetPhysicalCoreCount();
        Count = Physical > 1 ? Physical - 1 : 1;
    }

    Pimpl->Workers     = new Thread[Count];
    Pimpl->WorkerCount = Count;
    Pimpl->ActiveLimit.store(Count, std::memory_order_relaxed);

    for(uint32_t Index = 0; Index < Count; ++Index)
    {
        /* Short on purpose: Linux truncates at 15 characters, and these exist to
         * be read in a profiler's thread list. */
        char Name[16];
        std::snprintf(Name, sizeof(Name), "Job/%u", Index);

        if(!Pimpl->Workers[Index].Start(Name, [this, Index] { WorkerMain(Index); }))
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

    /* Release, paired with the acquire in WorkerMain's loop condition. */
    Pimpl->bShuttingDown.store(true, std::memory_order_release);
    {
        /* Under ParkMutex, so a worker between its predicate and its wait
         * cannot miss this. */
        std::lock_guard<std::mutex> Lock(Pimpl->ParkMutex);
    }
    Pimpl->ParkWake.notify_all();

    /* Generously: a worker in TryWaitFor needs a permit or a timeout to notice
     * the flag, and an over-signalled semaphore simply accumulates. */
    Pimpl->WorkSignal.Signal(Pimpl->WorkerCount * 2);

    for(uint32_t Index = 0; Index < Pimpl->WorkerCount; ++Index)
    {
        Pimpl->Workers[Index].Join();
    }
    delete[] Pimpl->Workers;

    /*
     * DROP what is queued rather than draining it: a queued job may have been
     * dispatched by something already torn down, so running it here is how a
     * clean exit becomes a crash. WaitForIdle first is how a caller asks for the
     * opposite. Dropped jobs are still COMPLETED, so their waiters are released.
     */
    std::deque<uint32_t> ToRelease;
    {
        std::lock_guard<std::mutex> Lock(Pimpl->Mutex);

        ToRelease.insert(ToRelease.end(), Pimpl->Ready.begin(), Pimpl->Ready.end());
        ToRelease.insert(ToRelease.end(), Pimpl->MainQueue.begin(), Pimpl->MainQueue.end());
        Pimpl->Ready.clear();
        Pimpl->MainQueue.clear();
    }

    /* Outside the lock: CompleteSlot takes the mutex to recycle, so doing this in
     * the block above self-deadlocks -- on the teardown path, of all places. */
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

/* Mutex HELD. INVALID_SLOT when the ring is exhausted -- Dispatch turns that into
 * inline execution, OpenGroup into an invalid handle. */
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
     * Bumped on ACQUIRE, which deliberately leaves a window: a freed slot keeps
     * its old generation until reuse, so a finished job's handle still resolves.
     * That is safe because ReleaseSlot leaves PendingCount at 0 and bOpenGroup
     * false, so IsComplete, Wait, TryAdopt and CloseGroup each answer correctly
     * through it; the dangerous interleaving -- reacquisition between a caller's
     * ResolveHandle and its TryAdopt -- is caught by the re-check in TryAdopt.
     *
     * What it buys is the diagnostic: parenting to a finished job then reports
     * "already complete" rather than "recycled".
     *
     * Zero is skipped because Generation == 0 means an invalid handle.
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
 * Releases one reference, continuing while that empties a parent. Mutex NOT held.
 *
 * ITERATIVE: the chain is as deep as the job tree, which this class does not
 * control, so recursion would put a caller's data in charge of this stack.
 */
void JobSystem::CompleteSlot(uint32_t InSlot)
{
    while(InSlot != INVALID_SLOT)
    {
        Impl::Job& J = Pimpl->Slots[InSlot];

        /*
         * acq_rel is the one ordering that matters. Release: a waiter observing
         * zero must see the job's writes. Acquire: whoever drives it to zero must
         * see the children that decremented first. Everything else here is
         * ordered by the mutex; completion is deliberately lock-free.
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
 * Adds a child's reference, refusing a parent that is already complete.
 *
 * A CAS LOOP, not load-then-increment: the test and the increment must be one
 * operation, or the parent's own reference can drop in between and the child
 * attaches to a job whose waiters have already been released.
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
             * Confirm the slot did not recycle between the caller's
             * ResolveHandle and this CAS. Without it the child attaches to a
             * STRANGER: an unrelated Wait blocks on it and our completion
             * decrements someone else's counter -- not memory-unsafe, and very
             * hard to attribute afterwards.
             *
             * Handing the reference back cannot complete that stranger: the CAS
             * fired with Count > 0, so the count is >= 1 after the release.
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

/* Mutex NOT held. INVALID_SLOT means stale, which a caller must read as
 * "completed long ago" -- that is what a recycled slot means. */
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
         * Ring full, so run it here -- OUTSIDE the lock, which matters: the
         * payload may itself dispatch, and doing that under our own mutex is an
         * immediate self-deadlock. The handle is invalid because the work is
         * already done and no slot names it.
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
            /* No inline fallback, unlike Dispatch: running it here, on whatever
             * worker called, would do the exact thing the caller was avoiding. */
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

    /* The caller's reference, released by CloseGroup. It is what stops the count
     * reaching zero while children are still being added. */
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

    /* Cleared BEFORE the reference is released: after CompleteSlot the slot may
     * already belong to someone else. */
    Pimpl->Slots[Slot].bOpenGroup = false;

    CompleteSlot(Slot);
}

// === Execution ==============================================================

/*
 * Runs one ready job; false if the queue was empty.
 *
 * The only place a job is executed, and therefore the only place Executing and
 * the completion path are driven from. Workers and Wait both come through here.
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

        /* Incremented INSIDE the lock, with the pop. Bumped after, there would be
         * an instant where the job is in neither the queue nor the count, and
         * IsQuiescent would report idle with a job in flight -- exactly the window
         * CollectGarbage must not run in. */
        Pimpl->Executing.fetch_add(1, std::memory_order_relaxed);
    }

    Impl::Job& J = Pimpl->Slots[Slot];

    /* Copied out before running: CompleteSlot may recycle this slot the moment
     * the job finishes, and a job that dispatches can trigger that itself. */
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

void JobSystem::WorkerMain(const uint32_t InIndex)
{
    GOwningPool = this;

    /* The shutdown check is FIRST, and that is policy rather than style: the
     * destructor drops queued jobs, so draining before checking would run them
     * during teardown instead. */
    while(!Pimpl->bShuttingDown.load(std::memory_order_acquire))
    {
        /* Parked past the active limit: a real sleep, woken by a raised limit
         * or shutdown. Checked between jobs, so a lowered limit lets a running
         * job finish. */
        if(InIndex >= Pimpl->ActiveLimit.load(std::memory_order_acquire))
        {
            std::unique_lock<std::mutex> Lock(Pimpl->ParkMutex);
            Pimpl->ParkWake.wait(Lock, [this, InIndex]
            {
                return Pimpl->bShuttingDown.load(std::memory_order_acquire)
                    || InIndex < Pimpl->ActiveLimit.load(std::memory_order_acquire);
            });
            continue;
        }

        if(TryRunOneJob())
        {
            continue;
        }

        Pimpl->WorkerSleeps.fetch_add(1, std::memory_order_relaxed);

        /* Bounded, not Wait(): a thread inside Wait() can consume a permit meant
         * for a worker, so the count and the queue drift apart. The timeout turns
         * that into 2ms of latency rather than a worker asleep on a non-empty
         * queue, and is also how shutdown is noticed. */
        Pimpl->WorkSignal.TryWaitFor(Pimpl->IdleWaitMs.load(std::memory_order_relaxed));
    }

    GOwningPool = nullptr;
}

uint32_t JobSystem::TickMainThread(const uint32_t InMaxJobs)
{
    CORE_CHECK_MAIN_THREAD("JobSystem::TickMainThread")

    /* Snapshotted by COUNT rather than drained until empty: a main-thread job may
     * itself call DispatchOnMain, and "until empty" would run that in the same
     * tick and, pathologically, never return. */
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
     * TWO GENERATION READS, AROUND THE COUNT -- the subtlest thing in the file.
     *
     * One read is not enough: the generation can match, the job then complete and
     * its slot go to an unrelated job, and the count read belong to that stranger.
     * The waiter would stall on someone else's work with no explanation. If the
     * second read moved, the slot recycled, which means our job finished.
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
     * An open group can never complete -- the reference holding it open belongs to
     * this caller -- so this would be a hang, not a wait.
     *
     * THE RETURN AFTER CORE_CHECK IS LOAD-BEARING: per CoreLog.h, an installed
     * handler logs and RETURNS, so without the explicit bail the "fatal" check is
     * followed by the very hang it exists to prevent.
     */
    if(Pimpl->Slots[Slot].bOpenGroup)
    {
        CORE_CHECK(false, "Jobs: Wait on a group that was never closed. Call CloseGroup first "
                          "-- this wait could never return.")
        return;
    }

    /* Capped because Wait runs jobs, so a wait chain nests real stack frames. The
     * alternative to a diagnosable refusal is a stack overflow in a worker. */
    if(GWaitDepth >= MAX_WAIT_DEPTH)
    {
        CORE_CHECK(false, "Jobs: wait chain deeper than MAX_WAIT_DEPTH. A job is waiting on a "
                          "job that is waiting on a job, 32 deep -- restructure the fan-in.")
        return;
    }

    ++GWaitDepth;

    using Clock = std::chrono::steady_clock;
    Clock::time_point YieldUntil{};
    bool              bYielding = false;
    while(!IsComplete(InHandle))
    {
        if(TryRunOneJob())
        {
            Pimpl->StolenByWaiters.fetch_add(1, std::memory_order_relaxed);
            bYielding = false;
            continue;
        }

        /*
         * Nothing runnable, so what we wait on is executing elsewhere. Yield
         * first, then sleep; burning a core to watch another core work helps
         * nobody.
         *
         * KNOWINGLY THE CRUDE PART. The principled fix is a completion signal per
         * slot, and there are 4096 of them; the fix engines reach for is fibers,
         * which the header rules out.
         *
         * Yield for up to WAIT_YIELD_MS of wall time before sleeping, not a count
         * of yields: a sleep is at least a millisecond and often more (the OS
         * timer), so a wait that ends sooner -- a parallel pass's chunks, a
         * fraction of a millisecond -- paid a whole one. Profiled on D3D12's
         * parallel shadows: chunks done in 0.6 ms, ParallelFor back at 1.6.
         */
        if(!bYielding)
        {
            YieldUntil = Clock::now() + std::chrono::microseconds(WAIT_YIELD_US);
            bYielding  = true;
        }
        if(Clock::now() < YieldUntil)
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
    /* Wait's shape, against "nothing queued and nothing running" instead of one
     * handle, so the caller helps drain rather than watching workers do it. */
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

void JobSystem::SetActiveWorkerLimit(const uint32_t InLimit)
{
    const uint32_t Clamped = InLimit == 0 || InLimit > Pimpl->WorkerCount ? Pimpl->WorkerCount : InLimit;
    {
        std::lock_guard<std::mutex> Lock(Pimpl->ParkMutex);
        Pimpl->ActiveLimit.store(Clamped, std::memory_order_release);
    }
    Pimpl->ParkWake.notify_all();
}

uint32_t JobSystem::GetActiveWorkerCount() const
{
    return Pimpl->ActiveLimit.load(std::memory_order_acquire);
}

void JobSystem::SetIdleWaitMs(const uint32_t InMilliseconds)
{
    Pimpl->IdleWaitMs.store(InMilliseconds > 0 ? InMilliseconds : 1, std::memory_order_relaxed);
}

uint32_t JobSystem::GetIdleWaitMs() const
{
    return Pimpl->IdleWaitMs.load(std::memory_order_relaxed);
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
     * Two phases, and the order is forced: remove what is QUEUED first, then wait
     * out what is ALREADY RUNNING. The other way round lets a worker pull a
     * queued job from the dying image while we wait for a different one.
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
                    /* So nothing can call into the dying image even if the slot
                     * is somehow reached again before it recycles. */
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
        /* Loud, because a dropped job means somebody's result is now missing --
         * CoreModule.h's RELOAD HAZARDS, not fixable from here, but silence would
         * surface it as a rendering glitch three reloads later. */
        CORE_LOG_WARN(std::string("Jobs: image '") + Memory::GetImageName(InImage) +
                      "' is unloading; dropped " + std::to_string(Dropped) +
                      " queued job(s) that would have called into it. Anything waiting on "
                      "them has been released without their work being done.")
    }

    /* Waits out everything running, not just this image's -- the executing path
     * keeps no per-job image check. Stricter than needed, and correct. */
    WaitForIdle();

    return Dropped;
}

// === The locator ============================================================

namespace
{
    /* Non-owning, no locking, set once by the host -- ModuleManager's shape, and
     * constant-initialised into .bss for the reason CoreConfig.h:31 gives. */
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
