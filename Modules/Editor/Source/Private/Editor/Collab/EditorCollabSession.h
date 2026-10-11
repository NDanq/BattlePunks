#ifndef NIMBLEEDITOR_EDITORCOLLABSESSION_H
#define NIMBLEEDITOR_EDITORCOLLABSESSION_H

#if WITH_EDITOR

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

signals:
    void StateChanged();
    /* Someone joined, left or changed. */
    void RosterChanged();

private:
    void Poll();
    void Handle(const Collab::ClientEvent& InEvent);
    void PushPresence();
    void ReadDirtyFromGit();
    QString ToRepoRelative(const QString& InPath) const;
    QString ToAbsolute(const QString& InRepoPath) const;

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
