#if WITH_EDITOR

#include "Private/Editor/Collab/EditorCollabSession.h"

#include <QDir>
#include <QFileInfo>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/SourceControl/GitClient.h"
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
        Git->Run({ QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("user.name") },
                 [Found, Text](const GitClient::Result& InResult) { Found->User = Text(InResult); });
    }
    Git->Run({ QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin") },
             [Found, Text](const GitClient::Result& InResult) { Found->Remote = Text(InResult); });
    Git->Run({ QStringLiteral("rev-parse"), QStringLiteral("HEAD") },
             [Found, Text](const GitClient::Result& InResult) { Found->Head = Text(InResult); });
    Git->Run({ QStringLiteral("rev-parse"), QStringLiteral("HEAD:Engine") },
             [Found, Text](const GitClient::Result& InResult) { Found->EngineTree = InResult.Ok() ? Text(InResult) : QString(); });
    Git->Run({ QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"), QStringLiteral("HEAD") },
             [Found, Text](const GitClient::Result& InResult) { Found->CurrentBranch = Text(InResult); });
    Git->Run({ QStringLiteral("diff"), QStringLiteral("--quiet"), QStringLiteral("HEAD"), QStringLiteral("--"), QStringLiteral("Engine") },
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
        case EType::Notified:
            EditorNotifications::Notify(ToSeverity(InEvent.Severity), ToQ(InEvent.Title), ToQ(InEvent.Text), QStringLiteral("Collab"));
            break;
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
    Client.SetPresence(ToStd(Head), ToStd(OpenLevel),
                       bPlaying ? Collab::EPlayState::Playing : Collab::EPlayState::Editing, std::move(Dirty));
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
