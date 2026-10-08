#include "Public/NimbleEngine/NimbleEngine.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "Public/NimbleEngine/GameModule.h"
#include "Public/NimbleEngine/Level.h"


#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CorePack.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/NimbleEngine/InputRouter.h"
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
/* Likewise: BOOL is int, DWORD unsigned long. For Ctrl+C in a headless run. */
extern "C" __declspec(dllimport) int __stdcall SetConsoleCtrlHandler(int (__stdcall* InHandler)(unsigned long), int bInAdd);
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

/* The folder of a player's settings under <AppData>/NimbleEngine/; empty: the
 * game module's name. Two games sharing a module name would share settings. */
NIMBLE_CONFIG_STRING(GUserDataName, "Application", "UserDataName", "", nullptr);

/* 0 runs unlimited; SetFrameRateLimit is the runtime half. */
NIMBLE_CONFIG_INT(GFrameRateLimit, "Display", "FrameRateLimit", 60, nullptr);

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

    /* A headless run's exit condition, where a windowed one has PumpEvents:
     * set from the console's Ctrl+C / close handler, which runs on a thread of
     * its own -- hence atomic, and nothing else is done there. */
    std::atomic<bool> GConsoleQuit{ false };

#if defined(_WIN32)
    int __stdcall OnConsoleCtrl(unsigned long /*InCtrlType*/)
    {
        GConsoleQuit = true;
        return 1;   // handled: the loop ends and shuts down cleanly
    }
#else
    void OnInterrupt(int) { GConsoleQuit = true; }
#endif

    void InstallConsoleQuitHandler()
    {
        GConsoleQuit = false;
#if defined(_WIN32)
        SetConsoleCtrlHandler(&OnConsoleCtrl, 1);
#else
        std::signal(SIGINT, &OnInterrupt);
        std::signal(SIGTERM, &OnInterrupt);
#endif
    }

    void RemoveConsoleQuitHandler()
    {
#if defined(_WIN32)
        SetConsoleCtrlHandler(&OnConsoleCtrl, 0);
#else
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);
#endif
    }

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
    std::vector<std::string> ParseLaunchArgs(const int InArgc, const char* const* InArgv)
    {
        std::vector<std::string> Overrides;
        for(int Index = 1; Index < InArgc; ++Index)
        {
            const std::string Arg = InArgv[Index] ? InArgv[Index] : "";
            if(Arg.size() < 2 || Arg[0] != '-')
            {
                CORE_LOG_WARN("Ignoring the argument '" + Arg + "'")
                continue;
            }

            const std::string Body   = Arg.substr(Arg[1] == '-' ? 2 : 1);
            const size_t      Equals = Body.find('=');
            const std::string Key    = Body.substr(0, Equals);
            const std::string Value  = Equals == std::string::npos ? std::string() : Body.substr(Equals + 1);

            if(Key == "connect")
            {
                /* host[:port]. The last ':' splits, so a bare host keeps its default port. */
                const size_t Colon = Value.rfind(':');
                Overrides.push_back("Net.ServerAddress " + Value.substr(0, Colon));
                if(Colon != std::string::npos) { Overrides.push_back("Net.Port " + Value.substr(Colon + 1)); }
            }
            else if(Key == "port")   { Overrides.push_back("Net.Port " + Value); }
            else if(Key == "listen") { Overrides.push_back("Net.Listen 1"); }
            else if(Key.find('.') != std::string::npos && Equals != std::string::npos)
            {
                Overrides.push_back(Key + " " + Value);
            }
            else
            {
                CORE_LOG_WARN("Ignoring the argument '" + Arg + "' -- expected -Section.Key=Value, -connect=host[:port], "
                              "-port=N or -listen")
            }
        }

#if !WITH_NETCORE
        for(const std::string& Override : Overrides)
        {
            if(Override.rfind("Net.", 0) == 0)
            {
                CORE_LOG_WARN("'" + Override + "': this build has networking compiled out (NIMBLE_WITH_NETCORE=OFF)")
            }
        }
#endif
        return Overrides;
    }

    namespace
    {
        /* What Exec() and Exec(IGameMode&) run when given no level: an empty
         * World and optionally a mode. Lives on Exec's stack frame. */
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
        /* FIRST: every asset path is relative to the exe's folder, and IDEs
         * (CLion) don't start there, silently falling back to defaults. */
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

        if(!Desc.bHeadless && !GameWindowSystem::Initialize())
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
        SetFileProvider(&LayeredAssets);

        /* A packaged game's files -- config included -- are its content packs
         * (CorePack.h), mounted over the folder before anything is read. A
         * development build has none and reads the loose files. Logged once
         * the run log is open. */
        const int PackCount = MountPackFiles(LayeredAssets, "Paks");

        ConfigLoadStats Stats;
        const bool bConfigLoaded = LoadConfigFile("Config/DefaultEngine.ini", &Stats);

        /* This run's log, beside the executable as a project's is beside it:
         * Saved/Logs/<Module>[Server]-<time>.log (CoreLog.h). */
        {
            const ConfigVar* Module = ConfigFindVarByPath("Game.Module");
            std::string Name = Module && Module->GetString()[0] ? Module->GetString() : GameModuleName;
            if(Name.empty()) { Name = "Game"; }
            if(Desc.bHeadless && Desc.NetMode == ENetMode::Server) { Name += "Server"; }
            if(OpenRunLog("Saved/Logs", Name))
            {
                CORE_LOG("Logging to " + GetRunLogPath())
            }
            if(PackCount > 0)
            {
                CORE_LOG("Content from " + std::to_string(PackCount) + " pack(s) in Paks/")
            }
        }
        if(!bConfigLoaded)
        {
            CORE_LOG_WARN("No Config/DefaultEngine.ini Found")
        }
        LoadUserSettings();

        /* The command line wins over the file. */
        for(const std::string& Override : ConfigOverrides)
        {
            /* A var the game module declares does not exist until it loads:
             * recorded as a config file's line is, it is applied when it
             * registers, as [Section] Key in the .ini would be. */
            const size_t Space = Override.find(' ');
            const size_t Dot   = Override.find('.');
            if(Space != std::string::npos && Dot != std::string::npos && Dot < Space
               && !ConfigFindVarByPath(Override.substr(0, Space)))
            {
                LoadConfigText("[" + Override.substr(0, Dot) + "]\n" + Override.substr(Dot + 1, Space - Dot - 1) + "="
                               + Override.substr(Space + 1) + "\n");
                continue;
            }

            std::string Response;
            if(!ExecConfigCommand(Override, Response))
            {
                CORE_LOG_WARN("Command line '" + Override + "': " + Response)
            }
        }

        /* [Modules] edited since this build was configured (GameSession.h). */
        WarnModuleConfigMismatch();

        if(Desc.bHeadless)
        {
            /* No window to draw to, so nothing to draw: the Null backend
             * needs no GPU and no native window (RenderThread::Start). */
            GRenderPlatform.SetFromString(std::to_string(int(RenderCore::ERenderBackend::Null)));
            InstallConsoleQuitHandler();
        }
        else
        {
            WindowDesc WinDesc;
            WinDesc.Width = GResolutionX.GetInt();
            WinDesc.Height = GResolutionY.GetInt();
            WinDesc.Title = GTitle.GetString();
            WinDesc.Flags = WF_Resizable | WF_HighDPI;

            Window = GameWindowSystem::CreateGameWindow(WinDesc);
            if(!Window)
            {
                CORE_LOG_ERROR("Failed to Create Window")
                return false;
            }
            WindowID = Window->GetID();

            Router = EngineCore::Memory::Alloc<InputRouter>();
            Router->Attach(Window);

            /* ONCE, HERE, ON THIS THREAD. GetNativeHandle() is a live SDL query,
             * not a cached read like the geometry accessors. Everything else uses
             * this copy; the render thread never calls back into GameWindow. */
            NativeWindow = Window->GetNativeHandle();
            if(!NativeWindow.IsValid())
            {
                CORE_LOG_ERROR("Window has no native handle; cannot initialize the renderer")
                return false;
            }
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
        Net     = EngineCore::Memory::Alloc<NetHost>();
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

    const InputState& Application::GetInput() const
    {
        return Router ? Router->GetState() : IGameHost::GetInput();
    }

    void Application::SetMouseCaptured(const bool bInCaptured)
    {
        if(Router) { Router->SetMouseCaptured(bInCaptured); }
    }

    bool Application::IsMouseCaptured() const
    {
        return Router && Router->IsMouseCaptured();
    }

    std::string Application::GetClipboardText() const
    {
        return GameWindowSystem::GetClipboardText();
    }

    void Application::SetClipboardText(const std::string& InUtf8)
    {
        GameWindowSystem::SetClipboardText(InUtf8);
    }

    void Application::SetTextInputActive(const bool bInActive)
    {
        if(Router) { Router->SetTextInputActive(bInActive); }
    }

    bool Application::RunFrame(const uint64_t InMaxFrames)
    {
        /* steady_clock: a delta must not go backwards because the wall clock
         * was corrected mid-session. */
        using Clock = std::chrono::steady_clock;

        /* "stat unit": the game thread's frame starts here, before the pump. */
        Rendering->BeginGameFrame();

        /* First, where no World is ticking. A failed load ends the session.
         * The network lets go of a World about to be freed, and takes the
         * new one. */
        Net->BeginFrame();
        Session->ApplyPendingLevel();
        if(bQuitRequested || Session->IsQuitRequested())
        {
            return false;
        }

        if(Desc.bHeadless)
        {
            /* No window: the console's Ctrl+C is the exit condition. */
            if(GConsoleQuit) { return false; }
        }
        else
        {
            /* PumpEvents' return IS the exit condition; delegates run inside it,
             * on this thread. */
            if(!GameWindowSystem::PumpEvents())
            {
                return false;
            }

            /* What arrived in the pump becomes this frame's input. */
            Router->Latch(FrameNumber);
        }

        /* Connections, spawns, state and RPCs, before the World sees the frame. */
        Net->Poll();

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

        /* What changed this frame goes out. */
        Net->Flush();

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
        if(!Desc.bHeadless)
        {
            Rendering->PresentToWindow(NativeWindow);
        }

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

    void Application::LoadUserSettings()
    {
        std::string Name = GUserDataName.GetString();
        if(Name.empty())
        {
            const ConfigVar* Module = ConfigFindVarByPath("Game.Module");
            Name = Module && Module->GetString()[0] ? Module->GetString() : GameModuleName;
        }
        const std::string Root = Name.empty() ? std::string() : appGetAppDataDir("NimbleEngine");
        if(Root.empty())
        {
            CORE_LOG_WARN("No user settings folder: a player's settings will not be kept")
            return;
        }
        const std::string Folder = Root + "/" + Name;
        appCreateDirectories(Folder);
        const std::string Path = Folder + "/User.ini";
        if(LoadConfigFile(Path, nullptr, EConfigLayer::User))
        {
            CORE_LOG("User settings from " + Path)
        }
        ConfigSetUserFile(Path);
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

        /* Outlives the level, whose code it holds. After SetCollectorThread:
         * it allocates. */
        GameModuleHost Game;
        GameModuleName = InGameModule ? InGameModule : "";

        if(Initialize())
        {
            /* After Initialize, which read the config naming the module. */
            ILevel* Level = InLevel;
            Game.SetRenderer(Rendering);
            if(!Level && Game.Load(InGameModule ? InGameModule : ""))
            {
                Level = Game.ResolveStartLevel();
                if(!Level)
                {
                    CORE_LOG_ERROR("Game module '" + Game.GetModuleName() + "' has no start level: the project lists no worlds "
                                   "(Config/DefaultEngine.ini [World_*], Project Settings > Worlds)")
                }
            }

            const int      MaxFramesCfg = GMaxFrames.GetInt();
            const uint64_t MaxFrames    = MaxFramesCfg > 0 ? uint64_t(MaxFramesCfg) : 0;

            /* Negative clamps to unlimited rather than wrapping unsigned. A
             * headless server runs at its tick rate instead. */
            int LimitCfg = GFrameRateLimit.GetInt();
            if(Desc.bHeadless && Desc.NetMode == ENetMode::Server) { LimitCfg = int(GetConfiguredServerTickRate()); }
            FrameRateLimit      = LimitCfg > 0 ? uint32_t(LimitCfg) : 0u;
            NextFrameDeadlineNs = 0;

            TimerResolutionScope TimerRes;

            /* Before the load, so the level and mode already know whether
             * they are a server or a client (NetFacade.h). Nothing arrives
             * until the first frame's poll, which binds the loaded World. */
            NetHostDesc NetDesc = NetHostDesc::FromConfig(Desc.NetMode);
            NetDesc.bDedicated  = Desc.bHeadless;
            if(Level)
            {
                Net->Start(*Session, NetDesc);
            }

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

        /* Clients told, before their entities' World goes. */
        if(Net) { Net->Stop(); }

        /* Whatever is loaded now, possibly a swapped-in level. */
        if(Session)
        {
            Session->UnloadLevel();
        }

        /* After the level (its code), before the engine (OnShutdown may log). */
        Game.Unload();

        /* Unconditional -- null-safe and idempotent, so a failed Initialize()
         * still shuts SDL down. */
        Shutdown();
        CloseRunLog();
    }

    void Application::Shutdown()
    {
        /*
         * Null-safe: runs even if Initialize() failed partway. The World is
         * already gone (GameSession::UnloadLevel), so lua_close is safe; the
         * renderer then drains and stops its thread.
         */
        /* First: it holds the session. */
        if(Net)
        {
            Net->Stop();
            EngineCore::Memory::Dealloc(Net);
            Net = nullptr;
        }

        /* Then the session: it references both services below. */
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

        if(Router)
        {
            Router->Detach();
            EngineCore::Memory::Dealloc(Router);
            Router = nullptr;
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

        if(Desc.bHeadless)
        {
            RemoveConsoleQuitHandler();
        }
        else
        {
            GameWindowSystem::Shutdown();
        }

        /* Last: GDI's teardown can still read an asset. Cleared because
         * AssetProvider dies with this object (CoreJob.h's locator rule). */
        SetFileProvider(nullptr);
    }

};
