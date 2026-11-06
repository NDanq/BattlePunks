#ifndef ENGINECORE_COREQUEUE_H
#define ENGINECORE_COREQUEUE_H

#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

/*
 * A thread-safe FIFO queue over std::deque.
 *
 * WHY THIS IS SYNCHRONIZED WHEN THE QQueue IT REPLACES WAS NOT
 *
 * The queue this was written for -- GDI's GRenderCommands -- is a genuine
 * cross-thread producer/consumer: ENQUEUE_RENDERCOMMAND pushes from the UI
 * thread while the render thread's Tick() drains. QQueue offers no
 * synchronization whatsoever, so that pairing has always been a data race.
 * Nothing protected it; the Qt event-loop handoff around it made the race
 * rare, not absent.
 *
 * Replacing QQueue with a bare std::deque would have preserved the race while
 * removing the timing that hid it. So the replacement locks. The cost is a
 * mutex acquisition per command -- a handful per frame, against work measured
 * in GPU milliseconds.
 *
 * WHAT THIS IS NOT
 *
 * Not lock-free, and deliberately so: the contention here is a few items per
 * frame between two threads. A lock-free ring would be more code, more subtle,
 * and no faster at this volume.
 *
 * SINGLE-CONSUMER NOTE
 *
 * IsEmpty() followed by Dequeue() is check-then-act and is only safe when
 * exactly one thread consumes -- which is the case here (only the render
 * thread drains). TryDequeue() has no such caveat and is the primitive to
 * prefer; it is why the drain loop no longer needs IsEmpty() at all.
 */

namespace EngineCore
{
    template<typename T>
    class ThreadSafeQueue
    {
    public:
        ThreadSafeQueue() = default;

        // Non-copyable, non-movable: it owns a mutex, and a queue shared
        // between two threads should never be silently copied out from under
        // one of them.
        ThreadSafeQueue(const ThreadSafeQueue&)            = delete;
        ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

        void Enqueue(T InValue)
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Items.push_back(std::move(InValue));
        }

        /*
         * Pops the front item into OutValue. Returns false if the queue was
         * empty, leaving OutValue untouched.
         *
         * This is the race-free primitive: the emptiness test and the pop
         * happen under one lock, so a drain loop needs no separate IsEmpty()
         * call and cannot pop from an empty queue.
         */
        bool TryDequeue(T& OutValue)
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            if(Items.empty())
            {
                return false;
            }
            OutValue = std::move(Items.front());
            Items.pop_front();
            return true;
        }

        /*
         * Snapshot emptiness. Only meaningful as a hint, or under the
         * single-consumer condition described at the top of this file: another
         * thread may enqueue the instant after this returns.
         */
        bool IsEmpty() const
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            return Items.empty();
        }

        size_t Size() const
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            return Items.size();
        }

        /* Discards everything. Whatever the items own is the caller's problem
         * -- this queue holds raw pointers in its main use and deletes nothing. */
        void Clear()
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Items.clear();
        }

        /*
         * Moves the entire contents into OutItems (appending) under a single
         * lock, and returns whether anything was taken.
         *
         * BEHAVIOURAL WARNING -- this is NOT equivalent to a TryDequeue loop.
         *
         * A `while(TryDequeue(x)) Execute(x);` loop will also run commands that
         * are enqueued *while it is executing*, because it re-checks the queue
         * each time. A batch drain takes a snapshot, so anything enqueued
         * during execution waits for the next drain.
         *
         * GDI's render loop relies on the former: commands issued from within
         * a command's Execute() run in the same frame. Do not "optimize" that
         * loop into this call.
         *
         * It exists for consumers that genuinely want a frame-boundary
         * snapshot, and for shutdown paths that need to take ownership of
         * everything still queued.
         */
        bool DrainTo(std::vector<T>& OutItems)
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            if(Items.empty())
            {
                return false;
            }

            OutItems.reserve(OutItems.size() + Items.size());
            for(T& Item : Items)
            {
                OutItems.push_back(std::move(Item));
            }
            Items.clear();
            return true;
        }

    private:
        mutable std::mutex Mutex;
        std::deque<T>      Items;
    };
}

#endif //ENGINECORE_COREQUEUE_H
