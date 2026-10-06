#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"

#include <cstdint>
#include <cstdio>

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
        App.SetConfigOverrides(NimbleEngine::ParseLaunchArgs(argc, argv));
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
