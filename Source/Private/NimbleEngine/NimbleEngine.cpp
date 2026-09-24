#include "Public/NimbleEngine/NimbleEngine.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#include "Public/NimbleEngine/GameModule.h"
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

/* [Scripting] StartupScript is the session's -- see GameSession.cpp. */

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
         * on Exec's own stack frame, which outlives every use of it: the
         * session points at it only between LoadLevel and UnloadLevel, and
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
        /*
         * FIRST: run from the executable's own directory. Every asset path
         * below is bundle-relative -- Config/, Scripts/, the cooked shaders,
         * Saves/ -- and all of them are staged beside the exe. An IDE does not
         * start a program there on its own: CLion starts it in the CMake build
         * tree and ignores VS_DEBUGGER_WORKING_DIRECTORY, and the app then ran
         * on built-in defaults (a Null renderer, a black window) with nothing
         * to say why. Setting it here makes every launcher equivalent.
         */
        const std::string ExecutableDir = GameWindowSystem::GetExecutableDir();
        if(ExecutableDir.empty())
        {
            CORE_LOG_WARN("Could not determine the executable's directory; assets resolve against the launch directory")
        }
        else
        {
            std::error_code DirError;
            std::filesystem::current_path(std::filesystem::u8path(ExecutableDir), DirError);
            if(DirError)
            {
                CORE_LOG_WARN("Could not change to the executable's directory '" + ExecutableDir + "': " + DirError.message())
            }
        }

        if(!GameWindowSystem::Initialize())
        {
            CORE_LOG_ERROR("Failed to Initialize Window")
            return false;
        }

        /*
         * The asset seam, before anything asks for an asset. Without a provider
         * every read fails -- Vulkan reports "No file provider installed" for
         * its shaders. Empty base directory: provider paths are bundle-relative,
         * and the working directory holds the executable and its staged Config/
         * and Scripts/, as LoadConfigFile below and Sandbox's
         * VS_DEBUGGER_WORKING_DIRECTORY assume. A member because the render
         * thread's shader loads outlive this function; Shutdown() clears it.
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
         * Memory::Alloc so a missed Dealloc shows in ReportLeaks(). No World and
         * no BindWorld here -- the World is the level's, and LoadLevel binds
         * both of these to it. Order relative to the renderer is arbitrary.
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

        /* The game runs in this; the host lends it the two services above. */
        Session = EngineCore::Memory::Alloc<GameSession>(*this, *Scripts, *Rendering);
        return true;
    }

    void Application::Exec()
    {
        DefaultLevelImpl Fallback(nullptr);
        ExecWithLevel(Fallback);
    }

    void Application::Exec(IGameMode& InMode)
    {
        DefaultLevelImpl Fallback(&InMode);
        ExecWithLevel(Fallback);
    }

    void Application::Exec(ILevel& InLevel)
    {
        ExecWithLevel(InLevel);
    }

    GameSession& Application::GetSession()
    {
        CORE_CHECK(Session != nullptr, "Application::GetSession() called outside Exec")
        return *Session;
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

    bool Application::RunFrame(const uint64_t InMaxFrames)
    {
        /* steady_clock: a delta must not go backwards because the wall clock
         * was corrected mid-session. */
        using Clock = std::chrono::steady_clock;

        /* First, where no World is ticking. A requested level that fails to
         * load ends the session, and with it this loop. */
        Session->ApplyPendingLevel();
        if(bQuitRequested || Session->IsQuitRequested())
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

        Session->Tick(Frame);

        /* The fence: at most one frame in flight. Waits for the PREVIOUS
         * frame's marker, not an empty queue -- Simulate may have enqueued. */
        Rendering->WaitForPreviousFrame();

        /* The only sound point: the render thread is idle and never touches a
         * TRef (RenderCommand.h's capture rule), so nothing else mutates one. */
        Memory::CollectGarbage();

        /*
         * Puts the previous frame on screen. Here because the fence has just
         * returned, so that frame is complete -- GDI's precondition -- and
         * EndFrame has not started the next. On the game thread by design: GDI's
         * present path has its own command pool and semaphores
         * (Renderer::PresentToWindow). NativeWindow, not a live SDL query.
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

        /* A mode may have called GameSession::RequestQuit() from inside OnTick. */
        return !bQuitRequested && !Session->IsQuitRequested();
    }

    void Application::ExecGame(const char* InDefaultModule)
    {
        Run(nullptr, InDefaultModule ? InDefaultModule : "");
    }

    void Application::ExecWithLevel(ILevel& InLevel)
    {
        Run(&InLevel, nullptr);
    }

    void Application::Run(ILevel* InLevel, const char* InGameModule)
    {
        /* First, before anything defaults them. Both are first-caller-wins. */
        Threads::SetMainThread();
        Memory::SetCollectorThread();

        /* Outlives the loop and the level: the level's code is in the module.
         * Constructed after SetCollectorThread, since it allocates. */
        GameModuleHost Game;

        if(Initialize())
        {
            /* After Initialize, which read the config naming the module -- and
             * the module's own config vars back-fill from that same file as
             * they register. */
            ILevel* Level = InLevel;
            if(!Level && Game.Load(InGameModule ? InGameModule : ""))
            {
                Level = Game.ResolveStartLevel();
                if(!Level)
                {
                    CORE_LOG_ERROR("Game module '" + Game.GetModuleName() + "' has no start level")
                }
            }

            const int      MaxFramesCfg = GMaxFrames.GetInt();
            const uint64_t MaxFrames    = MaxFramesCfg > 0 ? uint64_t(MaxFramesCfg) : 0;

            /* Negative clamps to unlimited rather than wrapping unsigned. */
            const int LimitCfg  = GFrameRateLimit.GetInt();
            FrameRateLimit      = LimitCfg > 0 ? uint32_t(LimitCfg) : 0u;
            NextFrameDeadlineNs = 0;

            TimerResolutionScope TimerRes;

            if(!Level || !Session->LoadLevel(*Level))
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

                /* Last, so the wait overlaps the render thread's work. The
                 * break above skips it on the way out. */
                PaceToDeadline(NextFrameDeadlineNs, FrameRateLimit, TimerRes);
            }
        }
        else
        {
            CORE_LOG_ERROR("Failed to initialize Application!")
        }

        /* Whatever is loaded now, possibly a level RequestLevel swapped in.
         * Null-safe if the load never happened, or Initialize failed before the
         * session existed. */
        if(Session)
        {
            Session->UnloadLevel();
        }

        /* After the level, whose code it is; before the engine, which its
         * OnShutdown may still log through. */
        Game.Unload();

        /* Unconditional -- null-safe and idempotent, so a failed Initialize()
         * still shuts SDL down. */
        Shutdown();
    }

    void Application::Shutdown()
    {
        /*
         * Null-safe throughout: runs even if Initialize() failed partway. The
         * World is already gone (GameSession::UnloadLevel, which owns the
         * ordering that matters). lua_close is safe because UnbindWorld released every
         * script instance's registry ref; the renderer then drains and stops its thread.
         */
        /* First: it holds references to both services below. Its level is
         * already unloaded (ExecWithLevel), so this only frees it. */
        if(Session)
        {
            EngineCore::Memory::Dealloc(Session);
            Session = nullptr;
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

        /* Through the id, never the pointer: PumpEvents reaps closed windows,
         * so on an ordinary exit `Window` is already dangling. */
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

        /* Last: GDI's teardown can still read an asset. Cleared because
         * AssetProvider dies with this object (CoreJob.h's locator rule). */
        SetFileProvider(nullptr);
    }
};
