/*
 * NimbleEditorServer -- the editor collaboration server
 * (Engine/Modules/Editor/COLLABORATION.md).
 *
 *   NimbleEditorServer [--port N] [--password TEXT] [--logs DIR]
 *                      [--repo DIR [--data DIR] [--preset NAME]]
 *
 * Editors connect over UDP (7788 by default) and are put in a session per
 * project and branch; see Modules/Collab. No Qt, no game code: Core, Collab
 * and ENet. Logs to <DIR>/EditorServer-<time>.log (Saved/Logs by default).
 * The password may come from NIMBLE_COLLAB_PASSWORD instead of the command
 * line, so it stays out of process listings. Ctrl+C stops it.
 *
 * --repo is this host's checkout of the project: each session then gets a
 * content worker (NimbleContentWorker, beside this executable) that builds the
 * session branch with --preset (server-relwithdebinfo-msvc on Windows) in a
 * worktree under --data (a short per-user folder), checks its content, and
 * catches pushes made outside the editor. It builds and runs whatever is
 * pushed: a trusted team's server only.
 *
 * A session is for a trusted team on a LAN or VPN: don't expose it to the
 * internet.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "Public/Collab/CollabServer.h"
#include "Public/Core/CoreLog.h"

namespace
{
    std::atomic<bool> GStop{ false };

    void OnSignal(int)
    {
        GStop.store(true);
    }

    void PrintUsage()
    {
        std::printf("NimbleEditorServer [--port N] [--password TEXT] [--logs DIR] [--repo DIR [--data DIR] [--preset NAME]]\n"
                    "  --port      UDP port to listen on (default %u)\n"
                    "  --password  session password (or set NIMBLE_COLLAB_PASSWORD)\n"
                    "  --logs      folder for the run log (default Saved/Logs)\n"
                    "  --repo      this host's checkout of the project: a content worker builds and checks each session\n"
                    "  --data      worktrees and worker logs (default %%LOCALAPPDATA%%/NimbleCollab, ~/.local/share/NimbleCollab)\n"
                    "  --preset    the game's CMake preset for the worker (default %s)\n"
                    "  --cmake     the CMake the worker builds with (default: Visual Studio's own on Windows, else cmake)\n",
                    unsigned(Collab::DefaultPort), Collab::GetDefaultWorkerPreset());
    }
}

int main(int argc, char** argv)
{
    Collab::ServerConfig Config;
    std::string LogsDir = "Saved/Logs";
    if(const char* Env = std::getenv("NIMBLE_COLLAB_PASSWORD"))
    {
        Config.Password = Env;
    }

    for(int Index = 1; Index < argc; ++Index)
    {
        const std::string Arg = argv[Index];
        const bool bHasValue = Index + 1 < argc;
        if(Arg == "--port" && bHasValue)
        {
            const int Port = std::atoi(argv[++Index]);
            if(Port <= 0 || Port > 65535)
            {
                std::fprintf(stderr, "Bad port: %s\n", argv[Index]);
                return 2;
            }
            Config.Port = uint16_t(Port);
        }
        else if(Arg == "--password" && bHasValue)
        {
            Config.Password = argv[++Index];
        }
        else if(Arg == "--logs" && bHasValue)
        {
            LogsDir = argv[++Index];
        }
        else if(Arg == "--repo" && bHasValue)
        {
            Config.RepoPath = argv[++Index];
        }
        else if(Arg == "--data" && bHasValue)
        {
            Config.DataDir = argv[++Index];
        }
        else if(Arg == "--preset" && bHasValue)
        {
            Config.WorkerPreset = argv[++Index];
        }
        else if(Arg == "--cmake" && bHasValue)
        {
            Config.WorkerCMake = argv[++Index];
        }
        else if(Arg == "--help" || Arg == "-h")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", Arg.c_str());
            PrintUsage();
            return 2;
        }
    }

    if(EngineCore::OpenRunLog(LogsDir, "EditorServer"))
    {
        CORE_LOG("[Collab] logging to " + EngineCore::GetRunLogPath())
    }

    std::signal(SIGINT, &OnSignal);
    std::signal(SIGTERM, &OnSignal);

    Collab::CollabServer Server;
    if(!Server.Start(Config))
    {
        EngineCore::CloseRunLog();
        return 1;
    }
    std::printf("NimbleEditorServer on UDP %u. Ctrl+C to stop.\n", unsigned(Server.GetPort()));

    size_t LastMembers = size_t(-1);
    while(!GStop.load())
    {
        Server.Poll();

        const size_t Members = Server.GetMemberCount();
        if(Members != LastMembers)
        {
            LastMembers = Members;
            std::printf("%zu editor(s) in %zu session(s)\n", Members, Server.GetSessions().size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    Server.Stop();
    EngineCore::CloseRunLog();
    return 0;
}
