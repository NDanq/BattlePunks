#include "Public/NimbleEngine/NimbleEngine.h"

#include <chrono>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreMemory.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/GameWindow/GameWindowSystem.h"
#include "Public/GameWindow/GameWindow.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/Renderer.h"

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

/* RenderCore::ERenderBackend::Null draws nothing and needs no GPU, which
 * removes every variable except the threading. Games/Sandbox/Config/
 * DefaultEngine.ini documents all five backends -- see GDIDefinitions.h's own
 * comment on why Null is zero. */
NIMBLE_CONFIG_INT(GRenderPlatform, "Display", "RHI", int(RenderCore::ERenderBackend::Null), nullptr);

/* 0 runs until the window closes. Non-zero is the only way to get a
 * deterministic run out of something whose exit condition is an OS event. */
NIMBLE_CONFIG_INT(GMaxFrames, "Application", "MaxFrames", 0, nullptr);

/* 0 runs unlimited; SetFrameRateLimit is the runtime half. */
NIMBLE_CONFIG_INT(GFrameRateLimit, "Display", "FrameRateLimit", 60, nullptr);

/* Bundle-relative path to a Lua file run once, after IGameMode::OnLoad succeeds
 * and before the first tick -- see LoadAndStartMode's own comment on why that
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

        /*
         * THE RESOURCE SEAM, installed before anything can ask for an asset.
         *
         * CoreFileProvider.h's own comment describes FilesystemProvider as
         * "what the smoke tests and a non-Qt host use" -- this engine is
         * that non-Qt host, and until now it simply never installed one, so
         * EngineCore::GetFileProvider() returned null and every asset read
         * failed. That was invisible while the only backend that started was
         * Null (which never loads a shader), and surfaced the moment Vulkan
         * came up for real as:
         *
         *   [VKShaderResource] No file provider installed for:
         *   Shaders/Cooked/Vulkan/Basic.vsh.spv
         *
         * Empty base directory: paths handed to a provider are
         * bundle-relative ("Shaders/Cooked/Vulkan/Basic.vsh.spv"), and the
         * working directory is already the one holding the executable and
         * its staged Config/ and Scripts/ -- the same assumption
         * LoadConfigFile's own relative path just below makes, and that
         * Games/Sandbox/CMakeLists.txt's staging steps and
         * VS_DEBUGGER_WORKING_DIRECTORY are built around.
         *
         * OWNED BY THE APPLICATION, as a member rather than a local or a
         * static: the provider must outlive every subsystem that might read
         * through it (GDI's shader loads run on the render thread, well
         * after this function returns), and Shutdown() clears the locator
         * before it goes away -- a dangling IFileProvider* is the same
         * hazard CoreJob.h's own locator warns about.
         */
        SetFileProvider(&AssetProvider);

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
            CORE_LOG_ERROR("Window has no native handle; cannot initialize the renderer")
            return false;
        }

        /*
         * The scene and its Lua layer. Core allocations (Memory::Alloc, not
         * plain new) so a missed Dealloc at teardown is exactly the kind of
         * thing Sandbox's ReportLeaks() already catches at process exit.
         *
         * Before the renderer on purpose: nothing here touches a window or a
         * render context, and BindWorld installs the script-tick systems into
         * Scene before anything could conceivably tick it (the render thread
         * is not up yet either). Ordering relative to the renderer is
         * otherwise arbitrary.
         */
        Scene = EngineCore::Memory::Alloc<GameCore::World>();
        Scripts = EngineCore::Memory::Alloc<GameCore::ScriptEngine>();
        if(!Scripts->Initialize())
        {
            CORE_LOG_ERROR("Failed to initialize the script engine")
            return false;
        }
        Scripts->BindWorld(Scene);

        const RenderCore::ERenderBackend Backend = RenderCore::ERenderBackend(GRenderPlatform.GetInt());

        Rendering = EngineCore::Memory::Alloc<RenderCore::Renderer>();
        if(!Rendering->Initialize(Backend, NativeWindow))
        {
            CORE_LOG_ERROR("Failed to initialize RenderCore")
            return false;
        }
        Rendering->BindWorld(*Scene);

        return true;
    }

    void Application::Exec()
    {
        ExecWithMode(nullptr);
    }

    void Application::Exec(IGameMode& InMode)
    {
        ExecWithMode(&InMode);
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

    RenderCore::Renderer& Application::GetRenderer()
    {
        CORE_CHECK(Rendering != nullptr, "Application::GetRenderer() called before Initialize() or after Shutdown()")
        return *Rendering;
    }

    bool Application::LoadAndStartMode(IGameMode* InMode)
    {
        /* After the renderer is up, so a mode never observes a half-built
         * engine. A refusal is a clean shutdown, not an error. */
        if(InMode && !InMode->OnLoad(*this))
        {
            CORE_LOG_ERROR("Game mode refused to load")
            return false;
        }

        /*
         * BETWEEN THE TWO MODE CALLBACKS, and that placement is the whole
         * distinction between them -- see IGameMode's lifecycle comment. A
         * startup script sees a loaded mode (its own entities, its own bound
         * config), not a half-built engine mid-construction; and OnStart below
         * sees whatever the script created.
         *
         * Skipped entirely when empty, which is the default: a game with
         * nothing to say in Lua pays nothing for this. A failed script is
         * logged by ScriptEngine itself and is NOT fatal to the engine -- the
         * same "log and continue" stance CORE_LOG_WARN already takes for a
         * missing DefaultEngine.ini in Initialize().
         */
        const char* StartupScriptPath = GStartupScript.GetString();
        if(StartupScriptPath && StartupScriptPath[0] != '\0')
        {
            Scripts->RunFile(StartupScriptPath);
        }

        if(InMode)
        {
            InMode->OnStart();
        }

        return true;
    }

    bool Application::RunFrame(IGameMode* InMode, const uint64_t InMaxFrames)
    {
        /* steady_clock: a delta must not go backwards because the wall clock
         * was corrected mid-session. */
        using Clock = std::chrono::steady_clock;

        /* PumpEvents' return IS the exit condition; delegates run inside it,
         * on this thread. */
        if(!GameWindowSystem::PumpEvents())
        {
            return false;
        }

        /* Assembled on the game thread before anything overlaps; see
         * FrameContext for why the viewport is read here. */
        const int64_t NowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  Clock::now().time_since_epoch()).count();

        FrameContext Frame;
        Frame.DeltaSeconds   = FrameNumber == 0
                             ? 0.0
                             : double(NowNs - PreviousTickNs) / 1'000'000'000.0;
        Frame.FrameNumber    = FrameNumber;
        Frame.ViewportPixels = Window ? Window->GetPixelSize() : Size();
        PreviousTickNs       = NowNs;

        Simulate(InMode, Frame);

        /* THE FENCE: at most one frame in flight. A mode may have
         * ENQUEUE_RENDER_COMMAND'd at any point during Simulate() above --
         * see NimbleEngine.h's own comment on the frame marker mechanism that
         * makes that safe -- so this waits for the PREVIOUS frame's marker,
         * not for the queue to be empty of everything ever enqueued. */
        Rendering->WaitForPreviousFrame();

        /* THE SAFE POINT, and the only sound location: the render thread is
         * idle here (WaitForPreviousFrame just returned) and never touches a
         * TRef at all -- RenderCommand.h's capture rule keeps it that way --
         * so nothing outside this call stack can be mutating a TRef while this
         * runs. Cadence is a separate question from location. */
        Memory::CollectGarbage();

        /*
         * PUTS THE PREVIOUS FRAME ON SCREEN, and the only place in the loop
         * that touches the window's pixels.
         *
         * HERE, specifically: WaitForPreviousFrame() above has just returned,
         * so the frame being presented is complete and submitted -- which is
         * precisely GDI's own stated precondition ("Called from the UI thread
         * after RendererFinished()", GDI.h). Any earlier and there would be
         * nothing finished to present; any later and it would race the frame
         * EndFrame() is about to start building.
         *
         * GAME THREAD BY DESIGN, not by accident -- GDI's present path keeps
         * its own command pool and semaphores so a second thread can record
         * and submit it alongside the render thread; see
         * Renderer::PresentToWindow's own comment.
         *
         * NativeWindow, not Window->GetNativeHandle(): the handle was captured
         * once in Initialize() precisely because GetNativeHandle() is a live
         * SDL query rather than a cached read -- see this class's own member
         * comment.
         */
        Rendering->PresentToWindow(NativeWindow);

        RenderCore::FrameInfo Info;
        Info.FrameNumber    = FrameNumber;
        Info.DeltaSeconds   = Frame.DeltaSeconds;
        Info.ViewportPixels = Frame.ViewportPixels;
        Rendering->EndFrame(Info);

        ++FrameNumber;
        if(InMaxFrames != 0 && FrameNumber >= InMaxFrames)
        {
            return false;
        }

        /* A mode may have called RequestQuit() from inside OnTick. */
        return !bQuitRequested;
    }

    void Application::ExecWithMode(IGameMode* InMode)
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

            if(!LoadAndStartMode(InMode))
            {
                bQuitRequested = true;
            }

            while(!bQuitRequested)
            {
                if(!RunFrame(InMode, MaxFrames))
                {
                    bQuitRequested = true;
                    break;
                }

                /* LAST IN THE FRAME, so the wait overlaps the render thread
                 * still executing what was just submitted. Outside RunFrame
                 * because it is skipped entirely on the way out -- the break
                 * above is what makes that true. */
                PaceToDeadline(NextFrameDeadlineNs, FrameRateLimit, TimerRes);
            }
        }
        else
        {
            CORE_LOG_ERROR("Failed to initialize Application!")
        }

        /* Paired with the mode EXISTING, not with OnLoad succeeding: a mode
         * that refused to load still undoes what it did while deciding. */
        if(InMode)
        {
            InMode->OnUnload();
        }

        /* UNCONDITIONAL; every step below is null-safe and idempotent. Closes
         * the old leak where a failed CreateGameWindow never shut SDL down. */
        Shutdown();
    }

    /* Runs CONCURRENTLY with the render thread executing the previous
     * frame's commands: no ENQUEUE_RENDER_COMMAND from here would be
     * unsound (see IGameMode's own header comment), but this method itself
     * enqueues nothing -- a mode's OnTick is where that happens. */
    void Application::Simulate(IGameMode* InMode, const FrameContext& InFrame)
    {
        if(InMode)
        {
            InMode->OnTick(InFrame);
        }

        /*
         * The game acts FIRST -- it may spawn or destroy something this
         * frame -- then World's own phases run: TP_PrePhysics, TP_Physics
         * (+ the spatial refit at its end), TP_PostPhysics, TP_PostUpdate.
         * Still the game thread, still before RunFrame's
         * WaitForPreviousFrame()/EndFrame() calls -- Scene never touches
         * RenderCore directly; WorldBridge (inside Renderer::EndFrame)
         * reaches into it through World's own public delegates and pools.
         */
        if(Scene)
        {
            Scene->Tick(float(InFrame.DeltaSeconds));
        }
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
         *   2. Rendering->UnbindWorld() -- must precede Scene's destruction:
         *      WorldBridge holds bindings on Scene's own OnComponentAdded/
         *      OnComponentRemoved delegates, and ~World logs a warning for
         *      any built-in delegate still holding entries at destruction
         *      (World.cpp's ~Impl) -- exactly the diagnostic that would fire
         *      if this ran after step 3 instead of before it.
         *   3. Dealloc(Scene) -- by this point nothing holds a binding on it.
         *   4. Scripts->Shutdown() -- lua_close. Must come AFTER Scene is
         *      gone: nothing left holds a World* into a destroyed object.
         *   5. Dealloc(Scripts).
         *   6. Rendering->Shutdown() -- drains the scene and stops the render
         *      thread. Runs LAST among these five: nothing above touches
         *      RenderCore, so there is no ordering constraint forcing it
         *      earlier, and stopping the render thread before Scene/Scripts
         *      are torn down would buy nothing.
         *   7. Dealloc(Rendering).
         *
         * Reversed, step 4 before step 1 closes the Lua state while
         * ScriptComponent instances still hold registry refs into it, and
         * UnbindWorld's OnDestroy calls would run against a dead lua_State.
         */
        if(Scripts && Scene)
        {
            Scripts->UnbindWorld();
        }
        if(Rendering && Scene)
        {
            Rendering->UnbindWorld();
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
        if(Rendering)
        {
            Rendering->Shutdown();
            EngineCore::Memory::Dealloc(Rendering);
            Rendering = nullptr;
        }

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

        /*
         * LAST, and after the renderer is stopped: GDI's own teardown path
         * can still read an asset (a shader reload, a cached pipeline blob),
         * so the locator must stay valid until every subsystem that might
         * use it is down. Cleared rather than left dangling because
         * AssetProvider is a member about to be destroyed with this object
         * -- the same "clear it before the pool dies" discipline CoreJob.h's
         * own locator states. Unconditional and idempotent like everything
         * else here.
         */
        SetFileProvider(nullptr);
    }
};
