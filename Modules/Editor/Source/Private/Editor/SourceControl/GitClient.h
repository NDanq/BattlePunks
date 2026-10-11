#ifndef NIMBLEEDITOR_GITCLIENT_H
#define NIMBLEEDITOR_GITCLIENT_H

#if WITH_EDITOR

#include <deque>
#include <functional>

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include "Private/Editor/SourceControl/GitStatus.h"

/*
 * The `git` command line for the Source Control dock, modelled on SourceBuild:
 * one QProcess at a time, the rest queued in order, each answered on the GUI
 * thread.
 *
 * NO PROMPTS. Every command runs with GIT_TERMINAL_PROMPT=0, so a missing
 * credential fails instead of waiting on a console nobody sees.
 * Authentication is the user's credential helper's (Git Credential Manager
 * on Windows); the editor never sees a password. Commit messages go in on
 * stdin (`git commit -F -`), never on the command line.
 */
class GitClient : public QObject
{
    Q_OBJECT

public:
    struct Result
    {
        bool       bStarted = false;   // false: git isn't installed, or the folder is gone
        int        ExitCode = -1;
        QByteArray Out;
        QString    Err;               // trimmed

        bool Ok() const { return bStarted && ExitCode == 0; }

        /* The first line of stderr, else of stdout: for a notification. */
        QString Summary() const;
    };
    using Callback = std::function<void(const Result&)>;

    explicit GitClient(const QString& InRepositoryRoot, QObject* InParent = nullptr);
    ~GitClient() override;   // kills a command still running; queued ones are dropped

    /* The folder holding .git (a directory, or a worktree's file) at or above
     * InDir; empty if none. */
    static QString FindRepositoryRoot(const QString& InDir);

    /* The git on PATH. */
    static QString GetGitCommand();

    const QString& GetRoot() const { return Root; }

    /* Queues `git <InArgs>` in the repository. InCallback (may be null) runs
     * when it ends; InStdin is written then closed. */
    void Run(const QStringList& InArgs, Callback InCallback, const QByteArray& InStdin = QByteArray());

    bool IsBusy() const { return Process != nullptr || !Queue.empty(); }

    /* Tests: pumps events until the queue is empty, or InTimeoutMs passes. */
    bool WaitForIdle(int InTimeoutMs = 30000);

    // --- The dock's operations ------------------------------------------------

    /* `status --porcelain=v2 --branch -z`; OutStatus empty with false on failure. */
    void Status(std::function<void(bool bInOk, const EditorGit::Status& InStatus, const QString& InError)> InCallback);

    /* `add -A` (deletions too); empty does nothing. */
    void Stage(const QStringList& InPaths, Callback InCallback);

    /* Back out of the index; bInInitial before the first commit (no HEAD). */
    void Unstage(const QStringList& InPaths, bool bInInitial, Callback InCallback);

    void Commit(const QString& InMessage, Callback InCallback);
    void Fetch(Callback InCallback);

    /* Fast-forward only, or rebase local commits onto the upstream (local
     * edits autostashed). Never a merge. */
    void Pull(bool bInRebase, Callback InCallback);

    /* With no upstream yet, sets origin's branch of the same name as it. */
    void Push(bool bInSetUpstream, Callback InCallback);

    /* InPath's diff, staged or in the working tree, as text. */
    void Diff(const QString& InPath, bool bInStaged, Callback InCallback);

    /* Local branches, current first is not guaranteed. */
    void Branches(std::function<void(const QStringList& InBranches)> InCallback);

    /* `switch`, creating it from HEAD if bInCreate. */
    void SwitchBranch(const QString& InBranch, bool bInCreate, Callback InCallback);

    /* Paths with `filter=lfs` in .gitattributes, roughly: whether to look for locks. */
    bool UsesLfs() const;

signals:
    void BusyChanged(bool bInBusy);

private:
    struct Job
    {
        QStringList Args;
        QByteArray  Stdin;
        Callback    Done;
    };

    void StartNext();
    void OnFinished(int InExitCode, QProcess::ExitStatus InStatus);

    QString          Root;
    std::deque<Job>  Queue;
    QProcess*        Process = nullptr;
    Job              Current;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GITCLIENT_H
