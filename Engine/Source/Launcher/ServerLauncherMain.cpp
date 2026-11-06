#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreJob.h"

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

namespace
{
    /* "-name=value": value, else empty. */
    std::string ArgValue(const char* InArg, const char* InName)
    {
        const std::string Arg = InArg ? InArg : "";
        const std::string Prefix = std::string("-") + InName + "=";
        return Arg.rfind(Prefix, 0) == 0 ? Arg.substr(Prefix.size()) : std::string();
    }
}

/*
 * -collab-report=<file> [-collab-project=<dir>] [-collab-engine=<dir>]:
 * no game, a content report instead (Application::ExecReport) -- what
 * Collaboration's content worker runs on each commit it builds.
 */
static int RunReport(int argc, char** argv)
{
    NimbleEngine::ContentReportOptions Options;
#if defined(NIMBLE_PROJECT_DIR)
    Options.ProjectDir = NIMBLE_PROJECT_DIR;
#endif
    std::vector<char*> Rest;
    for(int Index = 0; Index < argc; ++Index)
    {
        if(const std::string Report = ArgValue(argv[Index], "collab-report"); !Report.empty())        { Options.ReportPath = Report; }
        else if(const std::string Project = ArgValue(argv[Index], "collab-project"); !Project.empty()) { Options.ProjectDir = Project; }
        else if(const std::string Engine = ArgValue(argv[Index], "collab-engine"); !Engine.empty())    { Options.EngineDir = Engine; }
        else { Rest.push_back(argv[Index]); }
    }

    EngineCore::JobSystem Jobs;
    EngineCore::SetJobSystem(&Jobs);
    int Result = 1;
    {
        NimbleEngine::ApplicationDesc Desc;
        Desc.bHeadless = true;
        Desc.NetMode   = NimbleEngine::ENetMode::Server;
        NimbleEngine::Application App(Desc);
        App.SetConfigOverrides(NimbleEngine::ParseLaunchArgs(int(Rest.size()), Rest.data()));
        Result = App.ExecReport(NIMBLE_GAME_MODULE, Options);
    }
    Jobs.WaitForIdle();
    EngineCore::SetJobSystem(nullptr);
    return Result;
}

int main(int argc, char** argv)
{
    for(int Index = 1; Index < argc; ++Index)
    {
        if(!ArgValue(argv[Index], "collab-report").empty())
        {
            return RunReport(argc, argv);
        }
    }

    std::printf("%s dedicated server -- Ctrl+C to stop\n", NIMBLE_GAME_NAME);

    /* The job pool (CoreJob.h), as the game launcher's: alive before any module
     * loads and until the last has unloaded. */
    EngineCore::JobSystem Jobs;
    EngineCore::SetJobSystem(&Jobs);

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
    Jobs.WaitForIdle();
    EngineCore::SetJobSystem(nullptr);

    const uint64_t Leaked = EngineCore::Memory::ReportLeaks();
    if(Leaked != 0)
    {
        std::fprintf(stderr, "%s server: %llu allocation(s) leaked\n", NIMBLE_GAME_NAME,
                     static_cast<unsigned long long>(Leaked));
        return 1;
    }
    return 0;
}
