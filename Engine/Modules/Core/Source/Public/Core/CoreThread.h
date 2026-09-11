#ifndef ENGINECORE_CORETHREAD_H
#define ENGINECORE_CORETHREAD_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "Public/Core/CoreAPI.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"

/* For ImageID and GImageID. CoreModule.h includes this header for the same
 * reason and says so; the image barrier at the bottom of this file is the
 * thread-shaped twin of CoreGC.h's. */
#include "Public/Core/CoreMemory.h"

/*
 * Thread primitives: an owned, named, joinable worker, the counting semaphore
 * C++17 does not have, and the thread-affinity rules the rest of Core depends on
 * but has never been able to check.
 *
 * The LOWER half of the concurrency subsystem -- it knows nothing about work or
 * scheduling. CoreJob.h sits on top, and they read in that order the way
 * CoreMemoryPool.h reads before CoreMemory.h.
 *
 * NOT A WRAPPER OVER std::thread. CorePlatform.h's rule is that portable std
 * primitives stay as they are, and creation, joining and thread_local are ported
 * correctly by three standard libraries already. What std::thread CANNOT express
 * is what a debugger and a profiler need -- name, affinity, priority, real core
 * count -- and those four app* functions are all this sits on.
 *
 * WHY A Thread CLASS, THEN. Three reasons, each a rule the codebase already
 * states somewhere and could not enforce:
 *
 *   1. NAMING IS NOT OPTIONAL. appThreadSetName must be the first statement the
 *      new thread runs, and a caller told to remember that will forget. The
 *      trampoline does it, so an unnamed worker is unreachable through this API.
 *
 *   2. THE IMAGE BARRIER. CoreModule.h:37 -- "Nothing a module allocates
 *      outlives the module." A std::thread running a lambda compiled into a
 *      module DLL is a worse version of that hazard: OnImageUnloading can
 *      destroy an object whose Destruct aims into a doomed image, but nothing
 *      can un-run a function EXECUTING in one. appDLLClose then unmaps the code
 *      under the instruction pointer and the crash has no symbol, no module, and
 *      no way back to the reload that caused it.
 *
 *      So every Thread records the ImageID that started it and
 *      Threads::OnImageUnloading joins them before the library closes. It cannot
 *      be a destructor -- ModuleManager chooses the point, between destroying
 *      the instance and closing the handle -- and it cannot be advisory.
 *
 *   3. THERE IS NO Detach(). See the deleted member below.
 *
 * THE RULES CORE HAS AND HAS NEVER CHECKED. Four subsystems state a threading
 * contract in prose and verify none of it:
 *
 *      ModuleManager.h:43   "THREADING: none" -- Tick/Load/Unload/Reload
 *      CoreConfig.h:412     the registry is not thread-safe
 *      CoreFileProvider     GetFileProvider, set once at startup
 *      CoreMemoryPool.h:33  the pool REGISTRY, same stance
 *
 * CollectGarbage is the exception -- it latches a ThreadID and refuses a foreign
 * caller. SetMainThread() and CORE_CHECK_MAIN_THREAD() generalise that, so the
 * four above fire on the first bad call rather than the twentieth stress run.
 *
 * DELIBERATELY ABSENT: a mutex, lock guard, rwlock or atomic wrapper -- std::
 * already has them and a house-style name buys nothing. A condvar wrapper, for
 * the same reason, EXCEPT that what a scheduler always wants from one is "wake N
 * of M", which is a counting semaphore, and C++17 has none. Hence Semaphore, and
 * nothing else. Thread-local storage, since `thread_local` works. Stack size,
 * which std::thread cannot set and which would mean the very thread-creation
 * seam this file exists to avoid.
 */

namespace EngineCore
{
    /* The ladder appThreadSetPriority takes -- see there for why it is not an
     * integer. THE VALUES ARE THE SEAM'S CONTRACT: do not renumber. */
    enum class ThreadPriority : uint8_t
    {
        /* Yields to everything: file watching, asset cooking, log flushing. */
        Background = 0,

        /* What a worker gets unless there is a reason. */
        Normal     = 1,

        /* Audio, and a render thread that must not be preempted by the pool it
         * feeds. NOT for job workers -- raising every worker raises nothing
         * relative to anything, and starves the main thread waiting on them. */
        High       = 2
    };

    /*
     * A counting semaphore. Signal() releases waiters; Wait() blocks until the
     * count is positive and consumes one.
     *
     * WHY THIS AND NOT A BARE condition_variable AT EACH CALL SITE. C++17 has no
     * std::counting_semaphore -- that is C++20, and CMakeLists.txt pins this
     * module at 17. The alternative is a mutex/condvar/counter triple written out
     * everywhere, and written slightly wrong it gives a LOST WAKEUP: a Signal
     * landing between a worker's "is there work?" test and its wait is dropped,
     * and the worker sleeps on a non-empty queue. Nothing corrupts, nothing
     * asserts, no test fails -- the frame is just occasionally 30ms longer. The
     * way not to have that is to write the counter down exactly once.
     *
     * NOT A MUTEX: no owner, no recursion, and any thread may consume any
     * Signal. Do not guard state with it.
     *
     * Holds std:: members across the DLL boundary -- the C4251 CMakeLists.txt
     * suppresses under its stated one-toolchain, one-CRT condition.
     */
    class CORE_API Semaphore
    {
    public:
        explicit Semaphore(uint32_t InInitialCount = 0);

        Semaphore(const Semaphore&)            = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        /* Releases up to InCount waiters. Signalling more than are waiting
         * ACCUMULATES -- the whole difference from an event, and what lets "I
         * queued 8 jobs" be Signal(8) without knowing who is awake. */
        void Signal(uint32_t InCount = 1);

        void Wait();

        /* False if the count was zero. Never blocks. */
        bool TryWait();

        /* False on timeout. A worker idles on this rather than Wait() so a
         * shutdown flag is noticed even if a peer consumed its Signal. */
        bool TryWaitFor(uint32_t InMilliseconds);

    private:
        std::mutex              Mutex;
        std::condition_variable Condition;
        uint32_t                Count = 0;
    };

    /*
     * One owned OS thread: named, tagged with its image, joinable, and not
     * detachable.
     *
     * NOT COPYABLE AND NOT MOVABLE. It is registered in a process-wide table by
     * ADDRESS so that OnImageUnloading can find it, and a move would leave that
     * table pointing at a husk. A pool that needs a variable number of these
     * holds TUnique<Thread> (CorePointer.h), not a vector of Thread by value.
     */
    class CORE_API Thread
    {
    public:
        /* std::function here, and NOT the raw pointer-plus-payload CoreJob.h
         * insists on. Arithmetic, not taste: a thread entry is constructed once
         * per thread, so one allocation for its captures is free, where a job
         * payload is constructed thousands of times a frame. Same trade, opposite
         * answers, four orders of magnitude apart. */
        using EntryFn = std::function<void()>;

        Thread() = default;

        /*
         * Joins if still running, and LOGS AN ERROR when it had to.
         *
         * Joining here is survival, not convenience -- std::thread's destructor
         * calls std::terminate on a joinable thread, so the choice is between
         * this and taking the process down. But arriving here still running
         * means the owner never wrote Join(), and that is always a lifetime
         * bug: whatever the entry function captured is being destroyed in an
         * order this destructor does not control. So it joins to survive, and
         * says so loudly enough to get fixed.
         */
        ~Thread();

        Thread(const Thread&)            = delete;
        Thread& operator=(const Thread&) = delete;
        Thread(Thread&&)                 = delete;
        Thread& operator=(Thread&&)      = delete;

        /*
         * Starts the thread. False if it is already running, if InEntry is
         * empty, or if the OS refused to create it.
         *
         * InName is COPIED into fixed storage, since GetName() and diagnostics
         * may read it long after a caller's temporary is gone. Truncated to
         * NAME_CAPACITY, and appThreadSetName truncates again and harder.
         *
         * ORDER INSIDE THE TRAMPOLINE, which is contract, not detail:
         *
         *      1. appThreadSetName     -- so a debugger attached at any later
         *                                 point sees it, INCLUDING during a
         *                                 crash in step 4
         *      2. appThreadSetPriority
         *      3. publish ID + bRunning -- GetID() returns 0 until here
         *      4. InEntry()
         *      5. clear bRunning, wake any joiner
         *
         * Step 3 last of the three because a thread visible in the registry is
         * one OnImageUnloading may try to join.
         *
         * DO NOT PASS InOwningImage. The default argument IS the mechanism: it is
         * evaluated AT THE CALL SITE, so GImageID is read in the caller's image,
         * the only place it means anything. Start's body cannot read it -- that
         * compiles into EngineCore.dll, where GImageID is always IMAGE_Host.
         * CoreMemory.h:251 documents the same trap for Memory::Allocate.
         *
         * A module wrapping Start in its own helper must forward GImageID
         * explicitly, or its threads register against the host and the barrier
         * will not join them.
         */
        bool Start(const char*    InName,
                   EntryFn        InEntry,
                   ThreadPriority InPriority      = ThreadPriority::Normal,
                   ImageID        InOwningImage   = GImageID);

        /*
         * Blocks until the entry function returns. Idempotent, because the three
         * shutdown paths that call it -- the owner, a pool destructor,
         * OnImageUnloading -- legitimately overlap.
         *
         * DOES NOT ASK THE THREAD TO STOP, and never will: killing a thread at an
         * arbitrary instruction leaves every lock it held locked. A worker
         * cooperates by watching its own flag and Join waits for it to notice, so
         * a Join on a thread that is not cooperating hangs -- the correct and
         * diagnosable outcome.
         */
        void Join();

        bool        IsRunning() const;
        ThreadID    GetID()     const;   // 0 until the thread is running
        const char* GetName()   const;

        /* Which image started it. Public because Threads::OnImageUnloading is a
         * free function and filters on it. */
        ImageID     GetOwningImage() const;

        /* POSIX only lets a thread set its OWN affinity, so there is no honest
         * per-object setter -- hence a static affecting the caller. Call it first
         * thing in an entry function, and read appThreadSetAffinity's note before
         * using it at all: pinning is usually a pessimization. */
        static bool SetAffinityForThisThread(uint64_t InMask);

        /*
         * DELIBERATELY ABSENT. A detached thread cannot be joined, so it cannot
         * be stopped before appDLLClose unmaps the code it is running -- reason 2
         * above. It also outlives Memory::ReportLeaks, so its allocations are
         * reported as leaks nobody can account for. Every real use of detach is
         * "I would rather not think about this lifetime", which hot reload makes
         * unaffordable.
         */
        void Detach() = delete;

        enum : size_t { NAME_CAPACITY = 32 };

    private:
        std::thread Impl;
        char        Name[NAME_CAPACITY] = {};

        /* Which image called Start, arriving through that function's defaulted
         * InOwningImage. See the note there before touching either. */
        ImageID     OwningImage = IMAGE_Host;

        /*
         * ATOMIC BECAUSE THE THREAD ITSELF WRITES THEM, while GetID, IsRunning
         * and GetLiveThreadCount read them from other threads without the
         * registry lock. Plain members would be a data race on every one of those
         * -- invisible in a debug build, reordered in a release one.
         *
         * Relaxed is enough: these are diagnostics and nothing is published
         * through them. The ordering that matters, the entry function's writes
         * reaching a joiner, is std::thread::join's.
         */
        std::atomic<ThreadID> ID{0};
        std::atomic<bool>     bRunning{false};
    };

    /*
     * Process-wide thread facts and the two barriers.
     *
     * Free functions in a nested namespace rather than statics on Thread, and
     * out-of-line CORE_API rather than inline, for the reason
     * CoreMemoryPool.h:138 gives about its own registry: the table must live in
     * EngineCore.dll so the host and every module see the same one. Inline it
     * and each module gets a private table -- and a module's threads become
     * invisible to the barrier whose entire job is to join them.
     */
    namespace Threads
    {
        // === Facts ==========================================================

        CORE_API uint32_t GetHardwareThreadCount();   // logical, >= 1
        CORE_API uint32_t GetPhysicalCoreCount();     // >= 1

        // === Portable niceties ==============================================
        // std::this_thread, wrapped only so a call site need not include
        // <thread> or spell out a chrono duration. These do NOT go through the
        // app* seam -- see CorePlatform.h's thread-attribute block.

        CORE_API void SleepMs(uint32_t InMilliseconds);

        /* A hint to the scheduler, not a sleep. Correct in a spin known to be
         * short (a queue push losing a race); wrong as a way to wait for
         * anything measured in microseconds or more -- that is what Semaphore
         * is for, and a yield loop burns a core to do it worse. */
        CORE_API void Yield();

        // === The main thread ================================================

        /* Names the thread owning the subsystems documented as single-threaded.
         * Modelled on Memory::SetCollectorThread down to the defaulting: the
         * first caller wins, and if nobody calls it the first
         * CORE_CHECK_MAIN_THREAD claims it. */
        CORE_API void     SetMainThread();
        CORE_API bool     IsMainThread();
        CORE_API ThreadID GetMainThreadID();

        // === The image barrier ==============================================

        /*
         * Joins every thread InImage started, WHILE ITS CODE IS STILL MAPPED, and
         * returns how many.
         *
         * The counterpart of Memory::OnImageUnloading, called from the same place
         * -- ModuleManager::TeardownInstance -- and it must run BEFORE that one:
         * a worker still inside module code can be constructing tracked objects
         * and copying TRefs, so destroying the image's objects first leaves the
         * collector walking a list a live thread is appending to.
         *
         * THIS CAN HANG, AND THAT IS THE DESIGN. There is no cancellation
         * (Thread::Join says why), so a module whose worker loops forever cannot
         * be unloaded. Giving up after a timeout and unmapping anyway is an
         * unexplainable crash minutes later. This logs the module and thread
         * names BEFORE it starts waiting, so even without a debugger the log says
         * which thread is holding the unload up.
         */
        CORE_API uint32_t OnImageUnloading(ImageID InImage);

        /* STARTED AND NOT YET JOINED, all images -- which includes a thread whose
         * entry function already returned, since that is exactly the set the
         * barrier has work to do on. Non-zero at the end of main() means
         * something was never joined. */
        CORE_API uint32_t GetLiveThreadCount();
    }
}

/*
 * Fires when called from anywhere but the main thread. Fatal, because what it
 * catches is a data race -- and since an installed handler logs and RETURNS
 * (CoreLog.h), the message names the misused subsystem: the log line has to be
 * enough on its own.
 *
 * ALWAYS COMPILED IN. Two uint64_t compared, against a mistake whose other
 * detection method is a corrupted module table three frames later.
 *
 * A bare if, matching CORE_CHECK, so a trailing semicolon is optional.
 */
#define CORE_CHECK_MAIN_THREAD(WhatSubsystem)                                  \
    CORE_CHECK(::EngineCore::Threads::IsMainThread(),                          \
               "Threading: " WhatSubsystem " was called from a thread other "  \
               "than the main thread; it is documented as single-threaded")

#endif //ENGINECORE_CORETHREAD_H
