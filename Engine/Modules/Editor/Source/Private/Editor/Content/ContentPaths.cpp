#if WITH_EDITOR

#include "Private/Editor/Content/ContentPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>

#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace EditorContent
{
    namespace
    {
        /* InPath relative to InRoot, forward slashes; empty if outside it. */
        QString RelativeTo(const QString& InRoot, const QString& InPath)
        {
            if(InRoot.isEmpty()) { return QString(); }
            const QString Relative = QDir(InRoot).relativeFilePath(QFileInfo(InPath).absoluteFilePath());
            if(Relative.isEmpty() || Relative == QStringLiteral(".") || Relative.startsWith(QStringLiteral(".."))
               || QDir::isAbsolutePath(Relative))
            {
                return QString();
            }
            return QDir::fromNativeSeparators(Relative);
        }
    }

    QString ToRuntimePath(const QString& InPath)
    {
        /* The engine's first: a staged copy of it may sit inside the project's Binaries. */
        const QString Engine = RelativeTo(GetEngineContentRoot(), InPath);
        if(!Engine.isEmpty()) { return QStringLiteral("Engine/Content/") + Engine; }
        return RelativeTo(EditorGraphs::GetProjectRoot(), InPath);
    }

    QString GetEngineContentRoot()
    {
        static const QString Root = []()
        {
            const auto Clean = [](const QString& InPath) { return QDir::fromNativeSeparators(QDir(InPath).absolutePath()); };
#ifdef NIMBLEEDITOR_ENGINE_CONTENT_DIR
            const QString Built = QString::fromUtf8(NIMBLEEDITOR_ENGINE_CONTENT_DIR);
            if(!Built.isEmpty() && QFileInfo(Built).isDir()) { return Clean(Built); }
#endif
            const QString Staged = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Engine/Content"));
            return QFileInfo(Staged).isDir() ? Clean(Staged) : QString();
        }();
        return Root;
    }

    bool IsEngineContent(const QString& InPath)
    {
        const QString Root = GetEngineContentRoot();
        if(Root.isEmpty()) { return false; }
        const QString Path = QDir::fromNativeSeparators(QFileInfo(InPath).absoluteFilePath());
        return Path.compare(Root, Qt::CaseInsensitive) == 0 || Path.startsWith(Root + QLatin1Char('/'), Qt::CaseInsensitive);
    }

    QStringList ListMaterialNames()
    {
        QStringList Names;
        const QString Engine = GetEngineContentRoot();
        for(const QString& Folder : { QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Materials")),
                                      Engine.isEmpty() ? QString() : QDir(Engine).filePath(QStringLiteral("Materials")) })
        {
            if(Folder.isEmpty() || !QFileInfo(Folder).isDir()) { continue; }
            QDirIterator It(Folder, { QStringLiteral("*.material.nmgraph"), QStringLiteral("*.material") }, QDir::Files,
                            QDirIterator::Subdirectories);
            while(It.hasNext())
            {
                const QString Name = MaterialNameFor(It.next());
                if(!Name.isEmpty()) { Names << Name; }
            }
        }
        Names.removeDuplicates();
        Names.sort(Qt::CaseInsensitive);
        return Names;
    }

    QString MaterialNameFor(const QString& InPath)
    {
        const QString Name = QFileInfo(InPath).fileName();
        const QString Lower = Name.toLower();
        for(const QString& Suffix : { QStringLiteral(".material.nmgraph"), QStringLiteral(".material"), QStringLiteral(".mtl.glsl") })
        {
            if(Lower.endsWith(Suffix)) { return Name.left(Name.size() - Suffix.size()); }
        }
        return QString();
    }

    QString SingleDroppedFile(const QMimeData* InData)
    {
        if(!InData || !InData->hasUrls() || InData->urls().size() != 1)
        {
            return QString();
        }
        const QUrl Url = InData->urls().front();
        return Url.isLocalFile() ? Url.toLocalFile() : QString();
    }

    void StageAsset(const QString& InPath)
    {
        const QString Staged = EditorGraphs::GetStagedPath(InPath);
        if(!Staged.isEmpty())
        {
            QDir().mkpath(QFileInfo(Staged).absolutePath());
            QFile::remove(Staged);
            QFile::copy(InPath, Staged);
        }
        const QString Runtime = ToRuntimePath(InPath);
        if(!Runtime.isEmpty())
        {
            RenderCore::MeshAssetCache::Invalidate(Runtime.toStdString());
            RenderCore::TextureAssetCache::Invalidate(Runtime.toStdString());
        }
    }

    void UnstageAsset(const QString& InPath)
    {
        const QString Staged = EditorGraphs::GetStagedPath(InPath);
        if(!Staged.isEmpty())
        {
            QFile::remove(Staged);
        }
        const QString Runtime = ToRuntimePath(InPath);
        if(!Runtime.isEmpty())
        {
            RenderCore::MeshAssetCache::Invalidate(Runtime.toStdString());
            RenderCore::TextureAssetCache::Invalidate(Runtime.toStdString());
        }
    }
}

#endif // WITH_EDITOR
