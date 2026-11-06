#ifndef ENGINECORE_COREJOB_H
#define ENGINECORE_COREJOB_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreThread.h"

/*
 * The job system: a fixed pool of named worker Threads, a queue of small
 * self-contained work items, and a handle you can wait on.
 *
 * This is the UPPER half of the concurrency subsystem. CoreThread.h is the
 * lower half and carries the IMAGE BARRIER and the MAIN-THREAD rules --
 * everything here assumes those, and none of it can be restated usefully from
 * this side.
 *
 * WHAT A CALLER SEES
 *
 *      JobHandle H = Jobs.Dispatch([Data]{ Chew(Data); });
 *      ... other work ...
 *      Jobs.Wait(H);
 *
 *      Jobs.ParallelFor(Vertices.Num(), 256,
 *                       [&](uint32_t Begin, uint32_t End){ Skin(Begin, End); });
 *
 * and that is nearly the whole surface. No dependency graph, no continuation, no
 * coroutine, no priority band -- absent for the reason CoreQueue.h gives for not
 * being lock-free: the work to schedule today is per-frame fan-out over arrays,
 * which a counter and a wait already serve.
 *
 * ===========================================================================
 * THE RULE THAT SHAPES EVERYTHING: A WAIT MUST WORK, NOT SLEEP
 * ===========================================================================
 *
 * Wait(H) does not block the calling thread. It runs OTHER JOBS until H is
 * complete, and only sleeps when the queue is empty.
 *
 * Not an optimization -- the difference between a job system and a deadlock. On
 * a 4-worker pool: job A dispatches B and blocks on it. Four A-shaped jobs are in
 * flight, all four workers blocked, B at the head of a queue nobody will drain
 * again. Nothing did anything wrong. A pool that blocks on Wait can only be used
 * by code that never nests, which is a rule nobody keeps.
 *
 * THE PRICE -- READ BEFORE WRITING A JOB. Inside Wait(), an unrelated job runs
 * ON YOUR STACK, above your frame:
 *
 *     * A JOB MUST NOT HOLD A LOCK ACROSS Wait(). The job running underneath you
 *       may want that same lock and cannot know you hold it. A self-deadlock
 *       with two unrelated stacks in it, and the likeliest way to misuse this.
 *
 *     * Anything thread_local read before a Wait() may have been written by the
 *       stranger that ran during it.
 *
 *     * Depth is bounded by the wait chain, not the job count -- hence
 *       MAX_WAIT_DEPTH.
 *
 * The alternative is fibers, which is how large engines solve it properly. It is
 * also stack-switching behind the app* seam, it breaks every debugger and
 * profiler until each is taught about it, and it makes thread_local a lie.
 *
 * ===========================================================================
 * THE COLLECTOR. THIS IS THE PART THAT WILL BITE.
 * ===========================================================================
 *
 * CoreGC.h:118 -- "THREADING: THE WORLD MUST BE STOPPED. No other thread may
 * construct, destroy, copy or assign a TRef while this runs. There is no read
 * barrier and no write barrier."
 *
 * A job pool is a machine for violating that sentence, and it presents as a
 * crash inside CollectGarbage with a stack unrelated to the cause.
 *
 * The existing guard is not enough: CollectGarbage checks that the CALLER is the
 * collector thread, which says nothing about the eight threads not calling it.
 * So:
 *
 *   1. The host collects at a frame boundary with the pool IDLE. WaitForIdle()
 *      gets there; IsQuiescent() lets the collector check rather than trust.
 *
 *   2. CoreGarbageCollector.cpp's thread check wants extending to refuse a
 *      non-quiescent JobSystem -- the shape of the refusal it already logs for a
 *      foreign thread, for a worse error. A prerequisite for collecting in a
 *      build that uses this, called out rather than done silently because it
 *      changes an existing refusal path.
 *
 *   3. A JOB MAY NOT CALL CollectGarbage -- the collector's re-entrancy
 *      CORE_CHECK will fire, and rightly.
 *
 * WHAT A JOB MAY TOUCH
 *
 *   Memory::Alloc / Dealloc ....... YES -- CoreMemoryPool.h:33 requires every
 *                                   pool to be safe from any thread.
 *   The pool REGISTRY ............. NO. Same note, opposite half: register at
 *                                   startup, main thread only.
 *   TUnique ....................... YES, if the job is the only owner.
 *   TRef copy / assign / destroy .. YES between collections (the count is
 *                                   atomic), NEVER while CollectGarbage runs.
 *   TWeakRef dereference .......... NO -- CorePointer.h:381, another thread's
 *                                   last TRef can drop between the test and the
 *                                   dereference. Promote on the main thread and
 *                                   hand the job the TRef.
 *   CORE_LOG / CORE_CHECK ......... ONLY IF THE HOST'S HANDLER IS THREAD-SAFE.
 *                                   CoreLog.h's seam says nothing about this,
 *                                   and a handler forwarding to a GUI logger is
 *                                   usually the least thread-safe object in the
 *                                   process. A host capability, not a given.
 *   ModuleManager, CoreConfig ..... NO, both single-threaded. DispatchOnMain.
 *   IFileProvider ................. DEPENDS ON THE PROVIDER; the Qt one is not.
 *                                   DispatchOnMain, or appFile* directly.
 *
 * ===========================================================================
 * WHY ONE QUEUE AND NOT PER-WORKER WORK STEALING
 * ===========================================================================
 *
 * Because CoreQueue.h's argument applies until it does not: "the contention
 * here is a few items per frame between two threads. A lock-free ring would be
 * more code, more subtle, and no faster at this volume."
 *
 * A job system can leave that volume behind in one call, though -- ParallelFor
 * over 100k vertices at a 256 grain is 390 trips through one mutex. So the shape
 * here keeps that an IMPLEMENTATION change: nothing public exposes a queue, a
 * worker index or an ordering guarantee, and stealing can be dropped in behind
 * Dispatch without a call site moving. What would justify it is a capture showing
 * time in the dispatch mutex, not the belief that real engines steal.
 *
 * NO ORDERING IS PROMISED, so that stays true. Two jobs dispatched in sequence
 * may run in either order or at once. If order matters, express it with Wait.
 */

namespace EngineCore
{
    /*
     * A reference to a dispatched job: INDEX PLUS GENERATION, and the generation
     * is the point. Slots live in a fixed ring and are reused, so a bare index
     * that outlives its job silently names a DIFFERENT one -- a Wait on it would
     * block on unrelated work or return instantly, depending on timing.
     *
     * Trivial and 8 bytes, so safe to pass and store. What is NOT safe is storing
     * one ACROSS FRAMES and expecting IsComplete to still mean anything. A stale
     * handle reads as complete, which is right for the common case (the job
     * finished long ago) and is why a mismatch logs rather than being fatal.
     */
    struct JobHandle
    {
        uint32_t Index      = 0;
        uint32_t Generation = 0;    // 0 is never a valid generation

        bool IsValid() const { return Generation != 0; }
    };

    /*
     * What a worker calls: a plain function pointer and the bytes it was given.
     *
     * WHY NOT std::function, WHICH Thread HAPPILY USES. First, counting: one that
     * does not fit its small-object buffer heap-allocates, so a frame dispatching
     * 4000 jobs makes 4000 allocations on the hot path. The inline payload is a
     * memcpy into a slot that already exists.
     *
     * Second, and this is the one that would have been found the hard way: a
     * std::function holding a lambda DEFINED IN A MODULE DLL stores an invoker
     * pointer into that image -- the hazard CoreThread.h's barrier exists for,
     * and worse for a queued job, which sits there doing nothing until a worker
     * picks it up milliseconds after the reload. A raw JobFn has the same problem
     * but it is visible in the type and one word wide.
     *
     * THE PAYLOAD IS memcpy'd AND NEVER DESTRUCTED -- hence the two
     * static_asserts on the templated Dispatch. A std::string capture does not
     * compile, which is correct: it would otherwise leak or be freed twice.
     */
    using JobFn = void (*)(void* InPayload);

    /*
     * Captures must fit. The number is arithmetic, not preference:
     *
     *      64  one cache line
     *    - 24  bookkeeping (function pointer 8, parent 4, generation 4,
     *          counter 4, image 2, two flags 2)
     *    ----
     *      40  left for the payload
     *
     * and a slot must be exactly one line, so that a worker claiming a job
     * touches one rather than two. CoreJob.cpp static_asserts sizeof(Job) == 64
     * and is what keeps this honest -- an earlier draft of this header said 48
     * here, which would have made the slot 72 bytes and, after alignment, 128.
     * If that assert ever fires, shrink this rather than letting the slot spill.
     *
     * 40 bytes is five pointers, or a pointer plus eight ints. A job needing
     * more should capture a POINTER to its state and own that elsewhere, which
     * is what a caller ends up doing anyway once the state is shared.
     */
    enum : size_t { JOB_PAYLOAD_CAPACITY = 40 };

    struct JobSystemStats
    {
        uint64_t Dispatched      = 0;    // lifetime totals, never decremented
        uint64_t Executed        = 0;
        uint64_t ExecutedInline  = 0;    // ran on the dispatcher: ring full
        uint64_t StolenByWaiters = 0;    // ran inside somebody's Wait()
        uint64_t WorkerSleeps    = 0;
        uint32_t WorkerCount     = 0;
        uint32_t PeakQueueDepth  = 0;
    };

    /*
     * The pool.
     *
     * A CLASS AND NOT A G-STATIC, for exactly the reason ModuleManager.h:20
     * gives: a seam with no owned state is right as a G-prefixed pointer behind
     * free functions, and this owns OS threads. A Meyers singleton would be
     * specifically wrong for the reason given there too -- its destructor runs
     * during static destruction, so it would join worker threads at an
     * unspecified point relative to the host's teardown, and the ordering of
     * "stop the workers" against "unload the modules whose code they are
     * running" is the entire hazard CoreThread.h's barrier exists to control.
     * The host must be able to write the destruction point in main().
     *
     * The locator pair at the bottom of this file still exists, for the same
     * reason ModuleManager's does.
     */
    class CORE_API JobSystem
    {
    public:
        /*
         * 0 means GetPhysicalCoreCount() - 1, at least one.
         *
         * PHYSICAL, NOT LOGICAL, and MINUS ONE: minus one because the main thread
         * is the one that waits, so a pool sized to every core makes the OS
         * preempt a worker to run the thread waiting for it; physical because
         * this work is compute-bound and a hyperthreaded part double-counts
         * execution resources. A host that knows better passes a number.
         *
         * Workers are named "Job/0", "Job/1", ..., which is what makes a profile
         * capture readable.
         */
        explicit JobSystem(uint32_t InWorkerCount = 0);

        /*
         * Signals every worker to stop, then joins them. Unstarted queued jobs
         * are DROPPED, and it logs how many.
         *
         * Dropping rather than draining: a queued job may have been dispatched by
         * something already torn down, so running it during shutdown is how a
         * clean exit becomes a crash. WaitForIdle() first is how a caller says
         * they want the opposite.
         */
        ~JobSystem();

        JobSystem(const JobSystem&)            = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        // === Dispatch =======================================================

        /*
         * The low-level form. InPayloadBytes bytes are copied into the job slot.
         *
         * InParent, if valid, makes this job a CHILD: the parent is not complete
         * until it and all its children are, so a Wait on the parent waits for
         * the whole tree. That single mechanism is what ParallelFor is built on
         * and is the only dependency primitive here -- fan-out and join, no
         * arbitrary graph.
         *
         * A CHILD OF A COMPLETED PARENT IS REFUSED with an error: the counter
         * already hit zero and its waiters were released, so the child would run
         * unobserved. WHICH IS WHY A PARENT IS ALMOST ALWAYS A GROUP -- using a
         * dispatched job works only if it cannot finish before the loop adding
         * its children does, and a worker can start it the instant it is queued.
         *
         * WHEN THE RING IS FULL IT RUNS THE JOB INLINE, ON THIS THREAD, counted
         * in ExecutedInline. It does not fail and does not grow: failing would
         * put error handling on every dispatch site for something the caller
         * cannot act on, and growing would let a runaway producer queue unbounded
         * work rather than be slowed by it. Running it here is the backpressure.
         *
         * THE HANDLE FROM THAT PATH IS INVALID -- the job finished before this
         * returned, so no slot names it. Wait returns immediately and IsComplete
         * reads true, both correct for work already done.
         *
         * DO NOT PASS InOwningImage. As with Thread::Start, the default argument
         * IS the mechanism: it is evaluated at the CALL SITE, so GImageID is read
         * in the caller's image. This body cannot read it -- it compiles into
         * EngineCore.dll, where GImageID is always IMAGE_Host -- and a job tagged
         * with the wrong image is one OnImageUnloading will not drop.
         * CoreMemory.h:251 states the original trap.
         */
        JobHandle Dispatch(JobFn       InFn,
                           const void* InPayload,
                           size_t      InPayloadBytes,
                           JobHandle   InParent       = {},
                           ImageID     InOwningImage  = GImageID);

        /*
         * The form call sites use. Generates the trampoline, copies the
         * callable into the slot.
         *
         * The two static_asserts are the JobFn contract made unignorable: the
         * payload is memcpy'd in and never destructed, so a capture that owns
         * anything is a leak and a capture that needs a copy constructor is
         * undefined behaviour. Capture PODs and pointers. If the diagnostic
         * below is what brought you here, the fix is to put the state somewhere
         * with a lifetime that outlives the wait -- usually the frame arena
         * (LinearMemoryPool) -- and capture a pointer to it.
         *
         * THIS ONE READS GImageID IN ITS BODY, which the out-of-line overload
         * above cannot. A template instantiates in the translation unit that
         * calls it, so this body compiles into the CALLER'S image and the read
         * is correct there. Same reasoning, opposite conclusion, purely because
         * one is inline and the other is not -- which is worth knowing before
         * "tidying" either of them.
         */
        template<typename TWork>
        JobHandle Dispatch(const TWork& InWork, JobHandle InParent = {})
        {
            static_assert(sizeof(TWork) <= JOB_PAYLOAD_CAPACITY,
                          "Job captures too much state. Capture a pointer to it instead "
                          "(see JOB_PAYLOAD_CAPACITY).");
            static_assert(std::is_trivially_copyable<TWork>::value
                       && std::is_trivially_destructible<TWork>::value,
                          "A job payload is memcpy'd and never destructed, so its captures "
                          "must be trivial. Capturing a std::string, a vector or a TRef by "
                          "value would leak it.");

            return Dispatch(&Trampoline<TWork>, &InWork, sizeof(TWork), InParent, GImageID);
        }

        /*
         * A parent that holds no work and is held OPEN until you close it.
         *
         * THE RACE THIS MAKES UNWRITABLE. The natural fan-out is to dispatch an
         * empty job and parent the chunks to it -- and it is wrong in the way
         * that survives testing: a worker can claim that empty job the instant it
         * is queued and drive the counter to zero WHILE THE LOOP IS STILL ADDING
         * CHILDREN, releasing the waiter early and orphaning the rest. Never on a
         * busy pool; constantly on an idle one with a slow first chunk.
         *
         * A group's count starts at one and that one is YOURS, so it cannot reach
         * zero until CloseGroup. It is never queued and never executes -- just a
         * counter in a slot, so a JobHandle can name it.
         *
         * IT CAN FAIL, unlike Dispatch: a group has no work, so there is nothing
         * to run inline instead. An exhausted ring returns an INVALID handle, and
         * the caller must fall back to doing the fan-out serially, as ParallelFor
         * does.
         */
        JobHandle OpenGroup();

        /*
         * Releases the dispatcher's reference. The group completes when it and
         * its children are done -- which may be immediately, inside this call,
         * if every child has already finished.
         *
         * EVERY OpenGroup NEEDS EXACTLY ONE. Left open, it is a Wait that never
         * returns and a slot never recycled; closed twice, it releases a
         * reference that is not yours and completes out from under its children.
         * Both are refused rather than corrupting the counter, and both mean the
         * fan-out has an early return in it.
         */
        void CloseGroup(JobHandle InGroup);

        /*
         * Splits [0, InCount) into chunks of at least InGrain and dispatches
         * one job per chunk, then waits for all of them. InBody is called as
         * InBody(Begin, End) with End exclusive.
         *
         * SYNCHRONOUS, because an async version needs InBody to outlive the call
         * -- a lifetime trap in a convenience function. Async fan-out is Dispatch
         * in a loop against one group, which is four lines that say so.
         *
         * InGrain IS NOT A DETAIL: too small and dispatch cost dominates the work
         * in each chunk -- a grain of 1 over 100k elements is 100k trips through
         * the queue for nanoseconds of work each, far slower than the serial
         * loop. Aim for tens of microseconds per chunk. Only the caller knows
         * what one element costs, so there is no automatic choice here.
         *
         * Runs the body DIRECTLY when InCount <= InGrain. The serial path is the
         * right answer more often than the parallel one.
         */
        template<typename TBody>
        void ParallelFor(uint32_t InCount, uint32_t InGrain, const TBody& InBody);

        /*
         * Queues work for the MAIN thread, drained by TickMainThread().
         *
         * For the table at the top of this file: a worker that finds it needs
         * ModuleManager, the config registry or a Qt file provider cannot touch
         * any of them, and handing the work back is the only correct move.
         *
         * The handle completes when the main thread runs it, so a worker CAN Wait
         * on it -- and should think first: if the main thread is itself inside
         * Wait() on that worker's job, that is a deadlock this cannot break.
         * Fire and forget is almost always what is wanted.
         */
        JobHandle DispatchOnMain(JobFn       InFn,
                                 const void* InPayload,
                                 size_t      InPayloadBytes,
                                 ImageID     InOwningImage = GImageID);

        /*
         * Runs main-thread jobs, up to InMaxJobs (0 = all queued now), and
         * returns how many. Call it once per frame near ModuleManager::Tick,
         * which is the company this work keeps.
         *
         * The cap stops a backlog swallowing a frame: a thousand queued jobs
         * should cost several frames of hitching, not one enormous one.
         */
        uint32_t TickMainThread(uint32_t InMaxJobs = 0);

        // === Waiting ========================================================

        /*
         * Runs other jobs until InHandle and all its children are complete.
         * Read the reentrancy block at the top of this file before calling it
         * from inside a job -- especially the part about locks.
         *
         * A stale handle (recycled generation) returns immediately and logs.
         * An invalid handle returns immediately and does not.
         *
         * WAITING ON AN OPEN GROUP IS FATAL, not a wait. The group is holding a
         * reference that only this thread can release, so the wait can never
         * end -- and a hang with no message is the worst thing a scheduler can
         * hand a caller. CORE_CHECK names the missing CloseGroup instead.
         */
        void Wait(JobHandle InHandle);

        /* Non-blocking test. Stale handles read as complete -- see JobHandle. */
        bool IsComplete(JobHandle InHandle) const;

        /*
         * Runs jobs until the queue is empty and nothing is executing.
         *
         * THE ONLY SAFE POINT TO COLLECT GARBAGE, which is why it is public and
         * not a detail of the destructor: frame ends, WaitForIdle(),
         * CollectGarbage(), next frame.
         *
         * Racy if anything else is still dispatching -- it promises only that the
         * pool was idle when it returned. Enough for the frame boundary, and why
         * the collector should check IsQuiescent rather than trust this was
         * called.
         */
        void WaitForIdle();

        /* True when nothing is queued and no job is executing. For the
         * collector's refusal check (see this file's header block) and for
         * assertions -- not a synchronization primitive. */
        bool IsQuiescent() const;

        // === Introspection ==================================================

        uint32_t GetWorkerCount() const;

        /*
         * How many workers take jobs; the rest PARK -- asleep on a condition, not
         * polling -- until the limit rises. 0 is every worker. The pool's threads
         * are made once and never change, so this is how a power profile trims
         * occupancy live: parked workers cost nothing, and a raised limit wakes
         * them at once. Never drops work -- the active workers, and every Wait,
         * still drain the queue. Clamped to [1, GetWorkerCount()].
         */
        void     SetActiveWorkerLimit(uint32_t InLimit);
        uint32_t GetActiveWorkerCount() const;

        /* How long an idle worker sleeps before it looks at the queue again when
         * no wake-up reaches it (milliseconds, at least 1; default 2). Longer is
         * fewer wake-ups on an idle pool -- see WorkerMain for why there is a
         * timeout at all. */
        void     SetIdleWaitMs(uint32_t InMilliseconds);
        uint32_t GetIdleWaitMs() const;

        /* True on a thread owned by this pool. Lets a subsystem refuse work it
         * cannot do off the main thread without having to be told where it is
         * running. */
        bool IsWorkerThread() const;

        void GetStats(JobSystemStats& OutStats) const;

        // === The image barrier ==============================================

        /*
         * Drops queued jobs TAGGED WITH InImage, then waits for any already
         * running. Returns how many were dropped.
         *
         * A TAG, NOT THE FUNCTION POINTER. Testing whether JobFn lands in the
         * image's mapped range means GetModuleHandleExW(FROM_ADDRESS) or dladdr
         * -- an OS mechanism needing a new app* seam on every backend, for
         * information the dispatcher already had. Each job records who dispatched
         * it instead, exactly as Thread does.
         *
         * ModuleManager::TeardownInstance calls this FIRST, before
         * Threads::OnImageUnloading, Memory::OnImageUnloading and appDLLClose.
         * Forced: a queued job in a doomed image is a function pointer into
         * memory about to be unmapped.
         *
         * ANYTHING WAITING ON A DROPPED JOB IS RELEASED and the job marked
         * complete without running -- leaving the waiter blocked would hang the
         * unload. Every drop is logged, because it means a module was reloaded
         * with work outstanding and somebody's result is now missing:
         * CoreModule.h's RELOAD HAZARDS, not fixable from here.
         *
         * A HOST job that merely touches module data is invisible to this under
         * either scheme; that one belongs to the module contract.
         */
        uint32_t OnImageUnloading(ImageID InImage);

        /* Wait chain depth cap. Fatal rather than a stack overflow -- a message
         * naming this constant beats a corrupted stack in a worker. */
        enum : uint32_t { MAX_WAIT_DEPTH = 32 };

        /* How long Wait yields, with nothing of its own to run, before it sleeps
         * (a millisecond at least): about one sleep's worth, so a wait that ends
         * within it costs no more than the sleep would have. */
        enum : uint32_t { WAIT_YIELD_US = 1000 };

        /* Ring size: fixed, allocated once, a power of two so the wrap is a mask.
         * Dispatch past it runs inline. */
        enum : uint32_t { MAX_PENDING_JOBS = 4096 };

    private:
        /* Instantiated in the image that called Dispatch, so its ADDRESS points
         * there -- which is why a queued job outliving its image is a pointer
         * into unmapped memory. OnImageUnloading removes that hazard, but
         * identifies the job by its ImageID tag, not by this address. */
        template<typename TWork>
        static void Trampoline(void* InPayload)
        {
            (*static_cast<TWork*>(InPayload))();
        }

        // === Internals ======================================================
        // Here only because C++ requires it. Each notes whether the pool's mutex
        // must be held -- the most dangerous thing to get wrong, since
        // CompleteSlot takes the lock to recycle and a non-recursive mutex makes
        // calling it from a locked region a self-deadlock.

        void     WorkerMain(uint32_t InIndex);

        /* Runs one ready job; false if the queue was empty. The ONLY place a job
         * executes -- workers and Wait both come through here. Mutex NOT held. */
        bool     TryRunOneJob();

        uint32_t AcquireSlot();                             // mutex HELD
        void     ReleaseSlot(uint32_t InSlot);              // mutex HELD
        void     CompleteSlot(uint32_t InSlot);             // mutex NOT held
        bool     TryAdopt(uint32_t InParentSlot,
                          uint32_t InExpectedGeneration);   // mutex NOT held
        uint32_t ResolveHandle(JobHandle InHandle) const;   // mutex NOT held

        /* Slot, ring, queues and workers all live in here. Keeping the SLOT out
         * of the header as well as the queue is what leaves both the 64-byte
         * layout and the queue topology free to change -- which is what keeps
         * work stealing a non-breaking change. */
        struct Impl;
        Impl* Pimpl = nullptr;
    };

    /*
     * The locator pair ModuleManager and IFileProvider both have: non-owning, no
     * locking, set once by the host so a subsystem can reach "the" pool without
     * being handed one through five constructors.
     *
     * SET IT BEFORE ANY MODULE LOADS, CLEAR IT BEFORE THE POOL DIES -- a dangling
     * pointer here is a dispatch into a destroyed thread pool.
     */
    CORE_API void       SetJobSystem(JobSystem* InSystem);
    CORE_API JobSystem* GetJobSystem();

    // === ParallelFor ========================================================

    template<typename TBody>
    void JobSystem::ParallelFor(uint32_t InCount, uint32_t InGrain, const TBody& InBody)
    {
        if(InCount == 0)
        {
            return;
        }

        if(InGrain == 0)
        {
            InGrain = 1;
        }

        /* The serial path. Not a fallback -- the right answer whenever the work
         * does not fill a second chunk. */
        if(InCount <= InGrain)
        {
            InBody(uint32_t(0), InCount);
            return;
        }

        /* The descriptor that lands in the payload, capturing the BODY BY
         * POINTER. Safe here and only here, because this waits before returning
         * -- and exactly why there is no async ParallelFor: the same code without
         * the wait leaves these pointers aimed at a dead frame. */
        struct Chunk
        {
            const TBody* Body;
            uint32_t     Begin;
            uint32_t     End;

            void operator()() const { (*Body)(Begin, End); }
        };

        static_assert(sizeof(Chunk) <= JOB_PAYLOAD_CAPACITY,
                      "ParallelFor chunk descriptor outgrew the job payload.");

        /* A GROUP and not an empty job -- OpenGroup's comment has the race in
         * full. The group cannot complete while this loop is still running,
         * because the reference released by CloseGroup below is held here. */
        const JobHandle Group = OpenGroup();

        /* An exhausted ring has no group to give and nothing to run inline in its
         * place. Serial is slower and completely correct. */
        if(!Group.IsValid())
        {
            InBody(uint32_t(0), InCount);
            return;
        }

        for(uint32_t Begin = 0; Begin < InCount; Begin += InGrain)
        {
            const uint32_t End = (Begin + InGrain < InCount) ? Begin + InGrain : InCount;

            Chunk Work{ &InBody, Begin, End };
            Dispatch(Work, Group);
        }

        /* Close before waiting, with no return path between the two: Wait on an
         * open group is a guaranteed hang. */
        CloseGroup(Group);
        Wait(Group);
    }
}

#endif //ENGINECORE_COREJOB_H
