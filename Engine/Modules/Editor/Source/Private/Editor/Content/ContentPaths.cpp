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
#include "Public/RenderCore/RenderTargetAsset.h"
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

    namespace
    {
        const QString MATERIAL_GRAPH     = QStringLiteral(".material.nmgraph");
        const QString SCRIPT_GRAPH       = QStringLiteral(".script.nmgraph");
        const QString POST_PROCESS_GRAPH = QStringLiteral(".pp.nmgraph");

        /* Every file under InDir that InPathFor gives a value for. */
        void FindGraphs(const QDir& InDir, QString (*InPathFor)(const QString&), QStringList& OutPaths)
        {
            for(const QFileInfo& Entry : InDir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name))
            {
                if(Entry.isDir())
                {
                    if(!EditorGraphs::IsIgnoredFolder(Entry.fileName())) { FindGraphs(QDir(Entry.absoluteFilePath()), InPathFor, OutPaths); }
                    continue;
                }
                const QString Path = InPathFor(Entry.absoluteFilePath());
                if(!Path.isEmpty()) { OutPaths << Path; }
            }
        }

        QStringList ListGraphs(QString (*InPathFor)(const QString&))
        {
            QStringList Paths;
            const QString Engine = GetEngineContentRoot();
            FindGraphs(QDir(EditorGraphs::GetProjectRoot()), InPathFor, Paths);
            if(!Engine.isEmpty()) { FindGraphs(QDir(Engine), InPathFor, Paths); }
            Paths.removeDuplicates();
            Paths.sort(Qt::CaseInsensitive);
            return Paths;
        }
    }

    QStringList ListMaterialPaths()
    {
        return ListGraphs(&MaterialPathFor);
    }

    QStringList ListPostProcessPaths()
    {
        return ListGraphs([](const QString& InPath)
        {
            return InPath.endsWith(POST_PROCESS_GRAPH, Qt::CaseInsensitive) ? ToRuntimePath(InPath) : QString();
        });
    }

    QString PostProcessPathFor(const QString& InPath)
    {
        if(InPath.endsWith(POST_PROCESS_GRAPH, Qt::CaseInsensitive)) { return ToRuntimePath(InPath); }
        return MaterialPathFor(InPath);
    }

    bool IsPostProcessFieldValue(const QString& InText)
    {
        return IsMaterialFieldValue(InText) || (InText.endsWith(POST_PROCESS_GRAPH) && InText.size() > POST_PROCESS_GRAPH.size());
    }

    QStringList ListTexturePaths()
    {
        return ListGraphs(&TexturePathFor);
    }

    QString TexturePathFor(const QString& InPath)
    {
        return InPath.endsWith(QStringLiteral(".nmtexture"), Qt::CaseInsensitive) ? ToRuntimePath(InPath) : QString();
    }

    QStringList ListRenderTargetPaths()
    {
        return ListGraphs(&RenderTargetPathFor);
    }

    QString RenderTargetPathFor(const QString& InPath)
    {
        const bool bTarget = InPath.endsWith(QStringLiteral(".nmrt2d"), Qt::CaseInsensitive)
                          || InPath.endsWith(QStringLiteral(".nmrt3d"), Qt::CaseInsensitive);
        return bTarget ? ToRuntimePath(InPath) : QString();
    }

    bool IsRenderTargetFieldValue(const QString& InText)
    {
        return InText.isEmpty() || RenderCore::IsRenderTargetPath(InText.toStdString());
    }

    QString MaterialPathFor(const QString& InPath)
    {
        if(InPath.isEmpty() || !InPath.endsWith(MATERIAL_GRAPH, Qt::CaseInsensitive)) { return QString(); }
        return ToRuntimePath(InPath);
    }

    bool IsMaterialFieldValue(const QString& InText)
    {
        return InText.isEmpty() || (InText.endsWith(MATERIAL_GRAPH) && InText.size() > MATERIAL_GRAPH.size());
    }

    QString ScriptPathFor(const QString& InPath)
    {
        if(InPath.isEmpty()) { return QString(); }
        if(!InPath.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive) && !InPath.endsWith(SCRIPT_GRAPH, Qt::CaseInsensitive))
        {
            return QString();
        }
        return ToRuntimePath(InPath);
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
            RenderCore::RenderTargetAssetCache::Invalidate(Runtime.toStdString());
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
            RenderCore::RenderTargetAssetCache::Invalidate(Runtime.toStdString());
        }
    }
}

#endif // WITH_EDITOR
