#ifndef NIMBLEEDITOR_EDITORCOLLABSESSION_H
#define NIMBLEEDITOR_EDITORCOLLABSESSION_H

#if WITH_EDITOR

#include <functional>
#include <memory>

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "Public/Collab/CollabClient.h"

class GitClient;

/*
 * This editor in a collaboration session (Modules/Editor/COLLABORATION.md, P2):
 * Collab's CollabClient, on the GUI thread, fed with what git and the editor
 * know and turned into notifications and signals for the Session dock and the
 * Content Browser.
 *
 * CONNECTING. Connect() first asks git who this editor is -- origin's URL,
 * HEAD, `rev-parse HEAD:Engine` and whether Engine/ has uncommitted changes --
 * then the server. The project is `[Game] Module` plus origin's normalized URL;
 * the branch is the session branch (Source Control's `[Collaboration] Branch`),
 * else the current one.
 *
 * PRESENCE. The open level, whether Play is running, and the dirty paths:
 * the files git sees changed (saved but not pushed), plus the open level while
 * it has unsaved edits. Re-read from git 5 s after anything changes, and every
 * 15 s.
 *
 * GUI thread only.
 */
class EditorCollabSession : public QObject
{
    Q_OBJECT

public:
    /* InProjectRoot: the game's folder; InConfigPath: its DefaultEngine.ini. */
    EditorCollabSession(const QString& InProjectRoot, const QString& InConfigPath, QObject* InParent = nullptr);
    ~EditorCollabSession() override;

    /* "host[:port]"; InUser empty: git's user.name, else the OS's. Notifies if
     * this isn't a git repository. */
    void Connect(const QString& InAddress, const QString& InUser, const QString& InPassword);

    /* With an identity made elsewhere: the tests, which have no repository. */
    void ConnectAs(const QString& InHost, uint16_t InPort, const Collab::ClientIdentity& InIdentity);

    void Leave();

    Collab::CollabClient::EState GetState() const { return Client.GetState(); }
    bool IsJoined() const { return Client.IsJoined(); }
    const Collab::CollabClient& GetClient() const { return Client; }
    QString GetServerAddress() const { return ServerAddress; }

    /* The repository's root, for turning paths to and from repository-relative. */
    QString GetRepositoryRoot() const { return RepoRoot; }

    // --- What this editor is doing ----------------------------------------------
    /* Absolute paths; empty for none. */
    void SetOpenLevel(const QString& InPath);
    void SetLevelUnsaved(bool bInUnsaved);
    void SetPlaying(bool bInPlaying);
    /* Re-read git's changed files soon (a save). */
    void RequestDirtyRefresh();

    /* Absolute path -> who else has it dirty. */
    QHash<QString, QStringList> GetOthersDirty() const;

    /* "user (branch @ abc1234)" lines, for collab.status. */
    QStringList DescribeRoster() const;

    // --- The branch moving (P3) -------------------------------------------------
    /*
     * This editor pushed (Source Control's Pushed): the others are told what
     * moved, with its commits and changed paths read from git.
     */
    void AnnouncePush(const QString& InBranch, const QString& InOldOid, const QString& InNewOid);

    /* What Pull & Reload does after the pull, the main window's to provide. */
    struct ReloadHooks
    {
        /* The open level changed upstream and has unsaved edits: reload anyway? */
        std::function<bool()> ConfirmDiscardLevel;
        std::function<void()> ReloadLevel;
        /* Compile Source; its own toast reports, and it reloads the module. */
        std::function<void()> CompileGameModule;
        std::function<void()> ReloadConfig;
    };
    void SetReloadHooks(ReloadHooks InHooks) { Hooks = std::move(InHooks); }

    /*
     * Everything others pushed since the last pull, as one toast's Pull & Reload:
     * pull (fast-forward, or a rebase over local commits), then reload the
     * open level if it changed (asking first if it has unsaved edits), compile
     * the game module if its source changed, reload Config, and announce the
     * rest (EditorContentEvents). Engine changes are never pulled: the editor
     * says a rebuild and restart are needed, and leaves the session.
     */
    void PullAndReload();

    /* Pushed by others and not pulled yet: repository-relative paths, commits. */
    QStringList GetPendingPaths() const;
    int GetPendingCommitCount() const { return int(Pending.Commits.size()); }

signals:
    void StateChanged();
    /* Someone joined, left or changed. */
    void RosterChanged();
    /* Pull & Reload ended; bInPulled false if it didn't pull. */
    void PullAndReloadFinished(bool bInPulled);
    /* The server's build of the session branch moved on (GetClient().GetBuildStatus()). */
    void BuildChanged();

private:
    void Poll();
    void Handle(const Collab::ClientEvent& InEvent);
    void PushPresence();
    void ReadDirtyFromGit();
    QString ToRepoRelative(const QString& InPath) const;
    QString ToAbsolute(const QString& InRepoPath) const;
    void OnBranchAdvanced(const Collab::ClientEvent& InEvent);
    void OnBuildStatus(const Collab::BuildStatusMessage& InStatus);
    void OnValidation(const Collab::ValidationReportMessage& InReport);
    void PostPending();
    void ClearPending();

    /* Others' pushes since the last pull, merged. */
    Collab::BranchMove Pending;
    QStringList        Pushers;
    bool               bPulling = false;
    ReloadHooks        Hooks;
    QString            SessionBranch;
    QString            GameDir;     // the project, repository-relative

    Collab::CollabClient Client;
    GitClient*  Git = nullptr;
    QString     ProjectRoot;
    QString     ConfigPath;
    QString     RepoRoot;
    QString     ServerAddress;
    bool        bConnecting = false;   // asking git, before the client connects

    QString     OpenLevel;          // repository-relative
    bool        bLevelUnsaved = false;
    bool        bPlaying      = false;
    QString     Head;
    QStringList GitDirty;           // repository-relative

    QTimer PollTimer;
    QTimer DirtyTimer;
    QTimer DirtyHeartbeat;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORCOLLABSESSION_H
