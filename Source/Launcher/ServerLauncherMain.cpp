#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/*
 * Every game's dedicated server: the engine's, compiled once per game by
 * nimble_add_server_launcher (Engine/cmake/NimbleModule.cmake) into a console
 * executable, <Game>Server. It runs the same game module and start level as
 * the game, headless -- no window, no input, the Null renderer -- as the
 * session's authority, at [Net] ServerTickRate, until Ctrl+C.
 *
 *     LightLabServer -port=7777
 *     LightLab -connect=127.0.0.1:7777
 *
 * The command line is config overrides, as the game's (ParseLaunchArgs).
 *
 * No log handler, as the game's launcher: EngineCore's default prints to the
 * console, which is where a server's logs belong, and a Fatal still aborts.
 */

#if !defined(NIMBLE_GAME_MODULE) || !defined(NIMBLE_GAME_NAME)
#error "ServerLauncherMain.cpp is built by nimble_add_server_launcher(), which defines NIMBLE_GAME_MODULE and NIMBLE_GAME_NAME"
#endif

#if !WITH_NETCORE
#error "ServerLauncherMain.cpp needs NetCore; nimble_add_server_launcher skips the server when NIMBLE_WITH_NETCORE is off"
#endif

int main(int argc, char** argv)
{
    std::printf("%s dedicated server -- Ctrl+C to stop\n", NIMBLE_GAME_NAME);

    {
        NimbleEngine::ApplicationDesc Desc;
        Desc.bHeadless = true;
        Desc.NetMode   = NimbleEngine::ENetMode::Server;

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

    const uint64_t Leaked = EngineCore::Memory::ReportLeaks();
    if(Leaked != 0)
    {
        std::fprintf(stderr, "%s server: %llu allocation(s) leaked\n", NIMBLE_GAME_NAME,
                     static_cast<unsigned long long>(Leaked));
        return 1;
    }
    return 0;
}
