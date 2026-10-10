#if WITH_EDITOR

#include "Private/Editor/DocumentationWindow.h"

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QHash>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Project/ProjectTemplate.h"

namespace
{
    constexpr int PathRole    = Qt::UserRole;       // a file item's absolute path
    constexpr int SearchRole  = Qt::UserRole + 1;   // its name, title and contents, lower case
    constexpr int AnchorRole  = Qt::UserRole;       // an outline item's heading anchor

    QPointer<DocumentationWindow>& TheWindow()
    {
        static QPointer<DocumentationWindow> Window;
        return Window;
    }

    bool IsMarkdown(const QString& InPath)
    {
        const QString Suffix = QFileInfo(InPath).suffix();
        return Suffix.compare(QStringLiteral("md"), Qt::CaseInsensitive) == 0
            || Suffix.compare(QStringLiteral("markdown"), Qt::CaseInsensitive) == 0;
    }

    bool IsSkippedFolder(const QFileInfo& InFolder)
    {
        const QString Name = InFolder.fileName();
        if(Name.startsWith(QLatin1Char('.')) || Name.startsWith(QStringLiteral("cmake-build"), Qt::CaseInsensitive))
        {
            return true;
        }
        for(const char* Skipped : { "External", "Binaries", "Build", "Saved", "Intermediate", "node_modules" })
        {
            if(Name.compare(QLatin1String(Skipped), Qt::CaseInsensitive) == 0) { return true; }
        }
        /* A build tree, whatever it is called (Engine/Build). */
        return QFileInfo::exists(QDir(InFolder.absoluteFilePath()).filePath(QStringLiteral("CMakeCache.txt")));
    }

    void CollectMarkdown(const QDir& InDir, QStringList& OutFiles)
    {
        for(const QFileInfo& File : InDir.entryInfoList({ QStringLiteral("*.md"), QStringLiteral("*.markdown") }, QDir::Files))
        {
            OutFiles << File.absoluteFilePath();
        }
        for(const QFileInfo& Sub : InDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        {
            if(!IsSkippedFolder(Sub)) { CollectMarkdown(QDir(Sub.absoluteFilePath()), OutFiles); }
        }
    }

    bool IsUnder(const QString& InPath, const QString& InRoot)
    {
        if(InRoot.isEmpty()) { return false; }
        const QString Relative = QDir(InRoot).relativeFilePath(InPath);
        return !Relative.startsWith(QStringLiteral("..")) && !QDir::isAbsolutePath(Relative);
    }

    /* The first "# Title" line, else empty. */
    QString TitleOf(const QString& InText)
    {
        static const QRegularExpression Heading(QStringLiteral("^#{1,6}\\s+(.+?)\\s*#*\\s*$"),
                                                QRegularExpression::MultilineOption);
        const QRegularExpressionMatch Match = Heading.match(InText);
        return Match.hasMatch() ? Match.captured(1) : QString();
    }
}

DocumentationWindow* DocumentationWindow::Open(const QString& InPath)
{
    DocumentationWindow* Window = TheWindow();
    if(!Window)
    {
        Window = new DocumentationWindow();
        TheWindow() = Window;
        Window->resize(1180, 760);
        Window->ShowFile(InPath.isEmpty() ? Window->DefaultPage() : InPath);
    }
    else if(!InPath.isEmpty())
    {
        Window->ShowFile(InPath);
    }
    Window->show();
    Window->raise();
    Window->activateWindow();
    return Window;
}

void DocumentationWindow::CloseIfOpen()
{
    if(TheWindow()) { TheWindow()->close(); }
}

QStringList DocumentationWindow::FindMarkdownFiles(const QString& InRoot)
{
    QStringList Files;
    if(!InRoot.isEmpty() && QFileInfo(InRoot).isDir())
    {
        CollectMarkdown(QDir(InRoot), Files);
    }
    Files.sort(Qt::CaseInsensitive);
    return Files;
}

QString DocumentationWindow::HeadingAnchor(const QString& InHeading)
{
    QString Anchor;
    for(const QChar Each : InHeading.trimmed().toLower())
    {
        if(Each.isLetterOrNumber() || Each == QLatin1Char('_') || Each == QLatin1Char('-')) { Anchor += Each; }
        else if(Each.isSpace())                                                            { Anchor += QLatin1Char('-'); }
    }
    return Anchor;
}

DocumentationWindow::DocumentationWindow()
    : QMainWindow(nullptr)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Documentation"));

    ProjectRoot = QDir::cleanPath(EditorGraphs::GetProjectRoot());
    EngineRoot  = QDir::cleanPath(EditorProject::GetEngineDir());
    if(EngineRoot == QStringLiteral(".")) { EngineRoot.clear(); }

    BuildUi();
    Rescan();
}

void DocumentationWindow::BuildUi()
{
    QToolBar* Bar = addToolBar(QStringLiteral("Navigation"));
    Bar->setMovable(false);

    BackAction = Bar->addAction(style()->standardIcon(QStyle::SP_ArrowBack), QStringLiteral("Back"));
    BackAction->setShortcut(QKeySequence::Back);
    BackAction->setToolTip(QStringLiteral("Back (Alt+Left)"));
    BackAction->setEnabled(false);

    ForwardAction = Bar->addAction(style()->standardIcon(QStyle::SP_ArrowForward), QStringLiteral("Forward"));
    ForwardAction->setShortcut(QKeySequence::Forward);
    ForwardAction->setToolTip(QStringLiteral("Forward (Alt+Right)"));
    ForwardAction->setEnabled(false);

    Bar->addSeparator();

    QAction* Refresh = Bar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), QStringLiteral("Refresh"));
    Refresh->setShortcut(QKeySequence::Refresh);
    Refresh->setToolTip(QStringLiteral("Find new and removed .md files (F5)"));
    connect(Refresh, &QAction::triggered, this, &DocumentationWindow::Rescan);

    QAction* External = Bar->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), QStringLiteral("Open Externally"));
    External->setToolTip(QStringLiteral("Open this file in the system's editor for .md files"));
    connect(External, &QAction::triggered, this, [this]()
    {
        if(!CurrentPath.isEmpty()) { QDesktopServices::openUrl(QUrl::fromLocalFile(CurrentPath)); }
    });

    QAction* Find = new QAction(this);
    Find->setShortcut(QKeySequence::Find);
    addAction(Find);

    /* Left: filter, files, and the page's headings. */
    auto* Left = new QWidget(this);
    auto* LeftLayout = new QVBoxLayout(Left);
    LeftLayout->setContentsMargins(4, 4, 0, 4);

    Filter = new QLineEdit(Left);
    Filter->setPlaceholderText(QStringLiteral("Filter by name or contents (Ctrl+F)"));
    Filter->setClearButtonEnabled(true);
    LeftLayout->addWidget(Filter);
    connect(Filter, &QLineEdit::textChanged, this, &DocumentationWindow::ApplyFilter);
    connect(Find, &QAction::triggered, this, [this]() { Filter->setFocus(); Filter->selectAll(); });

    auto* LeftSplit = new QSplitter(Qt::Vertical, Left);

    Files = new QTreeWidget(LeftSplit);
    Files->setHeaderHidden(true);
    Files->setUniformRowHeights(true);
    connect(Files, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* InItem)
    {
        const QString Path = InItem->data(0, PathRole).toString();
        if(!Path.isEmpty() && Path != CurrentPath) { ShowFile(Path); }
    });
    connect(Files, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* InItem)
    {
        const QString Path = InItem->data(0, PathRole).toString();
        if(!Path.isEmpty() && Path != CurrentPath) { ShowFile(Path); }
    });

    auto* OutlineBox = new QWidget(LeftSplit);
    auto* OutlineLayout = new QVBoxLayout(OutlineBox);
    OutlineLayout->setContentsMargins(0, 0, 0, 0);
    OutlineLayout->addWidget(new QLabel(QStringLiteral("On this page"), OutlineBox));
    Outline = new QTreeWidget(OutlineBox);
    Outline->setHeaderHidden(true);
    Outline->setUniformRowHeights(true);
    OutlineLayout->addWidget(Outline);
    connect(Outline, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* InItem)
    {
        Page->scrollToAnchor(InItem->data(0, AnchorRole).toString());
    });

    LeftSplit->setStretchFactor(0, 3);
    LeftSplit->setStretchFactor(1, 2);
    LeftLayout->addWidget(LeftSplit);

    /* Right: the page. */
    Page = new QTextBrowser(this);
    Page->setOpenLinks(false);
    Page->document()->setDocumentMargin(16);
    connect(Page, &QTextBrowser::anchorClicked, this, &DocumentationWindow::OnLinkClicked);
    connect(Page, &QTextBrowser::sourceChanged, this, &DocumentationWindow::OnSourceChanged);
    connect(Page, &QTextBrowser::backwardAvailable, BackAction, &QAction::setEnabled);
    connect(Page, &QTextBrowser::forwardAvailable, ForwardAction, &QAction::setEnabled);
    connect(Page, &QTextBrowser::highlighted, this, [this](const QUrl& InLink)
    {
        if(InLink.isEmpty()) { statusBar()->clearMessage(); }
        else                 { statusBar()->showMessage(InLink.toString()); }
    });
    connect(BackAction, &QAction::triggered, Page, &QTextBrowser::backward);
    connect(ForwardAction, &QAction::triggered, Page, &QTextBrowser::forward);

    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Left);
    Split->addWidget(Page);
    Split->setStretchFactor(0, 0);
    Split->setStretchFactor(1, 1);
    Split->setSizes({ 300, 880 });
    setCentralWidget(Split);

    PathLabel = new QLabel(this);
    statusBar()->addPermanentWidget(PathLabel);

    /* An editor may save by replacing the file, which drops the watch: re-add it. */
    Watcher = new QFileSystemWatcher(this);
    connect(Watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& InPath)
    {
        QTimer::singleShot(150, this, [this, InPath]() { OnFileChanged(InPath); });
    });
}

void DocumentationWindow::Rescan()
{
    Files->clear();

    struct Group
    {
        QString     Label;
        QString     Root;
        QStringList Paths;
    };
    QList<Group> Groups;

    const bool bProjectInEngine = IsUnder(ProjectRoot, EngineRoot);
    if(!ProjectRoot.isEmpty())
    {
        Groups << Group{ QStringLiteral("%1 (Project)").arg(QDir(ProjectRoot).dirName()), ProjectRoot,
                         FindMarkdownFiles(ProjectRoot) };
    }
    if(!EngineRoot.isEmpty())
    {
        QStringList EngineFiles;
        for(const QString& Each : FindMarkdownFiles(EngineRoot))
        {
            if(!bProjectInEngine || !IsUnder(Each, ProjectRoot)) { EngineFiles << Each; }
        }
        /* The repository's own README, beside Engine/ and Games/. */
        QDir Repository(EngineRoot);
        if(Repository.cdUp())
        {
            for(const QFileInfo& Each : Repository.entryInfoList({ QStringLiteral("*.md") }, QDir::Files, QDir::Name))
            {
                EngineFiles.prepend(Each.absoluteFilePath());
            }
        }
        Groups << Group{ QStringLiteral("Engine"), EngineRoot, EngineFiles };
    }

    for(const Group& Each : Groups)
    {
        if(Each.Paths.isEmpty()) { continue; }

        auto* Top = new QTreeWidgetItem(Files, { Each.Label });
        Top->setToolTip(0, QDir::toNativeSeparators(Each.Root));
        QFont Bold = Top->font(0);
        Bold.setBold(true);
        Top->setFont(0, Bold);

        QHash<QString, QTreeWidgetItem*> Folders;
        for(const QString& Path : Each.Paths)
        {
            const QFileInfo Info(Path);
            QString Folder = QDir(Each.Root).relativeFilePath(Info.absolutePath());
            if(Folder == QStringLiteral("."))     { Folder.clear(); }
            else if(Folder == QStringLiteral("..")) { Folder = QStringLiteral("Repository"); }

            QTreeWidgetItem* Parent = Top;
            if(!Folder.isEmpty())
            {
                QTreeWidgetItem*& FolderItem = Folders[Folder];
                if(!FolderItem)
                {
                    FolderItem = new QTreeWidgetItem(Top, { Folder });
                    FolderItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                    FolderItem->setData(0, SearchRole, Folder.toLower());
                }
                Parent = FolderItem;
            }

            QString Text;
            QFile File(Path);
            if(File.open(QIODevice::ReadOnly | QIODevice::Text)) { Text = QString::fromUtf8(File.readAll()); }
            const QString Title = TitleOf(Text);

            auto* Item = new QTreeWidgetItem(Parent, { Info.fileName() });
            Item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            Item->setData(0, PathRole, Info.absoluteFilePath());
            Item->setData(0, SearchRole, (Info.fileName() + QLatin1Char('\n') + Text).toLower());
            Item->setToolTip(0, (Title.isEmpty() ? QString() : Title + QLatin1Char('\n'))
                                    + QDir::toNativeSeparators(Info.absoluteFilePath()));
        }
    }
    Files->expandAll();

    if(Files->topLevelItemCount() == 0)
    {
        auto* None = new QTreeWidgetItem(Files, { QStringLiteral("No .md files found") });
        None->setDisabled(true);
    }

    ApplyFilter(Filter->text());
    SelectInTree(CurrentPath);
}

void DocumentationWindow::ApplyFilter(const QString& InText)
{
    const QStringList Words = InText.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto Matches = [&Words](const QTreeWidgetItem* InItem)
    {
        const QString Search = InItem->data(0, SearchRole).toString();
        for(const QString& Word : Words)
        {
            if(!Search.contains(Word)) { return false; }
        }
        return true;
    };

    for(int Top = 0; Top < Files->topLevelItemCount(); ++Top)
    {
        QTreeWidgetItem* Group = Files->topLevelItem(Top);
        bool bGroupVisible = false;
        for(int Child = 0; Child < Group->childCount(); ++Child)
        {
            QTreeWidgetItem* Item = Group->child(Child);
            bool bVisible = false;
            if(Item->childCount() == 0)
            {
                bVisible = Matches(Item);
            }
            else
            {
                /* A folder whose name matches shows all it holds. */
                const bool bFolderMatches = !Words.isEmpty() && Matches(Item);
                for(int Leaf = 0; Leaf < Item->childCount(); ++Leaf)
                {
                    QTreeWidgetItem* File = Item->child(Leaf);
                    const bool bShow = bFolderMatches || Matches(File);
                    File->setHidden(!bShow);
                    bVisible = bVisible || bShow;
                }
                if(!Words.isEmpty()) { Item->setExpanded(true); }
            }
            Item->setHidden(!bVisible);
            bGroupVisible = bGroupVisible || bVisible;
        }
        Group->setHidden(!bGroupVisible && Group->childCount() > 0);
    }
}

void DocumentationWindow::SelectInTree(const QString& InPath)
{
    if(InPath.isEmpty()) { return; }
    for(QTreeWidgetItemIterator It(Files); *It; ++It)
    {
        if((*It)->data(0, PathRole).toString().compare(InPath, Qt::CaseInsensitive) == 0)
        {
            const QSignalBlocker Block(Files);
            Files->setCurrentItem(*It);
            for(QTreeWidgetItem* Up = (*It)->parent(); Up; Up = Up->parent()) { Up->setExpanded(true); }
            Files->scrollToItem(*It);
            return;
        }
    }
    Files->clearSelection();
}

QString DocumentationWindow::DefaultPage() const
{
    QStringList Candidates;
    if(!ProjectRoot.isEmpty())
    {
        Candidates << QDir(ProjectRoot).filePath(QStringLiteral("README.md"))
                   << QDir(ProjectRoot).filePath(QStringLiteral("Docs/GettingStarted.md"));
    }
    if(!EngineRoot.isEmpty())
    {
        Candidates << QDir(EngineRoot).filePath(QStringLiteral("../README.md"))
                   << QDir(EngineRoot).filePath(QStringLiteral("README.md"));
    }
    for(const QString& Each : Candidates)
    {
        if(QFileInfo(Each).isFile()) { return QDir::cleanPath(QFileInfo(Each).absoluteFilePath()); }
    }
    for(QTreeWidgetItemIterator It(Files); *It; ++It)
    {
        const QString Path = (*It)->data(0, PathRole).toString();
        if(!Path.isEmpty()) { return Path; }
    }
    return QString();
}

void DocumentationWindow::ShowFile(const QString& InPath, const QString& InFragment)
{
    if(InPath.isEmpty()) { return; }
    QUrl Url = QUrl::fromLocalFile(QFileInfo(InPath).absoluteFilePath());
    if(!InFragment.isEmpty()) { Url.setFragment(InFragment); }
    Page->setSource(Url, QTextDocument::MarkdownResource);
}

void DocumentationWindow::OnSourceChanged(const QUrl& InSource)
{
    const QString Path = InSource.toLocalFile();
    const bool bNewFile = Path.compare(CurrentPath, Qt::CaseInsensitive) != 0;

    if(bNewFile)
    {
        if(!CurrentPath.isEmpty()) { Watcher->removePath(CurrentPath); }
        CurrentPath = Path;
        if(QFileInfo::exists(Path)) { Watcher->addPath(Path); }
    }

    DecorateDocument();
    if(!InSource.fragment().isEmpty()) { Page->scrollToAnchor(InSource.fragment()); }

    const QString Title = Outline->topLevelItemCount() > 0 ? Outline->topLevelItem(0)->text(0) : QString();
    setWindowTitle(QStringLiteral("%1 - Documentation").arg(Title.isEmpty() ? QFileInfo(Path).fileName() : Title));
    PathLabel->setText(QDir::toNativeSeparators(Path));
    SelectInTree(Path);
}

void DocumentationWindow::OnLinkClicked(const QUrl& InLink)
{
    const QUrl Resolved = Page->source().resolved(InLink);
    if(Resolved.isLocalFile())
    {
        const QString Path = Resolved.toLocalFile();
        if(IsMarkdown(Path) && QFileInfo(Path).isFile())
        {
            Page->setSource(Resolved, QTextDocument::MarkdownResource);   // or just scrolls, for "#section"
            return;
        }
        if(!QFileInfo::exists(Path))
        {
            statusBar()->showMessage(QStringLiteral("Not found: %1").arg(QDir::toNativeSeparators(Path)), 5000);
            return;
        }
    }
    /* Web links, source files, folders, images: outside the editor. */
    QDesktopServices::openUrl(Resolved);
}

void DocumentationWindow::OnFileChanged(const QString& InPath)
{
    if(InPath.compare(CurrentPath, Qt::CaseInsensitive) != 0) { return; }
    if(!QFileInfo::exists(InPath))
    {
        statusBar()->showMessage(QStringLiteral("%1 was deleted").arg(QFileInfo(InPath).fileName()), 5000);
        return;
    }
    if(!Watcher->files().contains(InPath)) { Watcher->addPath(InPath); }

    const int Scroll = Page->verticalScrollBar()->value();
    Page->reload();
    Page->verticalScrollBar()->setValue(Scroll);
    statusBar()->showMessage(QStringLiteral("Reloaded %1").arg(QFileInfo(InPath).fileName()), 3000);
}

void DocumentationWindow::DecorateDocument()
{
    QTextDocument* Doc = Page->document();
    const QPalette Palette = Page->palette();
    const QBrush CodeBackground = Palette.brush(QPalette::AlternateBase);

    Outline->clear();
    QHash<QString, int> Used;
    QList<QPair<int, QTreeWidgetItem*>> Parents;   // heading level, item

    for(QTextBlock Block = Doc->begin(); Block.isValid(); Block = Block.next())
    {
        const QTextBlockFormat Format = Block.blockFormat();

        /* Headings: GitHub's anchors ("foo", then "foo-1"), and the outline. */
        if(const int Level = Format.headingLevel(); Level > 0)
        {
            const QString Text = Block.text().trimmed();
            QString Anchor = HeadingAnchor(Text);
            if(Anchor.isEmpty()) { continue; }
            int& Seen = Used[Anchor];
            if(Seen > 0) { Anchor += QStringLiteral("-%1").arg(Seen); }
            ++Seen;

            QTextCursor Cursor(Block);
            Cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            QTextCharFormat AnchorFormat;
            AnchorFormat.setAnchor(true);
            AnchorFormat.setAnchorNames({ Anchor });
            Cursor.mergeCharFormat(AnchorFormat);

            while(!Parents.isEmpty() && Parents.last().first >= Level) { Parents.removeLast(); }
            auto* Item = Parents.isEmpty() ? new QTreeWidgetItem(Outline, { Text })
                                           : new QTreeWidgetItem(Parents.last().second, { Text });
            Item->setData(0, AnchorRole, Anchor);
            Item->setToolTip(0, Text);
            Parents << qMakePair(Level, Item);
            continue;
        }

        /* Code blocks: shaded, as on GitHub. */
        if(Format.hasProperty(QTextFormat::BlockCodeFence) || Format.nonBreakableLines())
        {
            QTextCursor Cursor(Block);
            QTextBlockFormat Shaded;
            Shaded.setBackground(CodeBackground);
            Cursor.mergeBlockFormat(Shaded);
            continue;
        }

        /* `inline code`, shaded too. Gathered first: a format change splits fragments. */
        QList<QPair<int, int>> Spans;   // position, length
        for(QTextBlock::iterator It = Block.begin(); !It.atEnd(); ++It)
        {
            const QTextFragment Fragment = It.fragment();
            if(Fragment.isValid() && Fragment.charFormat().fontFixedPitch())
            {
                Spans << qMakePair(Fragment.position(), Fragment.length());
            }
        }
        for(const QPair<int, int>& Span : Spans)
        {
            QTextCursor Cursor(Doc);
            Cursor.setPosition(Span.first);
            Cursor.setPosition(Span.first + Span.second, QTextCursor::KeepAnchor);
            QTextCharFormat Shaded;
            Shaded.setBackground(CodeBackground);
            Cursor.mergeCharFormat(Shaded);
        }
    }
    Outline->expandAll();

    /* Tables: ruled, with room in the cells. */
    QList<QTextFrame*> Frames = Doc->rootFrame()->childFrames();
    while(!Frames.isEmpty())
    {
        QTextFrame* Frame = Frames.takeFirst();
        Frames << Frame->childFrames();
        if(auto* Table = qobject_cast<QTextTable*>(Frame))
        {
            QTextTableFormat TableFormat = Table->format();
            TableFormat.setBorder(1);
            TableFormat.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
            TableFormat.setBorderBrush(Palette.brush(QPalette::Mid));
            TableFormat.setBorderCollapse(true);
            TableFormat.setCellPadding(5);
            TableFormat.setCellSpacing(0);
            Table->setFormat(TableFormat);
        }
    }

    Doc->setModified(false);
}

#endif // WITH_EDITOR
