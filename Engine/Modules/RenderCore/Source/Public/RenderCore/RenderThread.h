#ifndef RENDERCORE_RENDERTHREAD_H
#define RENDERCORE_RENDERTHREAD_H

#include <cstdint>

#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreTypes.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderTypes.h"

/*
 * The render thread's whole life: the OS thread itself, the command queue
 * between it and the game thread, GDI's lifetime, and the frame/flush
 * fences. Replaces NimbleEngine.cpp's RenderThreadMain/DrainRenderCommands/
 * StartRenderThread/StopRenderThread, moved here so the render thread is
 * RenderCore's to own rather than the engine layer's.
 *
 * ONE PER PROCESS in practice (a game constructs exactly one, inside
 * Renderer -- a later build-order step), located by RenderCommand.h's free
 * functions through RenderThreadInternal.h's locator, the way JobSystem's
 * GetJobSystem()/SetJobSystem() work (CoreJob.h). A CLASS and not a
 * G-pointer with free functions alone, for CoreJob.h's own reason: this owns
 * an OS thread, and the host -- not a Meyers singleton's unspecified static
 * destruction order -- must be able to choose when it is constructed and
 * destroyed relative to everything else tearing down.
 */
namespace RenderCore
{
    class RenderContext;
    struct RenderCommandHeader;

    class RENDERCORE_API RenderThread
    {
    public:
        RenderThread();
        ~RenderThread();

        RenderThread(const RenderThread&)            = delete;
        RenderThread& operator=(const RenderThread&) = delete;

        /*
         * InitializeGDI(InBackend, InWindow) runs on THIS call's thread (the
         * game thread) -- matching NimbleEngine.cpp's own Initialize(), which
         * did the same before handing a context to the render thread. Starts
         * a Thread named "NimbleRender" at ThreadPriority::High (a render
         * thread must not be preempted by the work it feeds -- CoreThread.h's
         * own reasoning for the priority ladder) and blocks until the render
         * thread has called AllocateResourcesGDI() and reported whether that
         * succeeded.
         *
         * False if InBackend is not ERenderBackend::Null and InWindow has no
         * native handle (GDI needs a real drawable for anything else -- see
         * NimbleEngine.cpp's own equivalent check before this module
         * existed), if the OS thread failed to start, or if
         * AllocateResourcesGDI() failed to latch the render thread.
         */
        bool Start(ERenderBackend InBackend, EngineCore::WindowHandle InWindow);

        /*
         * Flush()es, then flag-then-kick-then-Join()s, the same order
         * StopRenderThread() used: the flag must be set before the kick or
         * the render thread could consume the signal, loop, and re-sleep
         * without ever seeing it. The render thread calls GDynamicGDI->Destroy()
         * then ReleaseGDI() on its own way out, before this returns -- both
         * belong on the render thread, matching NimbleEngine.cpp's own
         * comment on why (a GL/Vulkan context is thread-affine).
         *
         * Idempotent and safe to call on a RenderThread that never started.
         */
        void Stop();

        bool               IsRunning()    const;
        EngineCore::ThreadID GetThreadID() const;   // 0 until Start() succeeds

        /*
         * Queues InCommand. GAME THREAD ONLY -- what EnqueueRenderCommand
         * (RenderCommand.h) forwards to; nothing else should call this
         * directly.
         */
        void Enqueue(RenderCommandHeader* InCommand);

        /*
         * Enqueues a FRAME MARKER carrying InFrameNumber, then Signal()s the
         * kick -- ALWAYS, whether or not anything else is queued, because
         * kicks and markers must stay 1:1 (Semaphore::Signal accumulates; a
         * kick with no matching marker would let a later FrameDone.Wait()
         * return for the wrong frame). The render thread's drain loop
         * GDISwapBuffers()s and increments FramesRendered when it reaches
         * this marker, then Signal()s FrameDone.
         *
         * GAME THREAD ONLY.
         */
        void EndFrame(uint64_t InFrameNumber);

        /*
         * Blocks until the most recent EndFrame()'s marker has been reached,
         * if one is still outstanding -- the "at most one frame in flight"
         * fence that replaces NimbleEngine.cpp's old bFrameInFlight/FrameDone
         * pair. A no-op if no frame is currently in flight (the common case
         * on the very first tick, or if EndFrame was never called since the
         * last WaitForFrame).
         *
         * GAME THREAD ONLY.
         */
        void WaitForFrame();

        bool IsFrameInFlight() const;

        /*
         * Enqueues a FLUSH MARKER and blocks until the render thread reaches
         * it -- everything queued so far, from every caller, has executed.
         * Unlike EndFrame(), this does not present and does not count a
         * frame; it exists for a caller (Renderer::Shutdown, a test) that
         * needs "drained" without needing "presented".
         *
         * GAME THREAD ONLY.
         */
        void Flush();

        /* RENDER THREAD ONLY -- CORE_CHECKed. What GetRenderThreadContext()
         * (RenderCommand.h) actually returns. */
        RenderContext& GetContext();

        /* A snapshot, safe from any thread -- every counter behind it is an
         * atomic. See RenderTypes.h's own comment on what "snapshot" means
         * here. */
        void GetStats(RenderStats& OutStats) const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //RENDERCORE_RENDERTHREAD_H
