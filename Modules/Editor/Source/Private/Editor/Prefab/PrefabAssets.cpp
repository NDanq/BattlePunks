#if WITH_EDITOR

#include "Private/Editor/Prefab/PrefabAssets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>

#include "Public/Core/CoreLog.h"
#include "Public/Editor/EditorContext.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Prefab/PrefabEditorTab.h"
#include "Private/Editor/Prefab/PrefabEditorWindow.h"

namespace
{
    QPointer<EditorContext>& Context()
    {
        static QPointer<EditorContext> Current;
        return Current;
    }

    QString Canonical(const QString& InPath)
    {
        const QFileInfo Info(InPath);
        const QString Resolved = Info.canonicalFilePath();
        return QDir::cleanPath(Resolved.isEmpty() ? Info.absoluteFilePath() : Resolved);
    }

    const QString PrefabExtension = QStringLiteral(".nmprefab");
}

namespace EditorPrefabs
{
    void SetEditorContext(EditorContext* InContext)
    {
        Context() = InContext;
    }

    EditorContext* GetEditorContext()
    {
        return Context();
    }

    QString GetDefaultFolder()
    {
        return QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Prefabs"));
    }

    QString ToProjectPath(const QString& InRuntimePath)
    {
        return QDir(EditorGraphs::GetProjectRoot()).filePath(InRuntimePath);
    }

    QString ToRuntimePath(const QString& InProjectPath)
    {
        return EditorContent::ToRuntimePath(InProjectPath);
    }

    void Stage(const QString& InPath)
    {
        const QString Staged = EditorGraphs::GetStagedPath(InPath);
        if(Staged.isEmpty())
        {
            return;
        }
        QDir().mkpath(QFileInfo(Staged).absolutePath());
        QFile::remove(Staged);
        if(!QFile::copy(InPath, Staged))
        {
            CORE_LOG_WARN("[Prefab] could not stage " + std::string(Staged.toUtf8().constData()))
        }
    }

    PrefabEditorTab* OpenPrefab(const QString& InPath, QWidget* InParent)
    {
        if(PrefabEditorWindow* Window = PrefabEditorWindow::Find())
        {
            const QString Key = Canonical(InPath);
            for(PrefabEditorTab* Each : Window->GetTabs())
            {
                if(Canonical(Each->GetPath()) == Key)
                {
                    Window->ShowTab(Each);
                    return Each;
                }
            }
        }

        EditorContext* Ctx = Context();
        if(!Ctx || !Ctx->IsRendererReady())
        {
            QMessageBox::warning(InParent, QStringLiteral("Open Prefab"), QStringLiteral("The renderer is not running."));
            return nullptr;
        }

        auto* Tab = new PrefabEditorTab(*Ctx);
        if(!Tab->Load(InPath))
        {
            delete Tab;
            QMessageBox::warning(InParent, QStringLiteral("Open Prefab"),
                                 QStringLiteral("%1 is not a readable prefab; the Console says why.").arg(QDir::toNativeSeparators(InPath)));
            return nullptr;
        }
        PrefabEditorWindow::Get().AddTab(Tab);
        return Tab;
    }

    PrefabEditorTab* CreatePrefab(const QString& InFolder, QWidget* InParent)
    {
        const QString Folder = InFolder.isEmpty() ? GetDefaultFolder() : InFolder;
        if(EditorContent::IsEngineContent(Folder))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Prefab"), QStringLiteral("The engine's content is read-only here."));
            return nullptr;
        }

        bool bOk = false;
        QString Name = QInputDialog::getText(InParent, QStringLiteral("New Prefab"), QStringLiteral("Name:"),
                                             QLineEdit::Normal, QStringLiteral("NewPrefab"), &bOk).trimmed();
        if(!bOk || Name.isEmpty())
        {
            return nullptr;
        }
        if(Name.endsWith(PrefabExtension, Qt::CaseInsensitive)) { Name.chop(PrefabExtension.size()); }
        if(Name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Prefab"), QStringLiteral("A name cannot contain \\ / : * ? \" < > |"));
            return nullptr;
        }

        QDir().mkpath(Folder);
        const QString Path = QDir(Folder).filePath(Name + PrefabExtension);
        if(QFileInfo::exists(Path))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Prefab"), QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(Path)));
            return nullptr;
        }

        /* One entity, "Root", with a Transform: what CapturePrefab makes of it. */
        GameCore::PrefabAsset Asset;
        {
            GameCore::World Scratch;
            const GameCore::Entity Root = Scratch.CreateEntity("Root");
            Root.AddComponent<GameCore::TransformComponent>();
            GameCore::CapturePrefab(Scratch, Root.GetID(), Asset);
        }
        Asset.SetName(Name.toUtf8().constData());
        if(!GameCore::SavePrefabFile(Path.toUtf8().constData(), Asset))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Prefab"), QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(Path)));
            return nullptr;
        }
        Stage(Path);
        return OpenPrefab(Path, InParent);
    }

    bool CloseAllPrefabs()
    {
        /* The window asks each prefab in turn; it stays open on a Cancel. */
        PrefabEditorWindow* Window = PrefabEditorWindow::Find();
        return !Window || Window->close();
    }
}

#endif // WITH_EDITOR
