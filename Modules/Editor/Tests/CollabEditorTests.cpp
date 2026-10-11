/*
 * NimbleEditorCollabTests -- two editors' collaboration sessions in one
 * process, offscreen, against a CollabServer on 127.0.0.1: joining, the
 * Session dock's roster, presence (open level, playing, unsaved), the overlap
 * warning, a refusal and leaving, as notifications.
 *
 * ConnectAs, not Connect: the identity is made here rather than asked of git.
 */

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "Public/Collab/CollabServer.h"
#include "Public/Collab/ContentWorker.h"
#include "Public/Editor/EditorContentEvents.h"
#include "Public/Editor/EditorNotifications.h"
#include "Private/Editor/Collab/CollabSessionWindow.h"
#include "Private/Editor/Collab/EditorCollabSession.h"

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

static bool PumpUntil(Collab::CollabServer& InServer, const std::function<bool()>& InDone, const int InMs = 5000)
{
    QElapsedTimer Clock;
    Clock.start();
    while(Clock.elapsed() < InMs)
    {
        InServer.Poll();
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if(InDone())
        {
            return true;
        }
    }
    return InDone();
}

static Collab::ClientIdentity Identity(const char* InUser, const char* InEngine = "aaaaaaaaaaaa1111")
{
    Collab::ClientIdentity Out;
    Out.User       = InUser;
    Out.ProjectId  = Collab::MakeProjectId("WorldsLab", "https://github.com/Nimble/WorldsLab.git");
    Out.Branch     = "collab/forest";
    Out.Head       = "1234567890123456789012345678901234567890";
    Out.EngineTree = InEngine;
    return Out;
}

// --- Pull & Reload, on real repositories ----------------------------------------------

static bool RunGit(const QString& InDir, const QStringList& InArgs, QString* OutText = nullptr)
{
    QProcess Process;
    Process.setWorkingDirectory(InDir);
    Process.start(QStringLiteral("git"), InArgs);
    if(!Process.waitForStarted(10000) || !Process.waitForFinished(60000))
    {
        return false;
    }
    if(OutText)
    {
        *OutText = QString::fromUtf8(Process.readAllStandardOutput()).trimmed();
    }
    return Process.exitStatus() == QProcess::NormalExit && Process.exitCode() == 0;
}

static void WriteFile(const QString& InPath, const QByteArray& InText)
{
    QDir().mkpath(QFileInfo(InPath).absolutePath());
    QFile File(InPath);
    File.open(QIODevice::WriteOnly | QIODevice::Truncate);
    File.write(InText);
}

static QByteArray ReadFile(const QString& InPath)
{
    QFile File(InPath);
    return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
}

static void Identify(const QString& InRepo, const char* InName)
{
    RunGit(InRepo, { QStringLiteral("config"), QStringLiteral("user.name"), QString::fromLatin1(InName) });
    RunGit(InRepo, { QStringLiteral("config"), QStringLiteral("user.email"), QString::fromLatin1(InName) + QStringLiteral("@example.com") });
    RunGit(InRepo, { QStringLiteral("config"), QStringLiteral("core.autocrlf"), QStringLiteral("false") });
    RunGit(InRepo, { QStringLiteral("config"), QStringLiteral("commit.gpgsign"), QStringLiteral("false") });
}

/* Alice commits InFiles and pushes; her session announces it. */
static void AlicePushes(const QString& InAlice, EditorCollabSession& InSession, const QList<QPair<QString, QByteArray>>& InFiles,
                        const QString& InMessage)
{
    for(const auto& Each : InFiles)
    {
        WriteFile(QDir(InAlice).filePath(Each.first), Each.second);
    }
    QString Old, New;
    RunGit(InAlice, { QStringLiteral("rev-parse"), QStringLiteral("@{u}") }, &Old);
    RunGit(InAlice, { QStringLiteral("add"), QStringLiteral("-A") });
    RunGit(InAlice, { QStringLiteral("commit"), QStringLiteral("-qm"), InMessage });
    RunGit(InAlice, { QStringLiteral("push"), QStringLiteral("-q") });
    RunGit(InAlice, { QStringLiteral("rev-parse"), QStringLiteral("HEAD") }, &New);
    InSession.AnnouncePush(QStringLiteral("main"), Old, New);
}

static void TestPullAndReload(Collab::CollabServer& InServer, EditorNotifications& Notes)
{
    std::printf("Pull & Reload\n");
    QTemporaryDir Temp;
    const QString Remote = QDir(Temp.path()).filePath(QStringLiteral("remote.git"));
    const QString Alice  = QDir(Temp.path()).filePath(QStringLiteral("alice"));
    const QString Bob    = QDir(Temp.path()).filePath(QStringLiteral("bob"));
    RunGit(Temp.path(), { QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), QStringLiteral("-b"), QStringLiteral("main"), Remote });
    RunGit(Temp.path(), { QStringLiteral("-c"), QStringLiteral("core.autocrlf=false"), QStringLiteral("clone"), QStringLiteral("-q"), Remote, Alice });
    Identify(Alice, "alice");
    RunGit(Alice, { QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("main") });
    const QString LevelPath  = QStringLiteral("Games/WorldsLab/Content/Levels/Forest.nmworld");
    const QString SourcePath = QStringLiteral("Games/WorldsLab/Source/Pickup.cpp");
    WriteFile(QDir(Alice).filePath(LevelPath), "<World>\n</World>\n");
    WriteFile(QDir(Alice).filePath(SourcePath), "int Ammo = 0;\n");
    WriteFile(QDir(Alice).filePath(QStringLiteral("Engine/Core.cpp")), "// engine\n");
    RunGit(Alice, { QStringLiteral("add"), QStringLiteral("-A") });
    RunGit(Alice, { QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("Start") });
    Expect(RunGit(Alice, { QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("-u"), QStringLiteral("origin"), QStringLiteral("main") }),
           "a shared repository");
    RunGit(Temp.path(), { QStringLiteral("-c"), QStringLiteral("core.autocrlf=false"), QStringLiteral("clone"), QStringLiteral("-q"), Remote, Bob });
    Identify(Bob, "bob");

    Collab::ClientIdentity AliceId = Identity("alice");
    AliceId.Branch = "main";
    Collab::ClientIdentity BobId = Identity("bob");
    BobId.Branch = "main";
    EditorCollabSession AliceSession(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab")), QString());
    EditorCollabSession BobSession(QDir(Bob).filePath(QStringLiteral("Games/WorldsLab")), QString());
    AliceSession.ConnectAs(QStringLiteral("127.0.0.1"), InServer.GetPort(), AliceId);
    BobSession.ConnectAs(QStringLiteral("127.0.0.1"), InServer.GetPort(), BobId);
    Expect(PumpUntil(InServer, [&]() { return AliceSession.IsJoined() && BobSession.IsJoined(); }), "both join");

    int Reloads = 0, Compiles = 0, Configs = 0;
    EditorCollabSession::ReloadHooks Hooks;
    Hooks.ConfirmDiscardLevel = []() { return true; };
    Hooks.ReloadLevel       = [&Reloads]() { ++Reloads; };
    Hooks.CompileGameModule = [&Compiles]() { ++Compiles; };
    Hooks.ReloadConfig      = [&Configs]() { ++Configs; };
    BobSession.SetReloadHooks(Hooks);
    BobSession.SetOpenLevel(QDir(Bob).filePath(LevelPath));
    QStringList Announced;
    QObject::connect(&EditorContentEvents::Get(), &EditorContentEvents::ContentChanged,
                     [&Announced](const QStringList& InFiles) { Announced << InFiles; });

    /* A level edit and a GameModule change. */
    AlicePushes(Alice, AliceSession, { { LevelPath, "<World>\n  <Entity Name=\"Tree\"/>\n</World>\n" },
                                       { SourcePath, "int Ammo = 30;\n" } }, QStringLiteral("Trees and ammo"));
    Expect(PumpUntil(InServer, [&]()
    {
        const EditorNotifications::Notification* Offer = Notes.FindByKey(QStringLiteral("collab.branch"));
        return Offer && !Offer->Actions.isEmpty();
    }, 15000), "Alice's push: a toast for Bob");
    const EditorNotifications::Notification* Offer = Notes.FindByKey(QStringLiteral("collab.branch"));
    Expect(Offer && Offer->Title == QStringLiteral("alice pushed 1 commit"), "\"alice pushed 1 commit\"");
    Expect(Offer && Offer->Body.contains(QStringLiteral("1 content file")) && Offer->Body.contains(QStringLiteral("1 GameModule source file")),
           "naming what changed");
    Expect(Offer && Offer->Body.contains(QStringLiteral("Trees and ammo")), "and the latest commit");
    Expect(Offer && Offer->Actions.size() == 2 && Offer->Actions[0].Label == QStringLiteral("Pull & Reload"), "with Pull & Reload and Later");
    Expect(BobSession.GetPendingCommitCount() == 1 && BobSession.GetPendingPaths().size() == 2, "pending: one commit, two paths");

    bool bFinished = false, bPulled = false;
    QObject::connect(&BobSession, &EditorCollabSession::PullAndReloadFinished, [&](const bool bInPulled) { bFinished = true; bPulled = bInPulled; });
    Notes.RunAction(Offer->Id, 0);
    Expect(PumpUntil(InServer, [&]() { return bFinished; }, 30000) && bPulled, "Pull & Reload pulls");
    Expect(ReadFile(QDir(Bob).filePath(LevelPath)).contains("Tree"), "the level edit is in Bob's checkout");
    Expect(ReadFile(QDir(Bob).filePath(SourcePath)).contains("30"), "and the source change");
    Expect(Reloads == 1, "the open level reloads");
    Expect(Compiles == 1, "GameModule source changed: Compile Source");
    Expect(Configs == 0, "Config didn't change: not reloaded");
    Expect(Announced.size() == 2 && Announced.join(QLatin1Char('\n')).contains(QStringLiteral("Forest.nmworld")),
           "the changed files are announced (EditorContentEvents)");
    const EditorNotifications::Notification* Result = Notes.FindByKey(QStringLiteral("collab.branch"));
    Expect(Result && Result->Severity == EditorNotifications::ESeverity::Success && Result->Actions.isEmpty(), "a success toast replaces the offer");
    Expect(BobSession.GetPendingCommitCount() == 0, "nothing pending");

    /* An engine change: not pulled, and Bob leaves the session. */
    QString BobHead;
    RunGit(Bob, { QStringLiteral("rev-parse"), QStringLiteral("HEAD") }, &BobHead);
    AlicePushes(Alice, AliceSession, { { QStringLiteral("Engine/Core.cpp"), "// engine, changed\n" } }, QStringLiteral("Engine tweak"));
    Expect(PumpUntil(InServer, [&]()
    {
        const EditorNotifications::Notification* Next = Notes.FindByKey(QStringLiteral("collab.branch"));
        return Next && !Next->Actions.isEmpty();
    }, 15000), "an engine push: a toast for Bob");
    Offer = Notes.FindByKey(QStringLiteral("collab.branch"));
    Expect(Offer && Offer->Severity == EditorNotifications::ESeverity::Warning && Offer->Body.contains(QStringLiteral("rebuild")),
           "warning that it needs a rebuild");
    bFinished = false;
    Notes.RunAction(Offer->Id, 0);
    Expect(PumpUntil(InServer, [&]() { return bFinished && BobSession.GetState() == Collab::CollabClient::EState::Idle; }, 15000) && !bPulled,
           "Pull & Reload refuses, and Bob leaves the session");
    QString BobAfter;
    RunGit(Bob, { QStringLiteral("rev-parse"), QStringLiteral("HEAD") }, &BobAfter);
    Expect(BobAfter == BobHead, "nothing was pulled");
    Expect(ReadFile(QDir(Bob).filePath(QStringLiteral("Engine/Core.cpp"))) == QByteArray("// engine\n"), "the engine file is untouched");
    const EditorNotifications::Notification* Refused = Notes.FindByKey(QStringLiteral("collab.branch"));
    Expect(Refused && Refused->Severity == EditorNotifications::ESeverity::Error && Refused->Title.contains(QStringLiteral("engine")),
           "\"The session's engine changed\"");

    AliceSession.Leave();
    PumpUntil(InServer, [&]() { return InServer.GetSessions().empty(); });
}

// --- Play Together, with a content worker -----------------------------------------------

/* Builds and reports in no time: schema 0x0abc; the game server is a flag. */
class FakeToolchain final : public Collab::IWorkerToolchain
{
public:
    Collab::ToolchainBuild Build(const std::string&, const std::string&) override
    {
        Collab::ToolchainBuild Out;
        Out.bOk = true;
        return Out;
    }
    Collab::ToolchainReport Report(const std::string&, const std::string&, const std::string&) override
    {
        Collab::ToolchainReport Out;
        Out.bOk        = true;
        Out.Module     = "WorldsLabGame";
        Out.SchemaHash = 0x0abc;
        return Out;
    }
    bool StartGame(const std::string&, const std::string& InWorld, const uint16_t, std::string&) override
    {
        World    = InWorld;
        bRunning = true;
        return true;
    }
    bool IsGameRunning() override { return bRunning; }
    void StopGame() override { bRunning = false; }

    std::string       World;
    std::atomic<bool> bRunning{ false };
};

struct Player
{
    QString  Name;
    uint64_t Schema = 0x0abc;
    int      Joins  = 0;
    int      Leaves = 0;
    QString  Host;
    uint16_t Port = 0;
    QString  Level;
};

static void TestPlayTogether()
{
    std::printf("Play Together\n");
    QTemporaryDir Temp;
    const QString Remote = QDir(Temp.path()).filePath(QStringLiteral("remote.git"));
    const auto Clone = [&](const QString& InName)
    {
        const QString Dir = QDir(Temp.path()).filePath(InName);
        RunGit(Temp.path(), { QStringLiteral("-c"), QStringLiteral("core.autocrlf=false"), QStringLiteral("clone"), QStringLiteral("-q"), Remote, Dir });
        Identify(Dir, InName.toUtf8().constData());
        return Dir;
    };
    RunGit(Temp.path(), { QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), QStringLiteral("-b"), QStringLiteral("main"), Remote });
    const QString Alice = Clone(QStringLiteral("alice"));
    RunGit(Alice, { QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("main") });
    WriteFile(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab/Config/DefaultEngine.ini")),
              "[Game]\nModule=WorldsLabGame\n\n[World_A]\nFile=Content/Levels/A.nmworld\n");
    WriteFile(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab/Content/Levels/A.nmworld")), "<World Format=\"1\" />\n");
    WriteFile(QDir(Alice).filePath(QStringLiteral("Engine/Core.cpp")), "// engine\n");
    RunGit(Alice, { QStringLiteral("add"), QStringLiteral("-A") });
    RunGit(Alice, { QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("Start") });
    RunGit(Alice, { QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("-u"), QStringLiteral("origin"), QStringLiteral("main") });
    const QString Carol = Clone(QStringLiteral("carol"));   // stays behind
    WriteFile(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab/Content/Levels/A/more.nmentity")), "<Entity />\n");
    RunGit(Alice, { QStringLiteral("add"), QStringLiteral("-A") });
    RunGit(Alice, { QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("More") });
    RunGit(Alice, { QStringLiteral("push"), QStringLiteral("-q") });
    const QString Bob  = Clone(QStringLiteral("bob"));
    const QString Dave = Clone(QStringLiteral("dave"));
    const QString Host = Clone(QStringLiteral("host"));

    /* The server, its worker in-process with the fake toolchain. */
    FakeToolchain* Game = nullptr;
    std::atomic<bool> bStopWorker{ false };
    std::thread WorkerThread;
    Collab::CollabServer Server;
    Collab::ServerConfig Config;
    Config.Port         = 0;
    Config.RepoPath     = Host.toStdString();
    Config.DataDir      = QDir(Temp.path()).filePath(QStringLiteral("data")).toStdString();
    Config.GamePortBase = 27777;
    Config.WorkerFetchIntervalMs = 300;
    Config.LaunchWorker = [&](const Collab::WorkerConfig& InWorker)
    {
        auto Toolchain = std::make_unique<FakeToolchain>();
        Game = Toolchain.get();
        WorkerThread = std::thread([InWorker, &bStopWorker, Owned = Toolchain.release()]() mutable
        {
            Collab::ContentWorker Worker(InWorker, std::unique_ptr<Collab::IWorkerToolchain>(Owned));
            Worker.Run(bStopWorker);
        });
    };
    Config.StopWorker = [&](uint32_t)
    {
        bStopWorker = true;
        if(WorkerThread.joinable()) { WorkerThread.join(); }
    };
    Expect(Server.Start(Config), "a server with a content worker");

    /* Four editors: Bob up to date, Carol behind, Dave on another build of the game. */
    Player AliceP{ QStringLiteral("alice") }, BobP{ QStringLiteral("bob") }, CarolP{ QStringLiteral("carol") }, DaveP{ QStringLiteral("dave") };
    DaveP.Schema = 0x0def;
    EditorCollabSession AliceS(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab")), QString());
    EditorCollabSession BobS(QDir(Bob).filePath(QStringLiteral("Games/WorldsLab")), QString());
    EditorCollabSession CarolS(QDir(Carol).filePath(QStringLiteral("Games/WorldsLab")), QString());
    EditorCollabSession DaveS(QDir(Dave).filePath(QStringLiteral("Games/WorldsLab")), QString());
    const QList<QPair<EditorCollabSession*, Player*>> All = { { &AliceS, &AliceP }, { &BobS, &BobP }, { &CarolS, &CarolP }, { &DaveS, &DaveP } };
    for(const auto& Each : All)
    {
        Player* P = Each.second;
        EditorCollabSession::PlayHooks Hooks;
        Hooks.LocalSchemaHash = [P]() { return P->Schema; };
        Hooks.JoinRun = [P](const QString& InLevel, const QString& InHost, const uint16_t InPort)
        {
            ++P->Joins;
            P->Level = InLevel;
            P->Host  = InHost;
            P->Port  = InPort;
            return true;
        };
        Hooks.LeaveRun = [P]() { ++P->Leaves; };
        Each.first->SetPlayHooks(Hooks);

        Collab::ClientIdentity Id = Identity(P->Name.toUtf8().constData());
        Id.ProjectId = Collab::MakeProjectId("WorldsLabGame", Remote.toStdString());
        Id.Branch    = "main";
        Each.first->ConnectAs(QStringLiteral("127.0.0.1"), Server.GetPort(), Id);
    }
    Expect(PumpUntil(Server, [&]() { return AliceS.IsJoined() && BobS.IsJoined() && CarolS.IsJoined() && DaveS.IsJoined(); }), "four editors join");
    Expect(PumpUntil(Server, [&]() { return AliceS.GetClient().GetBuildStatus().State == Collab::EBuildState::Succeeded; }, 30000),
           "the worker builds the branch");

    /* Alice starts it: the server runs the game, she plays at once. */
    AliceS.RequestPlayTogether(QStringLiteral("Content/Levels/A.nmworld"));
    Expect(PumpUntil(Server, [&]() { return AliceP.Joins == 1; }, 10000), "Play Together: its starter joins at once");
    Expect(Game && Game->bRunning && Game->World == "A", "the worker runs the game server on world A");
    Expect(AliceP.Host == QStringLiteral("127.0.0.1") && AliceP.Port == 27777 && AliceP.Level == QStringLiteral("Content/Levels/A.nmworld"),
           "as a client of the session server's host, on the run's port and level");

    /* Bob may join; Carol and Dave may not, and say why. */
    Expect(PumpUntil(Server, [&]()
    {
        return BobS.IsRunActive() && CarolS.GetJoinBlocker().contains(QStringLiteral("Pull & Reload"))
            && DaveS.GetJoinBlocker().contains(QStringLiteral("schema"));
    }, 10000), "each editor works out whether it can join");
    PumpUntil(Server, [&]() { return false; }, 300);   // Bob's git check
    Expect(BobS.GetJoinBlocker().isEmpty(), "Bob can");
    BobS.JoinPlayTogether();
    Expect(BobP.Joins == 1 && BobP.Port == 27777, "and joins");
    CarolS.JoinPlayTogether();
    DaveS.JoinPlayTogether();
    Expect(CarolP.Joins == 0, "Carol, behind the run's commit, isn't pointed at the server");
    Expect(DaveP.Joins == 0, "Dave, on another build of the game, is refused before he connects");
    Expect(DaveS.GetJoinBlocker().contains(QStringLiteral("0000000000000def")) && DaveS.GetJoinBlocker().contains(QStringLiteral("0000000000000abc")),
           "naming both schemas");

    /* The server sees who's playing. */
    Expect(PumpUntil(Server, [&]()
    {
        int Playing = 0;
        for(const auto& Member : DaveS.GetClient().GetRoster())
        {
            Playing += Member.second.Play == Collab::EPlayState::PlayingTogether ? 1 : 0;
        }
        return Playing == 2;
    }), "two in the run, as presence");

    /* Alice ends it: everyone in it leaves. */
    AliceS.StopPlayTogether();
    Expect(PumpUntil(Server, [&]() { return AliceP.Leaves == 1 && BobP.Leaves == 1; }, 10000), "the starter ends it; the players leave");
    Expect(CarolP.Leaves == 0 && DaveP.Leaves == 0, "those who weren't in it don't");
    Expect(PumpUntil(Server, [&]() { return Game && !Game->bRunning; }), "and the game server stops");
    Expect(!BobS.IsRunActive(), "no run");

    for(const auto& Each : All)
    {
        Each.first->Leave();
    }
    PumpUntil(Server, [&]() { return Server.GetSessions().empty(); });
    Server.Stop();
    RunGit(Host, { QStringLiteral("worktree"), QStringLiteral("prune") });
}

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);
    std::printf("NimbleEditorCollabTests\n");
    EditorNotifications::Install();
    EditorNotifications& Notes = *EditorNotifications::Get();

    Collab::CollabServer Server;
    Collab::ServerConfig Config;
    Config.Port = 0;
    Expect(Server.Start(Config), "a server");
    const uint16_t Port = Server.GetPort();

    {
        /* Not repositories: paths pass through as given. */
        EditorCollabSession Alice(QDir::tempPath(), QString());
        EditorCollabSession Bob(QDir::tempPath(), QString());
        CollabSessionWindow AliceDock(Alice, nullptr);

        std::printf("Join\n");
        Alice.ConnectAs(QStringLiteral("127.0.0.1"), Port, Identity("alice"));
        Expect(Alice.GetState() == Collab::CollabClient::EState::Connecting, "connecting");
        Expect(PumpUntil(Server, [&]() { return Alice.IsJoined(); }), "Alice joins");
        const EditorNotifications::Notification* State = Notes.FindByKey(QStringLiteral("collab.state"));
        Expect(State && State->Severity == EditorNotifications::ESeverity::Success, "a success toast says so");

        Bob.ConnectAs(QStringLiteral("127.0.0.1"), Port, Identity("bob"));
        Expect(PumpUntil(Server, [&]() { return Bob.IsJoined() && Alice.GetClient().GetRoster().size() == 2; }), "Bob joins; Alice sees him");
        const EditorNotifications::Notification* Roster = Notes.FindByKey(QStringLiteral("collab.roster"));
        Expect(Roster && Roster->Title.contains(QStringLiteral("bob")), "\"bob joined the session\"");
        Expect(AliceDock.GetRoster()->topLevelItemCount() == 2, "the Session dock lists both");

        std::printf("Presence\n");
        const QString Level = QStringLiteral("Games/WorldsLab/Content/Levels/Forest.nmworld");
        Alice.SetOpenLevel(Level);
        Alice.SetPlaying(true);
        Expect(PumpUntil(Server, [&]()
        {
            const auto& Members = Bob.GetClient().GetRoster();
            for(const auto& Each : Members)
            {
                if(Each.second.User == "alice")
                {
                    return Each.second.OpenLevel == Level.toStdString() && Each.second.Play == Collab::EPlayState::Playing;
                }
            }
            return false;
        }), "Bob sees Alice's level, playing");
        Expect(Bob.DescribeRoster().join(QLatin1Char('\n')).contains(QStringLiteral("playing Forest.nmworld")), "collab.status says so");

        std::printf("Overlap\n");
        Alice.SetPlaying(false);
        Alice.SetLevelUnsaved(true);
        Bob.SetOpenLevel(Level);
        Bob.SetLevelUnsaved(true);
        const QString OverlapKey = QStringLiteral("collab.overlap.") + Level;
        Expect(PumpUntil(Server, [&]() { return Notes.FindByKey(OverlapKey) != nullptr; }), "both with unsaved edits to one level: a warning");
        const EditorNotifications::Notification* Overlap = Notes.FindByKey(OverlapKey);
        Expect(Overlap && Overlap->Severity == EditorNotifications::ESeverity::Warning, "a warning");
        Expect(PumpUntil(Server, [&]() { return Bob.GetOthersDirty().size() == 1; }), "GetOthersDirty");
        const QHash<QString, QStringList> Others = Bob.GetOthersDirty();
        Expect(!Others.isEmpty() && Others.begin().value() == QStringList{ QStringLiteral("alice") }, "names who");

        std::printf("Refused\n");
        EditorCollabSession Carol(QDir::tempPath(), QString());
        Carol.ConnectAs(QStringLiteral("127.0.0.1"), Port, Identity("carol", "zzzzzzzzzzzz9999"));
        Expect(PumpUntil(Server, [&]()
        {
            const EditorNotifications::Notification* Last = Notes.FindByKey(QStringLiteral("collab.state"));
            return Last && Last->Title.contains(QStringLiteral("refused"));
        }), "another engine: \"The session refused this editor\"");
        Expect(PumpUntil(Server, [&]() { return Carol.GetState() == Collab::CollabClient::EState::Idle; }), "and it's out");

        std::printf("Leave\n");
        Alice.Leave();
        Expect(PumpUntil(Server, [&]() { return !Alice.IsJoined() && Bob.GetClient().GetRoster().size() == 1; }), "Alice leaves; Bob sees");
        Roster = Notes.FindByKey(QStringLiteral("collab.roster"));
        Expect(Roster && Roster->Title.contains(QStringLiteral("alice left")), "\"alice left the session\"");
        Expect(Bob.GetOthersDirty().isEmpty(), "and her changes are no longer listed");
        Expect(AliceDock.GetRoster()->topLevelItemCount() == 0, "her dock empties");
        Bob.Leave();
        PumpUntil(Server, [&]() { return Server.GetSessions().empty(); });
    }

    if(RunGit(QDir::currentPath(), { QStringLiteral("--version") }))
    {
        TestPullAndReload(Server, Notes);
        TestPlayTogether();
    }
    else
    {
        std::printf("  (git isn't on PATH: Pull & Reload is skipped)\n");
    }

    Server.Stop();
    EditorNotifications::Uninstall();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
