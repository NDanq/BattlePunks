/*
 * NimbleEditorServer -- the editor collaboration server
 * (Engine/Modules/Editor/COLLABORATION.md).
 *
 *   NimbleEditorServer [--port N] [--password TEXT] [--logs DIR]
 *
 * Editors connect over UDP (7788 by default) and are put in a session per
 * project and branch; see Modules/Collab. No Qt, no game code: Core, Collab
 * and ENet. Logs to <DIR>/EditorServer-<time>.log (Saved/Logs by default).
 * The password may come from NIMBLE_COLLAB_PASSWORD instead of the command
 * line, so it stays out of process listings. Ctrl+C stops it.
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
        std::printf("NimbleEditorServer [--port N] [--password TEXT] [--logs DIR]\n"
                    "  --port      UDP port to listen on (default %u)\n"
                    "  --password  session password (or set NIMBLE_COLLAB_PASSWORD)\n"
                    "  --logs      folder for the run log (default Saved/Logs)\n",
                    unsigned(Collab::DefaultPort));
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
