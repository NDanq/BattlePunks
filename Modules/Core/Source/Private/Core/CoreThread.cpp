#include "Public/Core/CoreThread.h"

#include <chrono>
#include <string>
#include <utility>

/*
 * The portable half of the concurrency subsystem.
 *
 * Nothing here touches an OS API. Creation and joining are std::thread, the
 * four divergent attributes come through the app* seam, and everything else is
 * bookkeeping -- which is where the actual difficulty lives, because the
 * bookkeeping is what the image barrier reads.
 *
 * WHY THE REGISTRY IS A FIXED C ARRAY
 *
 * Same argument CoreMemoryPool.h:44 makes for GPools, and it matters more here.
 *
 *      Thread* GThreads[MAX_TRACKED_THREADS] = {};
 *
 * has no constructor, so it is constant-initialised into .bss before any
 * dynamic initialiser in any image runs. A std::vector would have a
 * constructor, therefore an ordering question, and a Thread started from a
 * file-scope constructor somewhere would register into a container that is not
 * built yet. std::mutex is constexpr-default-constructible, so the lock beside
 * it is constant-initialised too -- the same reason CoreGarbageCollector.cpp:38
 * gives for its pair.
 *
 * A linear scan over 256 slots to register and unregister is fine: this runs
 * once per thread lifetime, not once per frame. Core scans linearly everywhere.
 */

namespace EngineCore
{
namespace
{
    /*
     * 256 is not a budget, it is a tripwire. Nothing legitimate starts more than
     * a couple of dozen threads, so a build that exhausts this is leaking them
     * in a loop, and the refusal in Start says so with the count.
     */
    enum : size_t { MAX_TRACKED_THREADS = 256 };

    std::mutex GRegistryMutex;
    Thread*    GThreads[MAX_TRACKED_THREADS] = {};
    uint32_t   GThreadCount                  = 0;

    /*
     * The main thread, latched exactly as CoreGarbageCollector.cpp latches the
     * collector thread -- including the defaulting behaviour, where the first
     * caller to ask claims it.
     */
    std::mutex GMainThreadMutex;
    bool       GMainThreadSet = false;
    ThreadID   GMainThread    = 0;

    /* Caller must hold GRegistryMutex. False if the table is full. */
    bool RegisterThreadLocked(Thread* InThread)
    {
        for(size_t Slot = 0; Slot < MAX_TRACKED_THREADS; ++Slot)
        {
            if(GThreads[Slot] == nullptr)
            {
                GThreads[Slot] = InThread;
                ++GThreadCount;
                return true;
            }
        }
        return false;
    }

    /* Caller must hold GRegistryMutex. Silent if InThread is not present --
     * Join is documented idempotent, so a second unregister is expected. */
    void UnregisterThreadLocked(Thread* InThread)
    {
        for(size_t Slot = 0; Slot < MAX_TRACKED_THREADS; ++Slot)
        {
            if(GThreads[Slot] == InThread)
            {
                GThreads[Slot] = nullptr;
                --GThreadCount;
                return;
            }
        }
    }
}

// === Semaphore ==============================================================

Semaphore::Semaphore(const uint32_t InInitialCount)
    : Count(InInitialCount)
{
}

void Semaphore::Signal(const uint32_t InCount)
{
    if(InCount == 0)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> Lock(Mutex);
        Count += InCount;
    }

    /*
     * NOTIFY OUTSIDE THE LOCK, and notify_all for a count above one.
     *
     * Outside, because a waiter woken while this thread still holds the mutex
     * immediately blocks again on that same mutex -- the "hurry up and wait"
     * pessimisation. Correct either way; measurably worse inside.
     *
     * notify_all rather than N notify_ones because a condition_variable has no
     * "wake N" operation: N calls to notify_one can all land on the SAME waiter
     * if it wakes, re-waits and is picked again, leaving the others asleep on a
     * positive count. Waking everyone and letting the predicate send the extras
     * back is correct at any N; the thundering herd is bounded by the worker
     * count, which is single digits.
     */
    if(InCount == 1)
    {
        Condition.notify_one();
    }
    else
    {
        Condition.notify_all();
    }
}

void Semaphore::Wait()
{
    std::unique_lock<std::mutex> Lock(Mutex);

    /* The predicate form, not a bare wait: it re-checks on every wake, which is
     * what makes a spurious wakeup and a stolen count both harmless. This is the
     * lost-wakeup bug the header block describes, and it is absent for exactly
     * this reason -- the count is inspected under the same lock that Signal
     * incremented it under, so there is no window to lose a signal in. */
    Condition.wait(Lock, [this] { return Count > 0; });
    --Count;
}

bool Semaphore::TryWait()
{
    std::lock_guard<std::mutex> Lock(Mutex);
    if(Count == 0)
    {
        return false;
    }
    --Count;
    return true;
}

bool Semaphore::TryWaitFor(const uint32_t InMilliseconds)
{
    std::unique_lock<std::mutex> Lock(Mutex);

    if(!Condition.wait_for(Lock, std::chrono::milliseconds(InMilliseconds),
                           [this] { return Count > 0; }))
    {
        return false;   // timed out with the count still zero
    }

    --Count;
    return true;
}

// === Thread =================================================================

Thread::~Thread()
{
    if(Impl.joinable())
    {
        /*
         * Logged before the join, not after: if the join hangs -- and a
         * destructor reached with the thread still running is exactly the
         * situation where it might -- the log line is the only evidence of which
         * thread was involved. After the join it would never be written.
         *
         * The header explains why this joins at all rather than asserting:
         * std::thread's destructor calls std::terminate on a joinable thread, so
         * the alternative to joining is taking the process down.
         */
        CORE_LOG_ERROR(std::string("Threading: Thread '") + Name +
                       "' was destroyed without being joined. Joining now -- whatever its "
                       "entry function captured is being destroyed in an order this "
                       "destructor does not control. Call Join() explicitly.")
    }

    Join();
}

bool Thread::Start(const char*          InName,
                   EntryFn              InEntry,
                   const ThreadPriority InPriority,
                   const ImageID        InOwningImage)
{
    if(!InEntry)
    {
        CORE_LOG_ERROR("Threading: Thread::Start called with an empty entry function.")
        return false;
    }

    if(Impl.joinable())
    {
        CORE_LOG_ERROR(std::string("Threading: Thread::Start called on '") + Name +
                       "', which is already started. Join it first.")
        return false;
    }

    // Copy the name into fixed storage -- see the header on why it is not held
    // by pointer. Truncating rather than failing, as the seam does.
    size_t Len = 0;
    if(InName)
    {
        while(Len < NAME_CAPACITY - 1 && InName[Len] != '\0')
        {
            Name[Len] = InName[Len];
            ++Len;
        }
    }
    Name[Len] = '\0';

    OwningImage = InOwningImage;

    /*
     * REGISTERED BEFORE THE THREAD EXISTS, and that ordering is load-bearing. A
     * running thread that is not yet in the table is a thread the image barrier
     * cannot see, so a module unloading in that window would close its library
     * with a worker live inside it. Registering first makes the window empty;
     * the cost is having to unregister if creation then throws.
     */
    {
        std::lock_guard<std::mutex> Lock(GRegistryMutex);
        if(!RegisterThreadLocked(this))
        {
            CORE_LOG_ERROR(std::string("Threading: cannot start thread '") + Name +
                           "' -- the thread registry is full (256 live threads). An "
                           "untracked thread would be invisible to the image barrier, so "
                           "this refuses rather than starting one.")
            return false;
        }
    }

    bRunning.store(true, std::memory_order_relaxed);

    /*
     * The trampoline. Order is the contract documented on Start.
     *
     * Captures by value: Entry is moved in, and the name is copied rather than
     * read through `this`. Reading Name through the captured `this` would work
     * today and would be a use-after-free the moment anything is allowed to move
     * a Thread -- which is exactly the sort of latent hazard that makes the class
     * non-movable in the first place. The 32 bytes are cheap; the coupling is
     * not.
     */
    char TrampolineName[NAME_CAPACITY];
    for(size_t Char = 0; Char < NAME_CAPACITY; ++Char)
    {
        TrampolineName[Char] = Name[Char];
    }

    const uint8_t RawPriority = uint8_t(InPriority);

    /* std::thread's constructor throws on failure rather than returning an
     * error, and Core does not use exceptions (CoreMemoryPool.h says so of
     * IMemoryPool::Allocate). Catching here converts the one place the standard
     * library forces an exception on us back into the bool this seam promises. */
    try
    {
        Impl = std::thread([this, Entry = std::move(InEntry), TrampolineName, RawPriority]() mutable
        {
            /* 1. Name first, so a debugger attaching at ANY later point -- during
             *    a crash inside Entry included -- sees it. */
            appThreadSetName(TrampolineName);

            /* 2. Priority. A refusal is not fatal and not logged: on Linux a
             *    High request cannot succeed without privileges a game will not
             *    have, and a warning per worker per launch would be noise for a
             *    condition the caller already learns from the seam's bool. */
            (void)appThreadSetPriority(RawPriority);

            /* 3. Publish identity. Relaxed: this is a diagnostic, and nothing
             *    else is published through it. */
            ID.store(appThreadID(), std::memory_order_relaxed);

            /* 4. The actual work. */
            Entry();

            /* 5. No longer running. Note that this does NOT unregister -- a
             *    finished thread still needs joining, and the registry is the
             *    set of things the barrier has work to do on. Join unregisters. */
            bRunning.store(false, std::memory_order_relaxed);
        });
    }
    catch(...)
    {
        bRunning.store(false, std::memory_order_relaxed);

        std::lock_guard<std::mutex> Lock(GRegistryMutex);
        UnregisterThreadLocked(this);

        CORE_LOG_ERROR(std::string("Threading: the OS refused to create thread '") + Name + "'.")
        return false;
    }

    return true;
}

void Thread::Join()
{
    if(Impl.joinable())
    {
        Impl.join();
    }

    bRunning.store(false, std::memory_order_relaxed);
    ID.store(0, std::memory_order_relaxed);

    /*
     * Unregister AFTER the join, and note carefully that this takes
     * GRegistryMutex -- which is why Threads::OnImageUnloading must not hold
     * that lock while it joins. It snapshots under the lock and releases before
     * joining, or this line would deadlock against it. That constraint is the
     * single least obvious thing in this file.
     */
    std::lock_guard<std::mutex> Lock(GRegistryMutex);
    UnregisterThreadLocked(this);
}

bool Thread::IsRunning() const
{
    return bRunning.load(std::memory_order_relaxed);
}

ThreadID Thread::GetID() const
{
    return ID.load(std::memory_order_relaxed);
}

const char* Thread::GetName() const
{
    return Name;
}

ImageID Thread::GetOwningImage() const
{
    return OwningImage;
}

bool Thread::SetAffinityForThisThread(const uint64_t InMask)
{
    return appThreadSetAffinity(InMask);
}

// === Threads ================================================================

namespace Threads
{
    uint32_t GetHardwareThreadCount()
    {
        return appThreadHardwareCount();
    }

    uint32_t GetPhysicalCoreCount()
    {
        return appThreadPhysicalCoreCount();
    }

    void SleepMs(const uint32_t InMilliseconds)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(InMilliseconds));
    }

    void Yield()
    {
        std::this_thread::yield();
    }

    void SetMainThread()
    {
        std::lock_guard<std::mutex> Lock(GMainThreadMutex);
        GMainThread    = appThreadID();
        GMainThreadSet = true;
    }

    bool IsMainThread()
    {
        const ThreadID Current = appThreadID();

        std::lock_guard<std::mutex> Lock(GMainThreadMutex);

        /*
         * CLAIMS THE THREAD IF NOBODY HAS, which makes this a query with a side
         * effect -- deliberately, and exactly as CollectGarbage does for the
         * collector thread. The alternative is that a host which forgot
         * SetMainThread gets a fatal CORE_CHECK_MAIN_THREAD on its first
         * ModuleManager::Tick, which would punish the forgetting rather than the
         * actual mistake this is here to catch.
         */
        if(!GMainThreadSet)
        {
            GMainThread    = Current;
            GMainThreadSet = true;
            return true;
        }

        return Current == GMainThread;
    }

    ThreadID GetMainThreadID()
    {
        std::lock_guard<std::mutex> Lock(GMainThreadMutex);
        return GMainThread;
    }

    uint32_t OnImageUnloading(const ImageID InImage)
    {
        /*
         * SNAPSHOT UNDER THE LOCK, JOIN WITHOUT IT.
         *
         * Thread::Join takes GRegistryMutex to unregister, so joining while
         * holding it is a self-deadlock. The snapshot is a fixed local array for
         * the same reason the registry is: no allocation on a teardown path, and
         * nothing to construct.
         *
         * The window this opens: a Thread in the snapshot could in principle be
         * destroyed by another thread before it is joined here. It cannot in
         * practice, because unloading happens on the main thread with the module
         * quiesced -- the same assumption ModuleManager::Tick already makes about
         * every other part of teardown. Documented rather than defended, because
         * defending it would mean reference-counting Thread objects to protect
         * against a caller violating a rule that is stated in three headers.
         */
        Thread*  Snapshot[MAX_TRACKED_THREADS];
        uint32_t Found = 0;

        {
            std::lock_guard<std::mutex> Lock(GRegistryMutex);

            for(size_t Slot = 0; Slot < MAX_TRACKED_THREADS; ++Slot)
            {
                Thread* Candidate = GThreads[Slot];
                if(Candidate && Candidate->GetOwningImage() == InImage)
                {
                    Snapshot[Found++] = Candidate;
                }
            }
        }

        if(Found == 0)
        {
            return 0;
        }

        /*
         * Logged BEFORE the waiting starts, naming every thread. This call can
         * hang by design (see the header), and when it does this is the only
         * record of which thread is holding the unload up. After the join it
         * would be too late to be useful.
         */
        {
            std::string Message = std::string("Threading: image '") +
                                  Memory::GetImageName(InImage) +
                                  "' is unloading with " + std::to_string(Found) +
                                  " thread(s) still registered; joining before its code is "
                                  "unmapped:";

            for(uint32_t Index = 0; Index < Found; ++Index)
            {
                Message += std::string(" '") + Snapshot[Index]->GetName() + "'";
                Message += Snapshot[Index]->IsRunning() ? " (running)" : " (finished)";
            }

            CORE_LOG_WARN(Message)
        }

        for(uint32_t Index = 0; Index < Found; ++Index)
        {
            Snapshot[Index]->Join();
        }

        return Found;
    }

    uint32_t GetLiveThreadCount()
    {
        std::lock_guard<std::mutex> Lock(GRegistryMutex);
        return GThreadCount;
    }
}
}
