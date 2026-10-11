/*
 * NimbleContentWorker -- a collaboration session's content worker
 * (Modules/Collab/ContentWorker.h; Engine/Modules/Editor/COLLABORATION.md, P4).
 *
 * Started by NimbleEditorServer, one per session, never by hand:
 *
 *   NimbleContentWorker --connect host:port --session N --key K --repo DIR
 *                       --worktree DIR --branch B --module M --preset P
 *                       [--fetch-interval MS] [--logs DIR]
 *
 * Builds the session branch and checks its content with the game's own server
 * build; reports to the server. Ends when the server goes, or on Ctrl+C.
 */

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Public/Collab/ContentWorker.h"
#include "Public/Core/CoreLog.h"

namespace
{
    std::atomic<bool> GStop{ false };

    void OnSignal(int)
    {
        GStop.store(true);
    }
}

int main(int argc, char** argv)
{
    Collab::WorkerConfig Config;
    std::string LogsDir;
    for(int Index = 1; Index + 1 < argc; Index += 2)
    {
        const std::string Name  = argv[Index];
        const std::string Value = argv[Index + 1];
        if(Name == "--connect")
        {
            const size_t Colon = Value.rfind(':');
            Config.ServerHost = Value.substr(0, Colon);
            Config.ServerPort = Colon == std::string::npos ? Collab::DefaultPort : uint16_t(std::atoi(Value.c_str() + Colon + 1));
        }
        else if(Name == "--session")        { Config.SessionId = uint32_t(std::strtoul(Value.c_str(), nullptr, 10)); }
        else if(Name == "--key")            { Config.Key = Value; }
        else if(Name == "--repo")           { Config.RepoPath = Value; }
        else if(Name == "--worktree")       { Config.WorktreeDir = Value; }
        else if(Name == "--branch")         { Config.Branch = Value; }
        else if(Name == "--module")         { Config.Module = Value; }
        else if(Name == "--preset")         { Config.Preset = Value; }
        else if(Name == "--cmake")          { Config.CMake = Value == "auto" ? std::string() : Value; }
        else if(Name == "--fetch-interval") { Config.FetchIntervalMs = uint32_t(std::strtoul(Value.c_str(), nullptr, 10)); }
        else if(Name == "--logs")           { LogsDir = Value; }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", Name.c_str());
            return 2;
        }
    }
    if(Config.Key.empty() || Config.RepoPath.empty() || Config.WorktreeDir.empty() || Config.Branch.empty() || Config.Module.empty()
       || Config.Preset.empty())
    {
        std::fprintf(stderr, "NimbleContentWorker is started by NimbleEditorServer (--repo), not by hand.\n");
        return 2;
    }

    if(!LogsDir.empty() && EngineCore::OpenRunLog(LogsDir, "ContentWorker-" + std::to_string(Config.SessionId)))
    {
        CORE_LOG("[Worker] logging to " + EngineCore::GetRunLogPath())
    }
    std::signal(SIGINT, &OnSignal);
    std::signal(SIGTERM, &OnSignal);

    Collab::ContentWorker Worker(Config);
    const int Result = Worker.Run(GStop);
    EngineCore::CloseRunLog();
    return Result;
}
