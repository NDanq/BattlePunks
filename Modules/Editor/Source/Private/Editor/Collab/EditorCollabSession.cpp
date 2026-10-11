#if WITH_EDITOR

#include "Private/Editor/Collab/EditorCollabSession.h"

#include <algorithm>
#include <memory>

#include <QDir>
#include <QFileInfo>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/SourceControl/GitClient.h"
#include "Public/Editor/EditorContentEvents.h"
#include "Public/Editor/EditorNotifications.h"

using ESeverity = EditorNotifications::ESeverity;

namespace
{
    std::string ToStd(const QString& InText)
    {
        return std::string(InText.toUtf8().constData());
    }

    QString ToQ(const std::string& InText)
    {
        return QString::fromUtf8(InText.c_str(), int(InText.size()));
    }

    ESeverity ToSeverity(const Collab::ESeverity InSeverity)
    {
        switch(InSeverity)
        {
            case Collab::ESeverity::Success: return ESeverity::Success;
            case Collab::ESeverity::Warning: return ESeverity::Warning;
            case Collab::ESeverity::Error:   return ESeverity::Error;
            case Collab::ESeverity::Info:    break;
        }
        return ESeverity::Info;
    }

    const QString StateKey = QStringLiteral("collab.state");

    /* What Connect learns from git, a step at a time. */
    struct Gathered
    {
        QString Host;
        uint16_t Port = Collab::DefaultPort;
        QString User;
        QString Password;
        QString Remote;
        QString Head;
        QString EngineTree;
        bool    bEngineDirty = false;
        QString CurrentBranch;
    };
}

EditorCollabSession::EditorCollabSession(const QString& InProjectRoot, const QString& InConfigPath, QObject* InParent)
    : QObject(InParent)
    , ProjectRoot(QDir(InProjectRoot).absolutePath())
    , ConfigPath(InConfigPath)
{
    RepoRoot = GitClient::FindRepositoryRoot(ProjectRoot);
    if(!RepoRoot.isEmpty())
    {
        Git = new GitClient(RepoRoot, this);
        GameDir = QDir(RepoRoot).relativeFilePath(ProjectRoot);
        if(GameDir == QStringLiteral("."))
        {
            GameDir.clear();
        }
    }

    PollTimer.setInterval(20);
    connect(&PollTimer, &QTimer::timeout, this, &EditorCollabSession::Poll);

    DirtyTimer.setSingleShot(true);
    DirtyTimer.setInterval(5000);
    connect(&DirtyTimer, &QTimer::timeout, this, &EditorCollabSession::ReadDirtyFromGit);

    DirtyHeartbeat.setInterval(15000);
    connect(&DirtyHeartbeat, &QTimer::timeout, this, &EditorCollabSession::ReadDirtyFromGit);
}

EditorCollabSession::~EditorCollabSession()
{
    Client.Leave();
    Client.Poll();
}

void EditorCollabSession::Connect(const QString& InAddress, const QString& InUser, const QString& InPassword)
{
    if(!Git)
    {
        EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Can't join a session"),
                                    QStringLiteral("This project isn't in a git repository; a session shares its work through git."),
                                    QStringLiteral("Collab"), StateKey);
        return;
    }
    if(bConnecting || Client.GetState() != Collab::CollabClient::EState::Idle)
    {
        return;
    }

    auto Found = std::make_shared<Gathered>();
    const QString Address = InAddress.trimmed();
    const int Colon = Address.lastIndexOf(QLatin1Char(':'));
    Found->Host = Colon > 0 ? Address.left(Colon) : Address;
    if(Colon > 0)
    {
        bool bOk = false;
        const int Port = Address.mid(Colon + 1).toInt(&bOk);
        if(!bOk || Port <= 0 || Port > 65535)
        {
            EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Bad server address"),
                                        QStringLiteral("Use host or host:port, like 192.168.1.20:7788."), QStringLiteral("Collab"), StateKey);
            return;
        }
        Found->Port = uint16_t(Port);
    }
    if(Found->Host.isEmpty())
    {
        EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Enter the session server's address"), QString(),
                                    QStringLiteral("Collab"), StateKey);
        return;
    }
    Found->User     = InUser.trimmed();
    Found->Password = InPassword;

    bConnecting = true;
    emit StateChanged();

    /* One after another on the git queue; the last one connects. */
    const auto Text = [](const GitClient::Result& InResult) { return QString::fromUtf8(InResult.Out).trimmed(); };
    if(Found->User.isEmpty())
    {
        Git->RunQuiet({ QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("user.name") },
                 [Found, Text](const GitClient::Result& InResult) { Found->User = Text(InResult); });
    }
    Git->RunQuiet({ QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin") },
             [Found, Text](const GitClient::Result& InResult) { Found->Remote = Text(InResult); });
    Git->Run({ QStringLiteral("rev-parse"), QStringLiteral("HEAD") },
             [Found, Text](const GitClient::Result& InResult) { Found->Head = Text(InResult); });
    Git->RunQuiet({ QStringLiteral("rev-parse"), QStringLiteral("HEAD:Engine") },
             [Found, Text](const GitClient::Result& InResult) { Found->EngineTree = InResult.Ok() ? Text(InResult) : QString(); });
    Git->Run({ QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("HEAD") },
             [Found, Text](const GitClient::Result& InResult) { Found->CurrentBranch = Text(InResult); });
    Git->RunQuiet({ QStringLiteral("diff"), QStringLiteral("--quiet"), QStringLiteral("HEAD"), QStringLiteral("--"), QStringLiteral("Engine") },
             [this, Found](const GitClient::Result& InResult)
    {
        /* Exit 1: differences. Anything else but 0 is git failing; count it as dirty. */
        Found->bEngineDirty = InResult.ExitCode != 0;
        bConnecting = false;

        if(Found->Head.isEmpty())
        {
            EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Can't join a session"),
                                        QStringLiteral("This repository has no commits yet."), QStringLiteral("Collab"), StateKey);
            emit StateChanged();
            return;
        }
        if(Found->EngineTree.isEmpty())
        {
            EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Can't join a session"),
                                        QStringLiteral("There's no Engine/ folder at the repository's root in HEAD, so the "
                                                       "engine can't be compared with the others'."),
                                        QStringLiteral("Collab"), StateKey);
            emit StateChanged();
            return;
        }

        Collab::ClientIdentity Identity;
        Identity.User = ToStd(Found->User.isEmpty() ? qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER", QStringLiteral("editor")))
                                                    : Found->User);
        const EngineCore::ConfigVar* ModuleVar = EngineCore::ConfigFindVar("Game", "Module");
        const std::string Module = ModuleVar ? std::string(ModuleVar->GetString()) : ToStd(QDir(ProjectRoot).dirName());
        /* No origin: the repository's own folder stands in, which only matches this machine. */
        Identity.ProjectId = Collab::MakeProjectId(Module, ToStd(Found->Remote.isEmpty() ? RepoRoot : Found->Remote));

        ConfigIniFile Config;
        QString Branch;
        if(!ConfigPath.isEmpty() && Config.Load(ConfigPath))
        {
            Branch = Config.Get(QStringLiteral("Collaboration"), QStringLiteral("Branch"));
        }
        Identity.Branch       = ToStd(Branch.isEmpty() ? Found->CurrentBranch : Branch);
        Identity.Head         = ToStd(Found->Head);
        Identity.EngineTree   = ToStd(Found->EngineTree);
        Identity.bEngineDirty = Found->bEngineDirty;
        Identity.Password     = ToStd(Found->Password);
        Head = Found->Head;

        ConnectAs(Found->Host, Found->Port, Identity);
    });
}

void EditorCollabSession::ConnectAs(const QString& InHost, const uint16_t InPort, const Collab::ClientIdentity& InIdentity)
{
    ServerAddress = QStringLiteral("%1:%2").arg(InHost).arg(InPort);
    SessionBranch = ToQ(InIdentity.Branch);
    if(Head.isEmpty())
    {
        Head = ToQ(InIdentity.Head);
    }
    PushPresence();
    if(!Client.Connect(ToStd(InHost), InPort, InIdentity))
    {
        EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Couldn't reach the session server"),
                                    QStringLiteral("%1 doesn't resolve.").arg(InHost), QStringLiteral("Collab"), StateKey);
        emit StateChanged();
        return;
    }
    EditorNotifications::Notification Busy;
    Busy.Title      = QStringLiteral("Joining the session at %1…").arg(ServerAddress);
    Busy.Source     = QStringLiteral("Collab");
    Busy.ReplaceKey = StateKey;
    Busy.Progress   = EditorNotifications::Indeterminate;
    Busy.Actions    = { { QStringLiteral("Cancel"), [this]() { Leave(); } } };
    if(EditorNotifications* Notifications = EditorNotifications::Get())
    {
        Notifications->Post(std::move(Busy));
    }
    PollTimer.start();
    emit StateChanged();
}

void EditorCollabSession::Leave()
{
    Client.Leave();
}

void EditorCollabSession::Poll()
{
    Client.Poll();
    Collab::ClientEvent Event;
    while(Client.NextEvent(Event))
    {
        Handle(Event);
    }
    if(Client.GetState() == Collab::CollabClient::EState::Idle)
    {
        PollTimer.stop();
        DirtyHeartbeat.stop();
    }
}

void EditorCollabSession::Handle(const Collab::ClientEvent& InEvent)
{
    using EType = Collab::ClientEvent::EType;
    const QString User = ToQ(InEvent.User);
    switch(InEvent.Type)
    {
        case EType::Joined:
        {
            const int Others = int(Client.GetRoster().size()) - 1;
            EditorNotifications::Notify(ESeverity::Success, QStringLiteral("Joined the session at %1").arg(ServerAddress),
                                        Others == 0 ? QStringLiteral("You're the first one here.")
                                                    : QStringLiteral("%1 other editor(s) here.").arg(Others),
                                        QStringLiteral("Collab"), StateKey);
            DirtyHeartbeat.start();
            ReadDirtyFromGit();
            emit StateChanged();
            emit RosterChanged();
            break;
        }
        case EType::Rejected:
            EditorNotifications::Notify(ESeverity::Error, QStringLiteral("The session refused this editor"), ToQ(InEvent.Text),
                                        QStringLiteral("Collab"), StateKey);
            emit StateChanged();
            break;
        case EType::ConnectFailed:
            EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Couldn't reach the session server"), ToQ(InEvent.Text),
                                        QStringLiteral("Collab"), StateKey);
            emit StateChanged();
            break;
        case EType::Disconnected:
            ClearPending();
            if(bPlayingTogether)
            {
                OnPlayEnded(QStringLiteral("You left the session."));
            }
            EditorNotifications::Notify(InEvent.Text.find("You left") == 0 ? ESeverity::Info : ESeverity::Warning,
                                        QStringLiteral("Out of the session"), ToQ(InEvent.Text), QStringLiteral("Collab"), StateKey);
            emit StateChanged();
            emit RosterChanged();
            break;
        case EType::MemberJoined:
            EditorNotifications::Notify(ESeverity::Info, QStringLiteral("%1 joined the session").arg(User), QString(),
                                        QStringLiteral("Collab"), QStringLiteral("collab.roster"));
            emit RosterChanged();
            break;
        case EType::MemberLeft:
            EditorNotifications::Notify(ESeverity::Info, QStringLiteral("%1 left the session").arg(User.isEmpty() ? QStringLiteral("Someone") : User),
                                        ToQ(InEvent.Text), QStringLiteral("Collab"), QStringLiteral("collab.roster"));
            emit RosterChanged();
            break;
        case EType::MemberChanged:
            emit RosterChanged();
            break;
        case EType::Overlap:
        {
            const QString Path = ToQ(InEvent.Path);
            EditorNotifications::Notify(ESeverity::Warning,
                                        QStringLiteral("%1 is changing %2 too").arg(User, QFileInfo(Path).fileName()),
                                        QStringLiteral("You both have changes to %1 that aren't pushed. Agree who keeps theirs "
                                                       "before either of you pushes, or git will have a conflict to resolve.").arg(Path),
                                        QStringLiteral("Collab"), QStringLiteral("collab.overlap.") + Path);
            break;
        }
        case EType::BranchAdvanced:
            OnBranchAdvanced(InEvent);
            break;
        case EType::BuildStatus:
            OnBuildStatus(InEvent.Build);
            break;
        case EType::Validation:
            OnValidation(InEvent.Report);
            break;
        case EType::PlayInvite:
            OnPlayInvite(InEvent.Invite);
            break;
        case EType::PlayEnded:
            OnPlayEnded(ToQ(InEvent.Text));
            break;
        case EType::Notified:
        {
            /* Waiting on Play Together, it's the server's answer: in place of "Starting...". */
            const bool bPlayAnswer = bJoinWhenReady && !Client.IsRunActive();
            if(bPlayAnswer)
            {
                bJoinWhenReady = false;
            }
            EditorNotifications::Notify(ToSeverity(InEvent.Severity), ToQ(InEvent.Title), ToQ(InEvent.Text), QStringLiteral("Collab"),
                                        bPlayAnswer ? QStringLiteral("collab.play") : QString());
            break;
        }
    }
}

void EditorCollabSession::SetOpenLevel(const QString& InPath)
{
    const QString Relative = InPath.isEmpty() ? QString() : ToRepoRelative(InPath);
    if(Relative != OpenLevel)
    {
        OpenLevel = Relative;
        PushPresence();
    }
}

void EditorCollabSession::SetLevelUnsaved(const bool bInUnsaved)
{
    if(bInUnsaved != bLevelUnsaved)
    {
        bLevelUnsaved = bInUnsaved;
        PushPresence();
    }
}

void EditorCollabSession::SetPlaying(const bool bInPlaying)
{
    if(bInPlaying != bPlaying)
    {
        bPlaying = bInPlaying;
        PushPresence();
    }
}

void EditorCollabSession::RequestDirtyRefresh()
{
    if(Client.IsJoined())
    {
        DirtyTimer.start();
    }
}

void EditorCollabSession::ReadDirtyFromGit()
{
    if(!Git || !Client.IsJoined())
    {
        return;
    }
    Git->Status([this](const bool bInOk, const EditorGit::Status& InStatus, const QString&)
    {
        if(!bInOk)
        {
            return;
        }
        QStringList Paths;
        for(const EditorGit::FileEntry& Entry : InStatus.Entries)
        {
            if(Entry.Kind != EditorGit::EEntryKind::Ignored)
            {
                Paths << ToQ(Entry.Path);
            }
        }
        GitDirty = Paths;
        Head = ToQ(InStatus.Branch.Oid);
        PushPresence();

        /* Pulled some other way (Source Control, a terminal): the offer is stale. */
        if(!Pending.NewOid.empty() && !bPulling)
        {
            Git->RunQuiet({ QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"), ToQ(Pending.NewOid), QStringLiteral("HEAD") },
                     [this](const GitClient::Result& InResult)
            {
                if(InResult.Ok() && !bPulling)
                {
                    ClearPending();
                }
            });
        }
    });
}

void EditorCollabSession::PushPresence()
{
    std::vector<std::string> Dirty;
    for(const QString& Each : GitDirty)
    {
        Dirty.push_back(ToStd(Each));
    }
    if(bLevelUnsaved && !OpenLevel.isEmpty())
    {
        Dirty.push_back(ToStd(OpenLevel));
    }
    const Collab::EPlayState Play = bPlayingTogether ? Collab::EPlayState::PlayingTogether
                                  : bPlaying         ? Collab::EPlayState::Playing
                                                     : Collab::EPlayState::Editing;
    Client.SetPresence(ToStd(Head), ToStd(OpenLevel), Play, std::move(Dirty));
}

QHash<QString, QStringList> EditorCollabSession::GetOthersDirty() const
{
    QHash<QString, QStringList> Out;
    for(const auto& Each : Client.GetOthersDirty())
    {
        QStringList Users;
        for(const std::string& User : Each.second)
        {
            Users << ToQ(User);
        }
        Out.insert(QDir::cleanPath(ToAbsolute(ToQ(Each.first))), Users);
    }
    return Out;
}

QStringList EditorCollabSession::DescribeRoster() const
{
    QStringList Lines;
    for(const auto& Each : Client.GetRoster())
    {
        const Collab::MemberState& Member = Each.second;
        QString Line = ToQ(Member.User);
        if(Each.first == Client.GetMyId())
        {
            Line += QStringLiteral(" (you)");
        }
        Line += QStringLiteral(" — HEAD %1").arg(ToQ(Member.Head).left(8));
        if(!Member.OpenLevel.empty())
        {
            Line += QStringLiteral(", %1 %2").arg(Member.Play == Collab::EPlayState::Editing ? QStringLiteral("editing") : QStringLiteral("playing"),
                                                  QFileInfo(ToQ(Member.OpenLevel)).fileName());
        }
        Line += QStringLiteral(", %1 changed file(s)").arg(Member.Dirty.size());
        Lines << Line;
    }
    return Lines;
}

// --- The branch moving ---------------------------------------------------------------

namespace
{
    const QString BranchKey = QStringLiteral("collab.branch");

    /* How many paths in each group, for a toast: "12 content files, 2 GameModule source files". */
    struct GroupCounts
    {
        int Counts[size_t(EditorGit::EPathGroup::Count)] = {};

        int  Of(const EditorGit::EPathGroup InGroup) const { return Counts[size_t(InGroup)]; }
        bool Has(const EditorGit::EPathGroup InGroup) const { return Of(InGroup) > 0; }

        QString Describe() const
        {
            static const char* const Names[] = { "content file", "script", "GameModule source file", "config file",
                                                 "engine file", "other file" };
            QStringList Parts;
            for(size_t Index = 0; Index < size_t(EditorGit::EPathGroup::Count); ++Index)
            {
                if(Counts[Index] > 0)
                {
                    Parts << QStringLiteral("%1 %2%3").arg(Counts[Index]).arg(QString::fromLatin1(Names[Index]))
                                                      .arg(Counts[Index] == 1 ? QString() : QStringLiteral("s"));
                }
            }
            return Parts.join(QStringLiteral(", "));
        }
    };

    GroupCounts Count(const std::vector<std::string>& InPaths, const QString& InGameDir)
    {
        GroupCounts Out;
        const std::string Game = InGameDir.toStdString();
        for(const std::string& Path : InPaths)
        {
            ++Out.Counts[size_t(EditorGit::ClassifyPath(Path, Game))];
        }
        return Out;
    }
}

void EditorCollabSession::AnnouncePush(const QString& InBranch, const QString& InOldOid, const QString& InNewOid)
{
    if(!Git || !Client.IsJoined() || InNewOid.isEmpty())
    {
        return;
    }

    /* What moved: the commits (newest first, at most 200) and the paths they
     * changed. A branch's first push names its last commit only. */
    auto Move = std::make_shared<Collab::BranchMove>();
    Move->Branch = ToStd(InBranch);
    Move->OldOid = ToStd(InOldOid);
    Move->NewOid = ToStd(InNewOid);
    const QString Range = InOldOid.isEmpty() ? InNewOid : InOldOid + QStringLiteral("..") + InNewOid;
    QStringList LogArgs = { QStringLiteral("log"), QStringLiteral("--max-count=200"), QStringLiteral("--format=%H%x1f%an%x1f%s") };
    if(InOldOid.isEmpty())
    {
        LogArgs << QStringLiteral("-1");
    }
    LogArgs << Range;
    Git->Run(LogArgs, [Move](const GitClient::Result& InResult)
    {
        for(const QString& Line : QString::fromUtf8(InResult.Out).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        {
            const QStringList Fields = Line.split(QChar(0x1f));
            if(Fields.size() == 3)
            {
                Move->Commits.push_back({ ToStd(Fields[0]), ToStd(Fields[1]), ToStd(Fields[2]) });
            }
        }
    });
    const QStringList DiffArgs = InOldOid.isEmpty()
        ? QStringList{ QStringLiteral("show"), QStringLiteral("--name-only"), QStringLiteral("--format="), InNewOid }
        : QStringList{ QStringLiteral("diff"), QStringLiteral("--name-only"), InOldOid, InNewOid };
    Git->Run(DiffArgs, [this, Move](const GitClient::Result& InResult)
    {
        for(const QString& Line : QString::fromUtf8(InResult.Out).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        {
            Move->Paths.push_back(ToStd(Line.trimmed()));
        }
        Collab::SortUnique(Move->Paths);
        Client.AnnouncePush(*Move);
        CORE_LOG("[Collab] announced a push of " + Move->Branch + ": " + std::to_string(Move->Commits.size()) + " commit(s)")
    });
}

void EditorCollabSession::OnBranchAdvanced(const Collab::ClientEvent& InEvent)
{
    const Collab::BranchMove& Move = InEvent.Move;
    /* Newest first, as they arrived; a commit already pending isn't repeated. */
    std::vector<Collab::CommitInfo> Commits = Move.Commits;
    for(const Collab::CommitInfo& Old : Pending.Commits)
    {
        if(std::none_of(Commits.begin(), Commits.end(), [&Old](const Collab::CommitInfo& New) { return New.Oid == Old.Oid; }))
        {
            Commits.push_back(Old);
        }
    }
    Pending.Commits.swap(Commits);
    Pending.Paths.insert(Pending.Paths.end(), Move.Paths.begin(), Move.Paths.end());
    Collab::SortUnique(Pending.Paths);
    Pending.Branch = Move.Branch;
    if(Pending.OldOid.empty())
    {
        Pending.OldOid = Move.OldOid;
    }
    Pending.NewOid = Move.NewOid;
    const QString Who = ToQ(InEvent.User);
    if(!Pushers.contains(Who))
    {
        Pushers << Who;
    }
    CORE_LOG("[Collab] " + InEvent.User + " pushed " + Move.Branch + ": " + std::to_string(Move.Commits.size()) + " commit(s), "
             + std::to_string(Move.Paths.size()) + " path(s)")
    PostPending();
}

void EditorCollabSession::PostPending()
{
    if(Pending.Commits.empty() && Pending.Paths.empty())
    {
        return;
    }
    const GroupCounts Groups = Count(Pending.Paths, GameDir);
    const int Commits = int(Pending.Commits.size());

    EditorNotifications::Notification Note;
    Note.Severity   = Groups.Has(EditorGit::EPathGroup::Engine) ? ESeverity::Warning : ESeverity::Info;
    Note.Title      = QStringLiteral("%1 pushed %2 commit%3").arg(Pushers.join(QStringLiteral(" and ")))
                      .arg(Commits).arg(Commits == 1 ? QString() : QStringLiteral("s"));
    Note.Body       = Groups.Describe() + QLatin1Char('.');
    if(!Pending.Commits.empty())
    {
        Note.Body += QStringLiteral("\nLatest: ") + ToQ(Pending.Commits.front().Summary);
    }
    if(Groups.Has(EditorGit::EPathGroup::Engine))
    {
        Note.Body += QStringLiteral("\nIt changes the engine: that needs a rebuild and a restart, not a reload.");
    }
    Note.Source     = QStringLiteral("Collab");
    Note.ReplaceKey = BranchKey;
    Note.Actions    = { { QStringLiteral("Pull & Reload"), [this]() { PullAndReload(); } },
                        { QStringLiteral("Later"), nullptr } };
    if(EditorNotifications* Notes = EditorNotifications::Get())
    {
        Notes->Post(std::move(Note));
    }
}

// --- The server's build (P4) ----------------------------------------------------------

void EditorCollabSession::OnBuildStatus(const Collab::BuildStatusMessage& InStatus)
{
    const QString Short   = ToQ(InStatus.Oid).left(8);
    const QString Commit  = InStatus.Summary.empty() ? Short : QStringLiteral("%1 (%2)").arg(Short, ToQ(InStatus.Summary));
    const QString BuildKey = QStringLiteral("collab.build");
    EditorNotifications::Notification Note;
    Note.Source     = QStringLiteral("Collab");
    Note.ReplaceKey = BuildKey;
    switch(InStatus.State)
    {
        case Collab::EBuildState::Building:
            Note.Severity = ESeverity::Info;
            Note.Title    = QStringLiteral("The server is building %1").arg(Short);
            Note.Body     = QStringLiteral("%1's %2: the game module and its server, then every world and prefab.")
                            .arg(ToQ(InStatus.Author), Commit);
            Note.Progress = EditorNotifications::Indeterminate;
            break;
        case Collab::EBuildState::Succeeded:
            Note.Severity = ESeverity::Success;
            Note.Title    = QStringLiteral("The server built %1").arg(Short);
            Note.Body     = QStringLiteral("%1, in %2 s.").arg(Commit).arg(InStatus.Seconds);
            break;
        case Collab::EBuildState::Failed:
            Note.Severity = ESeverity::Error;
            Note.Title    = QStringLiteral("The session's build failed");
            Note.Body     = QStringLiteral("%1's %2:\n%3").arg(ToQ(InStatus.Author), Commit, ToQ(InStatus.FirstError));
            break;
        case Collab::EBuildState::Idle:
            emit BuildChanged();
            return;
    }
    if(EditorNotifications* Notes = EditorNotifications::Get())
    {
        Notes->Post(std::move(Note));
    }
    emit BuildChanged();
}

void EditorCollabSession::OnValidation(const Collab::ValidationReportMessage& InReport)
{
    if(InReport.Problems.empty())
    {
        return;
    }
    const QString Short = ToQ(InReport.Oid).left(8);
    const int Count = int(InReport.Problems.size());
    QStringList Lines;
    for(const Collab::ContentProblem& Each : InReport.Problems)
    {
        Lines << QStringLiteral("%1: %2").arg(ToQ(Each.Path), ToQ(Each.Message));
    }

    EditorNotifications::Notification Note;
    Note.Severity   = ESeverity::Warning;
    Note.Title      = QStringLiteral("%1 content problem%2 in %3").arg(Count).arg(Count == 1 ? QString() : QStringLiteral("s")).arg(Short);
    Note.Body       = Lines.mid(0, 3).join(QLatin1Char('\n'));
    if(Count > 3)
    {
        Note.Body += QStringLiteral("\n… and %1 more").arg(Count - 3);
    }
    Note.Source     = QStringLiteral("Collab");
    Note.ReplaceKey = QStringLiteral("collab.validation");
    Note.Actions    = { { QStringLiteral("Show in Console"), [Lines, Short]()
    {
        CORE_LOG_WARN("[Collab] content problems in " + ToStd(Short) + ":")
        for(const QString& Line : Lines)
        {
            CORE_LOG_WARN("[Collab]   " + ToStd(Line))
        }
    } } };
    if(EditorNotifications* Notes = EditorNotifications::Get())
    {
        Notes->Post(std::move(Note));
    }
}

// --- Play Together (P5) ----------------------------------------------------------------

namespace
{
    const QString PlayKey = QStringLiteral("collab.play");

    QString Hex(const uint64_t InValue)
    {
        return QString::number(qulonglong(InValue), 16).rightJustified(16, QLatin1Char('0'));
    }
}

void EditorCollabSession::RequestPlayTogether(const QString& InLevel)
{
    if(!Client.IsJoined())
    {
        EditorNotifications::Notify(ESeverity::Info, QStringLiteral("Join a session first"),
                                    QStringLiteral("Play Together runs on the session server's game server."), QStringLiteral("Collab"), PlayKey);
        return;
    }
    if(Client.IsRunActive())
    {
        JoinPlayTogether();
        return;
    }
    bJoinWhenReady = true;
    Client.RequestPlay(ToStd(InLevel));
    EditorNotifications::Notification Busy;
    Busy.Title      = QStringLiteral("Starting the game server…");
    Busy.Body       = InLevel.isEmpty() ? QStringLiteral("The game's start level.") : QFileInfo(InLevel).completeBaseName();
    Busy.Source     = QStringLiteral("Collab");
    Busy.ReplaceKey = PlayKey;
    Busy.Progress   = EditorNotifications::Indeterminate;
    if(EditorNotifications* Notes = EditorNotifications::Get())
    {
        Notes->Post(std::move(Busy));
    }
}

void EditorCollabSession::StopPlayTogether()
{
    Client.RequestStopPlay();
}

bool EditorCollabSession::IsRunStarter() const
{
    return Client.IsRunActive() && Client.GetActiveRun().FromId == Client.GetMyId();
}

void EditorCollabSession::OnPlayInvite(const Collab::PlayInviteMessage& InInvite)
{
    /* What this editor's game build says, now; its HEAD, from git. */
    const uint64_t Local = PlayHooksSet.LocalSchemaHash ? PlayHooksSet.LocalSchemaHash() : 0;
    QString Blocker;
    if(Local == 0)
    {
        Blocker = QStringLiteral("Your editor has no game module loaded. Compile Source, then join.");
    }
    else if(Local != InInvite.SchemaHash)
    {
        Blocker = QStringLiteral("Your build of the game differs from the server's (schema %1, the server's %2). "
                                 "Compile Source, then join.").arg(Hex(Local), Hex(InInvite.SchemaHash));
    }
    JoinBlocker = Blocker;

    if(!Git || !Blocker.isEmpty() || InInvite.Oid.empty())
    {
        PostInvite();
        return;
    }
    const std::string Oid = InInvite.Oid;
    Git->RunQuiet({ QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"), ToQ(Oid), QStringLiteral("HEAD") },
                  [this, Oid](const GitClient::Result& InResult)
    {
        if(!Client.IsRunActive() || Client.GetActiveRun().Oid != Oid)
        {
            return;   // ended or replaced meanwhile
        }
        if(!InResult.Ok())
        {
            JoinBlocker = QStringLiteral("The run is at %1, which you don't have yet. Pull & Reload first.").arg(ToQ(Oid).left(8));
        }
        PostInvite();
    });
}

void EditorCollabSession::PostInvite()
{
    const Collab::PlayInviteMessage& Invite = Client.GetActiveRun();
    emit RunChanged();

    /* The one who asked plays at once. */
    if(bJoinWhenReady && Invite.FromId == Client.GetMyId())
    {
        bJoinWhenReady = false;
        if(JoinBlocker.isEmpty())
        {
            JoinPlayTogether();
            return;
        }
    }

    const QString Level = Invite.Level.empty() ? QStringLiteral("the start level") : QFileInfo(ToQ(Invite.Level)).completeBaseName();
    EditorNotifications::Notification Note;
    Note.Source     = QStringLiteral("Collab");
    Note.ReplaceKey = PlayKey;
    Note.Title      = QStringLiteral("%1 started Play Together").arg(ToQ(Invite.From));
    if(JoinBlocker.isEmpty())
    {
        Note.Severity = ESeverity::Info;
        Note.Body     = QStringLiteral("%1, on the session server, at %2.").arg(Level, ToQ(Invite.Oid).left(8));
        Note.Actions  = { { QStringLiteral("Join"), [this]() { JoinPlayTogether(); } },
                          { QStringLiteral("Decline"), nullptr } };
    }
    else
    {
        Note.Severity = ESeverity::Warning;
        Note.Body     = Level + QStringLiteral(". You can't join yet: ") + JoinBlocker;
        if(JoinBlocker.contains(QStringLiteral("Compile Source")) && Hooks.CompileGameModule)
        {
            Note.Actions = { { QStringLiteral("Compile Source"), [this]() { Hooks.CompileGameModule(); } } };
        }
        else if(!Pending.Commits.empty())
        {
            Note.Actions = { { QStringLiteral("Pull & Reload"), [this]() { PullAndReload(); } } };
        }
    }
    if(EditorNotifications* Notes = EditorNotifications::Get())
    {
        Notes->Post(std::move(Note));
    }
}

void EditorCollabSession::JoinPlayTogether()
{
    if(!Client.IsRunActive() || bPlayingTogether)
    {
        return;
    }
    const Collab::PlayInviteMessage& Invite = Client.GetActiveRun();

    /* Again, now: a Compile Source since the invite may have fixed it -- or not. */
    const uint64_t Local = PlayHooksSet.LocalSchemaHash ? PlayHooksSet.LocalSchemaHash() : 0;
    if(Local != Invite.SchemaHash)
    {
        JoinBlocker = QStringLiteral("Your build of the game differs from the server's (schema %1, the server's %2). "
                                     "Compile Source, then join.").arg(Hex(Local), Hex(Invite.SchemaHash));
    }
    else if(JoinBlocker.contains(QStringLiteral("Compile Source")))
    {
        JoinBlocker.clear();
    }
    if(!JoinBlocker.isEmpty())
    {
        EditorNotifications::Notify(ESeverity::Warning, QStringLiteral("You can't join Play Together yet"), JoinBlocker,
                                    QStringLiteral("Collab"), PlayKey);
        emit RunChanged();
        return;
    }

    /* The game server is on the session server's host, as this editor reached it. */
    QString Host = ToQ(Invite.Address);
    if(Host.isEmpty())
    {
        Host = ServerAddress.left(ServerAddress.lastIndexOf(QLatin1Char(':')));
    }
    if(!PlayHooksSet.JoinRun || !PlayHooksSet.JoinRun(ToQ(Invite.Level), Host, Invite.Port))
    {
        EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Couldn't join Play Together"),
                                    QStringLiteral("Play didn't start; the Console says why."), QStringLiteral("Collab"), PlayKey);
        return;
    }
    bPlayingTogether = true;
    PushPresence();
    EditorNotifications::Notify(ESeverity::Success, QStringLiteral("Playing together"),
                                QStringLiteral("On %1:%2. Stop Play to leave the run.").arg(Host).arg(Invite.Port),
                                QStringLiteral("Collab"), PlayKey);
    emit RunChanged();
}

void EditorCollabSession::SetPlayingTogether(const bool bInPlaying)
{
    if(bInPlaying == bPlayingTogether)
    {
        return;
    }
    bPlayingTogether = bInPlaying;
    PushPresence();
    emit RunChanged();
}

void EditorCollabSession::OnPlayEnded(const QString& InReason)
{
    const bool bWasPlaying = bPlayingTogether;
    bPlayingTogether = false;
    bJoinWhenReady   = false;
    JoinBlocker.clear();
    if(bWasPlaying && PlayHooksSet.LeaveRun)
    {
        PlayHooksSet.LeaveRun();
    }
    PushPresence();
    EditorNotifications::Notify(ESeverity::Info, QStringLiteral("Play Together ended"), InReason, QStringLiteral("Collab"), PlayKey);
    emit RunChanged();
}

void EditorCollabSession::ClearPending()
{
    Pending = Collab::BranchMove();
    Pushers.clear();
    if(EditorNotifications* Notes = EditorNotifications::Get())
    {
        if(const EditorNotifications::Notification* Found = Notes->FindByKey(BranchKey))
        {
            if(!Found->Actions.isEmpty())   // the offer, not a result
            {
                Notes->Remove(Found->Id);
            }
        }
    }
}

QStringList EditorCollabSession::GetPendingPaths() const
{
    QStringList Out;
    for(const std::string& Each : Pending.Paths)
    {
        Out << ToQ(Each);
    }
    return Out;
}

void EditorCollabSession::PullAndReload()
{
    if(!Git || bPulling || (Pending.Paths.empty() && Pending.Commits.empty()))
    {
        return;
    }
    const GroupCounts Groups = Count(Pending.Paths, GameDir);
    const QString Who = Pushers.join(QStringLiteral(" and "));

    /* The engine: never pulled into a running editor. */
    if(Groups.Has(EditorGit::EPathGroup::Engine))
    {
        EditorNotifications::Notify(ESeverity::Error, QStringLiteral("The session's engine changed"),
                                    QStringLiteral("%1 pushed engine changes. Engine changes aren't supported during a session: "
                                                   "pull them, rebuild the engine and restart the editor, then join again. "
                                                   "Leaving the session.").arg(Who),
                                    QStringLiteral("Collab"), BranchKey);
        Pending = Collab::BranchMove();
        Pushers.clear();
        Leave();
        emit PullAndReloadFinished(false);
        return;
    }

    /* The open level, with unsaved edits, changed upstream: ask first. */
    const bool bLevelChanged = !OpenLevel.isEmpty()
                            && std::binary_search(Pending.Paths.begin(), Pending.Paths.end(), ToStd(OpenLevel));
    if(bLevelChanged && bLevelUnsaved && Hooks.ConfirmDiscardLevel && !Hooks.ConfirmDiscardLevel())
    {
        EditorNotifications::Notify(ESeverity::Info, QStringLiteral("Pull & Reload put off"),
                                    QStringLiteral("Save or keep your edits; the offer is in the Notifications dock."),
                                    QStringLiteral("Collab"), QStringLiteral("collab.pull"));
        PostPending();
        emit PullAndReloadFinished(false);
        return;
    }

    bPulling = true;
    const Collab::BranchMove Pulling = Pending;
    const int Commits = int(Pulling.Commits.size());
    EditorNotifications::Notification Busy;
    Busy.Title      = QStringLiteral("Pulling %1 commit%2…").arg(Commits).arg(Commits == 1 ? QString() : QStringLiteral("s"));
    Busy.Source     = QStringLiteral("Collab");
    Busy.ReplaceKey = BranchKey;
    Busy.Progress   = EditorNotifications::Indeterminate;
    if(EditorNotifications* Notes = EditorNotifications::Get())
    {
        Notes->Post(std::move(Busy));
    }

    /* Local commits: a rebase (autostashed) rather than a merge. */
    Git->Status([this, Pulling, Groups, Who, bLevelChanged, Commits](const bool bInOk, const EditorGit::Status& InStatus, const QString&)
    {
        const bool bRebase = bInOk && InStatus.Branch.Ahead > 0;
        Git->Pull(bRebase, [this, Pulling, Groups, Who, bLevelChanged, Commits](const GitClient::Result& InResult)
        {
            bPulling = false;
            if(!InResult.Ok())
            {
                EditorNotifications::Notification Failed;
                Failed.Severity   = ESeverity::Error;
                Failed.Title      = QStringLiteral("Pull failed");
                Failed.Body       = InResult.Summary();
                Failed.Source     = QStringLiteral("Collab");
                Failed.ReplaceKey = BranchKey;
                Failed.Actions    = { { QStringLiteral("Try Again"), [this]() { PullAndReload(); } } };
                if(EditorNotifications* Notes = EditorNotifications::Get())
                {
                    Notes->Post(std::move(Failed));
                }
                emit PullAndReloadFinished(false);
                return;
            }

            /* What the pull brought in, reloaded. */
            QStringList Done;
            if(bLevelChanged && Hooks.ReloadLevel)
            {
                Hooks.ReloadLevel();
                Done << QStringLiteral("reloaded the level");
            }
            if(Groups.Has(EditorGit::EPathGroup::GameModule) && Hooks.CompileGameModule)
            {
                Hooks.CompileGameModule();
                Done << QStringLiteral("compiling the game module");
            }
            if(Groups.Has(EditorGit::EPathGroup::Config) && Hooks.ReloadConfig)
            {
                Hooks.ReloadConfig();
                Done << QStringLiteral("reloaded Config");
            }
            QStringList Files;
            for(const std::string& Path : Pulling.Paths)
            {
                Files << QDir::cleanPath(ToAbsolute(ToQ(Path)));
            }
            EditorContentEvents::Get().Announce(Files);

            /* Anything pushed meanwhile stays pending. */
            std::vector<Collab::CommitInfo> Left;
            for(const Collab::CommitInfo& Each : Pending.Commits)
            {
                if(std::none_of(Pulling.Commits.begin(), Pulling.Commits.end(),
                                [&Each](const Collab::CommitInfo& Pulled) { return Pulled.Oid == Each.Oid; }))
                {
                    Left.push_back(Each);
                }
            }
            const bool bMore = !Left.empty();
            if(bMore)
            {
                Pending.Commits.swap(Left);
            }
            else
            {
                Pending = Collab::BranchMove();
                Pushers.clear();
            }

            EditorNotifications::Notify(ESeverity::Success,
                                        QStringLiteral("Pulled %1 commit%2 from %3").arg(Commits).arg(Commits == 1 ? QString() : QStringLiteral("s")).arg(Who),
                                        Done.isEmpty() ? Groups.Describe() + QLatin1Char('.')
                                                       : Groups.Describe() + QStringLiteral(": ") + Done.join(QStringLiteral(", ")) + QLatin1Char('.'),
                                        QStringLiteral("Collab"), bMore ? QStringLiteral("collab.pulled") : BranchKey);
            if(bMore)
            {
                PostPending();
            }
            ReadDirtyFromGit();
            emit PullAndReloadFinished(true);
        });
    });
}

QString EditorCollabSession::ToRepoRelative(const QString& InPath) const
{
    if(RepoRoot.isEmpty())
    {
        return QDir::fromNativeSeparators(InPath);
    }
    return QDir(RepoRoot).relativeFilePath(QFileInfo(InPath).absoluteFilePath());
}

QString EditorCollabSession::ToAbsolute(const QString& InRepoPath) const
{
    return RepoRoot.isEmpty() ? InRepoPath : QDir(RepoRoot).filePath(InRepoPath);
}

#endif // WITH_EDITOR
