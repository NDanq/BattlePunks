#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreJob.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/*
 * Every game's launcher: the engine's, compiled once per game by
 * nimble_add_game_launcher (Engine/cmake/NimbleModule.cmake) into an
 * executable named for the game. The game itself is its module, loaded (not
 * linked) by Application::ExecGame from [Game] Module, NIMBLE_GAME_MODULE if
 * unset.
 *
 * The command line is config overrides (ParseLaunchArgs): -connect=host:port
 * joins a server, -listen hosts one, -Section.Key=Value sets anything else.
 * -headless runs it with no window, input or renderer -- a client a bot
 * drives, for an unattended network test:
 *
 *     WorldsLab -headless -connect=127.0.0.1 -WorldsLab.PvpBot=1
 *
 * No log handler, deliberately: without one a Fatal aborts instead of
 * continuing.
 */

#if !defined(NIMBLE_GAME_MODULE) || !defined(NIMBLE_GAME_NAME)
#error "GameLauncherMain.cpp is built by nimble_add_game_launcher(), which defines NIMBLE_GAME_MODULE and NIMBLE_GAME_NAME"
#endif

int main(int argc, char** argv)
{
    {
        /* The job pool (CoreJob.h): physical cores less one, alive before any
         * module loads and until the last has unloaded -- module teardown drops
         * a module's queued jobs through it. Animation, texture compression and
         * parallel draw recording (MULTITHREADED_RENDERING.md) find it by
         * GetJobSystem(); without it they run serially. */
        EngineCore::JobSystem Jobs;
        EngineCore::SetJobSystem(&Jobs);
        {
            NimbleEngine::ApplicationDesc Desc;
            for(int Index = 1; Index < argc; ++Index)
            {
                const std::string Arg = argv[Index] ? argv[Index] : "";
                if(Arg == "-headless" || Arg == "--headless") { Desc.bHeadless = true; }
            }
            NimbleEngine::Application App(Desc);
#if defined(NIMBLE_PROJECT_DIR)
            App.SetProjectDir(NIMBLE_PROJECT_DIR);   // -uncooked reads its raw files
#endif
            std::vector<std::string> Overrides;
#if NIMBLE_RUN_UNCOOKED
            Overrides.push_back("Content.Uncooked 1");   // the build's default; -cooked turns it off
#endif
            for(std::string& Each : NimbleEngine::ParseLaunchArgs(argc, argv))
            {
                Overrides.push_back(std::move(Each));
            }
            App.SetConfigOverrides(std::move(Overrides));
            App.ExecGame(NIMBLE_GAME_MODULE);
        }

        Jobs.WaitForIdle();
        EngineCore::SetJobSystem(nullptr);
    }

    /* After the Application and module are gone (CoreGarbageCollector.h). */
    const uint64_t Leaked = EngineCore::Memory::ReportLeaks();

    if(Leaked != 0)
    {
        std::fprintf(stderr, "%s: %llu allocation(s) leaked\n", NIMBLE_GAME_NAME,
                     static_cast<unsigned long long>(Leaked));
        return 1;
    }

    return 0;
}
