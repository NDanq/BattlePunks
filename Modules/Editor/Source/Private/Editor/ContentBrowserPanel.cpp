#if WITH_EDITOR

#include "Public/Editor/ContentBrowserPanel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "CoreQtConversions.h"
#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Content/AssetImport.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Content/ImportWizard.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace
{
    constexpr int ThumbnailSize = 64;

    /* Thumbnails and tooltips past this size are not read on hover. */
    constexpr qint64 PreviewByteLimit = 64 * 1024 * 1024;

    bool IsInside(const QString& InPath, const QString& InRoot)
    {
        return InPath.compare(InRoot, Qt::CaseInsensitive) == 0
            || InPath.startsWith(InRoot + QLatin1Char('/'), Qt::CaseInsensitive);
    }

    QByteArray ReadAll(const QString& InPath)
    {
        QFile File(InPath);
        return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray();
    }

    EngineCore::Archive ArchiveOf(const QByteArray& InBytes)
    {
        return EngineCore::Archive::ForReading(std::vector<uint8_t>(InBytes.begin(), InBytes.end()));
    }

    /* The files that travel with InPath: a graph's generated output beside
     * it (Glow.material.nmgraph -> Glow.fsh, Glow.material; a script graph's
     * .lua), a world's entity folder (Level.nmworld -> Level/). Only those
     * that exist. */
    QStringList CompanionsOf(const QString& InPath)
    {
        const QFileInfo Info(InPath);
        const QDir Folder = Info.absoluteDir();
        const QString Name = Info.fileName();
        QStringList Out;

        const auto AddIfExists = [&](const QString& InFile)
        {
            if(QFileInfo::exists(Folder.filePath(InFile))) { Out << Folder.filePath(InFile); }
        };

        if(Name.endsWith(QStringLiteral(".material.nmgraph"), Qt::CaseInsensitive))
        {
            const QString Base = Name.left(Name.size() - int(qstrlen(".material.nmgraph")));
            AddIfExists(Base + QStringLiteral(".fsh"));
            AddIfExists(Base + QStringLiteral(".material"));
        }
        else if(Name.endsWith(QStringLiteral(".script.nmgraph"), Qt::CaseInsensitive))
        {
            AddIfExists(Name.left(Name.size() - int(qstrlen(".script.nmgraph"))) + QStringLiteral(".lua"));
        }
        else if(Name.endsWith(QStringLiteral(".nmworld"), Qt::CaseInsensitive))
        {
            const QString Entities = Folder.filePath(Info.completeBaseName());
            if(QFileInfo(Entities).isDir()) { Out << Entities; }
        }
        return Out;
    }

    /* Companion InCompanion renamed the way its owner went from InOld to
     * InNew: Glow.fsh follows Glow.material.nmgraph -> Shine.material.nmgraph
     * to Shine.fsh. */
    QString CompanionTarget(const QString& InCompanion, const QString& InOldOwner, const QString& InNewOwnerPath)
    {
        const auto BaseOf = [](const QString& InName)
        {
            const qsizetype Dot = InName.indexOf(QLatin1Char('.'));
            return Dot < 0 ? InName : InName.left(Dot);
        };
        const QFileInfo Companion(InCompanion);
        const QString OldBase = BaseOf(QFileInfo(InOldOwner).fileName());
        const QString NewBase = BaseOf(QFileInfo(InNewOwnerPath).fileName());
        QString Name = Companion.fileName();
        if(Name.startsWith(OldBase)) { Name = NewBase + Name.mid(OldBase.size()); }
        return QFileInfo(InNewOwnerPath).absoluteDir().filePath(Name);
    }

    /* A rename that also works for a folder; never over an existing one. */
    bool RenamePath(const QString& InFrom, const QString& InTo)
    {
        if(QFileInfo::exists(InTo)) { return false; }
        return QFileInfo(InFrom).isDir() ? QDir().rename(InFrom, InTo) : QFile::rename(InFrom, InTo);
    }

    bool IsAsset(const QString& InPath)
    {
        return InPath.endsWith(QStringLiteral(".nmmesh"), Qt::CaseInsensitive)
            || InPath.endsWith(QStringLiteral(".nmtexture"), Qt::CaseInsensitive);
    }
}

// === Models ==========================================================================

/* The tree: folders only -- the project root, the folders leading to it,
 * and its folders, minus the ones IsIgnoredFolder names. */
class ContentTreeProxy final : public QSortFilterProxyModel
{
public:
    ContentTreeProxy(const QString& InRoot, QObject* InParent) : QSortFilterProxyModel(InParent), Root(InRoot) {}

protected:
    bool filterAcceptsRow(const int InRow, const QModelIndex& InParent) const override
    {
        const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
        const QModelIndex Index = Source->index(InRow, 0, InParent);
        if(!Source->isDir(Index)) { return false; }

        const QString Path = Source->filePath(Index);
        if(IsInside(Root, Path)) { return true; }   // the root or a folder above it
        if(!IsInside(Path, Root)) { return false; } // somewhere else on the disk
        return !EditorGraphs::IsIgnoredFolder(Source->fileName(Index));
    }

    bool lessThan(const QModelIndex& InLeft, const QModelIndex& InRight) const override
    {
        const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
        return Source->fileName(InLeft).compare(Source->fileName(InRight), Qt::CaseInsensitive) < 0;
    }

private:
    QString Root;
};

/* The list: one folder's contents, folders first, the text filter on names,
 * thumbnails for images and textures, a summary tooltip for assets -- and
 * renames routed through the panel, so companions follow. */
class ContentListProxy final : public QSortFilterProxyModel
{
public:
    ContentListProxy(ContentBrowserPanel* InPanel, const QString& InRoot)
        : QSortFilterProxyModel(InPanel), Panel(InPanel), Root(InRoot) {}

    void SetNameFilter(const QString& InText)
    {
        NameFilter = InText.trimmed();
        invalidateFilter();
    }

    QVariant data(const QModelIndex& InIndex, const int InRole) const override
    {
        if(InIndex.column() == 0 && (InRole == Qt::DecorationRole || InRole == Qt::ToolTipRole))
        {
            const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
            const QModelIndex SourceIndex = mapToSource(InIndex);
            if(!Source->isDir(SourceIndex))
            {
                const QFileInfo Info = Source->fileInfo(SourceIndex);
                const Preview& Found = FindPreview(Info);
                if(InRole == Qt::DecorationRole && !Found.Thumbnail.isNull()) { return Found.Thumbnail; }
                if(InRole == Qt::ToolTipRole && !Found.Summary.isEmpty())
                {
                    return QStringLiteral("%1\n%2").arg(Info.fileName(), Found.Summary);
                }
            }
        }
        return QSortFilterProxyModel::data(InIndex, InRole);
    }

    bool setData(const QModelIndex& InIndex, const QVariant& InValue, const int InRole) override
    {
        if(InRole != Qt::EditRole || InIndex.column() != 0)
        {
            return QSortFilterProxyModel::setData(InIndex, InValue, InRole);
        }
        const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
        return Panel->RenameItem(Source->filePath(mapToSource(InIndex)), InValue.toString().trimmed());
    }

protected:
    bool filterAcceptsRow(const int InRow, const QModelIndex& InParent) const override
    {
        const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
        const QModelIndex Index = Source->index(InRow, 0, InParent);
        const QString Path = Source->filePath(Index);

        if(!IsInside(Path, Root) || Path.compare(Root, Qt::CaseInsensitive) == 0) { return true; }
        if(Source->isDir(Index) && EditorGraphs::IsIgnoredFolder(Source->fileName(Index))) { return false; }
        /* The filter narrows the folder being shown, not the folders above it. */
        if(!NameFilter.isEmpty() && Source->filePath(InParent).compare(Panel->GetCurrentFolder(), Qt::CaseInsensitive) == 0)
        {
            return Source->fileName(Index).contains(NameFilter, Qt::CaseInsensitive);
        }
        return true;
    }

    bool lessThan(const QModelIndex& InLeft, const QModelIndex& InRight) const override
    {
        const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
        const bool bLeftDir = Source->isDir(InLeft), bRightDir = Source->isDir(InRight);
        if(bLeftDir != bRightDir) { return bLeftDir; }
        return Source->fileName(InLeft).compare(Source->fileName(InRight), Qt::CaseInsensitive) < 0;
    }

private:
    struct Preview
    {
        qint64  Stamp = -1;
        QIcon   Thumbnail;
        QString Summary;
    };

    /* Made on first view, and again when the file changes. */
    const Preview& FindPreview(const QFileInfo& InInfo) const
    {
        const QString Key = InInfo.absoluteFilePath();
        const qint64 Stamp = InInfo.lastModified().toMSecsSinceEpoch() ^ InInfo.size();
        Preview& Entry = Previews[Key];
        if(Entry.Stamp == Stamp) { return Entry; }

        Entry = Preview();
        Entry.Stamp = Stamp;
        if(InInfo.size() > PreviewByteLimit) { return Entry; }

        const QString Path = InInfo.absoluteFilePath();
        if(Path.endsWith(QStringLiteral(".nmtexture"), Qt::CaseInsensitive))
        {
            RenderCore::TextureData Texture;
            EngineCore::Archive Ar = ArchiveOf(ReadAll(Path));
            if(RenderCore::SerializeTexture(Ar, Texture))
            {
                const QImage Image = EngineCore::QtBridge::ToQImage(Texture.Pixels);
                Entry.Thumbnail = QIcon(QPixmap::fromImage(Image.scaled(ThumbnailSize, ThumbnailSize, Qt::KeepAspectRatio,
                                                                        Qt::SmoothTransformation)));
                Entry.Summary = QStringLiteral("Texture, %1 x %2, %3%4")
                                .arg(Texture.Pixels.GetWidth()).arg(Texture.Pixels.GetHeight())
                                .arg(Texture.bSRGB ? QStringLiteral("sRGB") : QStringLiteral("linear"))
                                .arg(Texture.ImportInfo.IsEmpty() ? QString()
                                     : QStringLiteral("\nFrom %1").arg(QString::fromStdString(Texture.ImportInfo.SourcePath)));
            }
            else
            {
                Entry.Summary = QStringLiteral("Not a texture this editor can read");
            }
        }
        else if(Path.endsWith(QStringLiteral(".nmmesh"), Qt::CaseInsensitive))
        {
            RenderCore::MeshData Mesh;
            EngineCore::Archive Ar = ArchiveOf(ReadAll(Path));
            if(RenderCore::SerializeMesh(Ar, Mesh))
            {
                Entry.Summary = QStringLiteral("Mesh, %1 vertices, %2 triangles, %3 section(s)\nSize %4 x %5 x %6%7")
                                .arg(Mesh.GetVertexCount()).arg(Mesh.GetTriangleCount()).arg(Mesh.Sections.size())
                                .arg(double(Mesh.BoundsMax[0] - Mesh.BoundsMin[0]), 0, 'g', 4)
                                .arg(double(Mesh.BoundsMax[1] - Mesh.BoundsMin[1]), 0, 'g', 4)
                                .arg(double(Mesh.BoundsMax[2] - Mesh.BoundsMin[2]), 0, 'g', 4)
                                .arg(Mesh.ImportInfo.IsEmpty() ? QString()
                                     : QStringLiteral("\nFrom %1").arg(QString::fromStdString(Mesh.ImportInfo.SourcePath)));
            }
            else
            {
                Entry.Summary = QStringLiteral("Not a mesh this editor can read");
            }
        }
        else if(EditorContent::ClassifySource(Path) == EditorContent::EImportKind::Texture)
        {
            QImageReader Reader(Path);
            const QSize Size = Reader.size();
            if(Size.isValid())
            {
                Reader.setScaledSize(Size.scaled(ThumbnailSize, ThumbnailSize, Qt::KeepAspectRatio));
            }
            const QImage Image = Reader.read();
            if(!Image.isNull()) { Entry.Thumbnail = QIcon(QPixmap::fromImage(Image)); }
            Entry.Summary = QStringLiteral("Image, %1 x %2 -- drop it on a folder from outside the project to import it")
                            .arg(Size.width()).arg(Size.height());
        }
        return Entry;
    }

    ContentBrowserPanel*              Panel = nullptr;
    QString                           Root;
    QString                           NameFilter;
    mutable QHash<QString, Preview>   Previews;
};

// === The item view ===================================================================

/* The list, taking drops itself: from inside the project they move, from
 * outside they import. A drop on a folder item goes into that folder. */
class ContentItemView final : public QListView
{
public:
    explicit ContentItemView(ContentBrowserPanel* InPanel) : QListView(InPanel), Panel(InPanel)
    {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* InEvent) override
    {
        if(InEvent->mimeData()->hasUrls()) { InEvent->acceptProposedAction(); }
        else { InEvent->ignore(); }
    }

    void dragMoveEvent(QDragMoveEvent* InEvent) override
    {
        if(!InEvent->mimeData()->hasUrls()) { InEvent->ignore(); return; }
        QListView::dragMoveEvent(InEvent);   // the drop indicator over a folder
        InEvent->acceptProposedAction();
    }

    void dropEvent(QDropEvent* InEvent) override
    {
        QStringList Files;
        for(const QUrl& Url : InEvent->mimeData()->urls())
        {
            if(Url.isLocalFile()) { Files << QFileInfo(Url.toLocalFile()).absoluteFilePath(); }
        }

        /* Onto a folder item: into it. Anywhere else: the folder shown. */
        QString Target = Panel->GetCurrentFolder();
        const QModelIndex Under = indexAt(InEvent->position().toPoint());
        if(Under.isValid())
        {
            const QString UnderPath = Panel->PathOf(Under);
            if(QFileInfo(UnderPath).isDir()) { Target = UnderPath; }
        }

        InEvent->setDropAction(Qt::CopyAction);   // the source must not delete anything itself
        InEvent->accept();
        if(!Files.isEmpty()) { Panel->HandleDrop(Files, Target); }
    }

private:
    ContentBrowserPanel* Panel = nullptr;
};

// === The panel =======================================================================

ContentBrowserPanel::ContentBrowserPanel(QWidget* InParent)
    : QWidget(InParent)
    , Root(QDir::fromNativeSeparators(QDir(EditorGraphs::GetProjectRoot()).absolutePath()))
{
    Model = new QFileSystemModel(this);
    Model->setReadOnly(false);   // renames, through ContentListProxy::setData
    Model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    Model->setRootPath(Root);

    TreeProxy = new ContentTreeProxy(Root, this);
    TreeProxy->setSourceModel(Model);
    TreeProxy->setDynamicSortFilter(true);
    TreeProxy->sort(0);

    ListProxy = new ContentListProxy(this, Root);
    ListProxy->setSourceModel(Model);
    ListProxy->setDynamicSortFilter(true);
    ListProxy->sort(0);

    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(4, 4, 4, 4);
    Layout->setSpacing(4);

    auto* ToolBar = new QHBoxLayout();
    BuildToolBar(ToolBar);
    Layout->addLayout(ToolBar);

    PathBar = new QHBoxLayout();
    PathBar->setSpacing(0);
    Layout->addLayout(PathBar);

    Tree = new QTreeView(this);
    Tree->setModel(TreeProxy);
    for(int Column = 1; Column < Model->columnCount(); ++Column) { Tree->hideColumn(Column); }
    Tree->setHeaderHidden(true);
    Tree->setRootIndex(TreeProxy->mapFromSource(Model->index(QFileInfo(Root).absolutePath())));
    Tree->setDragDropMode(QAbstractItemView::NoDragDrop);
    connect(Tree, &QTreeView::clicked, this, [this](const QModelIndex& InIndex)
    {
        SetCurrentFolder(Model->filePath(TreeProxy->mapToSource(InIndex)));
    });

    List = new ContentItemView(this);
    List->setModel(ListProxy);
    List->setViewMode(QListView::IconMode);
    List->setIconSize(QSize(ThumbnailSize, ThumbnailSize));
    List->setGridSize(QSize(ThumbnailSize + 40, ThumbnailSize + 36));
    List->setResizeMode(QListView::Adjust);
    List->setWordWrap(true);
    List->setUniformItemSizes(true);
    List->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(List, &QListView::doubleClicked, this, [this](const QModelIndex& InIndex) { OpenItem(PathOf(InIndex)); });
    connect(List, &QWidget::customContextMenuRequested, this, &ContentBrowserPanel::ShowContextMenu);
    connect(List->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ContentBrowserPanel::UpdateActions);

    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Tree);
    Split->addWidget(List);
    Split->setStretchFactor(0, 0);
    Split->setStretchFactor(1, 1);
    Split->setSizes({ 200, 600 });
    Layout->addWidget(Split, 1);

    /* Keys, scoped to the panel so they do not steal the viewport's. */
    RenameAction = new QAction(QStringLiteral("Rename"), this);
    RenameAction->setShortcut(QKeySequence(Qt::Key_F2));
    RenameAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(RenameAction, &QAction::triggered, this, &ContentBrowserPanel::RenameSelected);
    addAction(RenameAction);

    DeleteAction = new QAction(QStringLiteral("Delete"), this);
    DeleteAction->setShortcut(QKeySequence::Delete);
    DeleteAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(DeleteAction, &QAction::triggered, this, &ContentBrowserPanel::DeleteSelected);
    addAction(DeleteAction);

    auto* NewFolderKey = new QAction(this);
    NewFolderKey->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    NewFolderKey->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(NewFolderKey, &QAction::triggered, this, &ContentBrowserPanel::NewFolder);
    addAction(NewFolderKey);

    auto* BackKey = new QAction(this);
    BackKey->setShortcut(QKeySequence(Qt::Key_Backspace));
    BackKey->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(BackKey, &QAction::triggered, this, &ContentBrowserPanel::NavigateUp);
    addAction(BackKey);

    SetCurrentFolder(Root);
    History.clear();
    UpdateActions();
}

void ContentBrowserPanel::BuildToolBar(QHBoxLayout* InLayout)
{
    const auto Button = [this, InLayout](const QIcon& InIcon, const QString& InText, const QString& InTip)
    {
        auto* B = new QToolButton(this);
        B->setIcon(InIcon);
        B->setText(InText);
        B->setToolTip(InTip);
        B->setAutoRaise(true);
        B->setToolButtonStyle(InIcon.isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonIconOnly);
        InLayout->addWidget(B);
        return B;
    };

    BackButton = Button(style()->standardIcon(QStyle::SP_ArrowBack), QStringLiteral("Back"), QStringLiteral("Back"));
    connect(BackButton, &QToolButton::clicked, this, &ContentBrowserPanel::NavigateBack);
    UpButton = Button(style()->standardIcon(QStyle::SP_FileDialogToParent), QStringLiteral("Up"),
                      QStringLiteral("Up a folder (Backspace)"));
    connect(UpButton, &QToolButton::clicked, this, &ContentBrowserPanel::NavigateUp);

    auto* NewFolderButton = Button(style()->standardIcon(QStyle::SP_FileDialogNewFolder), QStringLiteral("New Folder"),
                                   QStringLiteral("New Folder (Ctrl+Shift+N)"));
    connect(NewFolderButton, &QToolButton::clicked, this, &ContentBrowserPanel::NewFolder);

    auto* Import = Button(QIcon(), QStringLiteral("Import..."),
                          QStringLiteral("Import glTF meshes and images as engine assets into this folder"));
    connect(Import, &QToolButton::clicked, this, &ContentBrowserPanel::ImportWithDialog);

    InLayout->addSpacing(8);
    Filter = new QLineEdit(this);
    Filter->setPlaceholderText(QStringLiteral("Filter this folder"));
    Filter->setClearButtonEnabled(true);
    connect(Filter, &QLineEdit::textChanged, this, [this](const QString& InText) { ListProxy->SetNameFilter(InText); });
    InLayout->addWidget(Filter, 1);

    auto* ListMode = Button(style()->standardIcon(QStyle::SP_FileDialogDetailedView), QStringLiteral("List"),
                            QStringLiteral("Show as a list, or as icons"));
    ListMode->setCheckable(true);
    connect(ListMode, &QToolButton::toggled, this, [this](const bool bInList)
    {
        List->setViewMode(bInList ? QListView::ListMode : QListView::IconMode);
        List->setIconSize(bInList ? QSize(20, 20) : QSize(ThumbnailSize, ThumbnailSize));
        List->setGridSize(bInList ? QSize() : QSize(ThumbnailSize + 40, ThumbnailSize + 36));
        List->setWordWrap(!bInList);
    });
}

// === Navigation ======================================================================

void ContentBrowserPanel::SetCurrentFolder(const QString& InFolder)
{
    QString Folder = QDir::fromNativeSeparators(QDir(InFolder).absolutePath());
    if(!IsInside(Folder, Root) || !QFileInfo(Folder).isDir()) { Folder = Root; }
    if(Folder.compare(CurrentFolder, Qt::CaseInsensitive) == 0) { return; }

    if(!CurrentFolder.isEmpty()) { History << CurrentFolder; }
    CurrentFolder = Folder;

    const QModelIndex Source = Model->index(CurrentFolder);
    List->setRootIndex(ListProxy->mapFromSource(Source));
    ListProxy->invalidate();   // the name filter applies to this folder now

    const QModelIndex InTree = TreeProxy->mapFromSource(Source);
    Tree->setCurrentIndex(InTree);
    Tree->scrollTo(InTree);
    Tree->expand(InTree);

    RebuildPathBar();
    UpdateActions();
}

void ContentBrowserPanel::NavigateBack()
{
    if(History.isEmpty()) { return; }
    const QString Previous = History.takeLast();
    const QStringList Earlier = History;
    SetCurrentFolder(Previous);
    History = Earlier;   // going back does not record the folder it left
}

void ContentBrowserPanel::NavigateUp()
{
    if(CurrentFolder.compare(Root, Qt::CaseInsensitive) == 0) { return; }
    SetCurrentFolder(QFileInfo(CurrentFolder).absolutePath());
}

/* Project > Folder > Sub, each a button back to that folder. */
void ContentBrowserPanel::RebuildPathBar()
{
    while(QLayoutItem* Item = PathBar->takeAt(0))
    {
        delete Item->widget();
        delete Item;
    }

    QStringList Parts = QDir(Root).relativeFilePath(CurrentFolder).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if(Parts.size() == 1 && Parts.front() == QStringLiteral(".")) { Parts.clear(); }

    QString Path = Root;
    const auto AddSegment = [this](const QString& InText, const QString& InPath, const bool bInLast)
    {
        auto* Segment = new QToolButton(this);
        Segment->setText(InText);
        Segment->setAutoRaise(true);
        if(bInLast) { Segment->setStyleSheet(QStringLiteral("font-weight: bold;")); }
        connect(Segment, &QToolButton::clicked, this, [this, InPath]() { SetCurrentFolder(InPath); });
        PathBar->addWidget(Segment);
    };

    AddSegment(QFileInfo(Root).fileName(), Root, Parts.isEmpty());
    for(int At = 0; At < Parts.size(); ++At)
    {
        auto* Arrow = new QLabel(QStringLiteral(" > "), this);
        Arrow->setStyleSheet(QStringLiteral("color: palette(mid);"));
        PathBar->addWidget(Arrow);
        Path += QLatin1Char('/') + Parts[At];
        AddSegment(Parts[At], Path, At == Parts.size() - 1);
    }
    PathBar->addStretch(1);
}

void ContentBrowserPanel::UpdateActions()
{
    const bool bAny = !SelectedPaths().isEmpty();
    BackButton->setEnabled(!History.isEmpty());
    UpButton->setEnabled(CurrentFolder.compare(Root, Qt::CaseInsensitive) != 0);
    RenameAction->setEnabled(SelectedPaths().size() == 1);
    DeleteAction->setEnabled(bAny);
}

QString ContentBrowserPanel::GetImportFolder() const
{
    if(CurrentFolder.compare(Root, Qt::CaseInsensitive) != 0) { return CurrentFolder; }
    return QDir(Root).filePath(QStringLiteral("Content"));
}

QString ContentBrowserPanel::PathOf(const QModelIndex& InListIndex) const
{
    return Model->filePath(ListProxy->mapToSource(InListIndex));
}

QStringList ContentBrowserPanel::SelectedPaths() const
{
    QStringList Out;
    for(const QModelIndex& Index : List->selectionModel()->selectedIndexes())
    {
        if(Index.column() == 0) { Out << PathOf(Index); }
    }
    return Out;
}

void ContentBrowserPanel::SelectFiles(const QStringList& InPaths)
{
    List->selectionModel()->clearSelection();
    for(const QString& Path : InPaths)
    {
        const QModelIndex Index = ListProxy->mapFromSource(Model->index(Path));
        if(Index.isValid() && Index.parent() == List->rootIndex())
        {
            List->selectionModel()->select(Index, QItemSelectionModel::Select);
            List->scrollTo(Index);
        }
    }
}

// === Opening =========================================================================

void ContentBrowserPanel::OpenItem(const QString& InPath)
{
    const QFileInfo Info(InPath);
    if(Info.isDir())
    {
        SetCurrentFolder(InPath);
        return;
    }
    if(InPath.endsWith(QString::fromLatin1(NodeGraph::GRAPH_EXTENSION), Qt::CaseInsensitive))
    {
        EditorGraphs::OpenGraph(InPath, window());
        return;
    }
    if(InPath.endsWith(QStringLiteral(".nmworld"), Qt::CaseInsensitive))
    {
        emit OpenWorldRequested(InPath);
        return;
    }
    if(IsAsset(InPath))
    {
        emit StatusMessage(QStringLiteral("Drag %1 onto the viewport, or onto a Mesh field in Details").arg(Info.fileName()));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(InPath));
}

void ContentBrowserPanel::ShowInExplorer(const QString& InPath)
{
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"), { QStringLiteral("/select,"), QDir::toNativeSeparators(InPath) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(InPath).absolutePath()));
#endif
}

void ContentBrowserPanel::ShowContextMenu(const QPoint& InPos)
{
    const QModelIndex Under = List->indexAt(InPos);
    if(Under.isValid() && !List->selectionModel()->isSelected(Under))
    {
        List->selectionModel()->select(Under, QItemSelectionModel::ClearAndSelect);
    }
    const QStringList Selected = SelectedPaths();

    QMenu Menu(this);
    if(Selected.size() == 1)
    {
        const QString Path = Selected.front();
        Menu.addAction(QStringLiteral("Open"), this, [this, Path]() { OpenItem(Path); });
        Menu.addAction(QStringLiteral("Show in Explorer"), this, [this, Path]() { ShowInExplorer(Path); });
        Menu.addAction(QStringLiteral("Copy Path"), this, [Path]()
        {
            const QString Runtime = EditorContent::ToRuntimePath(Path);
            QApplication::clipboard()->setText(Runtime.isEmpty() ? QDir::toNativeSeparators(Path) : Runtime);
        });
        Menu.addSeparator();
    }
    bool bAnyAsset = false;
    for(const QString& Path : Selected) { bAnyAsset |= IsAsset(Path); }
    if(bAnyAsset)
    {
        Menu.addAction(QStringLiteral("Reimport"), this, &ContentBrowserPanel::ReimportSelected);
        Menu.addSeparator();
    }
    if(!Selected.isEmpty())
    {
        Menu.addAction(RenameAction);
        Menu.addAction(DeleteAction);
        Menu.addSeparator();
    }
    Menu.addAction(QStringLiteral("New Folder"), this, &ContentBrowserPanel::NewFolder);
    Menu.addAction(QStringLiteral("Import..."), this, &ContentBrowserPanel::ImportWithDialog);
    Menu.addAction(QStringLiteral("Show Folder in Explorer"), this, [this]()
    {
        QDesktopServices::openUrl(QUrl::fromLocalFile(CurrentFolder));
    });
    Menu.exec(List->viewport()->mapToGlobal(InPos));
}

// === Organising ======================================================================

QString ContentBrowserPanel::WhyProtected(const QString& InPath) const
{
    const QString Path = QDir::fromNativeSeparators(QFileInfo(InPath).absoluteFilePath());
    if(Path.compare(Root, Qt::CaseInsensitive) == 0)
    {
        return QStringLiteral("the project folder itself");
    }
    if(Path.startsWith(Root + QStringLiteral("/Config"), Qt::CaseInsensitive)
       && (Path.size() == Root.size() + 7 || Path.at(Root.size() + 7) == QLatin1Char('/')))
    {
        return QStringLiteral("the project's Config folder, which the build and the editor expect where it is");
    }

    const QStringList Involved = QStringList{ Path } << CompanionsOf(Path);
    for(const QString& Each : Involved)
    {
        if(Each.endsWith(QString::fromLatin1(NodeGraph::GRAPH_EXTENSION), Qt::CaseInsensitive) && EditorGraphs::IsGraphOpen(Each))
        {
            return QStringLiteral("%1 is open in a graph window -- close it first").arg(QFileInfo(Each).fileName());
        }
        if(!OpenWorld.isEmpty() && QFileInfo(Each).isFile()
           && QFileInfo(Each).absoluteFilePath().compare(QFileInfo(OpenWorld).absoluteFilePath(), Qt::CaseInsensitive) == 0)
        {
            return QStringLiteral("%1 is the open world").arg(QFileInfo(Each).fileName());
        }
    }
    /* A folder holding something open. */
    if(QFileInfo(Path).isDir())
    {
        if(!OpenWorld.isEmpty() && IsInside(QDir::fromNativeSeparators(QFileInfo(OpenWorld).absoluteFilePath()), Path))
        {
            return QStringLiteral("it holds the open world");
        }
    }
    return QString();
}

void ContentBrowserPanel::NewFolder()
{
    QString Name = QStringLiteral("New Folder");
    for(int Suffix = 2; QFileInfo::exists(QDir(CurrentFolder).filePath(Name)); ++Suffix)
    {
        Name = QStringLiteral("New Folder %1").arg(Suffix);
    }
    const QModelIndex Made = Model->mkdir(Model->index(CurrentFolder), Name);
    if(!Made.isValid())
    {
        emit StatusMessage(QStringLiteral("Could not make a folder in %1").arg(QDir::toNativeSeparators(CurrentFolder)));
        return;
    }
    const QModelIndex InList = ListProxy->mapFromSource(Made);
    List->setCurrentIndex(InList);
    List->edit(InList);
}

void ContentBrowserPanel::RenameSelected()
{
    const QModelIndexList Selected = List->selectionModel()->selectedIndexes();
    if(Selected.size() == 1) { List->edit(Selected.front()); }
}

bool ContentBrowserPanel::RenameItem(const QString& InPath, const QString& InNewName)
{
    if(InNewName.isEmpty() || InNewName == QFileInfo(InPath).fileName()) { return false; }
    if(InNewName.contains(QLatin1Char('/')) || InNewName.contains(QLatin1Char('\\')))
    {
        emit StatusMessage(QStringLiteral("A name cannot contain a slash"));
        return false;
    }
    const QString Why = WhyProtected(InPath);
    if(!Why.isEmpty())
    {
        emit StatusMessage(QStringLiteral("Cannot rename %1: %2").arg(QFileInfo(InPath).fileName(), Why));
        return false;
    }

    const QString Target = QFileInfo(InPath).absoluteDir().filePath(InNewName);
    const QStringList Companions = CompanionsOf(InPath);
    if(!RenamePath(InPath, Target))
    {
        emit StatusMessage(QStringLiteral("Could not rename %1 to %2 -- is the name taken?").arg(QFileInfo(InPath).fileName(), InNewName));
        return false;
    }
    for(const QString& Companion : Companions)
    {
        if(!RenamePath(Companion, CompanionTarget(Companion, InPath, Target)))
        {
            emit StatusMessage(QStringLiteral("Renamed, but %1 could not follow").arg(QFileInfo(Companion).fileName()));
        }
    }
    if(IsAsset(InPath))
    {
        EditorContent::UnstageAsset(InPath);
        EditorContent::StageAsset(Target);
    }
    emit StatusMessage(QStringLiteral("Renamed to %1").arg(InNewName));
    return true;
}

void ContentBrowserPanel::DeleteSelected()
{
    QStringList Paths = SelectedPaths();
    if(Paths.isEmpty()) { return; }

    for(const QString& Path : Paths)
    {
        const QString Why = WhyProtected(Path);
        if(!Why.isEmpty())
        {
            QMessageBox::information(this, QStringLiteral("Delete"),
                                     QStringLiteral("%1 cannot be deleted: %2.").arg(QFileInfo(Path).fileName(), Why));
            return;
        }
    }

    QStringList All;
    for(const QString& Path : Paths) { All << Path << CompanionsOf(Path); }
    All.removeDuplicates();

    QStringList Names;
    for(const QString& Path : All) { Names << QFileInfo(Path).fileName() + (QFileInfo(Path).isDir() ? QStringLiteral("/") : QString()); }
    const QString Listing = Names.size() <= 12 ? Names.join(QLatin1Char('\n'))
                                               : Names.mid(0, 12).join(QLatin1Char('\n')) + QStringLiteral("\n... and %1 more").arg(Names.size() - 12);
    if(QMessageBox::question(this, QStringLiteral("Delete"),
                             QStringLiteral("Move to the Recycle Bin?\n\n%1").arg(Listing)) != QMessageBox::Yes)
    {
        return;
    }

    int Failed = 0;
    for(const QString& Path : All)
    {
        if(!QFileInfo::exists(Path)) { continue; }   // inside a folder already moved
        if(QFile::moveToTrash(Path))
        {
            if(IsAsset(Path)) { EditorContent::UnstageAsset(Path); }
        }
        else
        {
            ++Failed;
        }
    }
    emit StatusMessage(Failed == 0 ? QStringLiteral("Moved %1 item(s) to the Recycle Bin").arg(All.size())
                                   : QStringLiteral("%1 item(s) could not be moved to the Recycle Bin").arg(Failed));
}

void ContentBrowserPanel::ReimportSelected()
{
    int Done = 0;
    QStringList Problems;
    for(const QString& Path : SelectedPaths())
    {
        if(!IsAsset(Path)) { continue; }
        const EditorContent::ImportResult Result = EditorContent::Reimport(Path, Root);
        CORE_LOG("[Import] " + Result.Message.toStdString())
        if(Result.bOK)
        {
            for(const QString& Written : Result.Written) { EditorContent::StageAsset(Written); }
            ++Done;
        }
        else
        {
            Problems << Result.Message;
        }
    }
    if(!Problems.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Reimport"), Problems.join(QLatin1Char('\n')));
    }
    emit StatusMessage(QStringLiteral("Reimported %1 asset(s)").arg(Done));
}

void ContentBrowserPanel::HandleDrop(const QStringList& InFiles, const QString& InTargetFolder)
{
    QStringList Inside, Outside;
    for(const QString& File : InFiles)
    {
        (IsInside(QDir::fromNativeSeparators(File), Root) ? Inside : Outside) << File;
    }
    if(!Inside.isEmpty()) { MoveInto(Inside, InTargetFolder); }
    if(Outside.isEmpty()) { return; }

    QStringList Importable, Plain;
    for(const QString& File : Outside)
    {
        (EditorContent::ClassifySource(File) != EditorContent::EImportKind::None ? Importable : Plain) << File;
    }
    if(!Importable.isEmpty()) { ImportFiles(Importable, InTargetFolder); }

    if(!Plain.isEmpty())
    {
        QStringList Names;
        for(const QString& File : Plain) { Names << QFileInfo(File).fileName(); }
        if(QMessageBox::question(this, QStringLiteral("Copy into the project"),
                                 QStringLiteral("These are not meshes or images to import. Copy them into %1 as they are?\n\n%2")
                                 .arg(QDir::toNativeSeparators(QDir(Root).relativeFilePath(InTargetFolder)), Names.join(QLatin1Char('\n'))))
           != QMessageBox::Yes)
        {
            return;
        }
        int Copied = 0;
        for(const QString& File : Plain)
        {
            const QString Target = QDir(InTargetFolder).filePath(QFileInfo(File).fileName());
            if(!QFileInfo::exists(Target) && QFileInfo(File).isFile() && QFile::copy(File, Target)) { ++Copied; }
        }
        emit StatusMessage(QStringLiteral("Copied %1 of %2 file(s); existing names were left alone").arg(Copied).arg(Plain.size()));
    }
}

void ContentBrowserPanel::MoveInto(const QStringList& InPaths, const QString& InTargetFolder)
{
    const QString Target = QDir::fromNativeSeparators(QDir(InTargetFolder).absolutePath());
    int Moved = 0;
    QStringList Problems;
    for(const QString& Path : InPaths)
    {
        const QFileInfo Info(Path);
        const QString From = QDir::fromNativeSeparators(Info.absoluteFilePath());
        if(Info.absolutePath().compare(Target, Qt::CaseInsensitive) == 0) { continue; }   // already there
        if(Info.isDir() && IsInside(Target, From))
        {
            Problems << QStringLiteral("%1 cannot go inside itself").arg(Info.fileName());
            continue;
        }
        const QString Why = WhyProtected(Path);
        if(!Why.isEmpty())
        {
            Problems << QStringLiteral("%1: %2").arg(Info.fileName(), Why);
            continue;
        }

        const QString To = QDir(Target).filePath(Info.fileName());
        const QStringList Companions = CompanionsOf(Path);
        if(!RenamePath(Path, To))
        {
            Problems << QStringLiteral("%1: %2 already has one").arg(Info.fileName(), QDir(Target).dirName());
            continue;
        }
        for(const QString& Companion : Companions)
        {
            if(!RenamePath(Companion, CompanionTarget(Companion, Path, To)))
            {
                Problems << QStringLiteral("%1 could not follow %2").arg(QFileInfo(Companion).fileName(), Info.fileName());
            }
        }
        if(IsAsset(Path))
        {
            EditorContent::UnstageAsset(Path);
            EditorContent::StageAsset(To);
        }
        ++Moved;
    }
    if(!Problems.isEmpty())
    {
        QMessageBox::information(this, QStringLiteral("Move"), Problems.join(QLatin1Char('\n')));
    }
    if(Moved > 0)
    {
        emit StatusMessage(QStringLiteral("Moved %1 item(s) to %2 -- anything naming them by path must be updated")
                           .arg(Moved).arg(QDir(Target).dirName()));
    }
}

// === Importing =======================================================================

void ContentBrowserPanel::ImportFiles(const QStringList& InFiles, const QString& InDestFolder)
{
    QDir().mkpath(InDestFolder);
    auto* Wizard = new ImportWizard(InFiles, InDestFolder, window());
    connect(Wizard, &ImportWizard::Imported, this, [this, InDestFolder](const QStringList& InWritten)
    {
        if(InWritten.isEmpty()) { return; }
        SetCurrentFolder(InDestFolder);
        SelectFiles(InWritten);
        emit StatusMessage(QStringLiteral("Imported %1 asset(s)").arg(InWritten.size()));
    });
    Wizard->show();
}

void ContentBrowserPanel::ImportWithDialog()
{
    const QStringList Files = QFileDialog::getOpenFileNames(this, QStringLiteral("Import"), QString(),
                                                            EditorContent::GetImportFileFilter());
    if(!Files.isEmpty()) { ImportFiles(Files, GetImportFolder()); }
}

#endif // WITH_EDITOR
