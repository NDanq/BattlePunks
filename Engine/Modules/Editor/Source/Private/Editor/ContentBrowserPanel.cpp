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
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "CoreQtConversions.h"
#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/NodeGraph/Schema.h"
#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/ParticleSystemAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Content/RenderTargetAssets.h"
#include "Private/Editor/Particles/ParticleAssets.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Private/Editor/UI/UIAssets.h"
#include "Private/Editor/Content/AssetImport.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Content/ImportWizard.h"
#include "Private/Editor/Content/AnimationAssets.h"
#include "Private/Editor/Content/MeshViewerWindow.h"
#include "Private/Editor/Content/TextureViewerWindow.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace
{
    constexpr int ThumbnailSize = 64;

    /* The engine's content folder as the tree and the path bar name it. */
    const char* const ENGINE_CONTENT_LABEL = "Engine Content";

    /* What a material's thumbnail draws it on. */
    const QString PREVIEW_SPHERE = QStringLiteral("Engine/Content/Meshes/Sphere.nmmesh");

    /* Thumbnails and tooltips past this size are not read on hover. */
    constexpr qint64 PreviewByteLimit = 64 * 1024 * 1024;

    /* InRoot may already end in a slash: QFileSystemModel's top row is "/" on
     * Linux, and appending another would make nothing inside it. */
    bool IsInside(const QString& InPath, const QString& InRoot)
    {
        const QString Prefix = InRoot.endsWith(QLatin1Char('/')) ? InRoot : InRoot + QLatin1Char('/');
        return InPath.compare(InRoot, Qt::CaseInsensitive) == 0
            || InPath.startsWith(Prefix, Qt::CaseInsensitive);
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

    /* Existing files that travel with InPath: a world's entity folder. (A
     * graph's generated code is inside it.) */
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

        if(Name.endsWith(QStringLiteral(".nmworld"), Qt::CaseInsensitive))
        {
            const QString Entities = Folder.filePath(Info.completeBaseName());
            if(QFileInfo(Entities).isDir()) { Out << Entities; }
        }
        return Out;
    }

    /* InCompanion renamed as its owner was (Level.nmworld's Level/ -> Arena/). */
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
            || InPath.endsWith(QStringLiteral(".nmtexture"), Qt::CaseInsensitive)
            || InPath.endsWith(QStringLiteral(".nmskel"), Qt::CaseInsensitive)
            || InPath.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive)
            || InPath.endsWith(QStringLiteral(".nmanimstate"), Qt::CaseInsensitive)
            || InPath.endsWith(QStringLiteral(".nmrt2d"), Qt::CaseInsensitive)
            || InPath.endsWith(QStringLiteral(".nmrt3d"), Qt::CaseInsensitive);
    }
}

// === Models ==========================================================================

/* The trees: folders only -- each root (the project's, the engine's
 * content), the folders leading to it, and its folders, minus the ones
 * IsIgnoredFolder names. Each tree view starts at its own root's parent. */
class ContentTreeProxy final : public QSortFilterProxyModel
{
public:
    ContentTreeProxy(const QString& InRoot, const QString& InEngineRoot, QObject* InParent)
        : QSortFilterProxyModel(InParent), Root(InRoot), EngineRoot(InEngineRoot) {}

    QVariant data(const QModelIndex& InIndex, const int InRole) const override
    {
        if(InRole == Qt::DisplayRole && InIndex.column() == 0 && !EngineRoot.isEmpty())
        {
            const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
            if(Source->filePath(mapToSource(InIndex)).compare(EngineRoot, Qt::CaseInsensitive) == 0)
            {
                return QString::fromLatin1(ENGINE_CONTENT_LABEL);
            }
        }
        return QSortFilterProxyModel::data(InIndex, InRole);
    }

protected:
    bool filterAcceptsRow(const int InRow, const QModelIndex& InParent) const override
    {
        const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
        const QModelIndex Index = Source->index(InRow, 0, InParent);
        if(!Source->isDir(Index)) { return false; }

        const QString Path = Source->filePath(Index);
        for(const QString& Each : { Root, EngineRoot })
        {
            if(Each.isEmpty()) { continue; }
            if(IsInside(Each, Path)) { return true; }   // the root or a folder above it
            if(IsInside(Path, Each)) { return !EditorGraphs::IsIgnoredFolder(Source->fileName(Index)); }
        }
        return false;                                   // somewhere else on the disk
    }

    bool lessThan(const QModelIndex& InLeft, const QModelIndex& InRight) const override
    {
        const auto* Source = static_cast<const QFileSystemModel*>(sourceModel());
        return Source->fileName(InLeft).compare(Source->fileName(InRight), Qt::CaseInsensitive) < 0;
    }

private:
    QString Root;
    QString EngineRoot;
};

/* One folder's contents: folders first, name filter, thumbnails, asset
 * tooltips; renames go through the panel so companions follow. */
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

        const QString Base = Panel->RootOf(Path);
        if(Base.isEmpty() || Path.compare(Base, Qt::CaseInsensitive) == 0) { return true; }
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
        bool    bRendered     = false;   // a mesh or material: its thumbnail is rendered
        bool    bRenderQueued = false;   // waiting for or in RenderNext
        qint64  RetryAt       = 0;       // after a failed render, not before this
    };

    /* A mesh's thumbnail is rendered (Renderer::RequestMeshPreview), and a
     * material's is the Sphere wearing it, so they come later: queued on
     * first view, one request at a time since each readback stalls the
     * renderer, and only while its folder is shown. */
    void QueueRender(const QString& InPath, Preview& InEntry) const
    {
        if(InEntry.bRenderQueued || QDateTime::currentMSecsSinceEpoch() < InEntry.RetryAt) { return; }
        InEntry.bRenderQueued = true;
        RenderQueue.append(InPath);
        RenderNext();
    }

    void RenderNext() const
    {
        while(!bRenderInFlight && !RenderQueue.isEmpty())
        {
            const QString Path = RenderQueue.takeFirst();
            auto Found = Previews.find(Path);
            if(Found == Previews.end()) { continue; }
            const QString Runtime = EditorContent::ToRuntimePath(Path);
            if(Runtime.isEmpty() || QFileInfo(Path).absolutePath().compare(Panel->GetCurrentFolder(), Qt::CaseInsensitive) != 0)
            {
                Found->bRenderQueued = false;   // again when its folder is shown
                continue;
            }

            const QString Material = EditorContent::MaterialPathFor(Path);
            if(!Material.isEmpty() && EditorGraphs::IsMaterialCooking(Material))
            {
                /* Just saved: drawn once the cook is in, not before. */
                Found->bRenderQueued = false;
                auto* Self = const_cast<ContentListProxy*>(this);
                QTimer::singleShot(500, Self, [Self, Path]()
                {
                    const auto* Source = static_cast<const QFileSystemModel*>(Self->sourceModel());
                    const QModelIndex Shown = Self->mapFromSource(Source->index(Path));
                    if(Shown.isValid()) { emit Self->dataChanged(Shown, Shown, { Qt::DecorationRole }); }
                });
                continue;
            }

            bRenderInFlight = true;
            const qreal Ratio = Panel->devicePixelRatioF();
            EditorGraphs::MeshPreviewOptions Options;
            Options.Mesh     = Material.isEmpty() ? Runtime : PREVIEW_SPHERE;
            Options.Material = Material;
            Options.Width    = int(ThumbnailSize * Ratio);
            Options.Height   = Options.Width;
            const qint64 Stamp = Found->Stamp;
            auto* Self = const_cast<ContentListProxy*>(this);
            EditorGraphs::RequestMeshPreview(Options, Self, [Self, Path, Stamp, Ratio](const QImage& InImage, const QString&)
            {
                Self->bRenderInFlight = false;
                auto Entry = Self->Previews.find(Path);
                if(Entry != Self->Previews.end()) { Entry->bRenderQueued = false; }
                if(Entry != Self->Previews.end() && !InImage.isNull())
                {
                    /* Of an older version: dropped, but the repaint asks for the new one. */
                    if(Entry->Stamp == Stamp)
                    {
                        QPixmap Pixmap = QPixmap::fromImage(InImage);
                        Pixmap.setDevicePixelRatio(Ratio);
                        Entry->Thumbnail = QIcon(Pixmap);
                    }
                    const auto* Source = static_cast<const QFileSystemModel*>(Self->sourceModel());
                    const QModelIndex Shown = Self->mapFromSource(Source->index(Path));
                    if(Shown.isValid()) { emit Self->dataChanged(Shown, Shown, { Qt::DecorationRole }); }
                }
                else if(Entry != Self->Previews.end() && Entry->Stamp == Stamp)
                {
                    /* The renderer may not be up yet; the next view asks again. */
                    Entry->RetryAt = QDateTime::currentMSecsSinceEpoch() + 2000;
                }
                Self->RenderNext();
            });
        }
    }

    /* Made on first view, and again when the file changes. */
    const Preview& FindPreview(const QFileInfo& InInfo) const
    {
        const QString Key = InInfo.absoluteFilePath();
        const qint64 Stamp = InInfo.lastModified().toMSecsSinceEpoch() ^ InInfo.size();
        Preview& Entry = Previews[Key];
        if(Entry.Stamp == Stamp)
        {
            if(Entry.bRendered && Entry.Thumbnail.isNull()) { QueueRender(Key, Entry); }
            return Entry;
        }

        const bool bWasQueued = Entry.bRenderQueued;   // still in RenderQueue
        Entry = Preview();
        Entry.Stamp = Stamp;
        Entry.bRenderQueued = bWasQueued;
        if(InInfo.size() > PreviewByteLimit) { return Entry; }

        const QString Path = InInfo.absoluteFilePath();
        if(Path.endsWith(QStringLiteral(".nmtexture"), Qt::CaseInsensitive))
        {
            RenderCore::TextureData Texture;
            EngineCore::Archive Ar = ArchiveOf(ReadAll(Path));
            if(RenderCore::SerializeTexture(Ar, Texture))
            {
                /* The smallest mip still as big as the thumbnail: less to scale. */
                const EngineCore::Image* Source = &Texture.Pixels;
                for(int Level = 1; Level < Texture.GetMipCount(); ++Level)
                {
                    const EngineCore::Image* Mip = Texture.GetMip(Level);
                    if(Mip->GetWidth() < ThumbnailSize && Mip->GetHeight() < ThumbnailSize) { break; }
                    Source = Mip;
                }
                const QImage Image = EngineCore::QtBridge::ToQImage(*Source);
                Entry.Thumbnail = QIcon(QPixmap::fromImage(Image.scaled(ThumbnailSize, ThumbnailSize, Qt::KeepAspectRatio,
                                                                        Qt::SmoothTransformation)));
                Entry.Summary = QStringLiteral("Texture, %1 x %2, %3, %4%5%6")
                                .arg(Texture.Pixels.GetWidth()).arg(Texture.Pixels.GetHeight())
                                .arg(Texture.bSRGB ? QStringLiteral("sRGB") : QStringLiteral("linear"))
                                .arg(Texture.GetMipCount() > 1 ? QStringLiteral("%1 mips").arg(Texture.GetMipCount()) : QStringLiteral("no mips"))
                                .arg(Texture.Compression == RenderCore::ETextureCompression::None ? QString()
                                     : QStringLiteral(", %1 (pending)").arg(QString::fromLatin1(RenderCore::GetTextureCompressionName(Texture.Compression))))
                                .arg(Texture.ImportInfo.IsEmpty() ? QString()
                                     : QStringLiteral("\nFrom %1").arg(QString::fromStdString(Texture.ImportInfo.SourcePath)));
            }
            else
            {
                Entry.Summary = QStringLiteral("Not a texture this editor can read");
            }
        }
        else if(!EditorContent::RenderTargetPathFor(Path).isEmpty())
        {
            /* Its bake, if it has one, as the thumbnail. */
            RenderCore::RenderTargetData Target;
            EngineCore::Archive Ar = ArchiveOf(ReadAll(Path));
            if(RenderCore::SerializeRenderTarget(Ar, Target) && !Target.Baked.IsNull())
            {
                const QImage Image = EngineCore::QtBridge::ToQImage(Target.Baked);
                Entry.Thumbnail = QIcon(QPixmap::fromImage(Image.scaled(ThumbnailSize, ThumbnailSize, Qt::KeepAspectRatio,
                                                                        Qt::SmoothTransformation)));
            }
            Entry.Summary = QStringLiteral("Render target: %1\nA Scene Capture Probe renders into it; double-click for its settings")
                            .arg(EditorRenderTargets::DescribeRenderTarget(Path));
        }
        else if(Path.endsWith(QStringLiteral(".nmmesh"), Qt::CaseInsensitive))
        {
            RenderCore::MeshData Mesh;
            EngineCore::Archive Ar = ArchiveOf(ReadAll(Path));
            if(RenderCore::SerializeMesh(Ar, Mesh))
            {
                Entry.Summary = QStringLiteral("%8, %1 vertices, %2 triangles, %3 section(s)\nSize %4 x %5 x %6%7")
                                .arg(Mesh.GetVertexCount()).arg(Mesh.GetTriangleCount()).arg(Mesh.Sections.size())
                                .arg(double(Mesh.BoundsMax[0] - Mesh.BoundsMin[0]), 0, 'g', 4)
                                .arg(double(Mesh.BoundsMax[1] - Mesh.BoundsMin[1]), 0, 'g', 4)
                                .arg(double(Mesh.BoundsMax[2] - Mesh.BoundsMin[2]), 0, 'g', 4)
                                .arg(Mesh.ImportInfo.IsEmpty() ? QString()
                                     : QStringLiteral("\nFrom %1").arg(QString::fromStdString(Mesh.ImportInfo.SourcePath)))
                                .arg(Mesh.IsSkinned() ? QStringLiteral("Skinned mesh (%1)").arg(QString::fromStdString(Mesh.Skeleton))
                                                      : QStringLiteral("Mesh"));
                Entry.bRendered = true;
                QueueRender(Key, Entry);
            }
            else
            {
                Entry.Summary = QStringLiteral("Not a mesh this editor can read");
            }
        }
        else if(Path.endsWith(QStringLiteral(".nmskel"), Qt::CaseInsensitive))
        {
            RenderCore::SkeletonData Skeleton;
            EngineCore::Archive Ar = ArchiveOf(ReadAll(Path));
            Entry.Summary = RenderCore::SerializeSkeleton(Ar, Skeleton)
                          ? QStringLiteral("Skeleton, %1 bones%2").arg(Skeleton.Bones.size())
                            .arg(Skeleton.ImportInfo.IsEmpty() ? QString()
                                 : QStringLiteral("\nFrom %1").arg(QString::fromStdString(Skeleton.ImportInfo.SourcePath)))
                          : QStringLiteral("Not a skeleton this editor can read");
        }
        else if(Path.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive))
        {
            RenderCore::AnimClipData Clip;
            EngineCore::Archive Ar = ArchiveOf(ReadAll(Path));
            Entry.Summary = RenderCore::SerializeAnimClip(Ar, Clip)
                          ? QStringLiteral("Animation clip, %1 s (%2 frames at %3 fps), %4 tracks, %5 notifies%6\nDouble-click to preview")
                            .arg(double(Clip.GetDuration()), 0, 'g', 4).arg(Clip.FrameCount).arg(double(Clip.SampleRate))
                            .arg(Clip.Tracks.size()).arg(Clip.Notifies.size())
                            .arg(Clip.bLoop ? QStringLiteral(", looping") : QString())
                          : QStringLiteral("Not an animation clip this editor can read");
        }
        else if(Path.endsWith(QStringLiteral(".nmanimstate"), Qt::CaseInsensitive))
        {
            RenderCore::AnimStateData State;
            EngineCore::Archive Ar = ArchiveOf(ReadAll(Path));
            if(RenderCore::SerializeAnimState(Ar, State))
            {
                size_t States = 0;
                for(const RenderCore::AnimLayer& Layer : State.Layers) { States += Layer.States.size(); }
                Entry.Summary = QStringLiteral("Anim state, %1 layer(s), %2 state(s), %3 parameter(s)\nDouble-click to edit")
                                .arg(State.Layers.size()).arg(States).arg(State.Parameters.size());
            }
            else
            {
                Entry.Summary = QStringLiteral("Not an anim state this editor can read");
            }
        }
        else if(!EditorContent::MaterialPathFor(Path).isEmpty())
        {
            Entry.Summary = QStringLiteral("Material graph\nDouble-click to edit");
            Entry.bRendered = true;
            QueueRender(Key, Entry);
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
    mutable QStringList               RenderQueue;
    mutable bool                      bRenderInFlight = false;
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
    , EngineRoot(EditorContent::GetEngineContentRoot())
{
    Model = new QFileSystemModel(this);
    Model->setReadOnly(false);   // renames, through ContentListProxy::setData
    Model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    Model->setRootPath(Root);

    TreeProxy = new ContentTreeProxy(Root, EngineRoot, this);
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

    /* The engine's content, below the project's: its meshes (Box, Sphere,
     * Plane), materials and shaders, to drag into the level or a Details
     * field. Read-only here. */
    EngineTree = new QTreeView(this);
    EngineTree->setModel(TreeProxy);
    for(int Column = 1; Column < Model->columnCount(); ++Column) { EngineTree->hideColumn(Column); }
    EngineTree->setHeaderHidden(true);
    EngineTree->setDragDropMode(QAbstractItemView::NoDragDrop);
    EngineTree->setToolTip(QStringLiteral("The engine's own content, read-only here: drag its meshes and materials "
                                          "into the level or a Details field"));
    if(!EngineRoot.isEmpty())
    {
        EngineTree->setRootIndex(TreeProxy->mapFromSource(Model->index(QFileInfo(EngineRoot).absolutePath())));
        EngineTree->expand(TreeProxy->mapFromSource(Model->index(EngineRoot)));
    }
    else
    {
        EngineTree->hide();
    }
    connect(EngineTree, &QTreeView::clicked, this, [this](const QModelIndex& InIndex)
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

    auto* Trees = new QSplitter(Qt::Vertical, this);
    Trees->addWidget(Tree);
    Trees->addWidget(EngineTree);
    Trees->setStretchFactor(0, 3);
    Trees->setStretchFactor(1, 1);

    auto* Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Trees);
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
    if(RootOf(Folder).isEmpty() || !QFileInfo(Folder).isDir()) { Folder = Root; }
    if(Folder.compare(CurrentFolder, Qt::CaseInsensitive) == 0) { return; }

    if(!CurrentFolder.isEmpty()) { History << CurrentFolder; }
    CurrentFolder = Folder;

    const QModelIndex Source = Model->index(CurrentFolder);
    List->setRootIndex(ListProxy->mapFromSource(Source));
    ListProxy->invalidate();   // the name filter applies to this folder now

    /* In whichever tree holds it; the other shows nothing current. */
    const QModelIndex InTree = TreeProxy->mapFromSource(Source);
    QTreeView* Shown = IsEngine(CurrentFolder) ? EngineTree : Tree;
    QTreeView* Other = Shown == Tree ? EngineTree : Tree;
    Other->selectionModel()->clearSelection();
    Other->setCurrentIndex(QModelIndex());
    Shown->setCurrentIndex(InTree);
    Shown->scrollTo(InTree);
    Shown->expand(InTree);

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
    if(CurrentFolder.compare(RootOf(CurrentFolder), Qt::CaseInsensitive) == 0) { return; }
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

    const QString Base = RootOf(CurrentFolder).isEmpty() ? Root : RootOf(CurrentFolder);
    QStringList Parts = QDir(Base).relativeFilePath(CurrentFolder).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if(Parts.size() == 1 && Parts.front() == QStringLiteral(".")) { Parts.clear(); }

    QString Path = Base;
    const auto AddSegment = [this](const QString& InText, const QString& InPath, const bool bInLast)
    {
        auto* Segment = new QToolButton(this);
        Segment->setText(InText);
        Segment->setAutoRaise(true);
        if(bInLast) { Segment->setStyleSheet(QStringLiteral("font-weight: bold;")); }
        connect(Segment, &QToolButton::clicked, this, [this, InPath]() { SetCurrentFolder(InPath); });
        PathBar->addWidget(Segment);
    };

    AddSegment(Base == EngineRoot ? QString::fromLatin1(ENGINE_CONTENT_LABEL) : QFileInfo(Root).fileName(), Base, Parts.isEmpty());
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
    const bool bAny    = !SelectedPaths().isEmpty();
    const bool bEngine = IsEngine(CurrentFolder);   // read-only here
    BackButton->setEnabled(!History.isEmpty());
    UpButton->setEnabled(CurrentFolder.compare(RootOf(CurrentFolder), Qt::CaseInsensitive) != 0);
    RenameAction->setEnabled(SelectedPaths().size() == 1 && !bEngine);
    DeleteAction->setEnabled(bAny && !bEngine);
}

QString ContentBrowserPanel::GetImportFolder() const
{
    if(CurrentFolder.compare(Root, Qt::CaseInsensitive) != 0 && !IsEngine(CurrentFolder)) { return CurrentFolder; }
    return QDir(Root).filePath(QStringLiteral("Content"));
}

QString ContentBrowserPanel::RootOf(const QString& InPath) const
{
    const QString Path = QDir::fromNativeSeparators(InPath);
    if(!EngineRoot.isEmpty() && IsInside(Path, EngineRoot)) { return EngineRoot; }   // first: it may lie in the project's Binaries
    if(IsInside(Path, Root)) { return Root; }
    return QString();
}

bool ContentBrowserPanel::IsEngine(const QString& InPath) const
{
    return !EngineRoot.isEmpty() && RootOf(QDir::fromNativeSeparators(QFileInfo(InPath).absoluteFilePath())) == EngineRoot;
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
    if(InPath.endsWith(QStringLiteral(".nmprefab"), Qt::CaseInsensitive))
    {
        EditorPrefabs::OpenPrefab(InPath, window());
        return;
    }
    if(InPath.endsWith(QStringLiteral(".nmui"), Qt::CaseInsensitive))
    {
        EditorUI::OpenUI(InPath, window());
        return;
    }
    if(InPath.endsWith(QString::fromLatin1(RenderCore::PARTICLE_SYSTEM_EXTENSION), Qt::CaseInsensitive))
    {
        EditorParticles::OpenParticleSystem(InPath, window());
        return;
    }
    if(InPath.endsWith(QStringLiteral(".nmmesh"), Qt::CaseInsensitive))
    {
        MeshViewerWindow::Open(InPath, window());
        return;
    }
    if(InPath.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive))
    {
        MeshViewerWindow::OpenAnimation(InPath, window());
        return;
    }
    if(InPath.endsWith(QStringLiteral(".nmanimstate"), Qt::CaseInsensitive))
    {
        EditorAnimation::OpenAnimState(InPath, window());
        return;
    }
    if(InPath.endsWith(QStringLiteral(".nmtexture"), Qt::CaseInsensitive))
    {
        TextureViewerWindow::Open(InPath, window());
        return;
    }
    if(!EditorContent::RenderTargetPathFor(InPath).isEmpty())
    {
        EditorRenderTargets::EditRenderTarget(InPath, window());
        return;
    }
    if(IsAsset(InPath))
    {
        emit StatusMessage(QStringLiteral("Drag %1 onto a Texture Sample's Texture, in a material graph").arg(Info.fileName()));
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
    const bool bEngine = IsEngine(CurrentFolder);
    bool bAnyAsset = false;
    for(const QString& Path : Selected) { bAnyAsset |= IsAsset(Path); }
    if(bAnyAsset && !bEngine)
    {
        Menu.addAction(QStringLiteral("Reimport"), this, &ContentBrowserPanel::ReimportSelected);
        Menu.addSeparator();
    }
    if(!Selected.isEmpty() && !bEngine)
    {
        Menu.addAction(RenameAction);
        Menu.addAction(DeleteAction);
        Menu.addSeparator();
    }
    if(!bEngine)
    {
        Menu.addAction(QStringLiteral("New Folder"), this, &ContentBrowserPanel::NewFolder);
        Menu.addAction(QStringLiteral("New Prefab..."), this, [this]() { EditorPrefabs::CreatePrefab(CurrentFolder, window()); });
        Menu.addAction(QStringLiteral("New UI..."), this, [this]() { EditorUI::CreateUI(CurrentFolder, window()); });
        Menu.addAction(QStringLiteral("New Particle System..."), this, [this]() { EditorParticles::CreateParticleSystem(CurrentFolder, window()); });
        Menu.addAction(QStringLiteral("New Anim State..."), this, [this]() { EditorAnimation::CreateAnimState(CurrentFolder, window()); });
        for(const NodeGraph::Schema* Schema : NodeGraph::GetSchemas())
        {
            /* By name: a refresh (RefreshGameDeclarations) replaces the schema. */
            const std::string SchemaName = Schema->GetName();
            QAction* NewGraph = Menu.addAction(QStringLiteral("New %1 Graph...").arg(QString::fromStdString(SchemaName)), this, [this, SchemaName]()
            {
                if(const NodeGraph::Schema* Current = NodeGraph::FindSchema(SchemaName)) { EditorGraphs::CreateGraph(*Current, window(), CurrentFolder); }
            });
            NewGraph->setToolTip(QStringLiteral("A %1 graph: generates %2")
                                 .arg(QString::fromStdString(SchemaName), QString::fromStdString(Schema->GetOutputExtension())));
        }
        Menu.addAction(QStringLiteral("New Render Target 2D..."), this, [this]()
        {
            EditorRenderTargets::CreateRenderTarget(CurrentFolder, RenderCore::ERenderTargetKind::Tex2D, window());
        });
        Menu.addAction(QStringLiteral("New Render Target 3D (Cube)..."), this, [this]()
        {
            EditorRenderTargets::CreateRenderTarget(CurrentFolder, RenderCore::ERenderTargetKind::Cube, window());
        });
    }
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
    if(IsEngine(Path))
    {
        return QStringLiteral("it is the engine's content, which the editor does not change");
    }
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
    if(IsEngine(CurrentFolder))
    {
        emit StatusMessage(QStringLiteral("The engine's content is read-only here"));
        return;
    }
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
        if(!IsAsset(Path) || IsEngine(Path)) { continue; }
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
    if(IsEngine(InTargetFolder))
    {
        emit StatusMessage(QStringLiteral("The engine's content is read-only here: drop into the project's folders"));
        return;
    }

    /* The project's own move; anything else -- the engine's content too -- is copied or imported. */
    QStringList Inside, Outside;
    for(const QString& File : InFiles)
    {
        (RootOf(QDir::fromNativeSeparators(QFileInfo(File).absoluteFilePath())) == Root ? Inside : Outside) << File;
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
