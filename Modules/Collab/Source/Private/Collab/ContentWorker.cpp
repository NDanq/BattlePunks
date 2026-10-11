#include "Public/Collab/ContentWorker.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <regex>
#include <thread>

#include "Private/Collab/CollabProcess.h"
#include "Private/Collab/CollabTransport.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"

namespace Collab
{
namespace
{
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;

    std::string Trim(std::string InText)
    {
        while(!InText.empty() && (InText.back() == '\n' || InText.back() == '\r' || InText.back() == ' ' || InText.back() == '\t'))
        {
            InText.pop_back();
        }
        size_t Start = 0;
        while(Start < InText.size() && (InText[Start] == ' ' || InText[Start] == '\t'))
        {
            ++Start;
        }
        return InText.substr(Start);
    }

    std::vector<std::string> SplitLines(const std::string& InText)
    {
        std::vector<std::string> Lines;
        size_t Start = 0;
        while(Start <= InText.size())
        {
            const size_t End = InText.find('\n', Start);
            std::string Line = InText.substr(Start, End == std::string::npos ? std::string::npos : End - Start);
            if(!Line.empty() && Line.back() == '\r')
            {
                Line.pop_back();
            }
            if(!Line.empty())
            {
                Lines.push_back(Line);
            }
            if(End == std::string::npos)
            {
                break;
            }
            Start = End + 1;
        }
        return Lines;
    }

    std::string Generic(const fs::path& InPath)
    {
        return InPath.generic_u8string();
    }

    std::string ReadFile(const fs::path& InPath)
    {
        std::vector<uint8_t> Bytes;
        if(!EngineCore::appFileReadAll(Generic(InPath), Bytes))
        {
            return std::string();
        }
        return std::string(Bytes.begin(), Bytes.end());
    }

    bool WriteFile(const fs::path& InPath, const std::string& InText)
    {
        EngineCore::FileHandle Out = EngineCore::appFileOpenWrite(Generic(InPath));
        if(!Out)
        {
            return false;
        }
        const bool bOk = EngineCore::appFileWrite(Out, InText.data(), InText.size()) == InText.size();
        EngineCore::appFileClose(Out);
        return bOk;
    }

    ProcessResult Git(const std::string& InDir, std::vector<std::string> InArgs)
    {
        InArgs.insert(InArgs.begin(), { "git", "-C", InDir });
        ProcessResult Result = RunProcess(InArgs, std::string(), { "GIT_TERMINAL_PROMPT=0" });
        if(!Result.bStarted || Result.ExitCode != 0)
        {
            std::string Command;
            for(const std::string& Each : InArgs) { Command += Each + " "; }
            CORE_LOG_WARN("[Worker] " + Command + "-> " + std::to_string(Result.ExitCode) + ": " + Trim(Result.Output))
        }
        return Result;
    }

    bool GitOk(const ProcessResult& InResult)
    {
        return InResult.bStarted && InResult.ExitCode == 0;
    }

    // === The real toolchain ===================================================

    class CMakeToolchain final : public IWorkerToolchain
    {
    public:
        explicit CMakeToolchain(std::string InCMake) : CMake(std::move(InCMake)) {}

        ToolchainBuild Build(const std::string& InGameDir, const std::string& InPreset) override
        {
            ToolchainBuild Out;
#if defined(_WIN32)
            /* Outside a developer prompt, vcvarsall first, in one cmd: a batch
             * file, since cmd /c doesn't take quoted arguments as CreateProcess
             * writes them. Visual Studio's own CMake and Ninja unless told
             * otherwise: the presets need CMake 3.21, and an older one is often
             * first on PATH. */
            const std::string VsRoot = FindVisualStudio();
            std::string Script = "@echo off\r\n";
            if(!std::getenv("INCLUDE"))
            {
                const fs::path VcVars = fs::u8path(VsRoot) / "VC/Auxiliary/Build/vcvarsall.bat";
                std::error_code Error;
                if(VsRoot.empty() || !fs::exists(VcVars, Error))
                {
                    Out.FirstError = "No Visual Studio with C++ tools found (vswhere), and not in a developer prompt.";
                    return Out;
                }
                Script += "call \"" + fs::path(VcVars).make_preferred().u8string() + "\" x64 >nul || exit /b 1\r\n";
            }
            std::string Tool = CMake;
            std::string MakeProgram;
            if(Tool.empty())
            {
                const fs::path Bundled = fs::u8path(VsRoot) / "Common7/IDE/CommonExtensions/Microsoft/CMake";
                std::error_code Error;
                if(!VsRoot.empty() && fs::exists(Bundled / "CMake/bin/cmake.exe", Error))
                {
                    Tool        = fs::path(Bundled / "CMake/bin/cmake.exe").make_preferred().u8string();
                    MakeProgram = fs::path(Bundled / "Ninja/ninja.exe").make_preferred().u8string();
                }
                else
                {
                    Tool = "cmake";
                }
            }
            Script += "\"" + Tool + "\" --preset " + InPreset
                    + (MakeProgram.empty() ? std::string() : " \"-DCMAKE_MAKE_PROGRAM=" + MakeProgram + "\"") + " || exit /b 1\r\n";
            Script += "\"" + Tool + "\" --build --preset " + InPreset + " || exit /b 1\r\n";
            fs::path Batch = fs::u8path(InGameDir).parent_path() / ("collab-build-" + InPreset + ".cmd");
            if(!WriteFile(Batch, Script))
            {
                Out.FirstError = "Couldn't write " + Generic(Batch);
                return Out;
            }
            const ProcessResult Result = RunProcess({ "cmd", "/c", Batch.make_preferred().u8string() }, InGameDir);
#else
            const std::string Tool = CMake.empty() ? std::string("cmake") : CMake;
            ProcessResult Result = RunProcess({ Tool, "--preset", InPreset }, InGameDir);
            if(Result.bStarted && Result.ExitCode == 0)
            {
                const ProcessResult Built = RunProcess({ Tool, "--build", "--preset", InPreset }, InGameDir);
                Result.ExitCode = Built.ExitCode;
                Result.Output  += Built.Output;
            }
#endif
            Out.Output = Result.Output;
            Out.bOk    = Result.bStarted && Result.ExitCode == 0;
            if(!Out.bOk)
            {
                Out.FirstError = Result.bStarted ? FindFirstBuildError(Result.Output) : std::string("cmake didn't start: is it on PATH?");
                if(Out.FirstError.empty())
                {
                    Out.FirstError = "The build failed (exit " + std::to_string(Result.ExitCode) + ").";
                }
            }
            return Out;
        }

        ToolchainReport Report(const std::string& InGameDir, const std::string& InEngineDir, const std::string& InReportPath) override
        {
            ToolchainReport Out;
            const fs::path Server = FindServerExecutable(fs::u8path(InGameDir) / "Binaries");
            if(Server.empty())
            {
                Out.Error = "No <Game>Server executable under Binaries/ after the build.";
                return Out;
            }
            std::error_code Ignored;
            fs::remove(fs::u8path(InReportPath), Ignored);
            const ProcessResult Result = RunProcess({ Server.u8string(), "-collab-report=" + InReportPath,
                                                      "-collab-project=" + InGameDir, "-collab-engine=" + InEngineDir },
                                                    Generic(Server.parent_path()));
            const std::string Text = ReadFile(fs::u8path(InReportPath));
            if(Text.empty())
            {
                Out.Error = "The content report wasn't written (exit " + std::to_string(Result.ExitCode) + "): "
                          + FindFirstBuildError(Result.Output);
                return Out;
            }
            ParseContentReport(Text, Out);
            if(Out.Module.empty())
            {
                Out.bOk   = false;
                Out.Error = "The game module didn't load in the server build.";
            }
            return Out;
        }

        ~CMakeToolchain() override
        {
            StopGame();
        }

        bool StartGame(const std::string& InGameDir, const std::string& InWorld, const uint16_t InPort, std::string& OutError) override
        {
            StopGame();
            const fs::path Server = FindServerExecutable(fs::u8path(InGameDir) / "Binaries");
            if(Server.empty())
            {
                OutError = "No <Game>Server executable under Binaries/.";
                return false;
            }
            /* Config overrides, as the launcher takes them (ParseLaunchArgs). */
            std::vector<std::string> Args = { Server.u8string(), "-Net.Port=" + std::to_string(InPort) };
            if(!InWorld.empty())
            {
                Args.push_back("-Game.StartLevel=" + InWorld);
            }
            Game = SpawnProcess(Args, Generic(Server.parent_path()));
            if(!Game.IsValid())
            {
                OutError = "Couldn't start " + Server.u8string() + ".";
                return false;
            }
            CORE_LOG("[Worker] game server up on UDP " + std::to_string(InPort) + (InWorld.empty() ? std::string() : ", " + InWorld))
            return true;
        }

        bool IsGameRunning() override
        {
            return IsProcessRunning(Game);
        }

        void StopGame() override
        {
            if(Game.IsValid())
            {
                KillProcess(Game);
                CORE_LOG("[Worker] game server stopped")
            }
        }

    private:
        ChildProcess Game;

#if defined(_WIN32)
        /* The latest Visual Studio with the C++ tools, by vswhere; empty if none. */
        static std::string FindVisualStudio()
        {
            const char* ProgramFiles = std::getenv("ProgramFiles(x86)");
            const fs::path VsWhere = fs::u8path(ProgramFiles ? ProgramFiles : "C:/Program Files (x86)")
                                   / "Microsoft Visual Studio/Installer/vswhere.exe";
            const ProcessResult Found = RunProcess({ VsWhere.u8string(), "-latest", "-products", "*", "-requires",
                                                     "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath" });
            return Found.bStarted ? Trim(Found.Output) : std::string();
        }
#endif

        std::string CMake;   // empty: find one

        /* The newest "<Game>Server" a build put under InBinaries/<Platform>_<Config>[_Server]/. */
        static fs::path FindServerExecutable(const fs::path& InBinaries)
        {
            fs::path Best;
            fs::file_time_type BestTime{};
            std::error_code Error;
            for(fs::directory_iterator Dir(InBinaries, Error), End; !Error && Dir != End; Dir.increment(Error))
            {
                if(!Dir->is_directory())
                {
                    continue;
                }
                std::error_code Inner;
                for(fs::directory_iterator It(Dir->path(), Inner), EndIt; !Inner && It != EndIt; It.increment(Inner))
                {
                    const std::string Name = Generic(It->path().filename());
#if defined(_WIN32)
                    const bool bServer = Name.size() > 10 && Name.compare(Name.size() - 10, 10, "Server.exe") == 0;
#else
                    const bool bServer = Name.size() > 6 && Name.compare(Name.size() - 6, 6, "Server") == 0;
#endif
                    if(bServer && It->is_regular_file() && Name.rfind("NimbleEditor", 0) != 0)
                    {
                        const fs::file_time_type Time = It->last_write_time(Inner);
                        if(Best.empty() || Time > BestTime)
                        {
                            Best     = It->path();
                            BestTime = Time;
                        }
                    }
                }
            }
            return Best;
        }
    };
}

std::unique_ptr<IWorkerToolchain> MakeCMakeToolchain(const std::string& InCMake)
{
    return std::make_unique<CMakeToolchain>(InCMake);
}

std::string FindWorldName(const std::string& InGameDir, const std::string& InLevel)
{
    /* [World_<Name>] sections, and their File= lines. */
    std::string Section;
    for(const std::string& Raw : SplitLines(ReadFile(fs::u8path(InGameDir) / "Config" / "DefaultEngine.ini")))
    {
        const std::string Line = Trim(Raw);
        if(Line.size() > 2 && Line.front() == '[' && Line.back() == ']')
        {
            Section = Line.substr(1, Line.size() - 2);
            continue;
        }
        if(Section.rfind("World_", 0) != 0 || Line.rfind("File", 0) != 0)
        {
            continue;
        }
        const size_t Equals = Line.find('=');
        if(Equals == std::string::npos || Trim(Line.substr(4, Equals - 4)) != "")
        {
            continue;
        }
        std::string File = Trim(Line.substr(Equals + 1));
        if(File.size() >= 2 && File.front() == '"' && File.back() == '"')
        {
            File = File.substr(1, File.size() - 2);
        }
        if(File == InLevel)
        {
            return Section.substr(6);
        }
    }
    return std::string();
}

std::string FindFirstBuildError(const std::string& InOutput)
{
    static const std::regex Compiler(R"(:\s*(fatal\s+)?error\b)");
    std::string Ninja;
    for(const std::string& Line : SplitLines(InOutput))
    {
        if(std::regex_search(Line, Compiler) || Line.rfind("CMake Error", 0) == 0)
        {
            return Trim(Line);
        }
        if(Ninja.empty() && Line.rfind("FAILED:", 0) == 0)
        {
            Ninja = Trim(Line);
        }
    }
    return Ninja;
}

bool ParseContentReport(const std::string& InText, ToolchainReport& OutReport)
{
    OutReport = ToolchainReport();
    for(const std::string& Line : SplitLines(InText))
    {
        const size_t Tab = Line.find('\t');
        const std::string Key   = Line.substr(0, Tab);
        const std::string Value = Tab == std::string::npos ? std::string() : Line.substr(Tab + 1);
        if(Key == "module")
        {
            OutReport.Module = Value;
        }
        else if(Key == "schema")
        {
            OutReport.SchemaHash = std::strtoull(Value.c_str(), nullptr, 16);
        }
        else if(Key == "checked")
        {
            OutReport.Checked = uint32_t(std::strtoul(Value.c_str(), nullptr, 10));
        }
        else if(Key == "world")
        {
            /* name, file, mode, networked */
            ToolchainWorld World;
            size_t Start = 0;
            std::string Fields[4];
            for(int Index = 0; Index < 4; ++Index)
            {
                const size_t Tab = Value.find('\t', Start);
                Fields[Index] = Value.substr(Start, Tab == std::string::npos ? std::string::npos : Tab - Start);
                Start = Tab == std::string::npos ? Value.size() : Tab + 1;
            }
            World.Name       = Fields[0];
            World.File       = Fields[1];
            World.GameMode   = Fields[2];
            World.bNetworked = Fields[3] == "1";
            OutReport.Worlds.push_back(World);
        }
        else if(Key == "problem")
        {
            const size_t Second = Value.find('\t');
            OutReport.Problems.push_back({ Value.substr(0, Second), Second == std::string::npos ? std::string() : Value.substr(Second + 1) });
        }
    }
    OutReport.bOk = !OutReport.Module.empty();
    return OutReport.bOk;
}

// === ContentWorker ===============================================================

struct ContentWorker::Impl
{
    WorkerConfig                      Config;
    std::unique_ptr<IWorkerToolchain> Toolchain;

    CollabTransport Transport;
    PeerID          Server = PEER_NONE;

    std::thread       Job;
    std::atomic<bool> bJobRunning{ false };
    std::atomic<bool> bFetchNow{ false };

    std::mutex                        OutboxMutex;
    std::vector<std::vector<uint8_t>> Outbox;

    /* The job thread's own. */
    std::string LastTip;    // origin's, as last fetched
    std::string BuiltTip;   // the last one built (or failed)
    std::atomic<bool> bLastBuildOk{ false };   // BuiltTip built and its report ran
    std::atomic<bool> bBuilding{ false };      // the worktree's build tree is being written: no game server now
    std::string       LastGameDir;             // BuiltTip's game folder; read only between jobs
    std::mutex        BuildMutex;              // BuiltTip and LastGameDir, which HandlePlay reads
    std::vector<ToolchainWorld> LastWorlds;     // BuiltTip's, from its report; under BuildMutex

    /* The main loop's: Play Together's game server. */
    bool     bGameRunning = false;
    uint16_t GamePort     = 0;
    std::atomic<bool> bHoldBuilds{ false };   // a game server runs: fetch and announce, don't build

    template<typename TMessage>
    void Post(TMessage& InMessage)
    {
        std::lock_guard<std::mutex> Lock(OutboxMutex);
        Outbox.push_back(Encode(InMessage));
    }

    std::string OriginRef() const { return "origin/" + Config.Branch; }

    bool EnsureWorktree()
    {
        std::error_code Error;
        const fs::path Worktree = fs::u8path(Config.WorktreeDir);
        if(fs::exists(Worktree / ".git", Error))
        {
            return GitOk(Git(Config.WorktreeDir, { "fetch", "--quiet", "origin", Config.Branch }));
        }
        if(!GitOk(Git(Config.RepoPath, { "fetch", "--quiet", "origin", Config.Branch })))
        {
            return false;
        }
        Git(Config.RepoPath, { "worktree", "prune" });
        fs::create_directories(Worktree.parent_path(), Error);
        if(!GitOk(Git(Config.RepoPath, { "worktree", "add", "--detach", "--force", Config.WorktreeDir, OriginRef() })))
        {
            return false;
        }
        CORE_LOG("[Worker] worktree at " + Config.WorktreeDir)
        return true;
    }

    /* The game's folder in the worktree: whose [Game] Module is Config.Module. */
    std::string FindGameDir() const
    {
        const std::regex ModuleLine("^\\s*Module\\s*=\\s*\"?" + Config.Module + "\"?\\s*$");
        const fs::path Worktree = fs::u8path(Config.WorktreeDir);
        std::vector<fs::path> Candidates = { Worktree };
        std::error_code Error;
        for(fs::directory_iterator It(Worktree / "Games", Error), End; !Error && It != End; It.increment(Error))
        {
            if(It->is_directory())
            {
                Candidates.push_back(It->path());
            }
        }
        for(const fs::path& Each : Candidates)
        {
            const std::string Ini = ReadFile(Each / "Config" / "DefaultEngine.ini");
            for(const std::string& Line : SplitLines(Ini))
            {
                if(std::regex_match(Line, ModuleLine))
                {
                    return Generic(Each);
                }
            }
        }
        return std::string();
    }

    BranchMove DescribeMove(const std::string& InOld, const std::string& InNew) const
    {
        BranchMove Move;
        Move.Branch = Config.Branch;
        Move.OldOid = InOld;
        Move.NewOid = InNew;
        const ProcessResult Log = Git(Config.WorktreeDir, { "log", "--max-count=200", "--format=%H%x1f%an%x1f%s", InOld + ".." + InNew });
        for(const std::string& Line : SplitLines(Log.Output))
        {
            const size_t A = Line.find('\x1f');
            const size_t B = A == std::string::npos ? std::string::npos : Line.find('\x1f', A + 1);
            if(B != std::string::npos)
            {
                Move.Commits.push_back({ Line.substr(0, A), Line.substr(A + 1, B - A - 1), Line.substr(B + 1) });
            }
        }
        const ProcessResult Diff = Git(Config.WorktreeDir, { "diff", "--name-only", InOld, InNew });
        Move.Paths = SplitLines(Diff.Output);
        SortUnique(Move.Paths);
        return Move;
    }

    /* WorkerPlay: start or stop the game server, on the main loop. */
    void HandlePlay(const std::vector<uint8_t>& InBytes)
    {
        WorkerPlayMessage Play;
        if(!Decode(InBytes, Play))
        {
            return;
        }
        std::string Tip, GameDir;
        std::vector<ToolchainWorld> Worlds;
        {
            std::lock_guard<std::mutex> Lock(BuildMutex);
            Tip     = BuiltTip;
            GameDir = LastGameDir;
            Worlds  = LastWorlds;
        }
        WorkerPlayStateMessage State;
        if(Play.bStop)
        {
            Toolchain->StopGame();
            bGameRunning = false;
            bHoldBuilds  = false;
            State.Error  = "Stopped.";
            Post(State);
            return;
        }
        if(bBuilding)
        {
            State.Error = "The server is building the branch; try again when it's built.";   // BuiltTip is the job's now
        }
        else if(!bLastBuildOk || (!Play.Oid.empty() && Play.Oid != Tip))
        {
            State.Error = "The server has no good build of " + (Play.Oid.empty() ? std::string("the branch") : Play.Oid.substr(0, 10)) + ".";
        }
        else
        {
            const std::string World = Play.Level.empty() ? std::string() : FindWorldName(GameDir, Play.Level);
            if(!Play.Level.empty() && World.empty())
            {
                State.Error = Play.Level + " isn't in the game's world list (Project Settings > Worlds).";
            }
            else if(Toolchain->StartGame(GameDir, World, Play.Port, State.Error))
            {
                bGameRunning   = true;
                bHoldBuilds    = true;
                GamePort       = Play.Port;
                State.bRunning = true;
                State.Port     = Play.Port;
                /* Whether players will see each other there: its mode, from the report. */
                for(const ToolchainWorld& Each : Worlds)
                {
                    if(Each.Name == World)
                    {
                        State.GameMode   = Each.GameMode;
                        State.bNetworked = Each.bNetworked;
                    }
                }
            }
        }
        if(!State.bRunning)
        {
            CORE_LOG_WARN("[Worker] no game server: " + State.Error)
        }
        Post(State);
    }

    /* The game server stopped by itself (a crash, a quit): say so. */
    void WatchGame()
    {
        if(bGameRunning && !Toolchain->IsGameRunning())
        {
            bGameRunning = false;
            bHoldBuilds  = false;
            WorkerPlayStateMessage State;
            State.Error = "The game server stopped.";
            CORE_LOG_WARN("[Worker] the game server stopped by itself")
            Post(State);
        }
    }

    void Cycle()
    {
        if(!EnsureWorktree())
        {
            return;
        }
        const ProcessResult Rev = Git(Config.WorktreeDir, { "rev-parse", OriginRef() });
        const std::string Tip = Trim(Rev.Output);
        if(!GitOk(Rev) || Tip.empty() || Tip == BuiltTip)
        {
            return;
        }

        /* A move nobody announced (a push from outside the editor): the server
         * relays it, unless an editor already did. */
        if(!LastTip.empty() && Tip != LastTip)
        {
            PushedMessage Moved;
            Moved.Move = DescribeMove(LastTip, Tip);
            Post(Moved);
        }
        LastTip = Tip;
        if(bHoldBuilds)
        {
            return;   // built once the run ends: the running server's files are locked
        }

        Git(Config.WorktreeDir, { "checkout", "--quiet", "--detach", "--force", Tip });
        /* Not -x, and Build/ and Binaries/ kept by name too: on a case-sensitive
         * file system a game's "build/" ignore line doesn't cover Build/. */
        Git(Config.WorktreeDir, { "clean", "--quiet", "-fd", "-e", "Build/", "-e", "Binaries/" });

        BuildStatusMessage Status;
        Status.Oid = Tip;
        const ProcessResult Who = Git(Config.WorktreeDir, { "log", "-1", "--format=%an%x1f%s", Tip });
        const std::string WhoLine = Trim(Who.Output);
        const size_t Split = WhoLine.find('\x1f');
        Status.Author  = WhoLine.substr(0, Split);
        Status.Summary = Split == std::string::npos ? std::string() : WhoLine.substr(Split + 1);
        Status.State   = EBuildState::Building;
        Post(Status);
        CORE_LOG("[Worker] building " + Tip.substr(0, 10) + " (" + Status.Summary + ")")
        const Clock::time_point Started = Clock::now();
        {
            std::lock_guard<std::mutex> Lock(BuildMutex);
            BuiltTip = Tip;
        }
        bLastBuildOk = false;
        bBuilding    = true;

        const std::string GameDir = FindGameDir();
        {
            std::lock_guard<std::mutex> Lock(BuildMutex);
            LastGameDir = GameDir;
        }
        if(GameDir.empty())
        {
            bBuilding         = false;
            Status.State      = EBuildState::Failed;
            Status.FirstError = "No game in the worktree has [Game] Module = " + Config.Module + ".";
            Post(Status);
            return;
        }

        const ToolchainBuild Built = Toolchain->Build(GameDir, Config.Preset);
        Status.Seconds = uint32_t(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - Started).count());
        if(!Built.bOk)
        {
            bBuilding         = false;
            Status.State      = EBuildState::Failed;
            Status.FirstError = Built.FirstError;
            CORE_LOG_WARN("[Worker] " + Tip.substr(0, 10) + " failed: " + Built.FirstError)
            Post(Status);
            return;
        }

        const std::string ReportPath = Generic(fs::u8path(Config.WorktreeDir).parent_path() / ("collab-report-" + std::to_string(Config.SessionId) + ".txt"));
        const ToolchainReport Report = Toolchain->Report(GameDir, Generic(fs::u8path(Config.WorktreeDir) / "Engine"), ReportPath);
        Status.Seconds = uint32_t(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - Started).count());
        if(!Report.bOk)
        {
            bBuilding         = false;
            Status.State      = EBuildState::Failed;
            Status.FirstError = Report.Error;
            Post(Status);
            return;
        }
        bLastBuildOk      = true;
        bBuilding         = false;
        {
            std::lock_guard<std::mutex> Lock(BuildMutex);
            LastWorlds = Report.Worlds;
        }
        Status.State      = EBuildState::Succeeded;
        Status.SchemaHash = Report.SchemaHash;
        Post(Status);

        /* Repository-relative, as the editors name files. */
        std::error_code Error;
        const fs::path Relative = fs::relative(fs::u8path(GameDir), fs::u8path(Config.WorktreeDir), Error);
        const std::string Prefix = Error || Relative == "." ? std::string() : Generic(Relative) + "/";
        ValidationReportMessage Validation;
        Validation.Oid     = Tip;
        Validation.Author  = Status.Author;
        Validation.Checked = Report.Checked;
        for(const ContentProblem& Each : Report.Problems)
        {
            Validation.Problems.push_back({ Prefix + Each.Path, Each.Message });
        }
        Post(Validation);
        CORE_LOG("[Worker] " + Tip.substr(0, 10) + " built in " + std::to_string(Status.Seconds) + " s; "
                 + std::to_string(Report.Checked) + " file(s) checked, " + std::to_string(Report.Problems.size()) + " problem(s)")
    }
};

ContentWorker::ContentWorker(const WorkerConfig& InConfig, std::unique_ptr<IWorkerToolchain> InToolchain)
    : P(new Impl())
{
    P->Config    = InConfig;
    P->Toolchain = InToolchain ? std::move(InToolchain) : MakeCMakeToolchain(InConfig.CMake);
}

ContentWorker::~ContentWorker()
{
    if(P->Job.joinable())
    {
        P->Job.join();
    }
}

int ContentWorker::Run(const std::atomic<bool>& InStop)
{
    if(!P->Transport.Connect(P->Config.ServerHost.c_str(), P->Config.ServerPort, 10000, P->Server))
    {
        return 1;
    }
    bool bIntroduced = false;
    int  Result = 0;
    Clock::time_point NextFetch = Clock::now();

    while(!InStop.load())
    {
        TransportEvent Event;
        bool bGone = false;
        while(P->Transport.Service(Event))
        {
            if(Event.EventType == TransportEvent::EType::Connected)
            {
                WorkerHelloMessage Hello;
                Hello.SessionId = P->Config.SessionId;
                Hello.Key       = P->Config.Key;
                P->Transport.Send(P->Server, Encode(Hello));
                bIntroduced = true;
                CORE_LOG("[Worker] connected for session " + std::to_string(P->Config.SessionId))
            }
            else if(Event.EventType == TransportEvent::EType::Disconnected)
            {
                bGone = true;
                break;
            }
            else if(PeekType(Event.Data) == EMessage::BranchAdvanced)
            {
                P->bFetchNow = true;   // an editor pushed: no need to wait
            }
            else if(PeekType(Event.Data) == EMessage::WorkerPlay)
            {
                P->HandlePlay(Event.Data);
            }
        }
        if(bGone)
        {
            CORE_LOG("[Worker] the server is gone")
            Result = 1;
            break;
        }

        P->WatchGame();
        {
            std::lock_guard<std::mutex> Lock(P->OutboxMutex);
            for(const std::vector<uint8_t>& Each : P->Outbox)
            {
                P->Transport.Send(P->Server, Each);
            }
            P->Outbox.clear();
        }
        P->Transport.Flush();

        const Clock::time_point Now = Clock::now();
        if(bIntroduced && !P->bJobRunning && (Now >= NextFetch || P->bFetchNow))
        {
            if(P->Job.joinable())
            {
                P->Job.join();
            }
            P->bFetchNow   = false;
            P->bJobRunning = true;
            NextFetch = Now + std::chrono::milliseconds(P->Config.FetchIntervalMs);
            P->Job = std::thread([Self = P.get()]()
            {
                Self->Cycle();
                Self->bJobRunning = false;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    P->Toolchain->StopGame();
    if(P->Job.joinable())
    {
        P->Job.join();
    }
    /* What the last job posted, then goodbye. */
    {
        std::lock_guard<std::mutex> Lock(P->OutboxMutex);
        for(const std::vector<uint8_t>& Each : P->Outbox)
        {
            P->Transport.Send(P->Server, Each);
        }
        P->Outbox.clear();
    }
    P->Transport.Flush();
    P->Transport.Close();
    return Result;
}
}
