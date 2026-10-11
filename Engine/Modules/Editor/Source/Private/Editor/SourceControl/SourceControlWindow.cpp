#if WITH_EDITOR

#include "Private/Editor/SourceControl/SourceControlWindow.h"

#include <memory>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/EditorIcons.h"
#include "Private/Editor/SourceControl/GitClient.h"
#include "Public/Editor/EditorNotifications.h"

using EditorGit::EEntryKind;
using EditorGit::EPathGroup;
using EditorGit::FileEntry;
using ESeverity = EditorNotifications::ESeverity;

namespace
{
    constexpr int PathRole  = Qt::UserRole;       // a file row's repository path
    constexpr int GroupRole = Qt::UserRole + 1;   // a group row's EPathGroup

    constexpr qint64 MaxUntrackedBytes = 256 * 1024;

    const QString SessionSection = QStringLiteral("Collaboration");
    const QString SessionKey     = QStringLiteral("Branch");

    QColor Amber() { return QColor(230, 170, 60); }
    QColor Red()   { return QColor(225, 85, 85); }
    QColor Green() { return QColor(90, 200, 110); }

    QColor StatusColor(const FileEntry& InEntry)
    {
        switch(InEntry.DisplayStatus())
        {
            case 'A': case '?': return Green();
            case 'D': case 'U': return Red();
            case 'R': case 'C': return QApplication::palette().color(QPalette::Highlight);
            default:            return Amber();
        }
    }

    /* + green, - red, @@ dim: the diff pane. */
    class DiffHighlighter final : public QSyntaxHighlighter
    {
    public:
        using QSyntaxHighlighter::QSyntaxHighlighter;

    protected:
        void highlightBlock(const QString& InText) override
        {
            QTextCharFormat Format;
            if(InText.startsWith(QStringLiteral("+++")) || InText.startsWith(QStringLiteral("---"))
               || InText.startsWith(QStringLiteral("diff ")) || InText.startsWith(QStringLiteral("index ")))
            {
                Format.setForeground(QApplication::palette().color(QPalette::PlaceholderText));
            }
            else if(InText.startsWith(QLatin1Char('+')))
            {
                Format.setForeground(Green());
            }
            else if(InText.startsWith(QLatin1Char('-')))
            {
                Format.setForeground(Red());
            }
            else if(InText.startsWith(QStringLiteral("@@")))
            {
                Format.setForeground(QApplication::palette().color(QPalette::Highlight));
            }
            else
            {
                return;
            }
            setFormat(0, int(InText.size()), Format);
        }
    };

    QString ToQ(const std::string& InText)
    {
        return QString::fromUtf8(InText.c_str(), int(InText.size()));
    }

    QPushButton* SmallButton(const QString& InText, const EditorIcons::EIcon InIcon, QWidget* InParent)
    {
        auto* Button = new QPushButton(EditorIcons::Get(InIcon), InText, InParent);
        Button->setIconSize(QSize(14, 14));
        Button->setAutoDefault(false);
        return Button;
    }

    void PostProgress(const QString& InTitle, const QString& InKey)
    {
        EditorNotifications::Notification Busy;
        Busy.Title      = InTitle;
        Busy.Source     = QStringLiteral("Git");
        Busy.ReplaceKey = InKey;
        Busy.Progress   = EditorNotifications::Indeterminate;
        if(EditorNotifications* Notifications = EditorNotifications::Get())
        {
            Notifications->Post(std::move(Busy));
        }
    }
}

SourceControlWindow::SourceControlWindow(const QString& InProjectRoot, const QString& InConfigPath, QWidget* InParent)
    : QDockWidget(QStringLiteral("Source Control"), InParent)
    , ProjectRoot(QDir(InProjectRoot).absolutePath())
    , ConfigPath(InConfigPath)
{
    setObjectName(QStringLiteral("SourceControlDock"));

    const QString Root = GitClient::FindRepositoryRoot(ProjectRoot);
    if(!Root.isEmpty())
    {
        Git = new GitClient(Root, this);
        GameDir = QDir(Root).relativeFilePath(ProjectRoot);
        if(GameDir == QStringLiteral("."))
        {
            GameDir.clear();
        }
        connect(Git, &GitClient::BusyChanged, this, &SourceControlWindow::SetBusy);
    }

    ConfigIniFile Config;
    if(!ConfigPath.isEmpty() && Config.Load(ConfigPath))
    {
        SessionBranch = Config.Get(SessionSection, SessionKey);
    }

    BuildUi();

    RefreshTimer.setSingleShot(true);
    RefreshTimer.setInterval(500);
    connect(&RefreshTimer, &QTimer::timeout, this, &SourceControlWindow::RefreshNow);

    if(Git)
    {
        WatchGitFolder();
        connect(&Watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& InPath)
        {
            /* git replaces the index rather than writing it: watch the new file. */
            if(!Watcher.files().contains(InPath) && QFileInfo::exists(InPath))
            {
                Watcher.addPath(InPath);
            }
            RequestRefresh();
        });
        connect(&Watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) { WatchGitFolder(); RequestRefresh(); });
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](const Qt::ApplicationState InState)
        {
            if(InState == Qt::ApplicationActive && isVisible())
            {
                RequestRefresh();
            }
        });
        connect(this, &QDockWidget::visibilityChanged, this, [this](const bool bInVisible)
        {
            if(bInVisible)
            {
                RequestRefresh();
            }
        });
    }
}

SourceControlWindow::~SourceControlWindow() = default;

void SourceControlWindow::BuildUi()
{
    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->setChildrenCollapsible(false);

    // --- Left: branch, sync, commit, files ---------------------------------------
    auto* Left = new QWidget(Split);
    auto* Column = new QVBoxLayout(Left);
    Column->setContentsMargins(6, 6, 6, 6);
    Column->setSpacing(6);

    auto* BranchRow = new QHBoxLayout();
    auto* BranchIcon = new QLabel(Left);
    BranchIcon->setPixmap(EditorIcons::Get(EditorIcons::EIcon::Branch).pixmap(QSize(16, 16), devicePixelRatioF()));
    BranchRow->addWidget(BranchIcon);
    BranchLabel = new QLabel(Left);
    BranchLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    BranchRow->addWidget(BranchLabel, 1);
    SessionButton = new QToolButton(Left);
    SessionButton->setText(QStringLiteral("Session Branch"));
    SessionButton->setPopupMode(QToolButton::InstantPopup);
    SessionButton->setToolTip(QStringLiteral("The branch a collaboration session works on ([Collaboration] Branch in the project's config)"));
    auto* SessionMenu = new QMenu(SessionButton);
    SessionButton->setMenu(SessionMenu);
    connect(SessionMenu, &QMenu::aboutToShow, this, &SourceControlWindow::FillSessionMenu);
    BranchRow->addWidget(SessionButton);
    Column->addLayout(BranchRow);

    auto* InfoRow = new QHBoxLayout();
    AheadBehind = new QLabel(Left);
    InfoRow->addWidget(AheadBehind);
    InfoRow->addStretch(1);
    LfsLabel = new QLabel(QStringLiteral("LFS"), Left);
    LfsLabel->setToolTip(QStringLiteral("Git LFS is set up for this repository (.gitattributes)"));
    LfsLabel->setForegroundRole(QPalette::PlaceholderText);
    InfoRow->addWidget(LfsLabel);
    Column->addLayout(InfoRow);

    auto* SyncRow = new QHBoxLayout();
    FetchButton = SmallButton(QStringLiteral("Fetch"), EditorIcons::EIcon::Fetch, Left);
    FetchButton->setToolTip(QStringLiteral("Download the remote's new commits without changing your files"));
    PullButton = SmallButton(QStringLiteral("Pull"), EditorIcons::EIcon::Behind, Left);
    PushButton = SmallButton(QStringLiteral("Push"), EditorIcons::EIcon::Push, Left);
    PushButton->setToolTip(QStringLiteral("Upload your commits to the remote"));
    RefreshButton = new QToolButton(Left);
    RefreshButton->setIcon(EditorIcons::Get(EditorIcons::EIcon::Refresh));
    RefreshButton->setAutoRaise(true);
    RefreshButton->setToolTip(QStringLiteral("Read the status again"));
    connect(FetchButton, &QPushButton::clicked, this, &SourceControlWindow::FetchRemote);
    connect(PullButton, &QPushButton::clicked, this, &SourceControlWindow::PullRemote);
    connect(PushButton, &QPushButton::clicked, this, &SourceControlWindow::PushRemote);
    connect(RefreshButton, &QToolButton::clicked, this, &SourceControlWindow::RefreshNow);
    SyncRow->addWidget(FetchButton);
    SyncRow->addWidget(PullButton);
    SyncRow->addWidget(PushButton);
    SyncRow->addStretch(1);
    SyncRow->addWidget(RefreshButton);
    Column->addLayout(SyncRow);

    SessionBanner = new QLabel(Left);
    SessionBanner->setWordWrap(true);
    SessionBanner->setTextFormat(Qt::RichText);
    SessionBanner->setStyleSheet(QStringLiteral("QLabel { border: 1px solid %1; border-radius: 4px; padding: 6px; color: %1; }").arg(Amber().name()));
    connect(SessionBanner, &QLabel::linkActivated, this, [this](const QString&)
    {
        if(!Git || SessionBranch.isEmpty())
        {
            return;
        }
        Git->SwitchBranch(SessionBranch, false, [this](const GitClient::Result& InResult)
        {
            ReportFailure(QStringLiteral("Couldn't switch to %1").arg(SessionBranch), InResult.Ok(), InResult.Summary(), QStringLiteral("git.switch"));
            RefreshNow();
        });
    });
    SessionBanner->hide();
    Column->addWidget(SessionBanner);

    ConflictBanner = new QWidget(Left);
    ConflictBanner->setObjectName(QStringLiteral("ConflictBanner"));
    ConflictBanner->setAttribute(Qt::WA_StyledBackground);
    ConflictBanner->setStyleSheet(QStringLiteral("QWidget#ConflictBanner { border: 1px solid %1; border-radius: 4px; }").arg(Red().name()));
    auto* ConflictRow = new QHBoxLayout(ConflictBanner);
    ConflictRow->setContentsMargins(6, 4, 6, 4);
    auto* MergeIcon = new QLabel(ConflictBanner);
    MergeIcon->setPixmap(EditorIcons::Get(EditorIcons::EIcon::Merge, Red()).pixmap(QSize(16, 16), devicePixelRatioF()));
    ConflictRow->addWidget(MergeIcon, 0, Qt::AlignTop);
    ConflictText = new QLabel(ConflictBanner);
    ConflictText->setWordWrap(true);
    ConflictRow->addWidget(ConflictText, 1);
    auto* MergeButton = new QPushButton(QStringLiteral("Open Merge Tool"), ConflictBanner);
    MergeButton->setAutoDefault(false);
    MergeButton->setToolTip(QStringLiteral("git mergetool, on the selected conflict or all of them"));
    connect(MergeButton, &QPushButton::clicked, this, &SourceControlWindow::OpenMergeTool);
    ConflictRow->addWidget(MergeButton, 0, Qt::AlignTop);
    ConflictBanner->hide();
    Column->addWidget(ConflictBanner);

    Message = new QPlainTextEdit(Left);
    Message->setPlaceholderText(QStringLiteral("Add pickups to the forest clearing"));
    Message->setTabChangesFocus(true);
    Message->setMaximumHeight(72);
    Column->addWidget(Message);

    auto* CommitRow = new QHBoxLayout();
    StagedLabel = new QLabel(Left);
    StagedLabel->setForegroundRole(QPalette::PlaceholderText);
    CommitRow->addWidget(StagedLabel, 1);
    CommitButton = SmallButton(QStringLiteral("Commit"), EditorIcons::EIcon::Commit, Left);
    CommitButton->setToolTip(QStringLiteral("Commit the staged files with this message (Ctrl+Enter)"));
    connect(CommitButton, &QPushButton::clicked, this, [this]() { CommitStaged(Message->toPlainText()); });
    connect(Message, &QPlainTextEdit::textChanged, this, &SourceControlWindow::UpdateHeader);
    CommitRow->addWidget(CommitButton);
    Column->addLayout(CommitRow);

    Tree = new QTreeWidget(Left);
    Tree->setObjectName(QStringLiteral("SourceControlTree"));
    Tree->setColumnCount(2);
    Tree->setHeaderHidden(true);
    Tree->setRootIsDecorated(true);
    Tree->setUniformRowHeights(true);
    Tree->header()->setStretchLastSection(false);
    Tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    Tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(Tree, &QTreeWidget::itemChanged, this, &SourceControlWindow::OnItemChanged);
    connect(Tree, &QTreeWidget::itemSelectionChanged, this, &SourceControlWindow::OnSelectionChanged);
    Column->addWidget(Tree, 1);

    NoRepoLabel = new QLabel(QStringLiteral("This project isn't in a git repository."), Left);
    NoRepoLabel->setWordWrap(true);
    NoRepoLabel->setForegroundRole(QPalette::PlaceholderText);
    Column->addWidget(NoRepoLabel);

    // --- Right: the diff ------------------------------------------------------------
    auto* Right = new QWidget(Split);
    auto* DiffColumn = new QVBoxLayout(Right);
    DiffColumn->setContentsMargins(6, 6, 6, 6);
    DiffColumn->setSpacing(6);
    auto* DiffRow = new QHBoxLayout();
    DiffTitle = new QLabel(QStringLiteral("Select a file to see its changes."), Right);
    DiffTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    DiffRow->addWidget(DiffTitle, 1);
    DiffStageButton = new QPushButton(Right);
    DiffStageButton->setAutoDefault(false);
    DiffStageButton->hide();
    connect(DiffStageButton, &QPushButton::clicked, this, [this]()
    {
        const FileEntry* Entry = FindEntry(DiffPath);
        if(!Entry)
        {
            return;
        }
        if(Entry->IsUnstaged())
        {
            StagePaths({ DiffPath });
        }
        else
        {
            UnstagePaths({ DiffPath });
        }
    });
    DiffRow->addWidget(DiffStageButton);
    DiffColumn->addLayout(DiffRow);
    DiffView = new QPlainTextEdit(Right);
    DiffView->setObjectName(QStringLiteral("SourceControlDiff"));
    DiffView->setReadOnly(true);
    DiffView->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont Mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    DiffView->setFont(Mono);
    new DiffHighlighter(DiffView->document());
    DiffColumn->addWidget(DiffView, 1);

    Split->setStretchFactor(0, 2);
    Split->setStretchFactor(1, 3);
    setWidget(Split);

    /* Ctrl+Enter in the message commits. */
    auto* CommitShortcut = new QAction(Message);
    CommitShortcut->setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter) });
    CommitShortcut->setShortcutContext(Qt::WidgetShortcut);
    connect(CommitShortcut, &QAction::triggered, CommitButton, &QPushButton::click);
    Message->addAction(CommitShortcut);

    const bool bRepo = Git != nullptr;
    for(QWidget* Each : std::initializer_list<QWidget*>{ FetchButton, PullButton, PushButton, RefreshButton, Message, CommitButton, Tree,
                                                         SessionButton, Right })
    {
        Each->setVisible(bRepo);
    }
    LfsLabel->setVisible(bRepo && Git->UsesLfs());
    NoRepoLabel->setVisible(!bRepo);
    UpdateHeader();
}

void SourceControlWindow::WatchGitFolder()
{
    const QString GitPath = QDir(Git->GetRoot()).filePath(QStringLiteral(".git"));
    if(!QFileInfo(GitPath).isDir())
    {
        return;   // a worktree's .git file: the editor's own saves and focus still refresh
    }
    QStringList Wanted = { GitPath };
    for(const QString& Name : { QStringLiteral("HEAD"), QStringLiteral("index") })
    {
        const QString File = QDir(GitPath).filePath(Name);
        if(QFileInfo::exists(File))
        {
            Wanted << File;
        }
    }
    for(const QString& Each : Wanted)
    {
        if(!Watcher.files().contains(Each) && !Watcher.directories().contains(Each))
        {
            Watcher.addPath(Each);
        }
    }
}

void SourceControlWindow::RequestRefresh()
{
    if(Git)
    {
        RefreshTimer.start();
    }
}

void SourceControlWindow::RefreshNow()
{
    RefreshTimer.stop();
    if(!Git)
    {
        emit Refreshed(false);
        return;
    }
    Git->Status([this](const bool bInOk, const EditorGit::Status& InStatus, const QString& InError)
    {
        if(bInOk)
        {
            Apply(InStatus);
        }
        else
        {
            BranchLabel->setText(QStringLiteral("git status failed: %1").arg(InError));
        }
        emit Refreshed(bInOk);
    });
}

void SourceControlWindow::Apply(const EditorGit::Status& InStatus)
{
    Last        = InStatus;
    bHaveStatus = true;
    Populate();
    UpdateHeader();
    UpdateBanners();

    /* The diff pane follows the file, or empties if it was committed away. */
    if(!DiffPath.isEmpty())
    {
        if(FindEntry(DiffPath))
        {
            ShowDiff(DiffPath);
        }
        else
        {
            DiffPath.clear();
            DiffTitle->setText(QStringLiteral("Select a file to see its changes."));
            DiffStageButton->hide();
            DiffView->clear();
        }
    }
}

void SourceControlWindow::Populate()
{
    const QSignalBlocker Block(Tree);
    bPopulating = true;

    /* Keep what was open and selected. */
    QSet<int> Collapsed;
    for(int Index = 0; Index < Tree->topLevelItemCount(); ++Index)
    {
        if(!Tree->topLevelItem(Index)->isExpanded())
        {
            Collapsed.insert(Tree->topLevelItem(Index)->data(0, GroupRole).toInt());
        }
    }
    QString Selected;
    if(QTreeWidgetItem* Current = Tree->currentItem())
    {
        Selected = Current->data(0, PathRole).toString();
    }
    Tree->clear();

    QTreeWidgetItem* Groups[size_t(EPathGroup::Count)] = {};
    QTreeWidgetItem* SelectItem = nullptr;
    for(const FileEntry& Entry : Last.Entries)
    {
        if(Entry.Kind == EEntryKind::Ignored)
        {
            continue;
        }
        const EPathGroup Group = EditorGit::ClassifyPath(Entry.Path, GameDir.toStdString());
        QTreeWidgetItem*& GroupItem = Groups[size_t(Group)];
        if(!GroupItem)
        {
            GroupItem = new QTreeWidgetItem();
            GroupItem->setData(0, GroupRole, int(Group));
            GroupItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            QFont Bold = Tree->font();
            Bold.setBold(true);
            GroupItem->setFont(0, Bold);
        }

        const QString Path = ToQ(Entry.Path);
        auto* Item = new QTreeWidgetItem(GroupItem);
        Item->setData(0, PathRole, Path);
        Item->setText(0, QFileInfo(Path).fileName());
        Item->setText(1, QString(QLatin1Char(Entry.DisplayStatus())));
        Item->setForeground(1, StatusColor(Entry));
        Item->setTextAlignment(1, Qt::AlignCenter);
        Item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        Item->setCheckState(0, Entry.IsStaged() ? (Entry.IsUnstaged() ? Qt::PartiallyChecked : Qt::Checked) : Qt::Unchecked);

        QString Tip = Path;
        if(Entry.Kind == EEntryKind::Renamed)
        {
            Item->setText(0, QFileInfo(ToQ(Entry.OrigPath)).fileName() + QStringLiteral(" → ") + QFileInfo(Path).fileName());
            Tip += QStringLiteral("\nrenamed from ") + ToQ(Entry.OrigPath);
        }
        if(Entry.IsConflicted())
        {
            Tip += QStringLiteral("\nconflicted: resolve it in your merge tool, then stage it");
        }
        if(const auto Lock = LockOwners.constFind(Path); Lock != LockOwners.constEnd())
        {
            Item->setIcon(0, EditorIcons::Get(EditorIcons::EIcon::Lock));
            Tip += QStringLiteral("\nLFS lock held by ") + *Lock;
        }
        if(Group == EPathGroup::Engine)
        {
            Item->setForeground(0, Amber());
            Item->setIcon(0, EditorIcons::Get(EditorIcons::EIcon::Warning, Amber()));
        }
        Item->setToolTip(0, Tip);
        if(Path == Selected)
        {
            SelectItem = Item;
        }
    }

    for(size_t Index = 0; Index < size_t(EPathGroup::Count); ++Index)
    {
        QTreeWidgetItem* GroupItem = Groups[Index];
        if(!GroupItem)
        {
            continue;
        }
        int Staged = 0, Partial = 0;
        for(int Child = 0; Child < GroupItem->childCount(); ++Child)
        {
            const Qt::CheckState State = GroupItem->child(Child)->checkState(0);
            Staged  += State == Qt::Checked ? 1 : 0;
            Partial += State == Qt::PartiallyChecked ? 1 : 0;
        }
        GroupItem->setCheckState(0, Staged == GroupItem->childCount() ? Qt::Checked
                                    : Staged + Partial > 0             ? Qt::PartiallyChecked
                                                                       : Qt::Unchecked);
        const EPathGroup Group = EPathGroup(Index);
        QString Label = QStringLiteral("%1  (%2)").arg(QString::fromLatin1(EditorGit::GetGroupName(Group))).arg(GroupItem->childCount());
        if(Group == EPathGroup::Engine)
        {
            Label += QStringLiteral(" — not supported in a session");
            GroupItem->setForeground(0, Amber());
            GroupItem->setToolTip(0, QStringLiteral("Engine changes can't be shared during a collaboration session: "
                                                    "everyone must run the same engine"));
        }
        GroupItem->setText(0, Label);
        Tree->addTopLevelItem(GroupItem);
        GroupItem->setExpanded(!Collapsed.contains(int(Group)));
    }

    if(SelectItem)
    {
        Tree->setCurrentItem(SelectItem);
    }
    bPopulating = false;
}

void SourceControlWindow::UpdateHeader()
{
    if(!Git)
    {
        BranchLabel->setText(QStringLiteral("No repository"));
        AheadBehind->clear();
        return;
    }
    const EditorGit::BranchInfo& Branch = Last.Branch;
    if(!bHaveStatus)
    {
        BranchLabel->setText(QStringLiteral("Reading status…"));
    }
    else if(Branch.bDetached)
    {
        BranchLabel->setText(QStringLiteral("<b>detached</b> at %1").arg(ToQ(Branch.Oid).left(8)));
    }
    else
    {
        QString Text = QStringLiteral("<b>%1</b>").arg(ToQ(Branch.Head).toHtmlEscaped());
        Text += Branch.bHasUpstream ? QStringLiteral(" → %1").arg(ToQ(Branch.Upstream).toHtmlEscaped())
                                    : QStringLiteral(" <span style='color:gray'>(not pushed yet)</span>");
        BranchLabel->setText(Text);
    }

    AheadBehind->setText(Branch.bHasAheadBehind ? QStringLiteral("↑ %1 ahead   ↓ %2 behind").arg(Branch.Ahead).arg(Branch.Behind)
                                                : QString());

    /* Local commits and new ones upstream: a rebase, never a merge. */
    const bool bRebase = Branch.Ahead > 0 && Branch.Behind > 0;
    PullButton->setText(bRebase ? QStringLiteral("Pull (Rebase)") : QStringLiteral("Pull"));
    PullButton->setToolTip(bRebase ? QStringLiteral("Replay your %1 local commit(s) on top of the remote's (git pull --rebase --autostash)").arg(Branch.Ahead)
                                   : QStringLiteral("Bring in the remote's commits; fast-forward only, never a merge"));
    PushButton->setToolTip(Branch.bHasUpstream ? QStringLiteral("Upload your commits to %1").arg(ToQ(Branch.Upstream))
                                               : QStringLiteral("Upload this branch to origin and track it"));

    int Staged = 0;
    for(const FileEntry& Entry : Last.Entries)
    {
        Staged += Entry.IsStaged() ? 1 : 0;
    }
    StagedLabel->setText(Staged == 0 ? QStringLiteral("Nothing staged") : QStringLiteral("%1 staged").arg(Staged));
    CommitButton->setEnabled(!Git->IsBusy() && Staged > 0 && !Message->toPlainText().trimmed().isEmpty());

    SessionButton->setText(SessionBranch.isEmpty() ? QStringLiteral("Session Branch") : QStringLiteral("Session: %1").arg(SessionBranch));
}

void SourceControlWindow::UpdateBanners()
{
    const EditorGit::BranchInfo& Branch = Last.Branch;
    const QString Head = ToQ(Branch.Head);
    const bool bElsewhere = !SessionBranch.isEmpty() && bHaveStatus && (Branch.bDetached || Head != SessionBranch);
    if(bElsewhere)
    {
        SessionBanner->setText(QStringLiteral("HEAD is on <b>%1</b>, not the session branch <b>%2</b>. <a href='switch'>Switch to it</a>")
                               .arg(Branch.bDetached ? QStringLiteral("a detached commit") : Head.toHtmlEscaped(), SessionBranch.toHtmlEscaped()));
    }
    SessionBanner->setVisible(bElsewhere);

    int Conflicts = 0;
    for(const FileEntry& Entry : Last.Entries)
    {
        Conflicts += Entry.IsConflicted() ? 1 : 0;
    }
    ConflictText->setText(Conflicts == 1 ? QStringLiteral("1 file has a conflict. Resolve it in your merge tool, then stage it.")
                                         : QStringLiteral("%1 files have conflicts. Resolve them in your merge tool, then stage them.").arg(Conflicts));
    ConflictBanner->setVisible(Conflicts > 0);
}

const FileEntry* SourceControlWindow::FindEntry(const QString& InPath) const
{
    const std::string Path = InPath.toStdString();
    for(const FileEntry& Entry : Last.Entries)
    {
        if(Entry.Path == Path)
        {
            return &Entry;
        }
    }
    return nullptr;
}

void SourceControlWindow::OnItemChanged(QTreeWidgetItem* InItem, const int InColumn)
{
    if(bPopulating || InColumn != 0 || !Git)
    {
        return;
    }
    const bool bStage = InItem->checkState(0) != Qt::Unchecked;

    QStringList Paths;
    if(InItem->data(0, PathRole).isValid())
    {
        Paths << InItem->data(0, PathRole).toString();
    }
    else
    {
        for(int Child = 0; Child < InItem->childCount(); ++Child)
        {
            Paths << InItem->child(Child)->data(0, PathRole).toString();
        }
    }
    if(bStage)
    {
        StagePaths(Paths);
    }
    else
    {
        UnstagePaths(Paths);
    }
}

void SourceControlWindow::OnSelectionChanged()
{
    QTreeWidgetItem* Item = Tree->currentItem();
    if(Item && Item->data(0, PathRole).isValid())
    {
        ShowDiff(Item->data(0, PathRole).toString());
    }
}

void SourceControlWindow::StagePaths(const QStringList& InPaths)
{
    if(!Git)
    {
        return;
    }
    Git->Stage(InPaths, [this](const GitClient::Result& InResult)
    {
        ReportFailure(QStringLiteral("Couldn't stage"), InResult.Ok(), InResult.Summary(), QStringLiteral("git.stage"));
        RefreshNow();
    });
}

void SourceControlWindow::UnstagePaths(const QStringList& InPaths)
{
    if(!Git)
    {
        return;
    }
    /* A new file's own unstage, and everything before the first commit. */
    Git->Unstage(InPaths, Last.Branch.bInitial, [this](const GitClient::Result& InResult)
    {
        ReportFailure(QStringLiteral("Couldn't unstage"), InResult.Ok(), InResult.Summary(), QStringLiteral("git.stage"));
        RefreshNow();
    });
}

void SourceControlWindow::CommitStaged(const QString& InMessage)
{
    if(!Git)
    {
        return;
    }
    const QString Text = InMessage.trimmed();
    if(Text.isEmpty())
    {
        EditorNotifications::Notify(ESeverity::Warning, QStringLiteral("Write a commit message first"), QString(),
                                    QStringLiteral("Git"), QStringLiteral("git.commit"));
        return;
    }
    Git->Commit(Text + QLatin1Char('\n'), [this, Text](const GitClient::Result& InResult)
    {
        if(ReportFailure(QStringLiteral("Commit failed"), InResult.Ok(), InResult.Summary(), QStringLiteral("git.commit")))
        {
            Message->clear();
            EditorNotifications::Notify(ESeverity::Success, QStringLiteral("Committed"), Text.section(QLatin1Char('\n'), 0, 0),
                                        QStringLiteral("Git"), QStringLiteral("git.commit"));
        }
        RefreshNow();
    });
}

void SourceControlWindow::FetchRemote()
{
    if(!Git)
    {
        return;
    }
    PostProgress(QStringLiteral("Fetching…"), QStringLiteral("git.sync"));
    Git->Fetch([this](const GitClient::Result& InResult)
    {
        if(ReportFailure(QStringLiteral("Fetch failed"), InResult.Ok(), InResult.Summary(), QStringLiteral("git.sync")))
        {
            RefreshNow();
            Git->Status([](bool bInOk, const EditorGit::Status& InStatus, const QString&)
            {
                const int Behind = bInOk ? InStatus.Branch.Behind : 0;
                EditorNotifications::Notify(ESeverity::Info,
                                            Behind > 0 ? QStringLiteral("%1 new commit(s) to pull").arg(Behind) : QStringLiteral("Up to date"),
                                            QString(), QStringLiteral("Git"), QStringLiteral("git.sync"));
            });
            FetchLocks();
        }
    });
}

void SourceControlWindow::PullRemote()
{
    if(!Git)
    {
        return;
    }
    const bool bRebase = Last.Branch.Ahead > 0;
    PostProgress(bRebase ? QStringLiteral("Pulling with rebase…") : QStringLiteral("Pulling…"), QStringLiteral("git.sync"));
    Git->Pull(bRebase, [this, bRebase](const GitClient::Result& InResult)
    {
        if(ReportFailure(QStringLiteral("Pull failed"), InResult.Ok(), InResult.Summary(), QStringLiteral("git.sync")))
        {
            EditorNotifications::Notify(ESeverity::Success, bRebase ? QStringLiteral("Pulled and rebased") : QStringLiteral("Pulled"),
                                        QString::fromUtf8(InResult.Out).trimmed().section(QLatin1Char('\n'), -1),
                                        QStringLiteral("Git"), QStringLiteral("git.sync"));
        }
        RefreshNow();
    });
}

void SourceControlWindow::PushRemote()
{
    if(!Git)
    {
        return;
    }
    PostProgress(QStringLiteral("Pushing…"), QStringLiteral("git.sync"));

    /* Where the upstream was and where it goes: what the session is told (Pushed). */
    const QString Branch = ToQ(Last.Branch.Head);
    auto OldOid = std::make_shared<QString>();
    if(Last.Branch.bHasUpstream)
    {
        Git->RunQuiet({ QStringLiteral("rev-parse"), QStringLiteral("@{u}") }, [OldOid](const GitClient::Result& InResult)
        {
            if(InResult.Ok()) { *OldOid = QString::fromUtf8(InResult.Out).trimmed(); }
        });
    }
    Git->Push(!Last.Branch.bHasUpstream, [this, Branch, OldOid](const GitClient::Result& InResult)
    {
        if(ReportFailure(QStringLiteral("Push failed"), InResult.Ok(), InResult.Summary(), QStringLiteral("git.sync")))
        {
            EditorNotifications::Notify(ESeverity::Success, QStringLiteral("Pushed"), Branch,
                                        QStringLiteral("Git"), QStringLiteral("git.sync"));
            Git->Run({ QStringLiteral("rev-parse"), QStringLiteral("HEAD") }, [this, Branch, OldOid](const GitClient::Result& InHead)
            {
                if(InHead.Ok())
                {
                    emit Pushed(Branch, *OldOid, QString::fromUtf8(InHead.Out).trimmed());
                }
            });
        }
        RefreshNow();
    });
}

void SourceControlWindow::ShowDiff(const QString& InPath)
{
    const FileEntry* Entry = FindEntry(InPath);
    if(!Git || !Entry)
    {
        return;
    }
    DiffPath = InPath;

    const EPathGroup Group = EditorGit::ClassifyPath(Entry->Path, GameDir.toStdString());
    const QString State = Entry->IsConflicted() ? QStringLiteral("conflicted")
                        : Entry->IsStaged() && Entry->IsUnstaged() ? QStringLiteral("partly staged")
                        : Entry->IsStaged() ? QStringLiteral("staged")
                                            : QStringLiteral("not staged");
    DiffTitle->setText(QStringLiteral("<b>%1</b> · %2 · %3")
                       .arg(InPath.toHtmlEscaped(), QString::fromLatin1(EditorGit::GetGroupName(Group)), State));
    DiffStageButton->setText(Entry->IsUnstaged() ? QStringLiteral("Stage") : QStringLiteral("Unstage"));
    DiffStageButton->show();

    if(Entry->Kind == EEntryKind::Untracked)
    {
        /* New to git: the file itself, as added lines. */
        QFile File(QDir(Git->GetRoot()).filePath(InPath));
        if(!File.open(QIODevice::ReadOnly))
        {
            DiffView->setPlainText(QStringLiteral("(couldn't read the file)"));
            return;
        }
        const QByteArray Bytes = File.read(MaxUntrackedBytes);
        if(Bytes.left(8000).contains('\0'))
        {
            DiffView->setPlainText(QStringLiteral("Binary file, new."));
            return;
        }
        QStringList Lines = QString::fromUtf8(Bytes).split(QLatin1Char('\n'));
        for(QString& Line : Lines)
        {
            Line.prepend(QLatin1Char('+'));
        }
        if(File.size() > MaxUntrackedBytes)
        {
            Lines << QStringLiteral("(only the first %1 KB shown)").arg(MaxUntrackedBytes / 1024);
        }
        DiffView->setPlainText(Lines.join(QLatin1Char('\n')));
        return;
    }

    /* Staged first, then what's in the working tree on top of it. */
    const bool bStaged   = Entry->IsStaged();
    const bool bUnstaged = Entry->IsUnstaged();
    const QString Path   = InPath;
    auto Shown = std::make_shared<QString>();
    const auto Show = [this, Path, Shown](const GitClient::Result& InResult, const QString& InHeading, const bool bInLast)
    {
        if(DiffPath != Path)
        {
            return;   // another file was picked meanwhile
        }
        QString Text = QString::fromUtf8(InResult.Out);
        if(InResult.Ok() && Text.contains(QStringLiteral("Binary files")))
        {
            Text = QStringLiteral("Binary file changed.");
        }
        if(!InHeading.isEmpty() && !Text.isEmpty())
        {
            *Shown += InHeading + QLatin1Char('\n');
        }
        *Shown += InResult.Ok() ? Text : InResult.Summary();
        if(bInLast)
        {
            DiffView->setPlainText(Shown->isEmpty() ? QStringLiteral("(no textual changes)") : *Shown);
        }
    };
    if(bStaged)
    {
        Git->Diff(Path, true, [Show, bUnstaged](const GitClient::Result& InResult)
        {
            Show(InResult, bUnstaged ? QStringLiteral("=== staged ===") : QString(), !bUnstaged);
        });
    }
    if(bUnstaged)
    {
        Git->Diff(Path, false, [Show, bStaged](const GitClient::Result& InResult)
        {
            Show(InResult, bStaged ? QStringLiteral("=== not staged ===") : QString(), true);
        });
    }
}

void SourceControlWindow::SetSessionBranch(const QString& InBranch)
{
    SessionBranch = InBranch.trimmed();
    if(!ConfigPath.isEmpty())
    {
        ConfigIniFile Config;
        Config.Load(ConfigPath);
        Config.Set(SessionSection, SessionKey, SessionBranch);
        if(!Config.Save(ConfigPath))
        {
            EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Couldn't save the session branch"), Config.GetError(),
                                        QStringLiteral("Git"));
        }
    }
    UpdateHeader();
    UpdateBanners();
}

void SourceControlWindow::FillSessionMenu()
{
    QMenu* Menu = SessionButton->menu();
    Menu->clear();
    if(!Git)
    {
        return;
    }
    const QString Head = ToQ(Last.Branch.Head);
    if(!Head.isEmpty() && Head != SessionBranch)
    {
        connect(Menu->addAction(QStringLiteral("Use %1 (current)").arg(Head)), &QAction::triggered, this, [this, Head]() { SetSessionBranch(Head); });
    }
    connect(Menu->addAction(QStringLiteral("New Branch from HEAD…")), &QAction::triggered, this, [this]()
    {
        bool bOk = false;
        const QString Name = QInputDialog::getText(this, QStringLiteral("New Session Branch"), QStringLiteral("Branch name:"),
                                                   QLineEdit::Normal, QStringLiteral("collab/"), &bOk).trimmed();
        if(!bOk || Name.isEmpty())
        {
            return;
        }
        Git->SwitchBranch(Name, true, [this, Name](const GitClient::Result& InResult)
        {
            if(ReportFailure(QStringLiteral("Couldn't create %1").arg(Name), InResult.Ok(), InResult.Summary(), QStringLiteral("git.switch")))
            {
                SetSessionBranch(Name);
            }
            RefreshNow();
        });
    });
    if(!SessionBranch.isEmpty())
    {
        connect(Menu->addAction(QStringLiteral("Clear Session Branch")), &QAction::triggered, this, [this]() { SetSessionBranch(QString()); });
    }

    /* The local branches, filled in when git answers. */
    Menu->addSection(QStringLiteral("Local branches"));
    QPointer<QMenu> Guard(Menu);
    Git->Branches([this, Guard](const QStringList& InBranches)
    {
        if(!Guard)
        {
            return;
        }
        for(const QString& Name : InBranches)
        {
            QAction* Pick = Guard->addAction(Name);
            Pick->setCheckable(true);
            Pick->setChecked(Name == SessionBranch);
            connect(Pick, &QAction::triggered, this, [this, Name]() { SetSessionBranch(Name); });
        }
    });
}

void SourceControlWindow::OpenMergeTool()
{
    if(!Git)
    {
        return;
    }
    QStringList Paths;
    if(QTreeWidgetItem* Current = Tree->currentItem())
    {
        const FileEntry* Entry = FindEntry(Current->data(0, PathRole).toString());
        if(Entry && Entry->IsConflicted())
        {
            Paths << ToQ(Entry->Path);
        }
    }

    /* Without merge.tool, git would fall back to a terminal tool nobody can see. */
    Git->RunQuiet({ QStringLiteral("config"), QStringLiteral("--get"), QStringLiteral("merge.tool") },
             [this, Paths](const GitClient::Result& InResult)
    {
        if(!InResult.Ok() || InResult.Out.trimmed().isEmpty())
        {
            EditorNotifications::Notify(ESeverity::Warning, QStringLiteral("No merge tool set up"),
                                        QStringLiteral("Set one with git config --global merge.tool <name> (and mergetool.<name>.cmd if git doesn't know it), "
                                                       "then try again."),
                                        QStringLiteral("Git"), QStringLiteral("git.mergetool"));
            return;
        }
        const QStringList Args = QStringList{ QStringLiteral("mergetool"), QStringLiteral("--no-prompt"), QStringLiteral("--") } + Paths;
        if(!QProcess::startDetached(GitClient::GetGitCommand(), Args, Git->GetRoot()))
        {
            EditorNotifications::Notify(ESeverity::Error, QStringLiteral("Couldn't start git mergetool"), QString(),
                                        QStringLiteral("Git"), QStringLiteral("git.mergetool"));
        }
    });
}

void SourceControlWindow::FetchLocks()
{
    if(!Git || !Git->UsesLfs())
    {
        return;
    }
    Git->Run({ QStringLiteral("lfs"), QStringLiteral("locks"), QStringLiteral("--json") }, [this](const GitClient::Result& InResult)
    {
        LockOwners.clear();
        if(InResult.Ok())
        {
            /* [{"id": "...", "path": "...", "owner": {"name": "..."}, "locked_at": "..."}] */
            const QJsonArray Locks = QJsonDocument::fromJson(InResult.Out).array();
            for(const QJsonValue& Each : Locks)
            {
                const QJsonObject Lock = Each.toObject();
                LockOwners.insert(Lock.value(QStringLiteral("path")).toString(),
                                  Lock.value(QStringLiteral("owner")).toObject().value(QStringLiteral("name")).toString());
            }
        }
        Populate();
    });
}

void SourceControlWindow::SetBusy(const bool bInBusy)
{
    for(QWidget* Each : std::initializer_list<QWidget*>{ FetchButton, PullButton, PushButton })
    {
        Each->setEnabled(!bInBusy);
    }
    UpdateHeader();
}

bool SourceControlWindow::ReportFailure(const QString& InTitle, const bool bInOk, const QString& InSummary, const QString& InKey)
{
    if(bInOk)
    {
        return true;
    }
    EditorNotifications::Notification Failed;
    Failed.Severity   = ESeverity::Error;
    Failed.Title      = InTitle;
    Failed.Body       = InSummary;
    Failed.Source     = QStringLiteral("Git");
    Failed.ReplaceKey = InKey;
    if(EditorNotifications* Notifications = EditorNotifications::Get())
    {
        Notifications->Post(std::move(Failed));
    }
    return false;
}

#endif // WITH_EDITOR
