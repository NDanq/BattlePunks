#include "Public/RenderCore/RenderThread.h"

#include <atomic>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreQueue.h"
#include "Public/Core/CoreThread.h"
#include "Public/RenderCore/RenderCommand.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Private/RenderCore/RenderCoreInternal.h"
#include "Private/RenderCore/RenderThreadInternal.h"

/*
 * The drain loop, and everything around it. See RenderThread.h for the
 * per-method contracts; this file is the "how", modelled directly on
 * NimbleEngine.cpp's RenderThreadMain/DrainRenderCommands/StartRenderThread/
 * StopRenderThread -- the exact code this module supersedes -- with ONE
 * structural addition: the frame and flush MARKERS, which are what let a
 * caller enqueue throughout its tick instead of only from behind a fence.
 */

namespace RenderCore
{
    struct RenderThread::Impl
    {
        EngineCore::Thread    OSThread;
        EngineCore::Semaphore ReadyForUse;      // render -> caller: AllocateResourcesGDI() has run, bAllocateOK is readable
        EngineCore::Semaphore Kick;              // caller -> render: something is queued
        EngineCore::Semaphore FrameDone;         // render -> caller: the most recent frame marker was reached
        EngineCore::Semaphore FlushDone;         // render -> caller: the most recent flush marker was reached

        EngineCore::ThreadSafeQueue<RenderCommandHeader*> Queue;

        std::atomic<bool> bStopRequested{false};
        std::atomic<bool> bAllocateOK{false};
        std::atomic<EngineCore::ThreadID> ThreadID{0};

        bool bFrameInFlight = false;   // game-thread only; guarded by there being exactly one caller

        RenderContext Context;
        ERenderBackend Backend = ERenderBackend::Null;

        /* Stats. Atomics because GetStats() may be called from any thread,
         * even while the render thread is mutating them -- see RenderTypes.h. */
        std::atomic<uint64_t> FramesRendered{0};
        std::atomic<uint64_t> CommandsExecuted{0};
        std::atomic<uint32_t> PeakQueueDepth{0};

        /*
         * Runs one command and frees it. Returns true if InCommand was a
         * marker -- a BOUNDARY the caller's drain loop must stop at, not
         * merely a command like any other: a command enqueued after
         * EndFrame() must still be sitting in the queue, untouched, the
         * instant WaitForFrame() returns on the other thread. Draining past
         * the marker in the same TryWaitFor wakeup (nothing stopped a plain
         * while(TryDequeue) loop from doing exactly that) would let the next
         * frame's command run before this frame is even reported done --
         * silently erasing the one-frame-of-lag guarantee ENQUEUE_RENDER_COMMAND's
         * own header comment states.
         */
        bool RunDrainStep(RenderCommandHeader* Cmd)
        {
            bool bWasMarker = false;

            if(Cmd->Kind == RCK_FrameMarker)
            {
                GDISwapBuffers();
                FramesRendered.fetch_add(1, std::memory_order_relaxed);
                FrameDone.Signal();
                bWasMarker = true;
            }
            else if(Cmd->Kind == RCK_FlushMarker)
            {
                FlushDone.Signal();
                bWasMarker = true;
            }
            else
            {
                Cmd->Fn(Cmd, Context);
                CommandsExecuted.fetch_add(1, std::memory_order_relaxed);
            }

            EngineCore::Memory::Free(Cmd);
            return bWasMarker;
        }

        void ThreadMain()
        {
            /*
             * THE LATCH -- whichever thread reaches this becomes the render
             * thread for GDIInRenderThread(), because the backend records
             * appThreadID() inside Allocate(). Recorded into ThreadID
             * separately (not derived from GDI) so IsInRenderThread()
             * (RenderCommand.cpp) never has to reach into GDI at all --
             * exactly the boundary RenderContext.h's own comment states.
             */
            AllocateResourcesGDI();
            ThreadID.store(EngineCore::appThreadID(), std::memory_order_release);

            /* A real test, not a restatement of "we called Allocate": a GL
             * backend with no drawable early-outs before SetThreadID, leaving
             * GDIInRenderThread() false forever -- NimbleEngine.cpp's own
             * comment on this exact check, carried over verbatim. */
            const bool bOK = GDIInRenderThread();
            bAllocateOK.store(bOK, std::memory_order_release);
            ReadyForUse.Signal();

            if(bOK)
            {
                while(!bStopRequested.load(std::memory_order_acquire))
                {
                    /* BOUNDED, never Wait(): the stop flag is noticed even if
                     * a signal was lost or consumed, which makes shutdown
                     * deadlock-proof however flag and signal interleave --
                     * NimbleEngine.cpp's own reasoning for the same 2ms poll. */
                    if(Kick.TryWaitFor(2))
                    {
                        /*
                         * STOPS AT THE FIRST MARKER -- see RunDrainStep's own
                         * comment. Kicks and markers are 1:1 (EndFrame/Flush
                         * both Signal() exactly once per marker enqueued), so
                         * the next TryWaitFor wakeup is what resumes draining
                         * whatever sits behind the marker just reached.
                         */
                        RenderCommandHeader* Cmd = nullptr;
                        while(Queue.TryDequeue(Cmd))
                        {
                            if(RunDrainStep(Cmd))
                            {
                                break;
                            }
                        }
                    }
                }
            }

            /* Stragglers from a frame that will never be kicked -- may
             * include a RemoveProxy command from a later build-order step;
             * nothing else frees them. */
            RenderCommandHeader* Cmd = nullptr;
            while(Queue.TryDequeue(Cmd))
            {
                RunDrainStep(Cmd);
            }

            /* ReleaseGDI deletes the driver WITHOUT calling Destroy(), so GPU
             * objects would outlive their owner. Both belong on the
             * context's thread -- NimbleEngine.cpp's own comment, carried
             * over verbatim. */
            if(GDynamicGDI)
            {
                GDynamicGDI->Destroy();
            }
            ReleaseGDI();
        }
    };

    RenderThread::RenderThread() : P(new Impl())
    {
    }

    RenderThread::~RenderThread()
    {
        Stop();
        delete P;
    }

    bool RenderThread::Start(const ERenderBackend InBackend, const EngineCore::WindowHandle InWindow)
    {
        if(IsRunning())
        {
            RENDERCORE_LOG_ERROR("RenderThread::Start called while already running")
            return false;
        }

        const ERenderPlatform Platform = ERenderPlatform(InBackend);

        InitializeGDI(Platform, InWindow);

        /*
         * WHICH BACKENDS CAN BE HANDED A CONTEXT FROM HERE.
         *
         * Null: trivially, it dereferences nothing.
         *
         * Vulkan: also yes, and this is NOT the "the host does not provide a
         * drawable yet" case an older comment here (inherited from
         * NimbleEngine.cpp, from before this module existed) claimed for
         * every non-Null backend. Vulkan needs no drawable AT THIS POINT:
         * VulkanGDIContext's own constructor calls GetSharedVkInstance(),
         * which creates the process-wide VkInstance from
         * GDIPlatform::appVulkanRequiredInstanceExtensions() alone, and the
         * SURFACE is created much later, by VKSwapchain::Initialize, out of
         * the WindowHandle GDI already stored in InitializeGDI above (it
         * calls GDIPlatform::appVulkanCreateSurface itself). Both halves of
         * that platform seam are already implemented for Windows in
         * GDIPlatformWindows.cpp. So the ONLY thing that was ever missing
         * was this assignment -- without it VulkanDynamicGDI::Allocate()
         * early-outs at its `if(!VKCtx || !VKCtx->VKContext)` guard, leaving
         * a driver with no device, no swapchain and no sync that silently
         * discards every draw call, which is exactly what "the quad is not
         * visible" looked like from the outside.
         *
         * OpenGL and Metal genuinely DO still need a drawable this function
         * has no way to build -- a GL context wants an HDC made current on
         * THIS thread (and OpenGLDynamicGDI::Allocate bails before even
         * latching the render thread without one, which is why selecting it
         * fails startup outright rather than drawing nothing). Those keep
         * the old behaviour: no context, and Start's readiness check below
         * turns that into a clean startup failure rather than a fatal
         * GDI_CHECK three calls later.
         */
        if(InBackend == ERenderBackend::Null)
        {
            AssignContextGDI(new NullGDIContext());
        }
#if WITH_VULKAN_GDI
        else if(InBackend == ERenderBackend::Vulkan)
        {
            if(!InWindow.IsValid())
            {
                RENDERCORE_LOG_WARN("RenderThread::Start: the Vulkan backend needs a window to present "
                                     "to; the swapchain will fail to initialise")
            }
            AssignContextGDI(new VulkanGDIContext());
        }
#endif
        else if(!InWindow.IsValid())
        {
            RENDERCORE_LOG_WARN("RenderThread::Start: backend is not Null and InWindow has no native "
                                 "handle; the render thread will fail to allocate")
        }

        P->Backend = InBackend;

        P->bStopRequested.store(false, std::memory_order_release);
        P->bAllocateOK.store(false, std::memory_order_release);

        SetActiveRenderThread(this);

        /* DO NOT pass InOwningImage: the default argument IS the mechanism,
         * evaluating at this call site so GImageID resolves in RenderCore.dll
         * -- see CoreThread.h's own note on Thread::Start. High priority so
         * the render thread is not preempted by what feeds it. */
        if(!P->OSThread.Start("NimbleRender", [this]() { P->ThreadMain(); }, EngineCore::ThreadPriority::High))
        {
            RENDERCORE_LOG_ERROR("RenderThread::Start: failed to start the render thread")
            SetActiveRenderThread(nullptr);
            return false;
        }

        P->ReadyForUse.Wait();

        if(!P->bAllocateOK.load(std::memory_order_acquire))
        {
            RENDERCORE_LOG_ERROR("RenderThread::Start: the render thread failed to allocate GDI resources; "
                                  "the backend never latched a render thread")
            P->OSThread.Join();
            SetActiveRenderThread(nullptr);
            return false;
        }

        P->Context.SetBackend(InBackend);
        return true;
    }

    void RenderThread::Stop()
    {
        if(!IsRunning())
        {
            return;
        }

        Flush();

        /* FLAG FIRST, THEN WAKE -- reversed, the thread could consume the
         * signal, loop and re-sleep without seeing the flag. */
        P->bStopRequested.store(true, std::memory_order_release);
        P->Kick.Signal();

        P->OSThread.Join();
        P->ThreadID.store(0, std::memory_order_release);
        P->bFrameInFlight = false;

        SetActiveRenderThread(nullptr);
    }

    bool RenderThread::IsRunning() const
    {
        return P->OSThread.IsRunning();
    }

    EngineCore::ThreadID RenderThread::GetThreadID() const
    {
        return P->ThreadID.load(std::memory_order_acquire);
    }

    void RenderThread::Enqueue(RenderCommandHeader* InCommand)
    {
        P->Queue.Enqueue(InCommand);

        const size_t Depth = P->Queue.Size();
        uint32_t Peak = P->PeakQueueDepth.load(std::memory_order_relaxed);
        while(Depth > Peak && !P->PeakQueueDepth.compare_exchange_weak(Peak, uint32_t(Depth), std::memory_order_relaxed))
        {
        }
    }

    void RenderThread::EndFrame(const uint64_t InFrameNumber)
    {
        auto* Marker = EngineCore::Memory::Alloc<RenderCommandHeader>();
        Marker->Kind = RCK_FrameMarker;
        Marker->DebugName = "FrameMarker";
        Marker->OwningImage = EngineCore::GImageID;

        P->Context.SetFrameNumber(InFrameNumber);

        Enqueue(Marker);

        /* ALWAYS -- kicks and markers stay 1:1. Signal()ing only when
         * "something else is queued too" would let a kick with no matching
         * marker silently drop, and Semaphore::Signal's accumulation means a
         * later WaitForFrame() would then return for the WRONG frame --
         * NimbleEngine.cpp's own comment on this exact invariant, restated
         * for the marker design. */
        P->Kick.Signal();
        P->bFrameInFlight = true;
    }

    void RenderThread::WaitForFrame()
    {
        if(!P->bFrameInFlight)
        {
            return;
        }
        P->FrameDone.Wait();
        P->bFrameInFlight = false;
    }

    bool RenderThread::IsFrameInFlight() const
    {
        return P->bFrameInFlight;
    }

    void RenderThread::Flush()
    {
        if(!IsRunning())
        {
            return;
        }

        auto* Marker = EngineCore::Memory::Alloc<RenderCommandHeader>();
        Marker->Kind = RCK_FlushMarker;
        Marker->DebugName = "FlushMarker";
        Marker->OwningImage = EngineCore::GImageID;

        Enqueue(Marker);
        P->Kick.Signal();
        P->FlushDone.Wait();
    }

    RenderContext& RenderThread::GetContext()
    {
        return P->Context;
    }

    void RenderThread::GetStats(RenderStats& OutStats) const
    {
        OutStats.FramesRendered         = P->FramesRendered.load(std::memory_order_relaxed);
        OutStats.CommandsExecuted       = P->CommandsExecuted.load(std::memory_order_relaxed);
        OutStats.CommandsExecutedInline = GetInlineRenderCommandCount();
        OutStats.DrawCalls              = P->Context.GetDrawCallCount();
        GetSceneStats(OutStats.ProxiesLive, OutStats.ProxiesCulledLastFrame);
        OutStats.PeakQueueDepth         = P->PeakQueueDepth.load(std::memory_order_relaxed);
    }

    // === The locator (RenderThreadInternal.h) ==================================

    namespace
    {
        RenderThread* GActiveRenderThread = nullptr;

        /* Written only from inside RenderScene::Render() (render thread),
         * read only from RenderThread::GetStats() (any thread) -- a plain
         * pair rather than atomics would be a data race under that reading,
         * so both fields are atomic despite there being exactly one writer;
         * cheap, and correct without having to argue that "one writer" stays
         * true as this module grows more than one RenderScene. */
        std::atomic<uint64_t> GProxiesLive{0};
        std::atomic<uint64_t> GProxiesCulled{0};
    }

    void SetActiveRenderThread(RenderThread* InThread)
    {
        GActiveRenderThread = InThread;
    }

    RenderThread* GetActiveRenderThread()
    {
        return GActiveRenderThread;
    }

    void SetSceneStats(const uint64_t InProxiesLive, const uint64_t InProxiesCulled)
    {
        GProxiesLive.store(InProxiesLive, std::memory_order_relaxed);
        GProxiesCulled.store(InProxiesCulled, std::memory_order_relaxed);
    }

    void GetSceneStats(uint64_t& OutProxiesLive, uint64_t& OutProxiesCulled)
    {
        OutProxiesLive   = GProxiesLive.load(std::memory_order_relaxed);
        OutProxiesCulled = GProxiesCulled.load(std::memory_order_relaxed);
    }
}
