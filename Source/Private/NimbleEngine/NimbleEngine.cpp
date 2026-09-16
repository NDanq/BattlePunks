#include "Public/NimbleEngine/NimbleEngine.h"

#include <chrono>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/GameWindow/GameWindowSystem.h"
#include "Public/GameWindow/GameWindow.h"
#include "Public/GDI/GDI.h"
#include "Public/GDI/Null/NullDynamicGDI.h"

#if defined(_WIN32)
/*
 * DECLARED, NOT INCLUDED. <windows.h> defines Yield() as an empty macro and
 * maps FindWindow to FindWindowA; this file calls Threads::Yield() and
 * GameWindowSystem::FindWindow, so including it breaks both.
 * WIN32_LEAN_AND_MEAN does not help -- both live in the core headers.
 * MMRESULT and UINT are unsigned int; TIMERR_NOERROR is 0.
 */
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int uPeriod);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int uPeriod);
#endif

NIMBLE_CONFIG_INT(GResolutionX, "Display", "ResolutionX", 1280, nullptr);
NIMBLE_CONFIG_INT(GResolutionY, "Display", "ResolutionY", 720, nullptr);
NIMBLE_CONFIG_STRING(GTitle,    "Application", "Title", "NimbleEngine", nullptr);

/* ERP_Null draws nothing and needs no GPU, which removes every variable except
 * the threading. Games/Sandbox/Config/DefaultEngine.ini documents all three. */
NIMBLE_CONFIG_INT(GRenderPlatform, "Display", "RHI", ERP_Null, nullptr);

/* 0 runs until the window closes. Non-zero is the only way to get a
 * deterministic run out of something whose exit condition is an OS event. */
NIMBLE_CONFIG_INT(GMaxFrames, "Application", "MaxFrames", 0, nullptr);

/* 0 runs unlimited; SetFrameRateLimit is the runtime half. */
NIMBLE_CONFIG_INT(GFrameRateLimit, "Display", "FrameRateLimit", 60, nullptr);

/* Bundle-relative path to a Lua file run once, after IGame::OnStartup succeeds
 * and before the first tick -- see ExecWithGame's own comment on why that
 * ordering, not Initialize(), is where this runs. Empty (the default) skips
 * scripting entirely; a game with nothing to say in Lua pays nothing for this. */
NIMBLE_CONFIG_STRING(GStartupScript, "Scripting", "StartupScript", "", nullptr);

using namespace EngineCore;

namespace
{
    /*
     * Raises the OS timer resolution, lazily, so an unlimited run never pays
     * for it. Lowered again when this leaves scope.
     *
     * MEASURED, because the numbers are the whole justification: at the default
     * ~15.6ms Windows tick, SleepMs(1), SleepMs(2) and SleepMs(5) ALL sleep
     * about 15.5ms, so a 60fps frame would land on 15.6 or 31.2 and alternate
     * between roughly 64 and 32fps. With timeBeginPeriod(1) they measure 1.99,
     * 2.94 and 5.94ms -- about 1ms of overshoot, which PaceToDeadline's spin
     * absorbs. Here rather than in Core only because Core is a vendored subtree
     * that must not require local edits.
     */
    class TimerResolutionScope
    {
    public:
        TimerResolutionScope() = default;

        TimerResolutionScope(const TimerResolutionScope&)            = delete;
        TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;

        void EnsureRaised()
        {
#if defined(_WIN32)
            /* TIMERR_NOERROR is 0; see the declaration at the top of this
             * file for why the constant is not spelled by name. */
            if(!bRaised && timeBeginPeriod(1) == 0)
            {
                bRaised = true;
            }
#endif
        }

        ~TimerResolutionScope()
        {
#if defined(_WIN32)
            if(bRaised)
            {
                timeEndPeriod(1);
            }
#endif
        }

    private:
        bool bRaised = false;
    };

    int64_t SteadyNowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /* Waits until InOutDeadlineNs, then advances it by one frame. SLEEP THE
     * BULK, SPIN THE TAIL: a pure spin burns the core pacing exists to give
     * back, a pure sleep overshoots by ~1ms -- 6% of a 60fps frame. */
    void PaceToDeadline(int64_t& InOutDeadlineNs, uint32_t InLimitHz, TimerResolutionScope& InTimer)
    {
        if(InLimitHz == 0)
        {
            /* Forget the deadline, so switching back to a limit starts from now
             * rather than catching up on frames that were never paced. */
            InOutDeadlineNs = 0;
            return;
        }

        const int64_t FrameNs = 1000000000LL / int64_t(InLimitHz);
        const int64_t NowNs   = SteadyNowNs();

        /* First paced frame, or the first after a spell of running unlimited. */
        if(InOutDeadlineNs == 0)
        {
            InOutDeadlineNs = NowNs + FrameNs;
            return;
        }

        /* MORE THAN A WHOLE FRAME LATE: start again from now. Advancing a
         * deadline already in the past keeps it there, so every later frame
         * would skip its wait chasing a rate the engine cannot meet. */
        if(NowNs > InOutDeadlineNs + FrameNs)
        {
            InOutDeadlineNs = NowNs + FrameNs;
            return;
        }

        /* 2ms, sized from the ~1ms sleep overshoot measured above. */
        const int64_t SpinWindowNs = 2000000;

        InTimer.EnsureRaised();

        for(;;)
        {
            const int64_t RemainingNs = InOutDeadlineNs - SteadyNowNs();
            if(RemainingNs <= 0)
            {
                break;
            }

            const int64_t SleepableNs = RemainingNs - SpinWindowNs;
            if(SleepableNs >= 1000000)
            {
                Threads::SleepMs(uint32_t(SleepableNs / 1000000));
            }
            else
            {
                Threads::Yield();
            }
        }

        InOutDeadlineNs += FrameNs;
    }
}

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

        /* ONCE, HERE, ON THIS THREAD. GetNativeHandle() is a live SDL query,
         * not a cached read like the geometry accessors. Everything else uses
         * this copy; the render thread never calls back into GameWindow. */
        NativeWindow = Window->GetNativeHandle();
        if(!NativeWindow.IsValid())
        {
            CORE_LOG_ERROR("Window has no native handle; cannot initialize GDI")
            return false;
        }

        /*
         * The scene and its Lua layer. Core allocations (Memory::Alloc, not
         * plain new) so a missed Dealloc at teardown is exactly the kind of
         * thing Sandbox's ReportLeaks() already catches at process exit.
         *
         * Before GDI on purpose: nothing here touches a window or a render
         * context, and BindWorld installs the script-tick systems into Scene
         * before anything could conceivably tick it (the render thread is not
         * up yet either). Ordering relative to GDI is otherwise arbitrary.
         */
        Scene = EngineCore::Memory::Alloc<GameCore::World>();
        Scripts = EngineCore::Memory::Alloc<GameCore::ScriptEngine>();
        if(!Scripts->Initialize())
        {
            CORE_LOG_ERROR("Failed to initialize the script engine")
            return false;
        }
        Scripts->BindWorld(Scene);

        const ERenderPlatform Platform = ERenderPlatform(GRenderPlatform.GetInt());

        InitializeGDI(Platform, NativeWindow);
        bGDIInitialized = true;

        /* Only the null backend can be handed a context from here; GL and
         * Vulkan need a real drawable the host does not provide yet. Leaving it
         * null lets StartRenderThread's readiness check turn that into a clean
         * startup failure -- see RenderThreadMain for why it is not a
         * tautology. */
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
     * WHY SUBMISSION IS FENCED (the topology itself is in NimbleEngine.h).
     * The drain must be `while(TryDequeue)` -- GDI relies on commands enqueued
     * from inside Execute() running the same frame, per CoreQueue.h's DrainTo
     * warning -- and such a loop cannot tell frame N's commands from N-1's, so
     * enqueueing N mid-drain would swallow both as one frame silently.
     *
     * Overlapping command BUILDING too is the render-proxy work; only this
     * fence changes when that lands.
     */
    void Application::Exec()
    {
        ExecWithGame(nullptr);
    }

    void Application::Exec(IGame& InGame)
    {
        ExecWithGame(&InGame);
    }

    void Application::RequestQuit()
    {
        bQuitRequested = true;
    }

    void Application::SetFrameRateLimit(uint32_t InFramesPerSecond)
    {
        FrameRateLimit = InFramesPerSecond;

        /* Start the new rate from the next frame rather than inheriting a
         * schedule computed for the old one. */
        NextFrameDeadlineNs = 0;
    }

    uint32_t Application::GetFrameRateLimit() const
    {
        return FrameRateLimit;
    }

    GameCore::World& Application::GetWorld()
    {
        CORE_CHECK(Scene != nullptr, "Application::GetWorld() called before Initialize() or after Shutdown()")
        return *Scene;
    }

    GameCore::ScriptEngine& Application::GetScriptEngine()
    {
        CORE_CHECK(Scripts != nullptr, "Application::GetScriptEngine() called before Initialize() or after Shutdown()")
        return *Scripts;
    }

    void Application::ExecWithGame(IGame* InGame)
    {
        /* First, before anything defaults them. Both are first-caller-wins. */
        Threads::SetMainThread();
        Memory::SetCollectorThread();

        if(Initialize())
        {
            const int      MaxFramesCfg = GMaxFrames.GetInt();
            const uint64_t MaxFrames    = MaxFramesCfg > 0 ? uint64_t(MaxFramesCfg) : 0;

            /* Negative clamps to unlimited rather than wrapping unsigned. */
            const int LimitCfg  = GFrameRateLimit.GetInt();
            FrameRateLimit      = LimitCfg > 0 ? uint32_t(LimitCfg) : 0u;
            NextFrameDeadlineNs = 0;

            TimerResolutionScope TimerRes;

            /* After the render thread is up, so a game never observes a
             * half-built engine. A refusal is a clean shutdown. */
            if(InGame && !InGame->OnStartup(*this))
            {
                CORE_LOG_ERROR("Game refused to start")
                bQuitRequested = true;
            }

            /*
             * AFTER OnStartup succeeds, not in Initialize() -- a startup
             * script sees a started game (its own entities, its own bound
             * config), not a half-built engine mid-construction. Skipped
             * entirely when empty, which is the default: a game with nothing
             * to say in Lua pays nothing for this. A failed script is logged
             * by ScriptEngine itself and is NOT fatal to the engine -- the
             * same "log and continue" stance CORE_LOG_WARN already takes for
             * a missing DefaultEngine.ini just above in Initialize().
             */
            if(!bQuitRequested)
            {
                const char* StartupScriptPath = GStartupScript.GetString();
                if(StartupScriptPath && StartupScriptPath[0] != '\0')
                {
                    Scripts->RunFile(StartupScriptPath);
                }
            }

            /* steady_clock: a delta must not go backwards because the wall
             * clock was corrected mid-session. */
            using Clock = std::chrono::steady_clock;
            Clock::time_point PreviousTick = Clock::now();

            while(!bQuitRequested)
            {
                /* PumpEvents' return IS the exit condition; delegates run
                 * inside it, on this thread. */
                if(!GameWindowSystem::PumpEvents())
                {
                    bQuitRequested = true;
                    break;
                }

                /* Assembled on the game thread before anything overlaps; see
                 * FrameContext for why the viewport is read here. */
                const Clock::time_point Now = Clock::now();

                FrameContext Frame;
                Frame.DeltaSeconds  = FrameNumber == 0
                                    ? 0.0
                                    : std::chrono::duration<double>(Now - PreviousTick).count();
                Frame.FrameNumber   = FrameNumber;
                Frame.ViewportPixels = Window ? Window->GetPixelSize() : Size();
                PreviousTick = Now;

                Simulate(InGame, Frame);

                if(bFrameInFlight)
                {
                    FrameDone.Wait();
                    bFrameInFlight = false;
                }

                /* THE SAFE POINT, and the only sound location: render thread
                 * blocked in TryWaitFor, queue empty -- the stopped world
                 * CollectGarbage requires. Cadence is a separate question.
                 * JobSystem's WaitForIdle()/IsQuiescent() go just above. */
                Memory::CollectGarbage();

                BuildRenderCommands();
                RenderKick.Signal();
                bFrameInFlight = true;

                /* PresentToWindow() would go here -- UI-thread by contract,
                 * and deliberately not wired: no-op on the base and on GL,
                 * Vulkan-only, no callers. SwapBuffers on the render thread is
                 * the OTHER present; the two are easy to get backwards. */

                ++FrameNumber;
                if(MaxFrames != 0 && FrameNumber >= MaxFrames)
                {
                    bQuitRequested = true;
                }

                /* LAST IN THE FRAME, so the wait overlaps the render thread
                 * still executing what was just submitted. Skipped on the way
                 * out. */
                if(!bQuitRequested)
                {
                    PaceToDeadline(NextFrameDeadlineNs, FrameRateLimit, TimerRes);
                }
            }
        }
        else
        {
            CORE_LOG_ERROR("Failed to initialize Application!")
        }

        /* Paired with the game EXISTING, not with OnStartup succeeding: a game
         * that refused to start still undoes what it did while deciding. */
        if(InGame)
        {
            InGame->OnShutdown();
        }

        /* UNCONDITIONAL; every step below is null-safe and idempotent. Closes
         * the old leak where a failed CreateGameWindow never shut SDL down. */
        Shutdown();
    }

    /* Runs CONCURRENTLY with the render thread executing the previous frame:
     * no enqueues, no GDI. See IGame::OnTick. */
    void Application::Simulate(IGame* InGame, const FrameContext& InFrame)
    {
        if(InGame)
        {
            InGame->OnTick(InFrame);
        }

        /*
         * The game acts FIRST -- it may spawn or destroy something this
         * frame -- then World's own phases run: TP_PrePhysics, TP_Physics
         * (+ the spatial refit at its end), TP_PostPhysics, TP_PostUpdate.
         * Still the game thread, still before the render-thread fence below
         * in ExecWithGame -- Scene never touches GDI, so nothing here needs
         * to wait on the render thread any more than IGame::OnTick already
         * does not.
         */
        if(Scene)
        {
            Scene->Tick(float(InFrame.DeltaSeconds));
        }
    }

    /* Runs after the fence, render thread idle and queue empty. EVERYTHING
     * HERE IS CAPTURED BY VALUE -- a captured pointer would be a live race
     * against the next frame's simulation. */
    void Application::BuildRenderCommands()
    {
        const Size  Viewport   = Window ? Window->GetPixelSize() : Size();
        const Color ClearColor = Color(24, 26, 32, 255);

        /* The macro declares a class at the call site and expands to a comma
         * expression: the name must be unique here, and the semicolon is
         * required. */
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

        /* DO NOT pass InOwningImage: the default argument IS the mechanism,
         * evaluating at this call site so GImageID resolves in NimbleEngine.dll
         * rather than EngineCore.dll. See CoreThread.h's Start note. High
         * priority so the render thread is not preempted by what it feeds. */
        if(!RenderThread.Start("NimbleRender",
                               [this]() { RenderThreadMain(); },
                               ThreadPriority::High))
        {
            CORE_LOG_ERROR("Failed to start the render thread")
            return false;
        }

        bRenderThreadLive = true;

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

        /* FLAG FIRST, THEN WAKE. Reversed, the thread could consume the
         * signal, loop and re-sleep without seeing the flag. TryWaitFor bounds
         * that, but leaning on the timeout is how it stops being a safety
         * net. */
        bRenderStopRequested.store(true, std::memory_order_release);
        RenderKick.Signal();

        RenderThread.Join();

        bRenderThreadLive = false;
        bFrameInFlight    = false;
    }

    void Application::Shutdown()
    {
        /*
         * UNCONDITIONAL, like everything below -- Shutdown() runs whether
         * Initialize() got all the way through or failed partway, so every
         * step here is null-safe. ORDER IS LOAD-BEARING:
         *
         *   1. Scripts->UnbindWorld() -- runs every live script's OnDestroy
         *      and releases its instance ref, unregisters the script-tick
         *      systems and the ScriptComponent removal observer from Scene.
         *      Needs BOTH a live lua_State (Scripts) AND a live Scene to walk.
         *   2. Dealloc(Scene) -- Scene's own destructor logs a warning for any
         *      of its built-in delegates still holding a binding (see
         *      World.cpp's ~Impl), which by this point should be none, since
         *      step 1 already unregistered GameCore's own.
         *   3. Scripts->Shutdown() -- lua_close. Must come AFTER Scene is
         *      gone: nothing left holds a World* into a destroyed object.
         *   4. Dealloc(Scripts).
         *
         * Reversed, step 3 before step 1 closes the Lua state while
         * ScriptComponent instances still hold registry refs into it, and
         * UnbindWorld's OnDestroy calls would run against a dead lua_State.
         */
        if(Scripts && Scene)
        {
            Scripts->UnbindWorld();
        }
        if(Scene)
        {
            EngineCore::Memory::Dealloc(Scene);
            Scene = nullptr;
        }
        if(Scripts)
        {
            Scripts->Shutdown();
            EngineCore::Memory::Dealloc(Scripts);
            Scripts = nullptr;
        }

        StopRenderThread();

        /* Only reachable when the render thread never ran; it releases GDI
         * itself otherwise. Nothing was allocated on this path, so releasing
         * from the main thread frees no GPU objects. */
        if(bGDIInitialized && GDynamicGDI)
        {
            GDynamicGDI->Destroy();
            ReleaseGDI();
        }
        bGDIInitialized = false;

        /* ReleaseGDI deletes the driver and GSync; the context was always ours. */
        delete RenderContext;
        RenderContext = nullptr;

        /* RE-RESOLVED THROUGH THE ID, never through the stored pointer:
         * PumpEvents reaps windows the user closed, so the ordinary exit is
         * exactly when `Window` is already dangling. Passing it to
         * DestroyGameWindow is a use-after-free that merely goes unpunished. */
        if(WindowID != 0)
        {
            if(GameWindow* LiveWindow = GameWindowSystem::FindWindow(WindowID))
            {
                GameWindowSystem::DestroyGameWindow(LiveWindow);
            }
            WindowID = 0;
        }
        Window = nullptr;

        GameWindowSystem::Shutdown();
    }

    /* ====================== RENDER THREAD FROM HERE ON ====================== */

    void Application::RenderThreadMain()
    {
        /*
         * THE LATCH, and the most important line in this file: whichever thread
         * calls this BECOMES the render thread for GDIInRenderThread(), because
         * the backend records appThreadID() inside Allocate().
         * GDIRenderTarget.cpp guards a FATAL GDI_CHECK on that predicate.
         */
        AllocateResourcesGDI();

        /* A real test, not a restatement of "we called Allocate": a GL
         * backend with no drawable early-outs before SetThreadID, leaving that
         * fatal check armed to fire frames later. */
        const bool bOK = GDIInRenderThread();
        bRenderInitOK.store(bOK, std::memory_order_release);
        RenderReady.Signal();

        if(bOK)
        {
            while(!bRenderStopRequested.load(std::memory_order_acquire))
            {
                /* BOUNDED, never Wait(): the stop flag is noticed even if a
                 * signal was lost or consumed, which makes shutdown
                 * deadlock-proof however flag and signal interleave. */
                if(RenderKick.TryWaitFor(2))
                {
                    DrainRenderCommands();

                    /* Render-thread present, offscreen. NOT PresentToWindow,
                     * which is the UI thread's. */
                    GDISwapBuffers();

                    /*
                     * ONLY when a kick was consumed. Semaphore::Signal
                     * ACCUMULATES, so signalling on a timeout tick banks a
                     * count and the next FrameDone.Wait() returns instantly --
                     * silently unfencing submission. Kicks and frames are 1:1.
                     */
                    FrameDone.Signal();
                }
            }
        }

        /* Stragglers from a frame that will never be kicked; nothing else
         * deletes them. */
        DrainRenderCommands();

        /* ReleaseGDI deletes the driver WITHOUT calling Destroy(), so GPU
         * objects would outlive their owner. Both belong on the context's
         * thread. */
        if(GDynamicGDI)
        {
            GDynamicGDI->Destroy();
        }
        ReleaseGDI();
    }

    void Application::DrainRenderCommands()
    {
        /* Brackets the WHOLE loop, not each command: GDI.cpp's thread_local
         * in-drain flag is what distinguishes an enqueue issued from inside an
         * Execute(), and per-command bracketing would erase that. */
        GDIRenderQueue_NoteDrain();

        GDIRenderCommandBase* Command = nullptr;
        while(GRenderCommands.TryDequeue(Command))
        {
            Command->Execute();

            /* Sound only because GDIRenderCommandBase has a virtual
             * destructor. */
            delete Command;
        }

        GDIRenderQueue_NoteDrainEnd();
    }
};
