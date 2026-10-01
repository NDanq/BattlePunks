#if WITH_EDITOR

#include "Private/Editor/Content/ContentPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>

#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace EditorContent
{
    QString ToRuntimePath(const QString& InPath)
    {
        const QString Relative = QDir(EditorGraphs::GetProjectRoot()).relativeFilePath(QFileInfo(InPath).absoluteFilePath());
        if(Relative.isEmpty() || Relative.startsWith(QStringLiteral("..")) || QDir::isAbsolutePath(Relative))
        {
            return QString();
        }
        return QDir::fromNativeSeparators(Relative);
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
