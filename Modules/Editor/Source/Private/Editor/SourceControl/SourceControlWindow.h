#ifndef NIMBLEEDITOR_SOURCECONTROLWINDOW_H
#define NIMBLEEDITOR_SOURCECONTROLWINDOW_H

#if WITH_EDITOR

#include <QDockWidget>
#include <QFileSystemWatcher>
#include <QHash>
#include <QString>
#include <QTimer>

#include "Private/Editor/SourceControl/GitStatus.h"

class GitClient;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

/*
 * The Source Control dock (View > Source Control): the project's git
 * repository, through GitClient.
 *
 * Left: the branch and its upstream, ahead/behind, Fetch / Pull / Push, the
 * commit message, and the changed files grouped by GitStatus's ClassifyPath --
 * Content, Scripts, GameModule source, Config, Engine (flagged: not supported
 * during a collaboration session), Other. A file's box stages or unstages it;
 * a group's, the whole group. Right: the selected file's text diff.
 *
 * Pull is fast-forward only; with local commits it offers a rebase instead
 * (autostashing local edits). Never a merge. Conflicts go to the user's own
 * merge tool (`git mergetool`), then are staged here.
 *
 * SESSION BRANCH. `[Collaboration] Branch=` in the project's config is the
 * branch a session works on; the dock warns when HEAD is elsewhere.
 *
 * Refreshes when .git/HEAD or .git/index change, when the world is saved
 * (RequestRefresh), and when the editor comes back to the front; 500 ms after
 * the last of them.
 */
class SourceControlWindow : public QDockWidget
{
    Q_OBJECT

public:
    /* InProjectRoot: the game's folder (EditorGraphs::GetProjectRoot);
     * InConfigPath: its DefaultEngine.ini, for the session branch. */
    SourceControlWindow(const QString& InProjectRoot, const QString& InConfigPath, QWidget* InParent);
    ~SourceControlWindow() override;

    /* Within 500 ms. */
    void RequestRefresh();
    /* Now; Refreshed follows. */
    void RefreshNow();

    /* Null outside a repository. */
    GitClient* GetGit() const { return Git; }

    /* The game's folder, repository-relative ("Games/WorldsLab"). */
    const QString& GetGameDir() const { return GameDir; }

    const EditorGit::Status& GetStatus() const { return Last; }

    QString GetSessionBranch() const { return SessionBranch; }
    /* Written to the project's config. */
    void SetSessionBranch(const QString& InBranch);

    // --- What the buttons do, for tests ---------------------------------------
    void StagePaths(const QStringList& InPaths);
    void UnstagePaths(const QStringList& InPaths);
    void CommitStaged(const QString& InMessage);
    void FetchRemote();
    void PullRemote();
    void PushRemote();
    /* Its diff into the right pane. */
    void ShowDiff(const QString& InPath);

    QTreeWidget*    GetTree() const { return Tree; }
    QPlainTextEdit* GetDiffView() const { return DiffView; }
    QPlainTextEdit* GetMessageEdit() const { return Message; }
    QLabel*         GetSessionBanner() const { return SessionBanner; }
    QWidget*        GetConflictBanner() const { return ConflictBanner; }
    QPushButton*    GetPullButton() const { return PullButton; }

signals:
    /* A status read finished; bInOk false if git failed. */
    void Refreshed(bool bInOk);

    /* Push moved InBranch's upstream from InOldOid (empty: it had none) to
     * InNewOid: the collaboration session tells the others. */
    void Pushed(const QString& InBranch, const QString& InOldOid, const QString& InNewOid);

private:
    void BuildUi();
    void WatchGitFolder();
    void Apply(const EditorGit::Status& InStatus);
    void Populate();
    void UpdateHeader();
    void UpdateBanners();
    void OnItemChanged(QTreeWidgetItem* InItem, int InColumn);
    void OnSelectionChanged();
    void FillSessionMenu();
    void OpenMergeTool();
    void FetchLocks();
    void SetBusy(bool bInBusy);

    const EditorGit::FileEntry* FindEntry(const QString& InPath) const;
    /* Notifies InTitle, with git's reason, if InResult failed; true if it didn't. */
    bool ReportFailure(const QString& InTitle, bool bInOk, const QString& InSummary, const QString& InKey);

    GitClient*        Git = nullptr;
    QString           ProjectRoot;
    QString           ConfigPath;
    QString           GameDir;
    QString           SessionBranch;
    EditorGit::Status Last;
    bool              bHaveStatus = false;
    bool              bPopulating = false;
    QString           DiffPath;
    QHash<QString, QString> LockOwners;   // LFS: path -> who holds it

    QTimer             RefreshTimer;
    QFileSystemWatcher Watcher;

    QLabel*         BranchLabel  = nullptr;
    QLabel*         AheadBehind  = nullptr;
    QLabel*         LfsLabel     = nullptr;
    QToolButton*    SessionButton = nullptr;
    QPushButton*    FetchButton  = nullptr;
    QPushButton*    PullButton   = nullptr;
    QPushButton*    PushButton   = nullptr;
    QToolButton*    RefreshButton = nullptr;
    QLabel*         SessionBanner = nullptr;
    QWidget*        ConflictBanner = nullptr;
    QLabel*         ConflictText = nullptr;
    QPlainTextEdit* Message      = nullptr;
    QLabel*         StagedLabel  = nullptr;
    QPushButton*    CommitButton = nullptr;
    QTreeWidget*    Tree         = nullptr;
    QLabel*         DiffTitle    = nullptr;
    QPushButton*    DiffStageButton = nullptr;
    QPlainTextEdit* DiffView     = nullptr;
    QLabel*         NoRepoLabel  = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SOURCECONTROLWINDOW_H
