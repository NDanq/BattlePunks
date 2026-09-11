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
 * and that is nearly the whole surface. There is no dependency graph, no
 * continuation, no coroutine, no priority band. Each of those is a real feature
 * of a mature scheduler and each is absent for the same reason CoreQueue.h
 * gives for not being lock-free: the work this has to schedule right now is
 * per-frame fan-out over arrays, that shape is served by a counter and a wait,
 * and the missing features cost correctness to add before there is a call site
 * that needs them.
 *
 * ===========================================================================
 * THE RULE THAT SHAPES EVERYTHING: A WAIT MUST WORK, NOT SLEEP
 * ===========================================================================
 *
 * Wait(H) does not block the calling thread. It runs OTHER JOBS until H is
 * complete, and only sleeps when the queue is empty.
 *
 * That is not an optimization, it is the difference between a job system and a
 * deadlock. Consider the obvious implementation on a 4-worker pool: job A
 * dispatches B and blocks on it. Four A-shaped jobs are in flight, all four
 * workers are blocked, B is at the head of a queue nobody will ever drain
 * again. The pool is dead and no individual piece of code did anything wrong.
 * A pool that blocks on Wait can only ever be used by code that never nests,
 * which is a rule nobody can keep.
 *
 * THE PRICE, AND IT IS A REAL ONE -- READ BEFORE WRITING A JOB:
 *
 *   REENTRANCY. Inside Wait(), an unrelated job runs ON YOUR STACK, above your
 *   frame. So:
 *
 *     * A JOB MUST NOT HOLD A LOCK ACROSS Wait(). The job that runs underneath
 *       you may want that same lock, and it cannot possibly know you hold it.
 *       This is a self-deadlock with two unrelated call stacks in it, and it is
 *       the single most likely way to misuse this API.
 *
 *     * A job must not assume it resumes on the thread that started it. It
 *       does, in fact -- jobs are never migrated mid-execution -- but anything
 *       thread_local you read before a Wait() may have been written by the
 *       stranger that ran during it.
 *
 *     * Recursion depth is bounded by the wait chain, not by the job count. A
 *       job that waits on a job that waits on a job nests three stack frames
 *       deep plus whatever those jobs are. Deep fan-in trees want an explicit
 *       depth cap, which is what MAX_WAIT_DEPTH below is.
 *
 *   The alternative -- fibers, so a waiting job can be parked and its stack set
 *   aside -- is how the large engines solve this properly. It is also a
 *   platform-specific stack-switching mechanism behind the app* seam, it breaks
 *   every debugger and profiler until each is taught about it, and it makes
 *   thread_local a lie. Not now, and not without a profile that demands it.
 *
 * ===========================================================================
 * THE COLLECTOR. THIS IS THE PART THAT WILL BITE.
 * ===========================================================================
 *
 * CoreGC.h:118 -- "THREADING: THE WORLD MUST BE STOPPED. No other thread may
 * construct, destroy, copy or assign a TRef while this runs. There is no read
 * barrier and no write barrier."
 *
 * A job pool is a machine for violating that sentence. One worker copying a
 * TRef while the collector sweeps is a corrupted object list, and it will
 * present as a crash inside CollectGarbage with a stack that has nothing to do
 * with the cause.
 *
 * The existing guard is not enough on its own: CollectGarbage checks that the
 * CALLER is the collector thread, which says nothing about the eight other
 * threads that are not calling it. So the contract is:
 *
 *   1. The host calls CollectGarbage at a frame boundary with the pool IDLE.
 *      WaitForIdle() is how you get there, and IsQuiescent() is how the
 *      collector can check rather than trust.
 *
 *   2. CoreGarbageCollector.cpp's existing thread check should be extended to
 *      refuse when a registered JobSystem reports non-quiescent -- the same
 *      shape as the refusal it already logs for a foreign thread, for a
 *      strictly worse error. That edit is a prerequisite for using this header
 *      in a build that also collects; it is called out here rather than done
 *      silently because it changes an existing refusal path.
 *
 *   3. A JOB MAY NOT CALL CollectGarbage. Not "should not" -- the re-entrancy
 *      CORE_CHECK in the collector will fire, and it is right to.
 *
 * WHAT A JOB MAY TOUCH
 *
 *   Memory::Alloc / Dealloc ....... YES. CoreMemoryPool.h:33 -- "Allocate and
 *                                   Deallocate on any pool must be safe from
 *                                   any thread; a pool that is not is a bug in
 *                                   that pool."
 *   The pool REGISTRY ............. NO. Same note, opposite half: register
 *                                   every pool at startup. Main thread only.
 *   TUnique ....................... YES, if the job is the only owner.
 *   TRef copy / assign / destroy .. YES between collections (the count is
 *                                   atomic, CoreMemory.cpp:470), NEVER while
 *                                   CollectGarbage runs -- see above.
 *   TWeakRef dereference .......... NO. CorePointer.h:381 is explicit that
 *                                   another thread's last TRef can drop between
 *                                   the validity test and the dereference.
 *                                   Promote on the main thread and hand the job
 *                                   the TRef.
 *   CORE_LOG / CORE_CHECK ......... ONLY IF THE HOST'S HANDLER IS THREAD-SAFE.
 *                                   CoreLog.h's seam is a bare function
 *                                   pointer and says nothing about this; a
 *                                   handler forwarding to a GUI logger is
 *                                   usually the least thread-safe object in the
 *                                   process. Treat logging from a worker as a
 *                                   host capability, not a given.
 *   ModuleManager, CoreConfig ..... NO. Both documented single-threaded. Use
 *                                   DispatchOnMain.
 *   IFileProvider ................. DEPENDS ON THE PROVIDER, and the Qt one is
 *                                   not. DispatchOnMain, or read through
 *                                   appFile* directly, which is.
 *
 * ===========================================================================
 * WHY ONE QUEUE AND NOT PER-WORKER WORK STEALING
 * ===========================================================================
 *
 * Because CoreQueue.h's argument applies until it does not: "the contention
 * here is a few items per frame between two threads. A lock-free ring would be
 * more code, more subtle, and no faster at this volume."
 *
 * The honest difference is that a job system can leave that volume behind in a
 * single call -- ParallelFor over 100k vertices at a 256 grain is 390 pushes
 * and 390 pops through one mutex, and eight workers hammering it will show up
 * in a profile. So the shape here is deliberately chosen to make that an
 * IMPLEMENTATION change: the queue topology is not in this header. Nothing
 * public exposes a queue, a worker index, or an ordering guarantee, so
 * per-worker deques with stealing can be dropped in behind Dispatch without a
 * call site moving.
 *
 * WHAT WOULD JUSTIFY IT: a capture showing measurable time in the dispatch
 * mutex, not the belief that stealing is what a real engine does.
 *
 * NO ORDERING IS PROMISED, precisely so that stays true. Jobs are not FIFO,
 * not LIFO, and two jobs dispatched in sequence may run in either order or at
 * the same time. If order matters, express it with Wait.
 */

namespace EngineCore
{
    /*
     * A reference to a dispatched job.
     *
     * INDEX PLUS GENERATION, and the generation is the entire point. Job slots
     * live in a fixed ring and are reused, so a bare index that outlives its
     * job silently names a DIFFERENT job -- and a Wait on it would block on
     * unrelated work, or return instantly, depending on timing. The generation
     * makes a stale handle detectable, so Wait can log and return rather than
     * do something arbitrary.
     *
     * COPYABLE, TRIVIAL, AND 8 BYTES. A handle is passed by value everywhere
     * and is safe to store; what is NOT safe is storing one across frames and
     * expecting IsComplete to still mean anything, because the slot will have
     * been recycled. A stale handle reads as complete -- which is the right
     * answer for the common case (the job finished long ago) and is why the
     * generation mismatch is a log, not a fatal.
     */
    struct JobHandle
    {
        uint32_t Index      = 0;
        uint32_t Generation = 0;    // 0 is never a valid generation

        bool IsValid() const { return Generation != 0; }
    };

    /*
     * What a worker actually calls: a plain function pointer and the bytes it
     * was given.
     *
     * WHY NOT std::function, WHICH CoreThread.h's Thread HAPPILY USES
     *
     * Two reasons, and the first is just counting. A std::function that does not
     * fit its small-object buffer heap-allocates on construction, so a frame
     * that dispatches 4000 jobs makes 4000 allocations and 4000 frees on the
     * hot path, at which point the scheduler is an allocator benchmark. The
     * inline payload below is a memcpy into a slot that already exists.
     *
     * The second reason is the one that would have been found the hard way. A
     * std::function holding a lambda DEFINED IN A MODULE DLL stores a pointer to
     * an invoker compiled into that image -- the same hazard as the Destruct
     * pointer CoreMemory.h:76 describes, and CoreThread.h's image barrier
     * exists for. A queued job is worse than a live object, because it sits
     * there being nothing until a worker picks it up some milliseconds after the
     * reload. A raw JobFn has exactly the same problem, but it is VISIBLE in the
     * type, one word wide, and the thing the image drain compares against.
     *
     * THE PAYLOAD IS memcpy'd AND NEVER DESTRUCTED. Hence the two
     * static_asserts on the templated Dispatch below. A lambda capturing a
     * std::string does not compile, which is the correct outcome: it would
     * otherwise leak, or be freed twice.
     */
    using JobFn = void (*)(void* InPayload);

    /* Captures must fit. 48 bytes is six pointers, or a pointer plus four
     * ints and room to spare, and is the same reasoning as the allocation
     * header in CoreMemory.h: a size chosen so the whole slot stays in one
     * cache line alongside the counter and the generation. A job that needs
     * more state should capture a POINTER to it and own that elsewhere -- which
     * is what a caller does anyway once the state is shared between jobs. */
    enum : size_t { JOB_PAYLOAD_CAPACITY = 48 };

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
         * InWorkerCount of 0 means GetPhysicalCoreCount() - 1, clamped to at
         * least one worker.
         *
         * PHYSICAL, NOT LOGICAL, and MINUS ONE. Minus one because the main
         * thread is the one that waits, and a pool sized to every core makes
         * the OS preempt a worker to run the thread that is waiting for it.
         * Physical because the work this pool is for is compute-bound, and on a
         * hyperthreaded part the logical count double-counts execution
         * resources -- see appThreadHardwareCount's note. A host that knows
         * better passes a number.
         *
         * Workers are named "Job/0", "Job/1", ... via appThreadSetName, which
         * is what makes a profile capture readable and is half the reason
         * Thread exists.
         */
        explicit JobSystem(uint32_t InWorkerCount = 0);

        /*
         * Signals every worker to stop, then joins them. Queued jobs that have
         * not started are DROPPED, and it logs how many.
         *
         * Dropping rather than draining is deliberate: a queued job may have
         * been dispatched by something already torn down, so running it during
         * shutdown is how a clean exit becomes a crash in the last millisecond
         * of the process. A caller who wants everything finished calls
         * WaitForIdle() first, which is a visible statement of that intent.
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
         * DISPATCHING A CHILD OF A COMPLETED PARENT IS A BUG and is refused
         * with an error: the parent's counter already hit zero and anything
         * waiting on it has already been released, so the child would run
         * unobserved.
         *
         * WHICH IS WHY A PARENT IS ALMOST ALWAYS A GROUP, NOT A JOB. Using a
         * dispatched job as a parent only works if it cannot possibly finish
         * before the loop that adds its children does -- and a worker can start
         * it the instant it is queued. OpenGroup/CloseGroup below exist to make
         * that race unwritable.
         *
         * WHEN THE RING IS FULL IT RUNS THE JOB INLINE, ON THIS THREAD, and
         * counts it in ExecutedInline. It does not fail, and it does not grow.
         * Failing would put error handling on every dispatch site for a
         * condition a caller cannot do anything about; growing would allocate
         * on the hot path and, worse, would let a runaway producer queue
         * unbounded work instead of being slowed down by it. Running it here is
         * the backpressure. A non-zero ExecutedInline in a profile means the
         * ring is too small or a producer is pathological, and both are worth
         * seeing.
         */
        JobHandle Dispatch(JobFn       InFn,
                           const void* InPayload,
                           size_t      InPayloadBytes,
                           JobHandle   InParent = {});

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

            return Dispatch(&Trampoline<TWork>, &InWork, sizeof(TWork), InParent);
        }

        /*
         * A parent that holds no work and is held OPEN until you close it.
         *
         * THE RACE THIS EXISTS TO MAKE UNWRITABLE. The natural way to fan out
         * is to dispatch an empty job and parent the chunks to it. It is also
         * wrong, and wrong in the way that survives testing: a worker can claim
         * that empty job the instant it is queued, run its nothing, and drive
         * the counter to zero WHILE THE DISPATCH LOOP IS STILL ADDING CHILDREN.
         * The waiter is released early and the remaining children are refused
         * as children of a completed parent. On a busy pool it never happens;
         * on an idle one with a slow first chunk it happens constantly.
         *
         * A group's counter starts at one and that one is YOURS. Children can
         * come and go and it cannot reach zero, because the reference the
         * dispatcher holds is not released until CloseGroup.
         *
         * A GROUP IS NEVER QUEUED and never executes. It is a counter with a
         * generation on it, occupying a job slot so that a JobHandle can name
         * it.
         */
        JobHandle OpenGroup();

        /*
         * Releases the dispatcher's reference. The group completes when it and
         * its children are done -- which may be immediately, inside this call,
         * if every child has already finished.
         *
         * EVERY OpenGroup NEEDS EXACTLY ONE OF THESE. A group left open is a
         * Wait that never returns and a job slot that is never recycled;
         * closing one twice releases a reference that is not yours and
         * completes the group out from under its children. Both are refused
         * with an error rather than corrupting the counter, and both mean the
         * fan-out was written with an early return in it.
         */
        void CloseGroup(JobHandle InGroup);

        /*
         * Splits [0, InCount) into chunks of at least InGrain and dispatches
         * one job per chunk, then waits for all of them. InBody is called as
         * InBody(Begin, End) with End exclusive.
         *
         * SYNCHRONOUS, i.e. it waits before returning, because the asynchronous
         * version needs InBody to outlive the call and that is a lifetime trap
         * in a convenience function. An asynchronous fan-out is Dispatch in a
         * loop against one parent handle, which is four lines and says what it
         * is doing.
         *
         * InGrain IS NOT A DETAIL. Too small and the dispatch cost per chunk
         * dominates the work in it -- a grain of 1 over 100k elements is 100k
         * trips through the queue to do nanoseconds of work each, and will be
         * slower than the serial loop by a wide margin. Start where a chunk is
         * some tens of microseconds. There is no automatic choice available
         * here: only the caller knows what one element costs.
         *
         * Runs the body DIRECTLY, with no jobs at all, when InCount <= InGrain.
         * The serial path is not a special case to be embarrassed about -- it
         * is the right answer more often than the parallel one.
         */
        template<typename TBody>
        void ParallelFor(uint32_t InCount, uint32_t InGrain, const TBody& InBody);

        /*
         * Queues work for the MAIN thread, drained by TickMainThread().
         *
         * This exists because of the table at the top of this file: a worker
         * that discovers it needs ModuleManager, the config registry, or a Qt
         * file provider cannot touch any of them. Handing the work back is the
         * only correct move, and a hand-rolled ThreadSafeQueue per subsystem to
         * do it is what this replaces.
         *
         * Returns a JobHandle that completes when the main thread runs it -- so
         * a worker CAN Wait on it, and must think hard before doing so: if the
         * main thread is itself inside Wait() on that worker's job, that is a
         * deadlock this system cannot break. Fire and forget is almost always
         * what is wanted.
         */
        JobHandle DispatchOnMain(JobFn InFn, const void* InPayload, size_t InPayloadBytes);

        /*
         * Runs main-thread jobs, up to InMaxJobs of them (0 = all queued now).
         * CORE_CHECK_MAIN_THREAD. Call it once per frame, near where
         * ModuleManager::Tick is called, since that is the company this work
         * keeps. Returns how many it ran.
         *
         * The cap exists so a frame cannot be swallowed by a backlog: a
         * thousand queued main-thread jobs should cost several frames of
         * hitching, not one enormous one.
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
         * THE ONLY SAFE POINT TO COLLECT GARBAGE, and the reason this is public
         * rather than an implementation detail of the destructor. A host's frame
         * ends: WaitForIdle(), then CollectGarbage(), then start the next frame.
         *
         * Racy by nature if anything else is still dispatching -- it can only
         * promise that the pool was idle at the instant it returned. Which is
         * sufficient for the frame-boundary use and is why the collector should
         * check IsQuiescent itself rather than trusting that this was called.
         */
        void WaitForIdle();

        /* True when nothing is queued and no job is executing. For the
         * collector's refusal check (see this file's header block) and for
         * assertions -- not a synchronization primitive. */
        bool IsQuiescent() const;

        // === Introspection ==================================================

        uint32_t GetWorkerCount() const;

        /* True on a thread owned by this pool. Lets a subsystem refuse work it
         * cannot do off the main thread without having to be told where it is
         * running. */
        bool IsWorkerThread() const;

        void GetStats(JobSystemStats& OutStats) const;

        // === The image barrier ==============================================

        /*
         * Drops queued jobs whose JobFn points into InImage, then waits for any
         * that are already running. Returns how many were dropped.
         *
         * ModuleManager::TeardownInstance calls this FIRST -- before
         * Threads::OnImageUnloading, before Memory::OnImageUnloading, before
         * appDLLClose. The ordering is forced: a queued job in a doomed image is
         * a function pointer into memory about to be unmapped, and the whole
         * point is to remove it before anything can pick it up.
         *
         * ANYTHING WAITING ON A DROPPED JOB IS RELEASED, and the job is marked
         * complete without having run. Leaving a waiter blocked forever on work
         * that can never run is not better, and it hangs the unload. This logs
         * every drop, because a dropped job means a module was reloaded with
         * work outstanding and somebody's result is now silently missing --
         * which is CoreModule.h's RELOAD HAZARDS list, not something this can
         * fix from here.
         *
         * A job dispatched by the host that merely TOUCHES module data is
         * invisible to this: the function pointer is in the host image. That
         * hazard belongs to the module contract and cannot be detected here.
         */
        uint32_t OnImageUnloading(ImageID InImage);

        /* Wait chain depth cap. Exceeding it is fatal rather than a stack
         * overflow, which is the difference between a message naming this
         * constant and a corrupted stack in a worker thread. */
        enum : uint32_t { MAX_WAIT_DEPTH = 32 };

        /* Job slots in the ring. Fixed, allocated once in the constructor -- a
         * power of two so the index wrap is a mask. Dispatch past it runs
         * inline; see Dispatch. */
        enum : uint32_t { MAX_PENDING_JOBS = 4096 };

    private:
        /*
         * The generated JobFn. Its ADDRESS is what gets stored in the slot, and
         * it is instantiated in the image that called Dispatch -- which is
         * precisely what makes OnImageUnloading able to recognise a module's
         * jobs, and precisely why a module's queued job is dangerous after
         * unload. Both halves of that follow from this one line.
         */
        template<typename TWork>
        static void Trampoline(void* InPayload)
        {
            (*static_cast<TWork*>(InPayload))();
        }

        /*
         * One slot. 64 bytes, and the layout is the reason for
         * JOB_PAYLOAD_CAPACITY being 48 rather than a rounder number: the
         * payload, the function pointer, the atomic counter and the bookkeeping
         * share one cache line, so a worker claiming a job touches one line
         * rather than two. Same argument as the 48-byte allocation header in
         * CoreMemory.h, and it is the only reason the capacity is not 64.
         *
         * PendingCount is atomic and counts this job plus its live children;
         * the slot is complete at zero. Parent is an index, not a JobHandle,
         * because a slot never outlives its own generation.
         */
        struct Job;

        struct Impl;
        Impl* Pimpl = nullptr;   /* Hides the ring, the queue and the worker
                                  * vector, so the queue TOPOLOGY is not part of
                                  * this header -- which is what keeps work
                                  * stealing a non-breaking change. */
    };

    /*
     * The locator pair, exactly as ModuleManager and IFileProvider have one:
     * non-owning raw pointer, no locking, set once by the host at startup so
     * that a subsystem can reach "the" pool without being handed one through
     * five layers of constructor.
     *
     * SET IT BEFORE ANY MODULE LOADS AND CLEAR IT BEFORE THE POOL DIES. A
     * dangling pointer here is a dispatch into a destroyed thread pool. Same
     * contract, same hazard, same reason it is still worth having.
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

        /*
         * The chunk descriptor is what actually lands in the job payload, and it
         * captures the BODY BY POINTER. That is safe here and only here: this
         * function waits before returning, so InBody outlives every job it
         * dispatched. It is also exactly why there is no asynchronous
         * ParallelFor -- the same code with the wait removed would leave these
         * pointers aimed at a dead frame.
         */
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

        for(uint32_t Begin = 0; Begin < InCount; Begin += InGrain)
        {
            const uint32_t End = (Begin + InGrain < InCount) ? Begin + InGrain : InCount;

            Chunk Work{ &InBody, Begin, End };
            Dispatch(Work, Group);
        }

        /* Close before waiting, always, and with no return path between the
         * two: Wait on an open group is a guaranteed hang, so it is fatal
         * rather than silent (see Wait). */
        CloseGroup(Group);
        Wait(Group);
    }
}

#endif //ENGINECORE_COREJOB_H
