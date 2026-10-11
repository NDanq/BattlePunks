/*
 * Collab over the loopback interface: a CollabServer and several CollabClients
 * in one process. Covers the protocol helpers, joining, presence and its dirty
 * paths sent as changes, overlaps, leaving, the refusals (engine mismatch,
 * uncommitted engine changes, password, protocol version, no hello) and
 * timeouts (nothing answering, an editor gone silent).
 *
 * Real sockets on 127.0.0.1, on ports the OS picks. Every wait is bounded.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "Private/Collab/CollabProcess.h"
#include "Private/Collab/CollabTransport.h"
#include "Public/Collab/CollabClient.h"
#include "Public/Collab/CollabProtocol.h"
#include "Public/Collab/CollabServer.h"
#include "Public/Collab/ContentWorker.h"

using namespace Collab;

namespace
{
    int GFailures = 0;

    void Check(const bool bInCondition, const char* InWhat)
    {
        if(!bInCondition)
        {
            ++GFailures;
            std::fprintf(stderr, "FAILED: %s\n", InWhat);
        }
        else
        {
            std::printf("ok: %s\n", InWhat);
        }
    }

    using Clock = std::chrono::steady_clock;

    /* Polls InServer and InClients until InDone or InMs pass. */
    bool PumpUntil(CollabServer& InServer, const std::vector<CollabClient*>& InClients, const std::function<bool()>& InDone,
                   const int InMs = 3000)
    {
        const Clock::time_point End = Clock::now() + std::chrono::milliseconds(InMs);
        while(Clock::now() < End)
        {
            InServer.Poll();
            for(CollabClient* Each : InClients)
            {
                Each->Poll();
            }
            if(InDone())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return InDone();
    }

    /* Drains InClient's events into a list, for assertions. */
    struct EventLog
    {
        std::vector<ClientEvent> Events;

        void Drain(CollabClient& InClient)
        {
            ClientEvent Event;
            while(InClient.NextEvent(Event))
            {
                Events.push_back(Event);
            }
        }

        const ClientEvent* Find(const ClientEvent::EType InType) const
        {
            for(const ClientEvent& Each : Events)
            {
                if(Each.Type == InType)
                {
                    return &Each;
                }
            }
            return nullptr;
        }

        int Count(const ClientEvent::EType InType) const
        {
            int N = 0;
            for(const ClientEvent& Each : Events)
            {
                N += Each.Type == InType ? 1 : 0;
            }
            return N;
        }

        void Clear() { Events.clear(); }
    };

    ClientIdentity Identity(const char* InUser, const char* InEngine = "aaaaaaaaaaaa1111", const char* InBranch = "collab/forest")
    {
        ClientIdentity Out;
        Out.User       = InUser;
        Out.ProjectId  = MakeProjectId("WorldsLab", "git@github.com:Nimble/WorldsLab.git");
        Out.Branch     = InBranch;
        Out.Head       = "1111111111111111111111111111111111111111";
        Out.EngineTree = InEngine;
        return Out;
    }

    /* Connects InClient and pumps until it is joined or refused; its events in OutLog. */
    bool JoinOrRefuse(CollabServer& InServer, std::vector<CollabClient*> InOthers, CollabClient& InClient, EventLog& OutLog,
                      const ClientIdentity& InIdentity)
    {
        if(!InClient.Connect("127.0.0.1", InServer.GetPort(), InIdentity, 3000))
        {
            return false;
        }
        InOthers.push_back(&InClient);
        return PumpUntil(InServer, InOthers, [&]()
        {
            OutLog.Drain(InClient);
            return OutLog.Find(ClientEvent::EType::Joined) || OutLog.Find(ClientEvent::EType::Rejected)
                || OutLog.Find(ClientEvent::EType::ConnectFailed);
        });
    }

    void TestHelpers()
    {
        std::printf("--- protocol helpers\n");
        Check(NormalizeRemoteUrl("git@GitHub.com:Nimble/WorldsLab.git") == "github.com/Nimble/WorldsLab", "scp syntax");
        Check(NormalizeRemoteUrl("https://me@github.com/Nimble/WorldsLab") == "github.com/Nimble/WorldsLab", "https with a user");
        Check(NormalizeRemoteUrl("ssh://git@github.com:22/Nimble/WorldsLab.git/") == "github.com/Nimble/WorldsLab", "ssh with a port");
        Check(NormalizeRemoteUrl("  https://GITHUB.com/Nimble/WorldsLab.GIT\n") == "github.com/Nimble/WorldsLab", "case and whitespace");
        Check(NormalizeRemoteUrl("C:\\Repos\\WorldsLab.git") == "C:/Repos/WorldsLab", "a Windows path");
        Check(NormalizeRemoteUrl("/srv/git/WorldsLab.git") == "/srv/git/WorldsLab", "a POSIX path");
        Check(MakeProjectId("WorldsLab", "git@github.com:Nimble/WorldsLab.git")
              == MakeProjectId("WorldsLab", "https://github.com/Nimble/WorldsLab"), "one project however it's cloned");

        Check(ConstantTimeEquals("hunter2", "hunter2"), "equal passwords");
        Check(!ConstantTimeEquals("hunter2", "hunter3") && !ConstantTimeEquals("hunter2", "hunter22")
              && !ConstantTimeEquals("", "x"), "unequal passwords");

        std::vector<std::string> Old = { "a", "b", "c" }, New = { "b", "c", "d" }, Added, Removed;
        DiffSorted(Old, New, Added, Removed);
        Check(Added == std::vector<std::string>{ "d" } && Removed == std::vector<std::string>{ "a" }, "DiffSorted");
        ApplyDiff(Old, Added, Removed);
        Check(Old == New, "ApplyDiff undoes it");

        PresenceMessage Out;
        Out.MemberId   = 7;
        Out.OpenLevel  = "Games/WorldsLab/Content/Levels/Forest.nmworld";
        Out.Play       = EPlayState::Playing;
        Out.bFullDirty = true;
        Out.DirtyAdded = { "x", "y" };
        const std::vector<uint8_t> Bytes = Encode(Out);
        Check(PeekType(Bytes) == EMessage::Presence, "PeekType");
        PresenceMessage In;
        Check(Decode(Bytes, In) && In.MemberId == 7 && In.OpenLevel == Out.OpenLevel && In.Play == EPlayState::Playing
              && In.bFullDirty && In.DirtyAdded == Out.DirtyAdded, "a message round-trips");
        HelloMessage Wrong;
        Check(!Decode(Bytes, Wrong), "decoding as another type fails");
        std::vector<uint8_t> Cut(Bytes.begin(), Bytes.begin() + long(Bytes.size() / 2));
        Check(!Decode(Cut, In), "a cut-short message fails");
        Check(PeekType({ 1, 2, 3 }) == EMessage::None, "garbage is no message");
    }

    void TestSession()
    {
        std::printf("--- join, presence, overlaps, leave\n");
        CollabServer Server;
        ServerConfig Config;
        Config.Port = 0;
        Check(Server.Start(Config) && Server.GetPort() != 0, "the server listens");

        CollabClient Alice, Bob;
        EventLog AliceLog, BobLog;
        Check(JoinOrRefuse(Server, {}, Alice, AliceLog, Identity("alice")) && AliceLog.Find(ClientEvent::EType::Joined),
              "Alice joins");
        Check(Alice.GetRoster().size() == 1 && Alice.GetMyId() != 0, "alone in the session");

        Alice.SetPresence("1111", "Games/WorldsLab/Content/Levels/Forest.nmworld", EPlayState::Editing,
                          { "Games/WorldsLab/Content/Levels/Forest.nmworld", "Games/WorldsLab/Source/Pickup.cpp" });

        Check(JoinOrRefuse(Server, { &Alice }, Bob, BobLog, Identity("bob")) && BobLog.Find(ClientEvent::EType::Joined), "Bob joins");
        Check(Bob.GetRoster().size() == 2, "Bob's roster has both");
        Check(PumpUntil(Server, { &Alice, &Bob }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::MemberJoined) != nullptr; }),
              "Alice hears Bob joined");
        Check(Alice.GetRoster().size() == 2, "Alice's roster has both");
        Check(Server.GetSessions().size() == 1 && Server.GetMemberCount() == 2, "one session, two members");

        /* Bob's roster entry for Alice carries her presence, sent before he joined. */
        const auto AliceSeen = Bob.GetRoster().find(Alice.GetMyId());
        Check(AliceSeen != Bob.GetRoster().end() && AliceSeen->second.Dirty.size() == 2
              && AliceSeen->second.OpenLevel == "Games/WorldsLab/Content/Levels/Forest.nmworld", "a joiner sees earlier presence");

        /* A change goes as a change. */
        BobLog.Clear();
        Alice.SetPresence("2222", "Games/WorldsLab/Content/Levels/Forest.nmworld", EPlayState::Playing,
                          { "Games/WorldsLab/Content/Levels/Forest.nmworld" });
        Check(PumpUntil(Server, { &Alice, &Bob }, [&]() { BobLog.Drain(Bob); return BobLog.Find(ClientEvent::EType::MemberChanged) != nullptr; }),
              "Bob hears Alice's presence");
        const MemberState& AliceNow = Bob.GetRoster().at(Alice.GetMyId());
        Check(AliceNow.Head == "2222" && AliceNow.Play == EPlayState::Playing, "her HEAD and play state");
        Check(AliceNow.Dirty == std::vector<std::string>{ "Games/WorldsLab/Content/Levels/Forest.nmworld" }, "her dirty paths, one removed");

        /* The same file dirty for both: an overlap, each way. */
        AliceLog.Clear();
        BobLog.Clear();
        Bob.SetPresence("2222", std::string(), EPlayState::Editing, { "Games/WorldsLab/Content/Levels/Forest.nmworld", "Games/WorldsLab/Config/DefaultGame.ini" });
        Check(BobLog.Count(ClientEvent::EType::Overlap) == 0, "(nothing until polled)");
        Bob.Poll();
        BobLog.Drain(Bob);
        const ClientEvent* BobOverlap = BobLog.Find(ClientEvent::EType::Overlap);
        Check(BobOverlap && BobOverlap->User == "alice" && BobOverlap->Path == "Games/WorldsLab/Content/Levels/Forest.nmworld",
              "Bob is warned Alice has the level dirty too");
        Check(PumpUntil(Server, { &Alice, &Bob }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::Overlap) != nullptr; }),
              "and Alice that Bob has");
        Check(Bob.WhoHasDirty("Games/WorldsLab/Content/Levels/Forest.nmworld") == std::vector<std::string>{ "alice" }, "WhoHasDirty");
        Check(Alice.GetOthersDirty().size() == 2, "GetOthersDirty: Bob's two");

        /* Not again for the same pair. */
        BobLog.Clear();
        Bob.SetPresence("3333", std::string(), EPlayState::Editing, { "Games/WorldsLab/Content/Levels/Forest.nmworld", "Games/WorldsLab/Config/DefaultGame.ini" });
        PumpUntil(Server, { &Alice, &Bob }, [&]() { return false; }, 100);
        BobLog.Drain(Bob);
        Check(BobLog.Count(ClientEvent::EType::Overlap) == 0, "an overlap is reported once");

        /* Another branch is another session -- another engine is fine there. */
        CollabClient Carol;
        EventLog CarolLog;
        Check(JoinOrRefuse(Server, { &Alice, &Bob }, Carol, CarolLog, Identity("carol", "bbbbbbbbbbbb2222", "main"))
              && CarolLog.Find(ClientEvent::EType::Joined), "another branch: its own session");
        Check(Server.GetSessions().size() == 2 && Carol.GetRoster().size() == 1, "two sessions");
        AliceLog.Clear();
        PumpUntil(Server, { &Alice, &Bob, &Carol }, [&]() { return false; }, 100);
        AliceLog.Drain(Alice);
        Check(!AliceLog.Find(ClientEvent::EType::MemberJoined), "and nobody else hears of her");

        /* Leaving. */
        AliceLog.Clear();
        BobLog.Clear();
        Bob.Leave();
        Check(PumpUntil(Server, { &Alice, &Bob, &Carol }, [&]()
        {
            AliceLog.Drain(Alice);
            BobLog.Drain(Bob);
            return AliceLog.Find(ClientEvent::EType::MemberLeft) && BobLog.Find(ClientEvent::EType::Disconnected);
        }), "Bob leaves; Alice hears");
        Check(AliceLog.Find(ClientEvent::EType::MemberLeft) && AliceLog.Find(ClientEvent::EType::MemberLeft)->User == "bob", "by name");
        Check(Alice.GetRoster().size() == 1 && Bob.GetState() == CollabClient::EState::Idle, "the rosters follow");

        Alice.Leave();
        Carol.Leave();
        Check(PumpUntil(Server, { &Alice, &Carol }, [&]() { return Server.GetSessions().empty(); }), "an empty session ends");
    }

    void TestPushes()
    {
        std::printf("--- pushes\n");
        CollabServer Server;
        ServerConfig Config;
        Config.Port = 0;
        Server.Start(Config);

        CollabClient Alice, Bob;
        EventLog AliceLog, BobLog;
        JoinOrRefuse(Server, {}, Alice, AliceLog, Identity("alice"));
        Check(JoinOrRefuse(Server, { &Alice }, Bob, BobLog, Identity("bob")) && BobLog.Find(ClientEvent::EType::Joined), "two editors");
        AliceLog.Clear();
        BobLog.Clear();

        BranchMove Move;
        Move.Branch  = "collab/forest";
        Move.OldOid  = "1111111111111111111111111111111111111111";
        Move.NewOid  = "2222222222222222222222222222222222222222";
        Move.Commits = { { "2222222222222222222222222222222222222222", "alice", "Trees" },
                         { "1515151515151515151515151515151515151515", "alice", "Pickup" } };
        Move.Paths   = { "Games/WorldsLab/Content/Levels/Forest.nmworld", "Games/WorldsLab/Source/Pickup.cpp" };
        Alice.AnnouncePush(Move);
        Check(PumpUntil(Server, { &Alice, &Bob }, [&]() { BobLog.Drain(Bob); return BobLog.Find(ClientEvent::EType::BranchAdvanced) != nullptr; }),
              "Bob hears Alice's push");
        const ClientEvent* Advanced = BobLog.Find(ClientEvent::EType::BranchAdvanced);
        Check(Advanced && Advanced->User == "alice" && Advanced->MemberId == Alice.GetMyId(), "from Alice");
        Check(Advanced && Advanced->Move.NewOid == Move.NewOid && Advanced->Move.OldOid == Move.OldOid, "old and new commit");
        Check(Advanced && Advanced->Move.Commits.size() == 2 && Advanced->Move.Commits[0].Summary == "Trees", "the commits");
        Check(Advanced && Advanced->Move.Paths == Move.Paths, "the changed paths");
        AliceLog.Drain(Alice);
        Check(!AliceLog.Find(ClientEvent::EType::BranchAdvanced), "not echoed to the pusher");
        Check(!Server.GetSessions().empty() && Server.GetSessions()[0].Tip == Move.NewOid, "the session's tip follows");

        /* The same tip again is nothing new; no old commit given, the tip stands in. */
        BobLog.Clear();
        Alice.AnnouncePush(Move);
        BranchMove Next = Move;
        Next.OldOid.clear();
        Next.NewOid = "3333333333333333333333333333333333333333";
        Bob.AnnouncePush(Next);
        Check(PumpUntil(Server, { &Alice, &Bob }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::BranchAdvanced) != nullptr; }),
              "Alice hears Bob's push");
        Advanced = AliceLog.Find(ClientEvent::EType::BranchAdvanced);
        Check(Advanced && Advanced->Move.OldOid == Move.NewOid, "its old commit is the session's tip");
        PumpUntil(Server, { &Alice, &Bob }, [&]() { return false; }, 100);
        BobLog.Drain(Bob);
        Check(BobLog.Count(ClientEvent::EType::BranchAdvanced) == 0, "a repeated push isn't relayed");

        /* Another branch: only the pusher is told, that nobody else was. */
        AliceLog.Clear();
        BobLog.Clear();
        BranchMove Elsewhere = Move;
        Elsewhere.Branch = "main";
        Elsewhere.NewOid = "4444444444444444444444444444444444444444";
        Alice.AnnouncePush(Elsewhere);
        Check(PumpUntil(Server, { &Alice, &Bob }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::Notified) != nullptr; }),
              "pushing another branch: the pusher is told");
        BobLog.Drain(Bob);
        Check(!BobLog.Find(ClientEvent::EType::BranchAdvanced), "and the others aren't");
    }

    // === The content worker ================================================

    namespace fs = std::filesystem;

    bool RunGit(const fs::path& InDir, std::vector<std::string> InArgs, std::string* OutText = nullptr)
    {
        InArgs.insert(InArgs.begin(), { "git", "-C", InDir.generic_u8string() });
        const ProcessResult Result = RunProcess(InArgs);
        if(OutText)
        {
            *OutText = Result.Output;
            while(!OutText->empty() && (OutText->back() == '\n' || OutText->back() == '\r')) { OutText->pop_back(); }
        }
        return Result.bStarted && Result.ExitCode == 0;
    }

    void WriteText(const fs::path& InPath, const std::string& InText)
    {
        fs::create_directories(InPath.parent_path());
        FILE* File = nullptr;
#if defined(_WIN32)
        _wfopen_s(&File, InPath.wstring().c_str(), L"wb");
#else
        File = std::fopen(InPath.c_str(), "wb");
#endif
        if(File)
        {
            std::fwrite(InText.data(), 1, InText.size(), File);
            std::fclose(File);
        }
    }

    /* The fake game server's state, shared with the test. */
    struct FakeGame
    {
        std::mutex        Mutex;
        std::atomic<bool> bRunning{ false };
        std::atomic<int>  Starts{ 0 };
        std::string       World;
        uint16_t          Port = 0;
    };

    /* Builds succeed unless the game folder holds BROKEN; the report finds
     * every .nmentity that says "not xml"; the game server is FakeGame. */
    class FakeToolchain final : public IWorkerToolchain
    {
    public:
        explicit FakeToolchain(FakeGame* InGame = nullptr) : Game(InGame) {}

        bool StartGame(const std::string&, const std::string& InWorld, const uint16_t InPort, std::string& OutError) override
        {
            if(!Game)
            {
                OutError = "no fake game";
                return false;
            }
            std::lock_guard<std::mutex> Lock(Game->Mutex);
            Game->World    = InWorld;
            Game->Port     = InPort;
            Game->bRunning = true;
            ++Game->Starts;
            return true;
        }
        bool IsGameRunning() override { return Game && Game->bRunning; }
        void StopGame() override { if(Game) { Game->bRunning = false; } }

        FakeGame* Game = nullptr;

        ToolchainBuild Build(const std::string& InGameDir, const std::string&) override
        {
            ToolchainBuild Out;
            if(fs::exists(fs::u8path(InGameDir) / "BROKEN"))
            {
                Out.Output     = "[1/2] Building Pickup.cpp\nFAILED: Pickup.cpp.obj\nPickup.cpp(3): error C2065: 'Ammo': undeclared identifier\n";
                Out.FirstError = FindFirstBuildError(Out.Output);
                return Out;
            }
            Out.bOk = true;
            return Out;
        }

        ToolchainReport Report(const std::string& InGameDir, const std::string&, const std::string&) override
        {
            ToolchainReport Out;
            Out.bOk        = true;
            Out.Module     = "WorldsLabGame";
            Out.SchemaHash = 0x2f8d597745b8342full;
            for(const fs::directory_entry& Each : fs::recursive_directory_iterator(fs::u8path(InGameDir) / "Content"))
            {
                const std::string Name = Each.path().generic_u8string();
                if(Name.size() > 8 && Name.compare(Name.size() - 8, 8, ".nmworld") == 0)
                {
                    ++Out.Checked;
                }
                if(Name.size() > 9 && Name.compare(Name.size() - 9, 9, ".nmentity") == 0)
                {
                    FILE* File = nullptr;
#if defined(_WIN32)
                    _wfopen_s(&File, Each.path().wstring().c_str(), L"rb");
#else
                    File = std::fopen(Each.path().c_str(), "rb");
#endif
                    char Buffer[64] = {};
                    if(File)
                    {
                        std::fread(Buffer, 1, sizeof(Buffer) - 1, File);
                        std::fclose(File);
                    }
                    if(std::string(Buffer).find("not xml") != std::string::npos)
                    {
                        Out.Problems.push_back({ fs::relative(Each.path(), fs::u8path(InGameDir)).generic_u8string(),
                                                 "is not valid XML" });
                    }
                }
            }
            return Out;
        }
    };

    void TestWorkerHelpers()
    {
        std::printf("--- worker helpers\n");
        Check(FindFirstBuildError("ninja: Entering\nFAILED: a.obj\nC:/x/a.cpp(12): error C2065: 'X': undeclared\nmore\n")
              == "C:/x/a.cpp(12): error C2065: 'X': undeclared", "the compiler's error, not Ninja's FAILED");
        Check(FindFirstBuildError("a.cpp:3:7: error: use of undeclared identifier\n") == "a.cpp:3:7: error: use of undeclared identifier",
              "clang's");
        Check(FindFirstBuildError("CMake Error at CMakeLists.txt:4 (add_library):\n") == "CMake Error at CMakeLists.txt:4 (add_library):",
              "CMake's");
        Check(FindFirstBuildError("FAILED: link\n") == "FAILED: link", "Ninja's when there's nothing better");
        Check(FindFirstBuildError("all good\n").empty(), "none in a clean log");

        ToolchainReport Report;
        Check(ParseContentReport("module\tWorldsLabGame\nschema\t2f8d597745b8342f\nchecked\t13\n"
                                 "problem\tContent/Levels/A/x.nmentity\tis not valid XML\n", Report), "a report parses");
        Check(Report.Module == "WorldsLabGame" && Report.SchemaHash == 0x2f8d597745b8342full && Report.Checked == 13, "its fields");
        Check(Report.Problems.size() == 1 && Report.Problems[0].Path == "Content/Levels/A/x.nmentity"
              && Report.Problems[0].Message == "is not valid XML", "its problem");
        Check(!ParseContentReport("module\t\nschema\t0\n", Report), "no module: no report");
    }

    void TestWorker()
    {
        std::printf("--- the content worker\n");
        if(!RunGit(fs::current_path(), { "--version" }))
        {
            std::printf("  (git isn't on PATH: skipped)\n");
            return;
        }
        const fs::path Temp = fs::temp_directory_path() / ("CollabWorkerTest-" + std::to_string(Clock::now().time_since_epoch().count()));
        fs::create_directories(Temp);
        const fs::path Remote = Temp / "remote.git";
        const fs::path Host   = Temp / "host";
        const fs::path Bob    = Temp / "bob";
        RunGit(Temp, { "init", "-q", "--bare", "-b", "main", Remote.generic_u8string() });
        RunGit(Temp, { "-c", "core.autocrlf=false", "clone", "-q", Remote.generic_u8string(), Bob.generic_u8string() });
        RunGit(Bob, { "config", "user.name", "bob" });
        RunGit(Bob, { "config", "user.email", "bob@example.com" });
        RunGit(Bob, { "config", "commit.gpgsign", "false" });
        RunGit(Bob, { "switch", "-q", "-c", "main" });
        WriteText(Bob / "Games/WorldsLab/Config/DefaultEngine.ini",
                  "[Game]\nModule=WorldsLabGame\n\n[World_A]\nFile=Content/Levels/A.nmworld\nGameMode=Arena\n");
        WriteText(Bob / "Games/WorldsLab/Content/Levels/A.nmworld", "<World Format=\"1\" />\n");
        WriteText(Bob / "Games/WorldsLab/Content/Levels/A/good.nmentity", "<Entity />\n");
        WriteText(Bob / "Engine/Core.txt", "engine\n");
        RunGit(Bob, { "add", "-A" });
        RunGit(Bob, { "commit", "-qm", "Start" });
        Check(RunGit(Bob, { "push", "-q", "-u", "origin", "main" }), "a shared repository");
        RunGit(Temp, { "-c", "core.autocrlf=false", "clone", "-q", Remote.generic_u8string(), Host.generic_u8string() });

        /* The worker, in-process with the fake toolchain. */
        std::atomic<bool> bStopWorker{ false };
        std::thread       WorkerThread;
        int               Launched = 0;
        CollabServer Server;
        ServerConfig Config;
        Config.Port     = 0;
        Config.RepoPath = Host.generic_u8string();
        Config.DataDir  = (Temp / "data").generic_u8string();
        Config.WorkerFetchIntervalMs = 300;
        Config.GamePortBase      = 17777;
        Config.PlayIdleTimeoutMs = 1500;
        FakeGame Game;
        Config.LaunchWorker = [&](const WorkerConfig& InWorker)
        {
            ++Launched;
            Check(InWorker.Module == "WorldsLabGame" && InWorker.Branch == "main", "the worker is told the game and the branch");
            Check(!InWorker.Key.empty() && InWorker.SessionId != 0, "and its key and session");
            WorkerThread = std::thread([InWorker, &bStopWorker, &Game]()
            {
                ContentWorker Worker(InWorker, std::make_unique<FakeToolchain>(&Game));
                Worker.Run(bStopWorker);
            });
        };
        Config.StopWorker = [&](uint32_t)
        {
            bStopWorker = true;
            if(WorkerThread.joinable()) { WorkerThread.join(); }
        };
        Check(Server.Start(Config), "a server with a checkout");

        ClientIdentity AliceId = Identity("alice");
        AliceId.ProjectId = MakeProjectId("WorldsLabGame", Remote.generic_u8string());
        AliceId.Branch    = "main";
        CollabClient Alice;
        EventLog AliceLog;
        Check(JoinOrRefuse(Server, {}, Alice, AliceLog, AliceId) && AliceLog.Find(ClientEvent::EType::Joined), "Alice joins");
        Check(Launched == 1, "a new session starts its worker");
        Check(PumpUntil(Server, { &Alice }, [&]() { return !Server.GetSessions().empty() && Server.GetSessions()[0].bWorkerConnected; }, 10000),
              "the worker connects with its key");

        /* The first build. */
        Check(PumpUntil(Server, { &Alice }, [&]() { AliceLog.Drain(Alice); return Alice.GetBuildStatus().State == EBuildState::Succeeded; }, 30000),
              "it builds the branch");
        Check(AliceLog.Find(ClientEvent::EType::BuildStatus) && AliceLog.Find(ClientEvent::EType::BuildStatus)->Build.State == EBuildState::Building,
              "Building first, then Succeeded");
        Check(Alice.GetBuildStatus().SchemaHash == 0x2f8d597745b8342full, "with the schema hash");
        Check(Alice.GetBuildStatus().Author == "bob" && Alice.GetBuildStatus().Summary == "Start", "naming the commit");
        bool bInWorktree = false;
        for(const fs::directory_entry& Each : fs::directory_iterator(Temp / "data/wt"))
        {
            bInWorktree = bInWorktree || fs::exists(Each.path() / "Games/WorldsLab/Content/Levels/A.nmworld");
            Check(Each.path().filename().u8string().size() == 8, "the worktree's folder name is short");
        }
        Check(bInWorktree, "in its own worktree");

        /* A push from a terminal, with a broken entity file: the worker finds it. */
        AliceLog.Clear();
        WriteText(Bob / "Games/WorldsLab/Content/Levels/A/bad.nmentity", "not xml at all\n");
        RunGit(Bob, { "add", "-A" });
        RunGit(Bob, { "commit", "-qm", "Bad entity" });
        RunGit(Bob, { "push", "-q" });
        Check(PumpUntil(Server, { &Alice }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::Validation) != nullptr; }, 30000),
              "a push outside the editor is found and checked");
        const ClientEvent* Advanced = AliceLog.Find(ClientEvent::EType::BranchAdvanced);
        Check(Advanced && Advanced->MemberId == 0 && Advanced->User == "bob", "relayed as bob's push, found by the worker");
        Check(Advanced && Advanced->Move.Commits.size() == 1 && Advanced->Move.Commits[0].Summary == "Bad entity", "with its commit");
        Check(Advanced && Advanced->Move.Paths == std::vector<std::string>{ "Games/WorldsLab/Content/Levels/A/bad.nmentity" }, "and its path");
        const ClientEvent* Validation = AliceLog.Find(ClientEvent::EType::Validation);
        Check(Validation && Validation->Report.Problems.size() == 1
              && Validation->Report.Problems[0].Path == "Games/WorldsLab/Content/Levels/A/bad.nmentity", "the broken entity file is flagged");
        Check(Validation && Validation->Report.Author == "bob" && Validation->Report.Checked == 1, "bob's, one world checked");

        /* A late joiner hears where the build stands. */
        CollabClient Carol;
        EventLog CarolLog;
        ClientIdentity CarolId = AliceId;
        CarolId.User = "carol";
        JoinOrRefuse(Server, { &Alice }, Carol, CarolLog, CarolId);
        Check(PumpUntil(Server, { &Alice, &Carol }, [&]() { return Carol.GetBuildStatus().State == EBuildState::Succeeded; }),
              "a late joiner gets the last build status");

        /* A build that fails says why. */
        AliceLog.Clear();
        WriteText(Bob / "Games/WorldsLab/BROKEN", "x\n");
        RunGit(Bob, { "add", "-A" });
        RunGit(Bob, { "commit", "-qm", "Breaks the build" });
        RunGit(Bob, { "push", "-q" });
        Check(PumpUntil(Server, { &Alice, &Carol }, [&]() { return Alice.GetBuildStatus().State == EBuildState::Failed; }, 30000),
              "a failed build is reported");
        Check(Alice.GetBuildStatus().FirstError == "Pickup.cpp(3): error C2065: 'Ammo': undeclared identifier", "with the first error");
        Check(Alice.GetBuildStatus().Author == "bob", "and whose commit");

        // --- Play Together ---------------------------------------------------------
        std::printf("--- Play Together\n");
        const std::string Level = "Content/Levels/A.nmworld";

        /* No good build: refused, with why. */
        AliceLog.Clear();
        Alice.RequestPlay(Level);
        Check(PumpUntil(Server, { &Alice, &Carol }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::Notified) != nullptr; }),
              "Play Together on a failed build: told");
        Check(AliceLog.Find(ClientEvent::EType::Notified) && AliceLog.Find(ClientEvent::EType::Notified)->Title == "The server's build isn't ready", "the build isn't ready");
        Check(Game.Starts == 0, "and no game server");

        /* Fixed: built, then a run. */
        fs::remove(Bob / "Games/WorldsLab/BROKEN");
        RunGit(Bob, { "add", "-A" });
        RunGit(Bob, { "commit", "-qm", "Fixes the build" });
        RunGit(Bob, { "push", "-q" });
        Check(PumpUntil(Server, { &Alice, &Carol }, [&]() { return Alice.GetBuildStatus().State == EBuildState::Succeeded; }, 30000),
              "the fix builds");
        const std::string Built = Alice.GetBuildStatus().Oid;

        AliceLog.Clear();
        CarolLog.Clear();
        Alice.RequestPlay(Level);
        Check(PumpUntil(Server, { &Alice, &Carol }, [&]()
        {
            AliceLog.Drain(Alice);
            CarolLog.Drain(Carol);
            return AliceLog.Find(ClientEvent::EType::PlayInvite) && CarolLog.Find(ClientEvent::EType::PlayInvite);
        }, 10000), "Play Together: everyone is invited");
        /* Alice plays: what keeps the run from ending as idle. */
        Alice.SetPresence("x", Level, EPlayState::PlayingTogether, {});
        {
            std::lock_guard<std::mutex> Lock(Game.Mutex);
            Check(Game.bRunning && Game.World == "A" && Game.Port == 17777, "the worker runs the game server on world A, port 17777");
        }
        const ClientEvent* CarolInvite = CarolLog.Find(ClientEvent::EType::PlayInvite);
        const PlayInviteMessage Invite = CarolInvite ? CarolInvite->Invite : PlayInviteMessage();
        Check(Invite.From == "alice" && Invite.FromId == Alice.GetMyId(), "from Alice");
        Check(Invite.Level == Level && Invite.Port == 17777 && Invite.Address.empty(), "the level, the port, on the session's host");
        Check(Invite.Oid == Built && Invite.SchemaHash == 0x2f8d597745b8342full, "at the build's commit and schema");
        Check(Carol.IsRunActive() && Carol.GetActiveRun().Port == 17777, "the client knows the run is up");

        /* A second request joins it; only the starter stops it. */
        CarolLog.Clear();
        Carol.RequestPlay(Level);
        Carol.RequestStopPlay();
        Check(PumpUntil(Server, { &Alice, &Carol }, [&]() { CarolLog.Drain(Carol); return CarolLog.Count(ClientEvent::EType::Notified) >= 2; }),
              "Carol is told");
        Check(CarolLog.Find(ClientEvent::EType::Notified) && CarolLog.Find(ClientEvent::EType::Notified)->Title == "A run is already going" && CarolLog.Find(ClientEvent::EType::PlayInvite),
              "a second request gets the run's invite");
        Check(!CarolLog.Events.empty() && CarolLog.Events.back().Title == "Only alice can stop this run", "only its starter stops it");
        Check(Game.Starts == 1 && Game.bRunning, "still the one game server");

        /* A late joiner is invited too. */
        CollabClient Dave;
        EventLog DaveLog;
        ClientIdentity DaveId = AliceId;
        DaveId.User = "dave";
        JoinOrRefuse(Server, { &Alice, &Carol }, Dave, DaveLog, DaveId);
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { DaveLog.Drain(Dave); return DaveLog.Find(ClientEvent::EType::PlayInvite) != nullptr; }),
              "a late joiner is invited");

        /* While it runs, a new commit is announced but not built. */
        AliceLog.Clear();
        WriteText(Bob / "Games/WorldsLab/Content/Levels/A/more.nmentity", "<Entity />\n");
        RunGit(Bob, { "add", "-A" });
        RunGit(Bob, { "commit", "-qm", "More" });
        RunGit(Bob, { "push", "-q" });
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::BranchAdvanced) != nullptr; }, 10000),
              "a push during the run is announced");
        PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { return false; }, 800);
        Check(Alice.GetBuildStatus().Oid == Built, "but not built while the game server runs");

        /* Someone playing keeps it; nobody, and it ends. */
        PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { return false; }, 2000);
        Check(Alice.IsRunActive() && Game.bRunning, "someone playing keeps the run past the idle timeout");
        AliceLog.Clear();
        Alice.SetPresence("x", Level, EPlayState::Editing, {});
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::PlayEnded) != nullptr; }, 5000),
              "nobody playing: the run ends");
        Check(AliceLog.Find(ClientEvent::EType::PlayEnded) && AliceLog.Find(ClientEvent::EType::PlayEnded)->Text == "Nobody was playing.", "saying why");
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { return !Game.bRunning; }), "the game server stops");
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { return Alice.GetBuildStatus().Oid != Built
                                                                      && Alice.GetBuildStatus().State == EBuildState::Succeeded; }, 30000),
              "and the held commit builds");

        /* The game server dying ends the run. */
        AliceLog.Clear();
        Alice.RequestPlay(Level);
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::PlayInvite) != nullptr; }, 10000),
              "a second run");
        AliceLog.Clear();
        Game.bRunning = false;   // it crashed
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::PlayEnded) != nullptr; }, 5000),
              "the game server stopping ends it");
        Check(AliceLog.Find(ClientEvent::EType::PlayEnded) && AliceLog.Find(ClientEvent::EType::PlayEnded)->Text == "The game server stopped.", "saying so");

        /* Its starter ends it. */
        AliceLog.Clear();
        Alice.RequestPlay(Level);
        PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::PlayInvite) != nullptr; }, 10000);
        CarolLog.Drain(Carol);   // the earlier runs' events, out of the way
        CarolLog.Clear();
        Alice.RequestStopPlay();
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { CarolLog.Drain(Carol); return CarolLog.Find(ClientEvent::EType::PlayEnded) != nullptr; }, 5000),
              "the starter stops it; everyone hears");
        Check(CarolLog.Find(ClientEvent::EType::PlayEnded) && CarolLog.Find(ClientEvent::EType::PlayEnded)->Text == "alice stopped it" && !Carol.IsRunActive(), "who stopped it");
        Check(PumpUntil(Server, { &Alice, &Carol, &Dave }, [&]() { return !Game.bRunning; }), "and the game server stops");
        Dave.Leave();

        /* The session ends: the worker stops. */
        Alice.Leave();
        Carol.Leave();
        Check(PumpUntil(Server, { &Alice, &Carol }, [&]() { return Server.GetSessions().empty(); }), "the session ends");
        Check(bStopWorker.load() && !WorkerThread.joinable(), "and its worker stops");
        Server.Stop();

        std::error_code Ignored;
        RunGit(Host, { "worktree", "prune" });
        fs::remove_all(Temp, Ignored);
    }

    void TestRefusals()
    {
        std::printf("--- refusals\n");
        CollabServer Server;
        ServerConfig Config;
        Config.Port     = 0;
        Config.Password = "sesame";
        Config.HelloTimeoutMs = 500;
        Check(Server.Start(Config), "a server with a password");

        CollabClient Alice;
        EventLog AliceLog;
        ClientIdentity AliceId = Identity("alice");
        AliceId.Password = "sesame";
        Check(JoinOrRefuse(Server, {}, Alice, AliceLog, AliceId) && AliceLog.Find(ClientEvent::EType::Joined), "the right password joins");

        CollabClient Mallory;
        EventLog MalloryLog;
        ClientIdentity Wrong = Identity("mallory");
        Wrong.Password = "open";
        JoinOrRefuse(Server, { &Alice }, Mallory, MalloryLog, Wrong);
        const ClientEvent* Refused = MalloryLog.Find(ClientEvent::EType::Rejected);
        Check(Refused && Refused->Reason == ERejectReason::Password, "a wrong password is refused");
        Check(PumpUntil(Server, { &Alice, &Mallory }, [&]() { return Mallory.GetState() == CollabClient::EState::Idle; }),
              "and disconnected");

        CollabClient Bob;
        EventLog BobLog;
        ClientIdentity Other = Identity("bob", "zzzzzzzzzzzz9999");
        Other.Password = "sesame";
        JoinOrRefuse(Server, { &Alice }, Bob, BobLog, Other);
        Refused = BobLog.Find(ClientEvent::EType::Rejected);
        Check(Refused && Refused->Reason == ERejectReason::EngineMismatch, "another engine is refused");
        Check(Refused && Refused->Text.find("zzzzzzzzzz") != std::string::npos && Refused->Text.find("aaaaaaaaaa") != std::string::npos, "saying which engines differ");

        CollabClient Dirty;
        EventLog DirtyLog;
        ClientIdentity DirtyId = Identity("dora");
        DirtyId.Password     = "sesame";
        DirtyId.bEngineDirty = true;
        JoinOrRefuse(Server, { &Alice }, Dirty, DirtyLog, DirtyId);
        Refused = DirtyLog.Find(ClientEvent::EType::Rejected);
        Check(Refused && Refused->Reason == ERejectReason::EngineDirty, "uncommitted engine changes are refused");
        Check(Server.GetMemberCount() == 1, "only Alice is in");

        /* A hello from another protocol version, by hand. */
        {
            CollabTransport Raw;
            PeerID ToServer = PEER_NONE;
            Check(Raw.Connect("127.0.0.1", Server.GetPort(), 3000, ToServer), "a raw connection");
            bool bRefused = false;
            PumpUntil(Server, { &Alice }, [&]()
            {
                TransportEvent Event;
                while(Raw.Service(Event))
                {
                    if(Event.EventType == TransportEvent::EType::Connected)
                    {
                        HelloMessage Old = HelloMessage();
                        Old.Protocol = ProtocolVersion + 98;
                        Old.User = "old";
                        Raw.Send(ToServer, Encode(Old));
                        Raw.Flush();
                    }
                    else if(Event.EventType == TransportEvent::EType::Received && PeekType(Event.Data) == EMessage::Reject)
                    {
                        RejectMessage Reject;
                        bRefused = Decode(Event.Data, Reject) && Reject.Reason == ERejectReason::Protocol;
                    }
                }
                return bRefused;
            });
            Check(bRefused, "another protocol version is refused");

            /* A connection that never says hello. */
            CollabTransport Quiet;
            PeerID QuietServer = PEER_NONE;
            Quiet.Connect("127.0.0.1", Server.GetPort(), 3000, QuietServer);
            bool bDropped = false;
            PumpUntil(Server, { &Alice }, [&]()
            {
                TransportEvent Event;
                while(Quiet.Service(Event))
                {
                    if(Event.EventType == TransportEvent::EType::Received && PeekType(Event.Data) == EMessage::Reject)
                    {
                        bDropped = true;
                    }
                }
                return bDropped;
            }, 3000);
            Check(bDropped, "no hello in time is refused");
        }
    }

    void TestTimeouts()
    {
        std::printf("--- timeouts\n");

        /* Nothing listening: a port that was free a moment ago. */
        uint16_t FreePort = 0;
        {
            CollabServer Gone;
            ServerConfig Config;
            Config.Port = 0;
            Gone.Start(Config);
            FreePort = Gone.GetPort();
        }
        CollabServer Server;
        ServerConfig Config;
        Config.Port      = 0;
        Config.TimeoutMs = 1000;
        Server.Start(Config);

        CollabClient Lonely;
        EventLog LonelyLog;
        Check(Lonely.Connect("127.0.0.1", FreePort, Identity("lonely"), 1000), "connecting to nothing starts");
        Check(PumpUntil(Server, { &Lonely }, [&]() { LonelyLog.Drain(Lonely); return LonelyLog.Find(ClientEvent::EType::ConnectFailed) != nullptr; }, 8000),
              "and fails with no answer");

        /* An editor that stops answering is dropped. */
        CollabClient Alice, Silent;
        EventLog AliceLog, SilentLog;
        JoinOrRefuse(Server, {}, Alice, AliceLog, Identity("alice"));
        Check(JoinOrRefuse(Server, { &Alice }, Silent, SilentLog, Identity("silent")) && SilentLog.Find(ClientEvent::EType::Joined),
              "a second editor joins");
        /* From here Silent is never polled again. */
        AliceLog.Clear();
        Check(PumpUntil(Server, { &Alice }, [&]() { AliceLog.Drain(Alice); return AliceLog.Find(ClientEvent::EType::MemberLeft) != nullptr; }, 15000),
              "a silent editor times out; the others hear");
        Check(Server.GetMemberCount() == 1, "and the server forgets it");
    }
}

int main()
{
    TestHelpers();
    TestSession();
    TestPushes();
    TestWorkerHelpers();
    TestWorker();
    TestRefusals();
    TestTimeouts();

    if(GFailures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", GFailures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
