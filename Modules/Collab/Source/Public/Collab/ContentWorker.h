#ifndef COLLAB_CONTENTWORKER_H
#define COLLAB_CONTENTWORKER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/Collab/CollabAPI.h"
#include "Public/Collab/CollabProtocol.h"

/*
 * A session's content worker (Modules/Editor/COLLABORATION.md, P4): what
 * NimbleContentWorker runs, one per session, started by the server.
 *
 * It keeps a git worktree of the session branch (detached at origin's tip,
 * from the server's own checkout), fetches every FetchIntervalMs and whenever
 * the server relays a push, and for each new tip:
 *
 *   1. tells the server what moved, if nobody announced it (a push from a
 *      terminal reaches the editors as a BranchAdvanced too);
 *   2. builds the game module and its dedicated server with a server preset;
 *   3. runs <Game>Server -collab-report: the NetCore schema hash, and every
 *      world and prefab loaded by that build;
 *   4. reports BuildStatus (Building, then Succeeded with the schema hash, or
 *      Failed with the first error) and a ValidationReport.
 *
 * It talks to the server as a Collab client over loopback, introduced by the
 * key the server started it with (WorkerHello) -- not stdin/stdout, so the
 * transport, framing and messages are the editors'. Game code only ever runs
 * in the <Game>Server child, never in the worker or the server.
 *
 * PLAY TOGETHER. Asked (WorkerPlay), it runs that same <Game>Server on a
 * level and port, for the editors to join -- only on a build that succeeded at
 * the commit asked for, and never mid-build. While it runs, new commits are
 * fetched and announced but not built: the running server's files are locked.
 *
 * The build, the report and the game server sit behind IWorkerToolchain, so
 * the tests drive the rest with a fake one.
 */
namespace Collab
{
    struct COLLAB_API WorkerConfig
    {
        std::string ServerHost = "127.0.0.1";
        uint16_t    ServerPort = DefaultPort;
        uint32_t    SessionId  = 0;
        std::string Key;

        std::string RepoPath;      // the server host's checkout: its origin is fetched
        std::string WorktreeDir;   // this session's worktree, made if missing
        std::string Branch;        // the session branch
        std::string Module;        // [Game] Module: finds the game's folder in the worktree
        std::string Preset;        // the game's CMake preset, e.g. server-relwithdebinfo-msvc
        std::string CMake;         // empty: Visual Studio's own on Windows (vswhere), else "cmake"

        uint32_t    FetchIntervalMs = 30000;
    };

    struct COLLAB_API ToolchainBuild
    {
        bool        bOk = false;
        std::string FirstError;
        std::string Output;
    };

    struct COLLAB_API ToolchainReport
    {
        bool        bOk = false;
        std::string Error;          // why there's no report
        std::string Module;
        uint64_t    SchemaHash = 0;
        uint32_t    Checked = 0;
        std::vector<ContentProblem> Problems;   // paths relative to the game's folder
    };

    class COLLAB_API IWorkerToolchain
    {
    public:
        virtual ~IWorkerToolchain() = default;

        /* Configures and builds InGameDir with InPreset. */
        virtual ToolchainBuild Build(const std::string& InGameDir, const std::string& InPreset) = 0;

        /* Runs the built <Game>Server's content report. */
        virtual ToolchainReport Report(const std::string& InGameDir, const std::string& InEngineDir,
                                       const std::string& InReportPath) = 0;

        /* Play Together: the built <Game>Server on InWorld ([World_*] name;
         * empty, the game's start level) and InPort, until StopGame. False,
         * with why, if it doesn't start. */
        virtual bool StartGame(const std::string& InGameDir, const std::string& InWorld, uint16_t InPort, std::string& OutError) = 0;
        virtual bool IsGameRunning() = 0;
        virtual void StopGame() = 0;
    };

    /* InGameDir/Config/DefaultEngine.ini's [World_<Name>] whose File is
     * InLevel (runtime-relative): Name; empty if none lists it. */
    COLLAB_API std::string FindWorldName(const std::string& InGameDir, const std::string& InLevel);

    /* CMake presets and the game's own server build: the real one. On Windows,
     * outside a developer prompt, vcvarsall (found with vswhere) first; and
     * InCMake empty, Visual Studio's own CMake and Ninja (the presets need
     * CMake 3.21; an older one may be first on PATH). */
    COLLAB_API std::unique_ptr<IWorkerToolchain> MakeCMakeToolchain(const std::string& InCMake = std::string());

    /* The first compiler, linker or CMake error in InOutput; empty if none. */
    COLLAB_API std::string FindFirstBuildError(const std::string& InOutput);

    /* Application::ExecReport's file read into OutReport. */
    COLLAB_API bool ParseContentReport(const std::string& InText, ToolchainReport& OutReport);

    class COLLAB_API ContentWorker
    {
    public:
        /* InToolchain: null for MakeCMakeToolchain's. */
        ContentWorker(const WorkerConfig& InConfig, std::unique_ptr<IWorkerToolchain> InToolchain = nullptr);
        ~ContentWorker();

        ContentWorker(const ContentWorker&)            = delete;
        ContentWorker& operator=(const ContentWorker&) = delete;

        /* Until InStop, or the server goes: 0, else 1. Blocking. */
        int Run(const std::atomic<bool>& InStop);

        struct Impl;

    private:
        std::unique_ptr<Impl> P;
    };
}

#endif //COLLAB_CONTENTWORKER_H
