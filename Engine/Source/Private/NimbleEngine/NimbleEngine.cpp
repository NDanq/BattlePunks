#include "Public/NimbleEngine/NimbleEngine.h"

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/GameWindow/GameWindowSystem.h"
#include "Public/GameWindow/GameWindow.h"
#include "Public/GDI/GDI.h"
#include "Public/GDI/Null/NullDynamicGDI.h"

NIMBLE_CONFIG_INT(GResolutionX, "Display", "ResolutionX", 1280, nullptr);
NIMBLE_CONFIG_INT(GResolutionY, "Display", "ResolutionY", 720, nullptr);
NIMBLE_CONFIG_STRING(GTitle,    "Application", "Title", "NimbleEngine", nullptr);

/* Which backend InitializeGDI builds. ERP_Null draws nothing and needs no GPU,
 * which is what makes it the right one to bring the thread split up on: it
 * removes every variable except the threading itself. */
NIMBLE_CONFIG_INT(GRenderPlatform, "Display", "RHI", ERP_Null, nullptr);

/* Quit after this many frames; 0 runs until the window closes. Exists so the
 * loop, the handshake and the leak check can be exercised without a human
 * closing a window -- there is no other way to get a deterministic run out of
 * something whose exit condition is an OS event. */
NIMBLE_CONFIG_INT(GMaxFrames, "Application", "MaxFrames", 0, nullptr);

using namespace EngineCore;

namespace NimbleEngine
{
    bool Application::Initialize()
    {
        if(!GameWindowSystem::Initialize())
        {
            CORE_LOG_ERROR("Failed to Initialize Window")
            return false;
        }

        ConfigLoadStats Stats;
        if(!LoadConfigFile("Config/DefaultEngine.ini", &Stats))
        {
            CORE_LOG_WARN("No Config/DefaultEngine.ini Found")
        }

        WindowDesc Desc;
        Desc.Width = GResolutionX.GetInt();
        Desc.Height = GResolutionY.GetInt();
        Desc.Title = GTitle.GetString();
        Desc.Flags = WF_Resizable | WF_HighDPI;

        Window = GameWindowSystem::CreateGameWindow(Desc);
        if(!Window)
        {
            CORE_LOG_ERROR("Failed to Create Window")
            return false;
        }
        WindowID = Window->GetID();

        /*
         * ONCE, HERE, ON THIS THREAD. GetNativeHandle() asks SDL every time it
         * is called -- unlike the geometry accessors, which are cached reads --
         * so the handle is captured now and that copy is what everything else
         * uses. The render thread must never call back into GameWindow for it.
         */
        NativeWindow = Window->GetNativeHandle();
        if(!NativeWindow.IsValid())
        {
            CORE_LOG_ERROR("Window has no native handle; cannot initialize GDI")
            return false;
        }

        const ERenderPlatform Platform = ERenderPlatform(GRenderPlatform.GetInt());

        InitializeGDI(Platform, NativeWindow);
        bGDIInitialized = true;

        /*
         * Only the null backend can be handed a context from here. A GL or
         * Vulkan backend needs a real drawable (IGDIGLContext, a surface), which
         * is the host's to provide and does not exist yet -- so rather than
         * assign something the backend cannot use, leave it null and let
         * StartRenderThread's readiness check turn it into a clean startup
         * failure. See RenderThreadMain for why that check is not a tautology.
         */
        if(Platform == ERP_Null)
        {
            RenderContext = new NullGDIContext();
            AssignContextGDI(RenderContext);
        }
        else
        {
            CORE_LOG_WARN("RHI is not ERP_Null and no drawable is available; "
                          "the render thread will fail to allocate")
        }

        return StartRenderThread();
    }

    /*
     * ONE-FRAME LATENCY, WITH SUBMISSION FENCED.
     *
     * Simulation of frame N overlaps the render thread draining N-1. Submission
     * does not: the fence below guarantees the queue is empty and the render
     * thread is idle before any command for N is enqueued.
     *
     * WHY THE FENCE IS NOT OPTIONAL. The drain is `while(TryDequeue)` -- it has
     * to be, because GDI relies on commands enqueued from inside Execute()
     * running in the same frame (CoreQueue.h's DrainTo warning says so at
     * length). A drain-until-empty loop cannot tell frame N's commands from
     * N-1's. Enqueueing N while N-1 is still draining would let the drain
     * swallow both as one frame, and nothing would report it.
     *
     * Reaching full one-frame latency -- command BUILDING overlapping too --
     * needs either double-buffered command lists or a frame-boundary sentinel in
     * the queue. That is the same body of work as render proxies and belongs
     * with it. Only this fence changes; the drain, the handshake and the
     * shutdown path are already in their final shape.
     */
    void Application::Exec()
    {
        /* First two statements in the process's life as an engine. SetMainThread
         * before anything can default it, and the collector thread immediately
         * after -- both are first-caller-wins. */
        Threads::SetMainThread();
        Memory::SetCollectorThread();

        if(Initialize())
        {
            const int      MaxFramesCfg = GMaxFrames.GetInt();
            const uint64_t MaxFrames    = MaxFramesCfg > 0 ? uint64_t(MaxFramesCfg) : 0;

            while(!bQuitRequested)
            {
                /* PumpEvents' return value IS the exit condition; there is no
                 * separate ShouldQuit to forget. Delegates run inside here, on
                 * this thread. */
                if(!GameWindowSystem::PumpEvents())
                {
                    bQuitRequested = true;
                    break;
                }

                Simulate();

                if(bFrameInFlight)
                {
                    FrameDone.Wait();
                    bFrameInFlight = false;
                }

                /*
                 * THE SAFE POINT. The render thread is blocked in TryWaitFor
                 * touching nothing, and the queue is empty -- the closest a
                 * two-thread engine gets to a stopped world, which is what
                 * CollectGarbage documents as its requirement.
                 *
                 * This is the only sound LOCATION for it. How often it should
                 * actually run is a policy question for whoever tunes the frame,
                 * and a counter here is the place to answer it. When JobSystem
                 * is wired, WaitForIdle()/IsQuiescent() go immediately above.
                 */
                Memory::CollectGarbage();

                BuildRenderCommands();
                RenderKick.Signal();
                bFrameInFlight = true;

                /* PresentToWindow() would go here -- it is UI-thread by
                 * contract. Deliberately not wired: it is a no-op on the base
                 * and on GL, implemented only by Vulkan, and has no callers
                 * anywhere. SwapBuffers on the render thread is the other,
                 * different present, and the two are easy to get backwards. */

                ++FrameNumber;
                if(MaxFrames != 0 && FrameNumber >= MaxFrames)
                {
                    bQuitRequested = true;
                }
            }
        }
        else
        {
            CORE_LOG_ERROR("Failed to initialize Application!")
        }

        /* UNCONDITIONAL, and every step below is null-safe and idempotent.
         * Fewer branches than unwinding each failure path separately, and it
         * closes the old leak where a failed CreateGameWindow returned without
         * ever shutting SDL down. */
        Shutdown();
    }

    /*
     * Game state only. Runs CONCURRENTLY with the render thread executing the
     * previous frame's commands, so it must not enqueue, must not touch GDI, and
     * must not read anything a queued command captured by pointer -- which is
     * the reason commands capture by value.
     */
    void Application::Simulate()
    {
    }

    /*
     * Runs after the fence, with the render thread idle and the queue empty.
     *
     * EVERYTHING CAPTURED HERE IS CAPTURED BY VALUE. GetPixelSize() is read on
     * this thread because GameWindow's accessors are cached reads that
     * deliberately do NOT assert the pump thread -- a cross-thread call would be
     * an unsynchronised read that nothing catches.
     */
    void Application::BuildRenderCommands()
    {
        const Size  Viewport   = Window ? Window->GetPixelSize() : Size();
        const Color ClearColor = Color(24, 26, 32, 255);

        /* The macro declares a class right here and expands to a comma
         * expression, so the name must be unique in this scope and the statement
         * needs its semicolon. */
        ENQUEUE_RENDERCOMMAND_TWOPARAM(FrameClear,
            EngineCore::Color, CmdClearColor, ClearColor,
            EngineCore::Size,  CmdViewport,   Viewport,
        {
            GDISetViewport(0, 0, uint(CmdViewport.Width), uint(CmdViewport.Height));
            GDIClear(CM_Color | CM_Depth, CmdClearColor);
        });
    }

    bool Application::StartRenderThread()
    {
        bRenderStopRequested.store(false, std::memory_order_release);
        bRenderInitOK.store(false, std::memory_order_release);

        /*
         * DO NOT pass InOwningImage. Its default argument is the mechanism: it
         * is evaluated at this call site, so GImageID resolves in
         * NimbleEngine.dll rather than inside EngineCore.dll, which is the only
         * place it means anything. CoreThread.h's Start note spells this out.
         *
         * High priority because a render thread must not be preempted by the
         * work it is fed -- CoreThread.h names this exact case.
         */
        if(!RenderThread.Start("NimbleRender",
                               [this]() { RenderThreadMain(); },
                               ThreadPriority::High))
        {
            CORE_LOG_ERROR("Failed to start the render thread")
            return false;
        }

        bRenderThreadLive = true;

        /* Blocks until AllocateResourcesGDI() has run over there. */
        RenderReady.Wait();

        if(!bRenderInitOK.load(std::memory_order_acquire))
        {
            CORE_LOG_ERROR("Render thread failed to allocate GDI resources; "
                           "the backend never latched a render thread")
            return false;
        }

        return true;
    }

    void Application::StopRenderThread()
    {
        if(!bRenderThreadLive)
        {
            return;
        }

        /*
         * FLAG FIRST, THEN WAKE. Reversed, the thread could consume the signal,
         * loop, and go back to sleep without ever seeing the flag. TryWaitFor
         * bounds that anyway -- worst case it exits 2ms late -- but relying on
         * the timeout to paper over an ordering bug is how the timeout stops
         * being a safety net.
         */
        bRenderStopRequested.store(true, std::memory_order_release);
        RenderKick.Signal();

        RenderThread.Join();

        bRenderThreadLive = false;
        bFrameInFlight    = false;
    }

    void Application::Shutdown()
    {
        StopRenderThread();

        /*
         * Only reachable when the render thread never ran -- it releases GDI
         * itself otherwise, and ReleaseGDI nulls GDynamicGDI. Nothing was
         * allocated on this path (Allocate() only ever runs over there), so
         * releasing from the main thread here frees no GPU objects.
         */
        if(bGDIInitialized && GDynamicGDI)
        {
            GDynamicGDI->Destroy();
            ReleaseGDI();
        }
        bGDIInitialized = false;

        /* ReleaseGDI deletes the driver and GSync; the context was always ours. */
        delete RenderContext;
        RenderContext = nullptr;

        /*
         * RE-RESOLVED THROUGH THE ID, never destroyed through the stored
         * pointer. PumpEvents reaps windows the user closed, so the ordinary
         * exit -- the user closing the window -- is exactly the case where
         * `Window` is already dangling by the time control reaches here.
         * Passing it to DestroyGameWindow is a use-after-free that only looks
         * harmless because the system happens to check its own registry first
         * and warn. Asking by id is the version that is correct rather than
         * merely unpunished.
         *
         * Main thread is the pump thread, which is where this is required.
         */
        if(WindowID != 0)
        {
            if(GameWindow* LiveWindow = GameWindowSystem::FindWindow(WindowID))
            {
                GameWindowSystem::DestroyGameWindow(LiveWindow);
            }
            WindowID = 0;
        }
        Window = nullptr;

        /* Refcounted, and returns immediately when it was never initialized. */
        GameWindowSystem::Shutdown();
    }

    /* ====================== RENDER THREAD FROM HERE ON ====================== */

    void Application::RenderThreadMain()
    {
        /*
         * THE LATCH, and the single most important line in this file. Whichever
         * thread calls this BECOMES the render thread as far as
         * GDIInRenderThread() is concerned, because the backend records
         * appThreadID() inside Allocate(). GDIRenderTarget.cpp guards a fatal
         * GDI_CHECK on that predicate.
         */
        AllocateResourcesGDI();

        /*
         * A real readiness test rather than a restatement of "we called
         * Allocate". A GL backend's Allocate() early-outs when it has no
         * drawable and never reaches SetThreadID, which would leave that fatal
         * check armed for the whole run and fire it frames later with nothing
         * pointing back here. Asking the predicate itself is what turns that
         * into a startup failure.
         */
        const bool bOK = GDIInRenderThread();
        bRenderInitOK.store(bOK, std::memory_order_release);
        RenderReady.Signal();

        if(bOK)
        {
            while(!bRenderStopRequested.load(std::memory_order_acquire))
            {
                /*
                 * BOUNDED, never Wait(). A worker idles on the bounded form so
                 * the stop flag is noticed even if a signal was lost or consumed
                 * by someone else -- CoreThread.h states this as the rule. It
                 * also makes shutdown deadlock-proof no matter how flag and
                 * signal interleave.
                 */
                if(RenderKick.TryWaitFor(2))
                {
                    DrainRenderCommands();

                    /* Render-thread present, targeting the offscreen drawable.
                     * NOT PresentToWindow, which is the UI thread's. */
                    GDISwapBuffers();

                    /*
                     * ONLY when a kick was actually consumed. Semaphore::Signal
                     * ACCUMULATES, so signalling on a timeout tick would bank a
                     * count and let the next FrameDone.Wait() return instantly
                     * -- silently unfencing submission, with nothing to catch
                     * it. Kicks consumed and frames signalled are 1:1.
                     */
                    FrameDone.Signal();
                }
            }
        }

        /* Stragglers: commands enqueued for a frame that will never be kicked.
         * Nothing else deletes them. */
        DrainRenderCommands();

        /* ReleaseGDI deletes the driver but does not call Destroy() on it
         * first, so GPU objects would outlive their owner. Both belong on the
         * thread that owns the context. */
        if(GDynamicGDI)
        {
            GDynamicGDI->Destroy();
        }
        ReleaseGDI();
    }

    void Application::DrainRenderCommands()
    {
        /*
         * The brackets go around the WHOLE loop, not each command. GDI.cpp's
         * thread_local in-drain flag is what makes an enqueue issued from inside
         * an Execute() distinguishable from an ordinary game-thread one, and
         * per-command bracketing would erase exactly that.
         */
        GDIRenderQueue_NoteDrain();

        GDIRenderCommandBase* Command = nullptr;
        while(GRenderCommands.TryDequeue(Command))
        {
            Command->Execute();

            /* Nothing else deletes these. Sound only because
             * GDIRenderCommandBase has a virtual destructor. */
            delete Command;
        }

        GDIRenderQueue_NoteDrainEnd();
    }
};
