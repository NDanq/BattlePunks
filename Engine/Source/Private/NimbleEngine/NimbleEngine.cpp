#include "Public/NimbleEngine/NimbleEngine.h"

#include <chrono>
#include <string>

#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Level.h"

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

/* Bundle-relative path to a Lua file run once per LEVEL LOAD, after
 * IGameMode::OnLoad and before the first tick -- see LoadLevel's own comment on
 * why that ordering, not Initialize(), is where this runs, and why it re-runs
 * on every swap. Empty (the default) skips scripting entirely; a game with
 * nothing to say in Lua pays nothing for this. */
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
    namespace
    {
        /*
         * The level Exec() and Exec(IGameMode&) run when the caller brought
         * none: an empty World and, optionally, a mode.
         *
         * Here rather than in the header because nothing outside this file
         * needs the type -- a caller that wants a real level writes one. Held
         * on ExecWithLevel's own stack frame, which outlives every use of it:
         * ActiveLevel points at it only between LoadLevel and UnloadLevel, and
         * both happen inside that call.
         */
        class DefaultLevelImpl final : public ILevel
        {
        public:
            explicit DefaultLevelImpl(IGameMode* InMode) : Mode(InMode) {}

            const char* GetName() const override { return "Default"; }
            IGameMode*  GetGameMode() override   { return Mode; }

        private:
            IGameMode* Mode = nullptr;
        };
    }

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
         * The Lua layer. Core allocations (Memory::Alloc, not plain new) so a
         * missed Dealloc at teardown is exactly the kind of thing Sandbox's
         * ReportLeaks() already catches at process exit.
         *
         * NO WORLD HERE, and no BindWorld: the World belongs to whichever level
         * is loaded (Level.h), so LoadLevel creates it and binds both of these
         * to it. What survives every level is exactly these two -- one
         * lua_State and one render thread per process, rebound as levels come
         * and go.
         *
         * Before the renderer on purpose: nothing here touches a window or a
         * render context. Ordering relative to the renderer is otherwise
         * arbitrary.
         */
        Scripts = EngineCore::Memory::Alloc<GameCore::ScriptEngine>();
        if(!Scripts->Initialize())
        {
            CORE_LOG_ERROR("Failed to initialize the script engine")
            return false;
        }

        const RenderCore::ERenderBackend Backend = RenderCore::ERenderBackend(GRenderPlatform.GetInt());

        Rendering = EngineCore::Memory::Alloc<RenderCore::Renderer>();
        if(!Rendering->Initialize(Backend, NativeWindow))
        {
            CORE_LOG_ERROR("Failed to initialize RenderCore")
            return false;
        }

        return true;
    }

    bool Application::LoadLevel(ILevel* InLevel)
    {
        CORE_CHECK(InLevel != nullptr, "Application::LoadLevel given a null level")
        CORE_CHECK(LevelWorld == nullptr, "Application::LoadLevel called with a level already loaded")

        /*
         * THE WORLD FIRST, and bound before OnLoad sees it: a level populates
         * entities in OnLoad, and both the script engine's tick systems and the
         * renderer's WorldBridge have to be installed before the first entity
         * appears or neither would ever learn about it.
         */
        LevelWorld = EngineCore::Memory::Alloc<GameCore::World>();

        ActiveLevel             = InLevel;
        ActiveLevel->OwnedWorld = LevelWorld;

        Scripts->BindWorld(LevelWorld);
        Rendering->BindWorld(*LevelWorld);

        CORE_LOG("[Level] loading '" + std::string(InLevel->GetName()) + "'")

        if(!InLevel->OnLoad(*this))
        {
            CORE_LOG_ERROR("Level '" + std::string(InLevel->GetName()) + "' refused to load")
            return false;
        }

        /* AFTER the level populated the World -- a mode acts on entities that
         * already exist, and OnLoad's own comment says so. */
        IGameMode* Mode = InLevel->GetGameMode();
        if(Mode && !Mode->OnLoad(*this))
        {
            CORE_LOG_ERROR("Game mode refused to load")
            return false;
        }

        /*
         * BETWEEN THE MODE'S TWO CALLBACKS, and that placement is the whole
         * distinction between them -- see IGameMode's lifecycle comment. A
         * startup script sees a loaded level (its entities, its bound config),
         * and OnStart below sees whatever the script created.
         *
         * Skipped entirely when empty, which is the default. A failed script is
         * logged by ScriptEngine itself and is NOT fatal -- the same "log and
         * continue" stance CORE_LOG_WARN takes for a missing DefaultEngine.ini
         * in Initialize().
         *
         * RUNS ON EVERY LEVEL LOAD, not just the first. The script's registered
         * classes survive a swap (they live in the Lua registry), but anything
         * it spawned does not -- it went down with the previous World -- so a
         * level that expects script-created entities needs the script re-run.
         */
        const char* StartupScriptPath = GStartupScript.GetString();
        if(StartupScriptPath && StartupScriptPath[0] != '\0')
        {
            Scripts->RunFile(StartupScriptPath);
        }

        /* Before OnStart, so a mode's OnStart always sees state that has just
         * been put back to its starting values -- GameState.h states that. */
        if(GameState* State = InLevel->GetGameState())
        {
            State->OnReset();
        }

        if(Mode)
        {
            Mode->OnStart();
        }

        return true;
    }

    void Application::UnloadLevel()
    {
        if(!ActiveLevel)
        {
            return;
        }

        /*
         * THE SAME ORDER Shutdown() uses, and for the same reasons -- this is
         * steps 1-3 of that sequence performed at runtime rather than at exit.
         * See Shutdown()'s own comment for the full argument; in brief:
         *
         *   1. the mode, then the level, undo what they bound -- both need a
         *      live World to unbind FROM.
         *   2. Scripts->UnbindWorld() runs every live script's OnDestroy and
         *      unregisters the script-tick systems. Needs a live lua_State AND
         *      a live World.
         *   3. Rendering->UnbindWorld() must precede the World's destruction:
         *      WorldBridge holds bindings on its OnComponentAdded/Removed
         *      delegates, and ~World logs a warning for any built-in delegate
         *      still holding entries at destruction.
         *   4. destroy the World.
         *
         * Every step is null-safe: this also runs on LoadLevel's failure path,
         * where OnLoad may have got halfway through.
         */
        if(IGameMode* Mode = ActiveLevel->GetGameMode())
        {
            Mode->OnUnload();
        }

        ActiveLevel->OnUnload();

        if(Scripts && LevelWorld)
        {
            Scripts->UnbindWorld();
        }
        if(Rendering && LevelWorld)
        {
            Rendering->UnbindWorld();
        }

        if(LevelWorld)
        {
            EngineCore::Memory::Dealloc(LevelWorld);
            LevelWorld = nullptr;
        }

        CORE_LOG("[Level] unloaded '" + std::string(ActiveLevel->GetName()) + "'")

        ActiveLevel->OwnedWorld = nullptr;
        ActiveLevel             = nullptr;
    }

    void Application::ApplyPendingLevel()
    {
        if(!PendingLevel)
        {
            return;
        }

        /* The whole reason RequestLevel defers: tearing a World down from
         * inside its own Tick would destroy the pools being iterated. */
        CORE_CHECK(!LevelWorld || !LevelWorld->IsTicking(),
                   "Application::ApplyPendingLevel ran while the World was ticking")

        ILevel* Next = PendingLevel;
        PendingLevel = nullptr;

        /* Fully down before the next one comes up, so the two never coexist
         * and a game cannot accidentally reference across them. */
        UnloadLevel();

        if(!LoadLevel(Next))
        {
            CORE_LOG_ERROR("Requested level failed to load; quitting")
            UnloadLevel();
            bQuitRequested = true;
        }
    }

    void Application::Exec()
    {
        DefaultLevelImpl Fallback(nullptr);
        ExecWithLevel(&Fallback);
    }

    void Application::Exec(IGameMode& InMode)
    {
        DefaultLevelImpl Fallback(&InMode);
        ExecWithLevel(&Fallback);
    }

    void Application::Exec(ILevel& InLevel)
    {
        ExecWithLevel(&InLevel);
    }

    void Application::RequestLevel(ILevel& InLevel)
    {
        if(PendingLevel)
        {
            CORE_LOG_WARN("Application::RequestLevel called twice before the first took effect; "
                          "the later request wins")
        }
        PendingLevel = &InLevel;
    }

    ILevel* Application::GetLevel()
    {
        return ActiveLevel;
    }

    GameState* Application::GetGameState()
    {
        return ActiveLevel ? ActiveLevel->GetGameState() : nullptr;
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
        /* The active level's, not this class's -- see the header. Null outside
         * a loaded level, which is the same programming error the old
         * "before Initialize()" check caught, just with a wider window. */
        CORE_CHECK(LevelWorld != nullptr,
                   "Application::GetWorld() called with no level loaded")
        return *LevelWorld;
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

    bool Application::RunFrame(const uint64_t InMaxFrames)
    {
        /* steady_clock: a delta must not go backwards because the wall clock
         * was corrected mid-session. */
        using Clock = std::chrono::steady_clock;

        /*
         * FIRST IN THE FRAME, before anything can tick: this is the point at
         * which no World is ticking, which is exactly what a deferred level
         * swap needs. May set bQuitRequested if the incoming level fails.
         */
        ApplyPendingLevel();
        if(bQuitRequested)
        {
            return false;
        }

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

        Simulate(Frame);

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

    void Application::ExecWithLevel(ILevel* InLevel)
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

            if(!LoadLevel(InLevel))
            {
                bQuitRequested = true;
            }

            while(!bQuitRequested)
            {
                if(!RunFrame(MaxFrames))
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

        /* Whatever is loaded at this point -- the level Exec was given, or a
         * different one a RequestLevel swapped in. Runs the mode's and the
         * level's OnUnload and destroys the World; null-safe if the load never
         * got that far. */
        UnloadLevel();

        /* UNCONDITIONAL; every step below is null-safe and idempotent. Closes
         * the old leak where a failed CreateGameWindow never shut SDL down. */
        Shutdown();
    }

    /* Runs CONCURRENTLY with the render thread executing the previous
     * frame's commands: no ENQUEUE_RENDER_COMMAND from here would be
     * unsound (see IGameMode's own header comment), but this method itself
     * enqueues nothing -- a mode's OnTick is where that happens. */
    void Application::Simulate(const FrameContext& InFrame)
    {
        /* Read fresh rather than cached: ApplyPendingLevel may have replaced
         * the level, and with it the mode, since the last frame. */
        if(ActiveLevel)
        {
            if(IGameMode* Mode = ActiveLevel->GetGameMode())
            {
                Mode->OnTick(InFrame);
            }
        }

        /*
         * The game acts FIRST -- it may spawn or destroy something this
         * frame -- then World's own phases run: TP_PrePhysics, TP_Physics
         * (+ the spatial refit at its end), TP_PostPhysics, TP_PostUpdate.
         * Still the game thread, still before RunFrame's
         * WaitForPreviousFrame()/EndFrame() calls -- the World never touches
         * RenderCore directly; WorldBridge (inside Renderer::EndFrame)
         * reaches into it through World's own public delegates and pools.
         */
        if(LevelWorld)
        {
            LevelWorld->Tick(float(InFrame.DeltaSeconds));
        }
    }

    void Application::Shutdown()
    {
        /*
         * UNCONDITIONAL, like everything below -- Shutdown() runs whether
         * Initialize() got all the way through or failed partway, so every
         * step here is null-safe.
         *
         * THE WORLD IS ALREADY GONE by the time this runs: ExecWithLevel calls
         * UnloadLevel() first, and that is what unbinds the script engine and
         * the renderer from the level's World and destroys it. UnloadLevel's
         * own comment carries the ordering argument that used to live here --
         * it is the part that is genuinely load-bearing, and it now runs on
         * every level swap rather than only at exit.
         *
         * What is left is the two process-lifetime objects, and the one
         * ordering rule between them:
         *
         *   1. Scripts->Shutdown() -- lua_close. Safe here because no World
         *      remains for a ScriptComponent to hold a registry ref from.
         *   2. Dealloc(Scripts).
         *   3. Rendering->Shutdown() -- drains the scene and stops the render
         *      thread. Nothing above touches RenderCore, so there is no
         *      constraint forcing it earlier.
         *   4. Dealloc(Rendering).
         */
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
