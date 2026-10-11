/*
 * NimbleEditorSourceControlTests -- the Source Control dock's git:
 *
 *   - `git status --porcelain=v2 --branch -z` parsed from fixtures: changed,
 *     renamed, conflicted, untracked, a detached HEAD, a branch with no
 *     upstream, before the first commit, paths with spaces;
 *   - ClassifyPath's groups;
 *   - against real repositories in a temporary folder (skipped without git):
 *     stage, unstage, commit, push to a local bare repository, and a second
 *     clone fetching and pulling it, fast-forward and rebase; then the dock
 *     itself on that clone, offscreen.
 */

#include <cstdio>
#include <string>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "Public/Editor/EditorNotifications.h"
#include "Private/Editor/SourceControl/GitClient.h"
#include "Private/Editor/SourceControl/GitStatus.h"
#include "Private/Editor/SourceControl/SourceControlWindow.h"

using namespace EditorGit;

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

/* Records joined with NUL, as -z writes them. */
static std::string Z(std::initializer_list<const char*> InRecords)
{
    std::string Out;
    for(const char* Each : InRecords)
    {
        Out += Each;
        Out += '\0';
    }
    return Out;
}

static const char* const ModeOid =
    "100644 100644 100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391";

static std::string Changed(const char* InXY, const char* InPath)
{
    return std::string("1 ") + InXY + " N... " + ModeOid + " " + InPath;
}

static void TestParser()
{
    std::printf("Porcelain v2\n");

    {
        const std::string Out = Z({ "# branch.oid 1234567890abcdef1234567890abcdef12345678",
                                    "# branch.head feature/forest",
                                    "# branch.upstream origin/feature/forest",
                                    "# branch.ab +2 -3" })
                              + Changed("M.", "Games/WorldsLab/Content/Levels/Forest.nmworld") + '\0'
                              + Changed(".M", "Games/WorldsLab/Source/Pickup.cpp") + '\0'
                              + Changed("MM", "Games/WorldsLab/Config/DefaultGame.ini") + '\0'
                              + Changed("D.", "Games/WorldsLab/Content/Old.nmentity") + '\0'
                              + Z({ "? Games/WorldsLab/Content/New Folder/Rock 02.nmmesh", "! Saved/Logs/Editor.log" });
        Status Parsed;
        std::string Error;
        Expect(ParseStatusV2(Out, Parsed, &Error), "an ordinary status parses");
        Expect(Parsed.Branch.Head == "feature/forest" && !Parsed.Branch.bDetached, "branch.head");
        Expect(Parsed.Branch.Oid.size() == 40 && !Parsed.Branch.bInitial, "branch.oid");
        Expect(Parsed.Branch.bHasUpstream && Parsed.Branch.Upstream == "origin/feature/forest", "branch.upstream");
        Expect(Parsed.Branch.bHasAheadBehind && Parsed.Branch.Ahead == 2 && Parsed.Branch.Behind == 3, "branch.ab: +2 -3");
        Expect(Parsed.Entries.size() == 6, "six entries");
        if(Parsed.Entries.size() == 6)
        {
            const FileEntry& Staged = Parsed.Entries[0];
            Expect(Staged.IsStaged() && !Staged.IsUnstaged() && Staged.DisplayStatus() == 'M', "M. is staged only");
            const FileEntry& Worktree = Parsed.Entries[1];
            Expect(!Worktree.IsStaged() && Worktree.IsUnstaged(), ".M is unstaged only");
            const FileEntry& Both = Parsed.Entries[2];
            Expect(Both.IsStaged() && Both.IsUnstaged(), "MM is both");
            Expect(Parsed.Entries[3].DisplayStatus() == 'D', "a staged deletion");
            const FileEntry& New = Parsed.Entries[4];
            Expect(New.Kind == EEntryKind::Untracked && New.Path == "Games/WorldsLab/Content/New Folder/Rock 02.nmmesh",
                   "untracked, spaces in the path kept");
            Expect(New.IsUnstaged() && !New.IsStaged() && New.DisplayStatus() == '?', "untracked is unstaged, '?'");
            Expect(Parsed.Entries[5].Kind == EEntryKind::Ignored, "ignored");
        }
    }

    {
        /* A rename: <path>, then <origPath> as the next record. */
        const std::string Out = Z({ "# branch.oid 1234567890abcdef1234567890abcdef12345678", "# branch.head main" })
                              + std::string("2 R. N... ") + ModeOid + " R100 Games/WorldsLab/Source/PickupBase.h" + '\0'
                              + "Games/WorldsLab/Source/Pickup.h" + '\0';
        Status Parsed;
        Expect(ParseStatusV2(Out, Parsed), "a rename parses");
        Expect(Parsed.Entries.size() == 1, "one entry, not two");
        if(!Parsed.Entries.empty())
        {
            const FileEntry& Rename = Parsed.Entries[0];
            Expect(Rename.Kind == EEntryKind::Renamed && Rename.DisplayStatus() == 'R', "renamed");
            Expect(Rename.Path == "Games/WorldsLab/Source/PickupBase.h", "the new path");
            Expect(Rename.OrigPath == "Games/WorldsLab/Source/Pickup.h", "the old path");
            Expect(Rename.IsStaged(), "a staged rename");
        }
        Expect(!Parsed.Branch.bHasUpstream && !Parsed.Branch.bHasAheadBehind, "no upstream: no upstream, no ahead/behind");
    }

    {
        /* A conflict mid-rebase, on a detached HEAD. */
        const std::string Out = Z({ "# branch.oid abcdefabcdefabcdefabcdefabcdefabcdefabcd", "# branch.head (detached)",
                                    "u UU N... 100644 100644 100644 100644 1111111111111111111111111111111111111111 "
                                    "2222222222222222222222222222222222222222 3333333333333333333333333333333333333333 "
                                    "Games/WorldsLab/Config/DefaultGame.ini" });
        Status Parsed;
        Expect(ParseStatusV2(Out, Parsed), "a conflict parses");
        Expect(Parsed.Branch.bDetached && Parsed.Branch.Head.empty(), "a detached HEAD");
        Expect(Parsed.Entries.size() == 1 && Parsed.Entries[0].IsConflicted(), "the conflict");
        Expect(!Parsed.Entries.empty() && Parsed.Entries[0].DisplayStatus() == 'U', "shown as U");
        Expect(!Parsed.Entries.empty() && Parsed.Entries[0].Path == "Games/WorldsLab/Config/DefaultGame.ini", "its path");
    }

    {
        const std::string Out = Z({ "# branch.oid (initial)", "# branch.head main", "? README.md" });
        Status Parsed;
        Expect(ParseStatusV2(Out, Parsed), "before the first commit parses");
        Expect(Parsed.Branch.bInitial && Parsed.Branch.Oid.empty(), "(initial)");
        Expect(Parsed.Branch.Head == "main", "on its branch");
    }

    {
        Status Parsed;
        Expect(ParseStatusV2(std::string(), Parsed) && Parsed.Entries.empty(), "empty output is a clean, headerless status");
        Expect(ParseStatusV2(Z({ "# stash 2", "# branch.head main" }), Parsed), "unknown headers are skipped");
        std::string Error;
        Expect(!ParseStatusV2(Z({ "x what" }), Parsed, &Error) && !Error.empty(), "an unknown record fails, with why");
        Expect(!ParseStatusV2(Z({ "1 M. N..." }), Parsed), "a short record fails");
        Expect(!ParseStatusV2(std::string("2 R. N... ") + ModeOid + " R100 new.txt", Parsed), "a rename without its old path fails");
    }
}

static void TestClassifier()
{
    std::printf("Path groups\n");
    const std::string Game = "Games/WorldsLab";
    Expect(ClassifyPath("Games/WorldsLab/Content/Levels/A.nmworld", Game) == EPathGroup::Content, "Content");
    Expect(ClassifyPath("Games/WorldsLab/Scripts/Door.nmgraph", Game) == EPathGroup::Scripts, "Scripts");
    Expect(ClassifyPath("Games/WorldsLab/Source/Pickup.cpp", Game) == EPathGroup::GameModule, "Source");
    Expect(ClassifyPath("Games/WorldsLab/CMakeLists.txt", Game) == EPathGroup::GameModule, "the game's CMakeLists");
    Expect(ClassifyPath("Games/WorldsLab/CMakePresets.json", Game) == EPathGroup::GameModule, "its presets");
    Expect(ClassifyPath("Games/WorldsLab/Config/DefaultEngine.ini", Game) == EPathGroup::Config, "Config");
    Expect(ClassifyPath("Engine/Modules/Core/Source/Core.cpp", Game) == EPathGroup::Engine, "Engine");
    Expect(ClassifyPath("engine/x.txt", Game) == EPathGroup::Engine, "case-insensitive");
    Expect(ClassifyPath("Games/Sandbox/Content/A.nmworld", Game) == EPathGroup::Other, "another game is Other");
    Expect(ClassifyPath("README.md", Game) == EPathGroup::Other, "the root's files are Other");
    Expect(ClassifyPath("Games/WorldsLabX/Content/A.nmworld", Game) == EPathGroup::Other, "a prefix isn't the folder");
    Expect(ClassifyPath("Content/A.nmworld", "") == EPathGroup::Content, "a game at the repository's root");
    Expect(ClassifyPath("Games/WorldsLab/Content/A", "Games/WorldsLab/") == EPathGroup::Content, "a trailing slash on the game");
}

// --- Real repositories ------------------------------------------------------------------

/* Synchronous, for setting up; GitClient is what's under test. */
static bool Git(const QString& InDir, const QStringList& InArgs, QString* OutText = nullptr)
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

static void Write(const QString& InPath, const QByteArray& InText)
{
    QDir().mkpath(QFileInfo(InPath).absolutePath());
    QFile File(InPath);
    File.open(QIODevice::WriteOnly | QIODevice::Truncate);
    File.write(InText);
}

static void Identify(const QString& InRepo, const char* InName)
{
    Git(InRepo, { QStringLiteral("config"), QStringLiteral("user.name"), QString::fromLatin1(InName) });
    Git(InRepo, { QStringLiteral("config"), QStringLiteral("user.email"), QString::fromLatin1(InName) + QStringLiteral("@example.com") });
    Git(InRepo, { QStringLiteral("config"), QStringLiteral("core.autocrlf"), QStringLiteral("false") });
    Git(InRepo, { QStringLiteral("config"), QStringLiteral("commit.gpgsign"), QStringLiteral("false") });
}

static Status ReadStatus(GitClient& InGit)
{
    Status Out;
    bool   bOk = false;
    InGit.Status([&Out, &bOk](const bool bInOk, const Status& InStatus, const QString&) { bOk = bInOk; Out = InStatus; });
    InGit.WaitForIdle();
    Expect(bOk, "git status succeeds");
    return Out;
}

static const FileEntry* Find(const Status& InStatus, const char* InPath)
{
    for(const FileEntry& Each : InStatus.Entries)
    {
        if(Each.Path == InPath)
        {
            return &Each;
        }
    }
    return nullptr;
}

static bool RunOk(GitClient& InGit, const std::function<void(GitClient::Callback)>& InStart, QString* OutSummary = nullptr)
{
    bool bOk = false;
    InStart([&bOk, OutSummary](const GitClient::Result& InResult)
    {
        bOk = InResult.Ok();
        if(OutSummary) { *OutSummary = InResult.Summary(); }
        if(!bOk) { std::printf("    git: %s\n", InResult.Summary().toUtf8().constData()); }
    });
    InGit.WaitForIdle();
    return bOk;
}

static void TestRepositories(const QString& InTemp)
{
    std::printf("Stage, commit, push, fetch, pull\n");

    const QString Remote = QDir(InTemp).filePath(QStringLiteral("remote.git"));
    const QString Alice  = QDir(InTemp).filePath(QStringLiteral("alice"));
    const QString Bob    = QDir(InTemp).filePath(QStringLiteral("bob"));
    Expect(Git(InTemp, { QStringLiteral("init"), QStringLiteral("--bare"), QStringLiteral("-b"), QStringLiteral("main"), Remote }),
           "a bare remote");
    Expect(Git(InTemp, { QStringLiteral("clone"), QStringLiteral("-q"), Remote, Alice }), "Alice clones it");
    Identify(Alice, "alice");
    Git(Alice, { QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("main") });

    GitClient AliceGit(Alice);
    QDir().mkpath(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab/Content")));
    Expect(QDir(GitClient::FindRepositoryRoot(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab/Content")))) == QDir(Alice),
           "FindRepositoryRoot walks up to the .git");

    /* Before the first commit. */
    Write(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab/Content/Forest.nmworld")), "<World>\n</World>\n");
    Write(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab/Source/Pickup.cpp")), "int Ammo = 0;\n");
    Status First = ReadStatus(AliceGit);
    Expect(First.Branch.bInitial, "a new repository is (initial)");
    Expect(Find(First, "Games/WorldsLab/Content/Forest.nmworld") && Find(First, "Games/WorldsLab/Source/Pickup.cpp"),
           "both new files are listed, untracked");

    Expect(RunOk(AliceGit, [&](GitClient::Callback Done)
    {
        AliceGit.Stage({ QStringLiteral("Games/WorldsLab/Content/Forest.nmworld"), QStringLiteral("Games/WorldsLab/Source/Pickup.cpp") }, Done);
    }), "Stage");
    Status Staged = ReadStatus(AliceGit);
    Expect(Find(Staged, "Games/WorldsLab/Content/Forest.nmworld") && Find(Staged, "Games/WorldsLab/Content/Forest.nmworld")->IsStaged(),
           "staged");

    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Unstage({ QStringLiteral("Games/WorldsLab/Source/Pickup.cpp") }, true, Done); }),
           "Unstage before the first commit");
    Status Unstaged = ReadStatus(AliceGit);
    Expect(Find(Unstaged, "Games/WorldsLab/Source/Pickup.cpp") && Find(Unstaged, "Games/WorldsLab/Source/Pickup.cpp")->Kind == EEntryKind::Untracked,
           "back to untracked");

    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Commit(QStringLiteral("Forest level\n\nWith a \"quoted\" body; and $SHELL."), Done); }),
           "Commit, the message on stdin");
    QString Subject;
    Git(Alice, { QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B") }, &Subject);
    Expect(Subject == QStringLiteral("Forest level\n\nWith a \"quoted\" body; and $SHELL."), "the message arrives intact");

    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Push(true, Done); }), "Push, setting the upstream");
    Status Pushed = ReadStatus(AliceGit);
    Expect(Pushed.Branch.bHasUpstream && Pushed.Branch.Upstream == "origin/main", "tracks origin/main");
    Expect(Pushed.Branch.Ahead == 0 && Pushed.Branch.Behind == 0, "level with it");

    /* Bob clones and pushes a change; Alice fetches and fast-forwards. */
    Expect(Git(InTemp, { QStringLiteral("clone"), QStringLiteral("-q"), Remote, Bob }), "Bob clones");
    Identify(Bob, "bob");
    Write(QDir(Bob).filePath(QStringLiteral("Games/WorldsLab/Content/Forest.nmworld")), "<World>\n  <Entity/>\n</World>\n");
    Git(Bob, { QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("Bob's entity") });
    Expect(Git(Bob, { QStringLiteral("push"), QStringLiteral("-q") }), "Bob pushes");

    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Fetch(Done); }), "Fetch");
    Status Fetched = ReadStatus(AliceGit);
    Expect(Fetched.Branch.Behind == 1 && Fetched.Branch.Ahead == 0, "one behind after fetching");

    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Pull(false, Done); }), "Pull, fast-forward");
    QFile Level(QDir(Alice).filePath(QStringLiteral("Games/WorldsLab/Content/Forest.nmworld")));
    Level.open(QIODevice::ReadOnly);
    Expect(Level.readAll().contains("<Entity/>"), "Bob's edit arrives");
    Level.close();

    /* Both commit: fast-forward refuses, rebase replays. */
    Write(QDir(Bob).filePath(QStringLiteral("Games/WorldsLab/Config/DefaultGame.ini")), "[Game]\nLives=3\n");
    Git(Bob, { QStringLiteral("add"), QStringLiteral("-A") });
    Git(Bob, { QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("Lives") });
    Git(Bob, { QStringLiteral("push"), QStringLiteral("-q") });
    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Stage({ QStringLiteral("Games/WorldsLab/Source/Pickup.cpp") }, Done); }),
           "Alice stages the source");
    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Commit(QStringLiteral("Pickup"), Done); }), "and commits it");
    RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Fetch(Done); });
    Status Diverged = ReadStatus(AliceGit);
    Expect(Diverged.Branch.Ahead == 1 && Diverged.Branch.Behind == 1, "one ahead, one behind");
    QString Why;
    Expect(!RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Pull(false, Done); }, &Why), "fast-forward only refuses to merge");
    Expect(!Why.isEmpty(), "and says why");
    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Pull(true, Done); }), "Pull with rebase");
    Status Rebased = ReadStatus(AliceGit);
    Expect(Rebased.Branch.Ahead == 1 && Rebased.Branch.Behind == 0, "her commit on top of his");
    QString Parents;
    Git(Alice, { QStringLiteral("rev-list"), QStringLiteral("--merges"), QStringLiteral("--count"), QStringLiteral("HEAD") }, &Parents);
    Expect(Parents == QStringLiteral("0"), "no merge commit");
    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Push(false, Done); }), "Push");

    /* Branches and switching. */
    QStringList Names;
    AliceGit.Branches([&Names](const QStringList& InNames) { Names = InNames; });
    AliceGit.WaitForIdle();
    Expect(Names == QStringList{ QStringLiteral("main") }, "Branches lists main");
    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.SwitchBranch(QStringLiteral("collab/forest"), true, Done); }),
           "SwitchBranch creates one");
    Expect(ReadStatus(AliceGit).Branch.Head == "collab/forest", "and HEAD is on it");
    Expect(RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.SwitchBranch(QStringLiteral("main"), false, Done); }), "and back");

    /* A bad command reports, rather than hanging. */
    QString Bad;
    Expect(!RunOk(AliceGit, [&](GitClient::Callback Done) { AliceGit.Run({ QStringLiteral("no-such-command") }, Done); }, &Bad),
           "an unknown command fails");
    Expect(!Bad.isEmpty(), "with git's reason");
}

static void Pump(const int InMs)
{
    QElapsedTimer Clock;
    Clock.start();
    while(Clock.elapsed() < InMs)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

static bool WaitRefreshed(SourceControlWindow& InDock)
{
    bool bDone = false;
    const auto Connection = QObject::connect(&InDock, &SourceControlWindow::Refreshed, [&bDone](bool) { bDone = true; });
    InDock.RefreshNow();
    QElapsedTimer Clock;
    Clock.start();
    while(!bDone && Clock.elapsed() < 30000)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        InDock.GetGit()->WaitForIdle(50);
    }
    QObject::disconnect(Connection);
    /* Whatever the refresh queued (diffs) too. */
    InDock.GetGit()->WaitForIdle();
    return bDone;
}

static QTreeWidgetItem* FindGroup(QTreeWidget* InTree, const char* InPrefix)
{
    for(int Index = 0; Index < InTree->topLevelItemCount(); ++Index)
    {
        if(InTree->topLevelItem(Index)->text(0).startsWith(QString::fromLatin1(InPrefix)))
        {
            return InTree->topLevelItem(Index);
        }
    }
    return nullptr;
}

static void TestDock(const QString& InTemp)
{
    std::printf("The dock\n");
    const QString Alice   = QDir(InTemp).filePath(QStringLiteral("alice"));
    const QString Project = QDir(Alice).filePath(QStringLiteral("Games/WorldsLab"));
    const QString Config  = QDir(Project).filePath(QStringLiteral("Config/DefaultEngine.ini"));
    Write(Config, "[Game]\nModule=WorldsLab\n");
    Write(QDir(Project).filePath(QStringLiteral("Content/Forest.nmworld")), "<World>\n  <Entity/>\n  <Entity Name=\"Tree\"/>\n</World>\n");
    Write(QDir(Project).filePath(QStringLiteral("Scripts/Door.nmgraph")), "<Graph/>\n");
    Write(QDir(Alice).filePath(QStringLiteral("Engine/Core.cpp")), "// engine\n");

    SourceControlWindow Dock(Project, Config, nullptr);
    Dock.resize(1000, 600);
    Dock.show();
    Expect(Dock.GetGit() != nullptr, "finds the repository above the project");
    Expect(Dock.GetGameDir() == QStringLiteral("Games/WorldsLab"), "the game's folder, repository-relative");
    Expect(WaitRefreshed(Dock), "refreshes");

    QTreeWidget* Tree = Dock.GetTree();
    QTreeWidgetItem* Content = FindGroup(Tree, "Content");
    QTreeWidgetItem* Engine  = FindGroup(Tree, "Engine");
    Expect(Content && Content->childCount() == 1, "Content: the level");
    Expect(FindGroup(Tree, "Scripts") && FindGroup(Tree, "Config"), "Scripts and Config groups");
    Expect(Engine && Engine->text(0).contains(QStringLiteral("not supported")), "Engine flagged as unsupported in a session");

    /* Checking a file stages it. */
    if(Content)
    {
        Content->child(0)->setCheckState(0, Qt::Checked);
        WaitRefreshed(Dock);
        const FileEntry* Level = nullptr;
        for(const FileEntry& Each : Dock.GetStatus().Entries)
        {
            if(Each.Path == "Games/WorldsLab/Content/Forest.nmworld") { Level = &Each; }
        }
        Expect(Level && Level->IsStaged(), "checking a file stages it");
    }

    /* Its diff. */
    Dock.ShowDiff(QStringLiteral("Games/WorldsLab/Content/Forest.nmworld"));
    Dock.GetGit()->WaitForIdle();
    Pump(20);
    Expect(Dock.GetDiffView()->toPlainText().contains(QStringLiteral("+  <Entity Name=\"Tree\"/>")), "the diff shows the added line");

    /* Checking a group stages it all. */
    if(QTreeWidgetItem* Scripts = FindGroup(Dock.GetTree(), "Scripts"))
    {
        Scripts->setCheckState(0, Qt::Checked);
        WaitRefreshed(Dock);
        Expect(FindGroup(Dock.GetTree(), "Scripts") && FindGroup(Dock.GetTree(), "Scripts")->checkState(0) == Qt::Checked,
               "checking a group stages the group");
    }

    /* Commit from the dock. */
    Dock.CommitStaged(QStringLiteral("Trees and doors"));
    Dock.GetGit()->WaitForIdle();
    WaitRefreshed(Dock);
    QString Subject;
    Git(Alice, { QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%s") }, &Subject);
    Expect(Subject == QStringLiteral("Trees and doors"), "Commit commits the staged files");
    Expect(!FindGroup(Dock.GetTree(), "Content") && FindGroup(Dock.GetTree(), "Engine"), "leaving the unstaged ones");

    /* The session branch: in the config, and warned about. */
    Dock.SetSessionBranch(QStringLiteral("collab/forest"));
    WaitRefreshed(Dock);
    QFile Ini(Config);
    Ini.open(QIODevice::ReadOnly);
    Expect(Ini.readAll().contains("Branch"), "the session branch goes into the config");
    Ini.close();
    Expect(Dock.GetSessionBanner()->isVisible() && Dock.GetSessionBanner()->text().contains(QStringLiteral("collab/forest")),
           "HEAD elsewhere: the banner says so");
    Dock.SetSessionBranch(QStringLiteral("main"));
    Expect(!Dock.GetSessionBanner()->isVisible(), "on it: no banner");

    /* Ahead: Pull would rebase. */
    Expect(Dock.GetPullButton()->text() == QStringLiteral("Pull"), "level or ahead only: plain Pull");
    Expect(!Dock.GetConflictBanner()->isVisible(), "no conflicts, no banner");

    /* Pushed from the dock. */
    Dock.PushRemote();
    Dock.GetGit()->WaitForIdle();
    WaitRefreshed(Dock);
    Expect(Dock.GetStatus().Branch.Ahead == 0, "PushRemote pushes");

    /* A conflict: Bob and Alice edit the same line; the rebase stops. */
    const QString Bob = QDir(InTemp).filePath(QStringLiteral("bob"));
    Git(Bob, { QStringLiteral("pull"), QStringLiteral("-q"), QStringLiteral("--ff-only") });
    Write(QDir(Bob).filePath(QStringLiteral("Games/WorldsLab/Config/DefaultGame.ini")), "[Game]\nLives=5\n");
    Git(Bob, { QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("Five lives") });
    Git(Bob, { QStringLiteral("push"), QStringLiteral("-q") });
    Write(QDir(Project).filePath(QStringLiteral("Config/DefaultGame.ini")), "[Game]\nLives=9\n");
    Git(Alice, { QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("Nine lives") });
    Dock.FetchRemote();
    Dock.GetGit()->WaitForIdle();
    WaitRefreshed(Dock);
    Expect(Dock.GetPullButton()->text() == QStringLiteral("Pull (Rebase)"), "ahead and behind: Pull offers a rebase");
    Dock.PullRemote();
    Dock.GetGit()->WaitForIdle();
    WaitRefreshed(Dock);
    Expect(Dock.GetConflictBanner()->isVisible(), "a conflict shows the banner");
    Expect(FindGroup(Dock.GetTree(), "Config") != nullptr, "the conflicted file is listed");
    Git(Alice, { QStringLiteral("rebase"), QStringLiteral("--abort") });
}

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);
    std::printf("NimbleEditorSourceControlTests\n");
    EditorNotifications::Install();

    TestParser();
    TestClassifier();

    if(Git(QDir::currentPath(), { QStringLiteral("--version") }))
    {
        QTemporaryDir Temp;
        Expect(Temp.isValid(), "a temporary folder");
        TestRepositories(Temp.path());
        TestDock(Temp.path());
    }
    else
    {
        std::printf("  (git isn't on PATH: the repository tests are skipped)\n");
    }

    EditorNotifications::Uninstall();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
