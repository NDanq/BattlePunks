#ifndef NIMBLEENGINE_NIMBLEENGINE_H
#define NIMBLEENGINE_NIMBLEENGINE_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/NetHost.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreTypes.h"
#include "Public/Core/CoreThread.h"

/*
 * Forward-declared, not included: GameWindow is linked PRIVATE so this header
 * cannot leak it to a game (Engine/CMakeLists.txt). An #include here means
 * something grew a dependency it should not have. GDI does not appear at all --
 * it lives behind RenderCore, the one image allowed to link it.
 */
class GameWindow;

/*
 * GameCore and RenderCore are the deliberate PUBLIC exceptions: World/Entity and
 * Renderer/RenderCommand are the game-facing surface, so Engine/CMakeLists.txt
 * links both PUBLIC and a game includes their headers itself.
 */
namespace GameCore
{
    class ScriptEngine;
}

namespace RenderCore
{
    class Renderer;
}

namespace NimbleEngine
{
    class ILevel;
    class InputRouter;
    class FrameRecorder;

    /* How an Application runs. */
    struct ApplicationDesc
    {
        /* No window, no input, the Null renderer: a dedicated server, or
         * anything else run from a console. Ctrl+C ends the loop. */
        bool     bHeadless = false;

        /* Server: listen on [Net] Port. Client is not set here -- a game
         * connects when [Net] ServerAddress is set (-connect=host:port), and
         * is Standalone otherwise. Server and Client need NetCore; without it
         * the Application warns and runs Standalone. */
        ENetMode NetMode   = ENetMode::Standalone;
    };

    /*
     * The launchers' command line, as config overrides applied after
     * Config/DefaultEngine.ini loads:
     *
     *     -Section.Key=Value     any config variable, e.g. -Display.RHI=0
     *     -connect=host[:port]   [Net] ServerAddress (and Port): join a server
     *     -port=N                [Net] Port
     *     -uncooked[=<folder>]   run uncooked (Debug, RelWithDebInfo): read the
     *                            project's raw files -- the folder, else the one
     *                            the game was built from (SetProjectDir)
     *     -cooked                not uncooked, whatever the build's default
     *     -record=<file.mp4>     [Record] File: the game as a video, through
     *                            ffmpeg (Editor-target builds; FrameRecorder.h)
     *     -headless              the game launcher runs without a window, input or
     *                            renderer (ApplicationDesc::bHeadless): a client
     *                            for bots and soak tests; no override here
     *
     * Anything else is warned about and ignored. Each entry of the result is
     * an ExecConfigCommand line ("Net.Port 7777").
     */
    NE_API std::vector<std::string> ParseLaunchArgs(int InArgc, const char* const* InArgv);

    /*
     * THE GAME EXECUTABLE'S HOST. Owns the working directory and config, the
     * SDL window and pump, pacing, the process-lifetime ScriptEngine and
     * Renderer, and each frame's fence, GC, present and EndFrame; drives one
     * GameSession (see GameSession.h).
     *
     * The main thread is both game and SDL pump thread (macOS requires it).
     * The render thread is reached only via the Renderer and
     * ENQUEUE_RENDER_COMMAND, with one frame of lag (RenderCommand.h).
     */
    /* Application::ExecReport's arguments. */
    struct NE_API ContentReportOptions
    {
        std::string ReportPath;   // written as UTF-8 lines; see ContentReport.cpp
        std::string ProjectDir;   // the game's source folder: Content/ is read from it
        std::string EngineDir;    // the Engine folder, for "Engine/Content/..." paths; may be empty
    };

    class NE_API Application final : public IGameHost
    {
    public:
        Application() = default;
        explicit Application(const ApplicationDesc& InDesc) : Desc(InDesc) {}
        ~Application() override = default;

        /* ParseLaunchArgs' overrides, applied once the config file has
         * loaded. Before Exec. */
        void SetConfigOverrides(std::vector<std::string> InOverrides) { ConfigOverrides = std::move(InOverrides); }

        /* The project folder the game was built from (the launcher's
         * NIMBLE_PROJECT_DIR): where -uncooked reads raw files. Before Exec. */
        void SetProjectDir(std::string InDir) { ProjectDir = std::move(InDir); }

    private:
        /* -uncooked's folder mounted over the packs and the executable's; the
         * folder, or empty when not running uncooked. */
        std::string MountUncookedProject();

    public:

        Application(const Application&)            = delete;
        Application& operator=(const Application&) = delete;

        /* No level of its own: window, loop, clear. An empty engine-owned level
         * still exists, so the renderer and scripts have a World to bind. */
        void Exec();

        /* Runs InLevel, which must outlive the call. The primary entry point. */
        void Exec(ILevel& InLevel);

        /* InMode against an empty engine-owned level -- for a mode that spawns
         * its own entities, small games and the smoke tests. */
        void Exec(IGameMode& InMode);

        /* Runs the game module named by [Game] Module (else InDefaultModule)
         * from its start level. What a game's launcher calls (GameModule.h). */
        void ExecGame(const char* InDefaultModule);

        /*
         * Collaboration's content check (ContentReport.cpp): the game module
         * loaded headless, then its NetCore schema hash and every world and
         * prefab under InOptions.ProjectDir/Content loaded, each one's
         * warnings and errors and any asset path it names that doesn't exist
         * written to InOptions.ReportPath. No level runs. 0 if the report was
         * written (problems or not), else 1. What NimbleContentWorker runs
         * through <Game>Server -collab-report=<file>.
         */
        int ExecReport(const char* InDefaultModule, const ContentReportOptions& InOptions);

        /* Ends the loop after this frame. Games use GameSession::RequestQuit. */
        void RequestQuit() { bQuitRequested = true; }

        /* Valid between Initialize and Shutdown (CORE_CHECKed). */
        GameSession& GetSession();

        // === IGameHost ===========================================================

        /* 0 is unlimited. Defaults to [Display] FrameRateLimit. */
        void     SetFrameRateLimit(uint32_t InFramesPerSecond) override;
        uint32_t GetFrameRateLimit() const override;
        const InputState& GetInput() const override;
        void SetMouseCaptured(bool bInCaptured) override;
        bool IsMouseCaptured() const override;
        void SetTextInputActive(bool bInActive) override;
        std::string GetClipboardText() const override;
        void        SetClipboardText(const std::string& InUtf8) override;

    private:
        bool Initialize();
        void Shutdown();

        /* The player's settings, over the project's: <AppData>/NimbleEngine/
         * <[Application] UserDataName, else the game module>/User.ini, loaded
         * and named as the file Config.Save writes (CoreConfig.h). */
        void LoadUserSettings();

        /* Run(&InLevel). The level-less overloads pass a .cpp-local empty level. */
        void ExecWithLevel(ILevel& InLevel);

        /* Engine up, level loaded, loop, unload, engine down. InLevel, or when
         * null the start level of InGameModule. */
        void Run(ILevel* InLevel, const char* InGameModule);

        /* Run's InGameModule, for Initialize: the user settings' folder. */
        std::string GameModuleName;

        /* Level swap, pump, Tick, fence, collect, present, end. False to stop.
         * Pacing lives in the caller. */
        bool RunFrame(uint64_t InMaxFrames);

        /* The network session (NetHost.h): started once the first level has
         * loaded (a client's first spawns need its World), driven around
         * every Tick. Does nothing without NetCore. */
        ApplicationDesc          Desc;
        std::vector<std::string> ConfigOverrides;
        std::string              ProjectDir;

        /* Created by Initialize. */
        NetHost*                 Net = nullptr;

        /* Where assets (cooked shaders above all) come from: the disk, with
         * mounted shader caches layered on top (RenderCore's ShaderCache.h);
         * installed by Initialize(). Members because they must outlive the
         * render thread's shader loads. An editor build asks the disk first,
         * so freshly cooked materials beat the last build's cache. */
        EngineCore::FilesystemProvider  AssetProvider;
        EngineCore::LayeredFileProvider LayeredAssets{ &AssetProvider, WITH_EDITOR != 0 };

        GameWindow*              Window        = nullptr;

        /* Outlives the window: PumpEvents reaps closed windows, so after the loop
         * only the id is safe to ask about. Never 0 on a live window. */
        uint32_t                 WindowID      = 0;

        /* Captured once in Initialize() -- GetNativeHandle() is a live SDL
         * query -- and handed to Rendering->Initialize(). */
        EngineCore::WindowHandle NativeWindow;

        /* Process lifetime; lent to the session. */
        GameCore::ScriptEngine*  Scripts   = nullptr;
        RenderCore::Renderer*    Rendering = nullptr;

        /* The window's SDL input, latched once a frame after the pump. */
        InputRouter*             Router    = nullptr;

        /* Created after Scripts and Rendering; destroyed before them. */
        GameSession*             Session   = nullptr;

        /* -record=<file.mp4>: the game as a video (FrameRecorder.h); null otherwise. */
        FrameRecorder*           Recorder  = nullptr;

        bool                     bQuitRequested    = false;
        uint64_t                 FrameNumber       = 0;

        /* Steady-clock ns of the previous tick, for DeltaSeconds. int64_t to keep
         * <chrono> out of this header. Zero until the first frame. */
        int64_t                  PreviousTickNs      = 0;

        /* 0 = unlimited. See SetFrameRateLimit. */
        uint32_t                 FrameRateLimit    = 0;

        /* Steady-clock ns when the next frame should start. A deadline, not a
         * duration, so one late frame does not shift every later one. Zero
         * until established. */
        int64_t                  NextFrameDeadlineNs = 0;
    };
};

#endif //NIMBLEENGINE_NIMBLEENGINE_H
