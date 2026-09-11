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
 * C++17 does not have, and the thread-affinity rules the rest of Core already
 * depends on but has never been able to check.
 *
 * This is the LOWER half of the concurrency subsystem. It knows nothing about
 * work, scheduling or dependencies. CoreJob.h sits on top and adds all of
 * that. Read them in that order, the way CoreMemoryPool.h is read before
 * CoreMemory.h.
 *
 * WHAT THIS IS NOT: A WRAPPER OVER std::thread
 *
 * CorePlatform.h's rule is that portable std primitives are not OS-specific
 * and stay as they are, and std::thread is exactly that -- creation, joining
 * and thread_local are ported correctly by three standard libraries already,
 * and reimplementing them behind the app* seam would be three files of
 * CreateThread/pthread_create for no portability that was missing.
 *
 * What std::thread CANNOT express is everything a debugger and a profiler
 * need, and those four things -- name, affinity, priority, real core count --
 * are the app* functions this header sits on. Nothing else here touches the
 * OS.
 *
 * WHY A Thread CLASS AT ALL, THEN
 *
 * Three reasons, and each one is a rule this codebase already states somewhere
 * else and could not previously enforce:
 *
 *   1. NAMING IS NOT OPTIONAL. appThreadSetName has to be the first statement
 *      the new thread executes, and a caller who has to remember that will
 *      forget it. Thread::Start's trampoline does it, so an unnamed worker
 *      thread is not reachable through this API.
 *
 *   2. THE IMAGE BARRIER. CoreModule.h:37 -- "Nothing a module allocates
 *      outlives the module." A std::thread whose entry point is a lambda
 *      compiled into a module DLL is that same hazard, and a strictly worse
 *      version of it: CoreGC.h's OnImageUnloading can destroy an object whose
 *      Destruct pointer aims into a doomed image, but nothing can un-run a
 *      function that is executing in one. appDLLClose while a worker is inside
 *      module code unmaps the code under the instruction pointer, and the
 *      crash lands with a return address into nothing -- no symbol, no
 *      module, no way back to the reload that caused it.
 *
 *      So every Thread records the ImageID that started it, and
 *      Threads::OnImageUnloading JOINS that image's threads before the library
 *      closes. This cannot be a destructor (it must happen at a point
 *      ModuleManager chooses, between destroying the instance and closing the
 *      handle) and it cannot be advisory.
 *
 *   3. THERE IS NO Detach(). See the deleted member below.
 *
 * THE AFFINITY RULES CORE ALREADY HAS, AND HAS NEVER CHECKED
 *
 * Four subsystems already state a threading contract in prose and verify none
 * of it:
 *
 *      ModuleManager.h:43   "THREADING: none" -- Tick/Load/Unload/Reload
 *      CoreConfig.h:412     the registry is not thread-safe
 *      CoreFileProvider     GetFileProvider, set once at startup
 *      CoreMemoryPool.h:33  the pool REGISTRY, same stance
 *
 * CollectGarbage is the one exception: it latches a ThreadID through
 * SetCollectorThread and refuses a foreign caller. That mechanism is right and
 * this generalises it. SetMainThread() + CORE_CHECK_MAIN_THREAD() turn the
 * four prose contracts above into something that fires on the first bad call
 * rather than on the twentieth run of a stress test.
 *
 * WHAT IS DELIBERATELY ABSENT
 *
 *   A mutex, a lock guard, a read/write lock, an atomic wrapper. std::mutex
 *   and std::atomic are portable, already used throughout Core's .cpp files,
 *   and wrapping them would buy a house-style name and nothing else.
 *
 *   A condition-variable wrapper, for the same reason -- except that the one
 *   thing a condvar is always used for in a scheduler, "wake N of M sleeping
 *   workers", is a counting semaphore, and C++17 has none. Hence Semaphore
 *   below, and nothing else.
 *
 *   Thread-local storage. `thread_local` is a language keyword and works.
 *
 *   Stack size. std::thread cannot set it, so offering it would mean the full
 *   thread-creation seam this file exists to avoid. Revisit if a deeply
 *   recursive job ever needs it; none does yet.
 */

namespace EngineCore
{
    /*
     * The three-rung ladder appThreadSetPriority takes. See the note there for
     * why this is not an integer.
     *
     * THE VALUES ARE THE SEAM'S CONTRACT -- appThreadSetPriority takes a
     * uint8_t and these are what it means. Do not renumber.
     */
    enum class ThreadPriority : uint8_t
    {
        /* Yields to everything. For file watching, asset cooking, log
         * flushing -- work that must not cost a frame. */
        Background = 0,

        /* What a worker gets unless there is a reason. A job pool at Normal on
         * N-1 cores leaves the OS a core to run the main thread on. */
        Normal     = 1,

        /* Audio, and a render thread that must not be preempted by the pool it
         * feeds. NOT for job workers: raising every worker above normal raises
         * nothing relative to anything, and starves the main thread that is
         * waiting on them. */
        High       = 2
    };

    /*
     * A counting semaphore. Signal() releases waiters; Wait() blocks until the
     * count is positive and consumes one.
     *
     * WHY THIS EXISTS WHEN A BARE condition_variable AT THE CALL SITE DOES NOT
     *
     * C++17 has no std::counting_semaphore -- that is C++20, and this module is
     * pinned at cxx_std_17 by CMakeLists.txt. So the alternative is a
     * mutex/condvar/counter triple written out at every call site, and the
     * failure mode when it is written out slightly wrong is a LOST WAKEUP: a
     * Signal that lands between a worker's "is there work?" test and its wait
     * is dropped, and the worker sleeps on a queue that is not empty. Nothing
     * is corrupted, nothing asserts, no test fails -- the frame just takes 30ms
     * longer, sometimes. That is the worst class of bug a scheduler can have,
     * and the way to not have it is to write the counter down exactly once.
     *
     * NOT A MUTEX. It has no owner and no recursion, and a Signal from one
     * thread may be consumed by any other. Do not use it to guard state.
     *
     * Holds a std::mutex and a std::condition_variable across the DLL boundary,
     * which is the C4251 CMakeLists.txt suppresses under its stated condition:
     * one toolchain and one CRT across host and modules. A shared build that
     * violates that has larger problems than this class.
     */
    class CORE_API Semaphore
    {
    public:
        explicit Semaphore(uint32_t InInitialCount = 0);

        Semaphore(const Semaphore&)            = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        /* Releases up to InCount waiters. Signalling more than are waiting is
         * legal and ACCUMULATES -- that is the whole difference from an event,
         * and it is what lets "I just queued 8 jobs" be expressed as Signal(8)
         * with no knowledge of how many workers happen to be awake. */
        void Signal(uint32_t InCount = 1);

        void Wait();

        /* False if the count was zero. Never blocks. */
        bool TryWait();

        /* False on timeout. A worker's idle path uses this rather than Wait()
         * so that a shutdown flag is still noticed when the Signal that should
         * have woken it was consumed by a peer first. */
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
        /*
         * std::function, and NOT the raw-function-pointer-plus-payload shape
         * CoreJob.h insists on.
         *
         * The reason is arithmetic, not taste. A thread entry point is
         * constructed once per thread -- eight times in a process, perhaps --
         * so one heap allocation for its captures is free. A JOB payload is
         * constructed thousands of times per frame, which is why CoreJob.h pays
         * for inline storage and a static_assert instead. Same trade, opposite
         * answers, because the frequencies differ by four orders of magnitude.
         */
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
         * InName is COPIED into fixed storage, not held by pointer: the name is
         * read back by GetName() and by diagnostics that may run long after a
         * caller's temporary std::string is gone. Truncated to NAME_CAPACITY,
         * and note that appThreadSetName truncates again and harder -- 16 bytes
         * on Linux.
         *
         * ORDER INSIDE THE TRAMPOLINE, which is contract and not implementation
         * detail:
         *
         *      1. appThreadSetName     -- so a debugger attached at any later
         *                                 point sees the name, INCLUDING during
         *                                 a crash in step 4
         *      2. appThreadSetPriority
         *      3. publish ID + bRunning -- GetID() returns 0 until here
         *      4. InEntry()
         *      5. clear bRunning, wake any joiner
         *
         * Step 3 after 1 and 2 is deliberate: a thread visible in the registry
         * is a thread OnImageUnloading may try to join, so it must be fully
         * configured before it can be seen.
         *
         * DO NOT PASS InOwningImage. Its default argument is the whole
         * mechanism: a default argument is evaluated AT THE CALL SITE, so
         * `GImageID` there is read in the CALLER'S translation unit and
         * therefore in the caller's image, which is the only place it means
         * anything. Start's body cannot read it -- the body compiles into
         * EngineCore.dll, where GImageID is always IMAGE_Host. CoreMemory.h:251
         * documents that trap for Memory::Allocate and solves it by passing the
         * id down; this is the same fix with the language doing the passing.
         *
         * Which also means a module that calls Start through a wrapper of its
         * own must forward GImageID explicitly, or its threads register against
         * the host and the barrier will not join them on unload.
         */
        bool Start(const char*    InName,
                   EntryFn        InEntry,
                   ThreadPriority InPriority      = ThreadPriority::Normal,
                   ImageID        InOwningImage   = GImageID);

        /*
         * Blocks until the entry function returns. Idempotent -- joining an
         * already-joined or never-started thread is a no-op rather than an
         * error, because the three shutdown paths that call it (the owner, a
         * pool destructor, OnImageUnloading) legitimately overlap.
         *
         * DOES NOT ASK THE THREAD TO STOP. There is no cancellation here and
         * there will not be: killing a thread at an arbitrary instruction
         * leaves every lock it held locked and every invariant it was halfway
         * through broken. A worker cooperates by watching its own flag --
         * CoreJob.h's pool uses an atomic bool plus a Semaphore signal -- and
         * Join waits for it to notice. A Join on a thread that is not
         * cooperating hangs, which is the correct and diagnosable outcome.
         */
        void Join();

        bool        IsRunning() const;
        ThreadID    GetID()     const;   // 0 until the thread is running
        const char* GetName()   const;

        /* Which image started it. Public because Threads::OnImageUnloading is a
         * free function and has to filter on it -- and because a diagnostic that
         * wants to say WHICH module a thread belongs to should not need a friend
         * declaration to find out. */
        ImageID     GetOwningImage() const;

        /*
         * Affinity is a property the OWNING thread sets on itself on POSIX, so
         * there is no honest per-object setter -- hence a static that affects
         * the caller. Call it as the first thing an entry function does.
         *
         * Read appThreadSetAffinity's note before using it at all: pinning is
         * usually a pessimization.
         */
        static bool SetAffinityForThisThread(uint64_t InMask);

        /*
         * DELIBERATELY ABSENT: Detach().
         *
         * A detached thread cannot be joined, therefore cannot be waited for,
         * therefore cannot be stopped before appDLLClose unmaps the code it is
         * running -- reason 2 in this file's header block. It also outlives
         * Memory::ReportLeaks, so anything it allocates is reported as a leak
         * nobody can account for. Every real use of detach is "I would rather
         * not think about this thread's lifetime", and hot reload makes that
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
         * ATOMIC BECAUSE THE THREAD ITSELF WRITES THEM. The trampoline publishes
         * ID and clears bRunning; GetID, IsRunning and GetLiveThreadCount read
         * them from other threads without taking the registry lock. Plain
         * members here would be a data race on every one of those reads -- the
         * kind that is invisible in a debug build and reorders in a release one.
         *
         * Relaxed ordering is sufficient and is what the implementation uses:
         * these are diagnostics, and nothing else is published through them. The
         * ordering that actually matters -- the entry function's writes becoming
         * visible to a joiner -- is std::thread::join's, not theirs.
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

        /*
         * Names the thread that owns the subsystems documented as
         * single-threaded -- ModuleManager, the config registry, the file
         * provider, the pool registry.
         *
         * Modelled on Memory::SetCollectorThread down to the defaulting:
         * whichever thread calls this first wins, and if nobody calls it the
         * first CORE_CHECK_MAIN_THREAD claims it. A host calls it at the top of
         * main() and never thinks about it again.
         */
        CORE_API void     SetMainThread();
        CORE_API bool     IsMainThread();
        CORE_API ThreadID GetMainThreadID();

        // === The image barrier ==============================================

        /*
         * Joins every thread InImage started, WHILE ITS CODE IS STILL MAPPED.
         * Returns how many it joined.
         *
         * The exact counterpart of Memory::OnImageUnloading in CoreGC.h, called
         * from the same place -- ModuleManager::TeardownInstance, between
         * destroying the module instance and appDLLClose -- and it must run
         * BEFORE the memory one. A worker still inside module code can be
         * constructing tracked objects and copying TRefs, so destroying that
         * image's objects first would leave the collector walking a list a live
         * thread is still appending to.
         *
         * THIS CAN HANG, AND THAT IS THE DESIGN. There is no cancellation
         * (Thread::Join says why), so a module whose worker loops forever cannot
         * be unloaded. The alternative -- give up after a timeout and unmap the
         * code anyway -- is an unexplainable crash minutes later in whatever ran
         * next. A hang has the offending thread on it in the debugger, and this
         * logs the module and thread names BEFORE it starts waiting so the log
         * says which one even without a debugger attached.
         *
         * The module author's obligation is the one CoreModule.h already states:
         * stop what you started, in OnShutdown.
         */
        CORE_API uint32_t OnImageUnloading(ImageID InImage);

        /*
         * Threads that have been STARTED AND NOT YET JOINED, all images.
         *
         * Note what that includes: a thread whose entry function has already
         * returned still counts until somebody calls Join, because that is
         * precisely the set the image barrier has work to do on. It is the
         * shutdown-accounting number in the shape of Memory::ReportLeaks --
         * non-zero at the end of main() means something was never joined.
         */
        CORE_API uint32_t GetLiveThreadCount();
    }
}

/*
 * Fires when called from anywhere but the main thread.
 *
 * Fatal, via CORE_CHECK, because what it catches is a data race. Per
 * CoreLog.h's fatal semantics a host with a handler installed will log and
 * RETURN, so the message names the misused subsystem rather than just
 * complaining about the thread -- the log line has to be enough on its own.
 *
 * ALWAYS COMPILED IN, not debug-only. It is two uint64_t compared, set against
 * a mistake whose other detection method is a corrupted module table three
 * frames later. CORE_CHECK is always on for the same reason and this inherits
 * that stance.
 *
 * Shaped as a bare if, matching CORE_CHECK, so a trailing semicolon at the call
 * site is optional.
 */
#define CORE_CHECK_MAIN_THREAD(WhatSubsystem)                                  \
    CORE_CHECK(::EngineCore::Threads::IsMainThread(),                          \
               "Threading: " WhatSubsystem " was called from a thread other "  \
               "than the main thread; it is documented as single-threaded")

#endif //ENGINECORE_CORETHREAD_H
