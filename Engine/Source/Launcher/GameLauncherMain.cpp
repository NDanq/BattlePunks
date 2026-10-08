#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"

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
        NimbleEngine::Application App;
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
