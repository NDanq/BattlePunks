#if WITH_EDITOR

#include "Private/Editor/Prefab/PrefabAssets.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>

#include "Public/Core/CoreLog.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Prefab/PrefabEditorTab.h"
#include "Private/Editor/Prefab/PrefabEditorWindow.h"

namespace
{
    /* A tree's shape for grouping: each entity's component types, in tree
     * order. Names are left out: they become overrides. */
    QString TreeSignature(GameCore::World& InWorld, const GameCore::EntityID InEntity)
    {
        const GameCore::Entity E = InWorld.GetEntity(InEntity);
        QStringList Types;
        for(GameCore::ComponentTypeID Type = 0; Type < GameCore::COMPONENT_TYPE_MAX; ++Type)
        {
            if(E.HasComponentByType(Type)) { Types << QString::number(Type); }
        }
        QString Signature = QStringLiteral("[%1").arg(Types.join(QLatin1Char(',')));
        for(GameCore::EntityID Child = E.GetFirstChild(); Child.IsSet(); Child = InWorld.GetEntity(Child).GetNextSibling())
        {
            Signature += TreeSignature(InWorld, Child);
        }
        return Signature + QLatin1Char(']');
    }

    QStringList& NativeNames()
    {
        static QStringList Names;
        return Names;
    }

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
        return QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content/Prefabs"));
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
            const QString Key = IsNativePrefabPath(InPath) ? InPath : Canonical(InPath);
            for(PrefabEditorTab* Each : Window->GetTabs())
            {
                if((IsNativePrefabPath(Each->GetPath()) ? Each->GetPath() : Canonical(Each->GetPath())) == Key)
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

    QString WhyNotConvertible(EditorDocument& InDocument, const GameCore::EntityID InEntity)
    {
        GameCore::World& W = InDocument.GetWorld();
        if(InDocument.IsSingleRoot())       { return QStringLiteral("A prefab's entities are already a prefab"); }
        if(InDocument.IsWorldBorrowed())    { return QStringLiteral("Not while playing"); }
        if(!W.IsAlive(InEntity))            { return QStringLiteral("Select an entity first"); }

        std::vector<GameCore::EntityID> Stack{ InEntity };
        while(!Stack.empty())
        {
            const GameCore::Entity E = W.GetEntity(Stack.back());
            Stack.pop_back();
            if(E.IsInInstance())
            {
                return QStringLiteral("%1 is part of a prefab instance, and prefabs do not nest").arg(QString::fromUtf8(E.GetName()));
            }
            for(GameCore::EntityID Child = E.GetFirstChild(); Child.IsSet(); Child = W.GetEntity(Child).GetNextSibling())
            {
                Stack.push_back(Child);
            }
        }
        return QString();
    }

    bool ConvertToPrefab(EditorDocument& InDocument, const GameCore::EntityID InEntity, QWidget* InParent)
    {
        const QString Why = WhyNotConvertible(InDocument, InEntity);
        if(!Why.isEmpty())
        {
            QMessageBox::warning(InParent, QStringLiteral("Convert to Prefab"), Why);
            return false;
        }

        QDir().mkpath(GetDefaultFolder());
        QString Name = InDocument.GetDisplayName(InEntity);
        Name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
        const QString Path = QFileDialog::getSaveFileName(InParent, QStringLiteral("Convert to Prefab"),
                                                          QDir(GetDefaultFolder()).filePath(Name + PrefabExtension),
                                                          QStringLiteral("Prefabs (*.nmprefab)"));
        if(Path.isEmpty())
        {
            return false;
        }
        const QString Runtime = ToRuntimePath(Path);
        if(Runtime.isEmpty())
        {
            QMessageBox::warning(InParent, QStringLiteral("Convert to Prefab"), QStringLiteral("Save the prefab inside the project."));
            return false;
        }

        GameCore::World& W = InDocument.GetWorld();
        GameCore::PrefabAsset Asset;
        if(!GameCore::CapturePrefab(W, InEntity, Asset))
        {
            return false;
        }
        Asset.SetName(QFileInfo(Path).completeBaseName().toUtf8().constData());
        if(!GameCore::SavePrefabFile(Path.toUtf8().constData(), Asset))
        {
            QMessageBox::warning(InParent, QStringLiteral("Convert to Prefab"), QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(Path)));
            return false;
        }
        Stage(Path);
        GameCore::ReloadPrefab(Runtime.toUtf8().constData());   // in case it replaced one already loaded

        const GameCore::EntityID Root = InDocument.ReplaceWithPrefab(InEntity, Runtime);
        if(!Root.IsSet())
        {
            QMessageBox::warning(InParent, QStringLiteral("Convert to Prefab"),
                                 QStringLiteral("%1 was written but could not be placed; the Console says why.").arg(QDir::toNativeSeparators(Path)));
            return false;
        }
        InDocument.SetSelection(Root);
        return true;
    }

    void CacheNativePrefabs()
    {
        QStringList Names;
        for(size_t Index = 0; Index < GameCore::GetNativePrefabCount(); ++Index)
        {
            const QString Name = QString::fromUtf8(GameCore::GetNativePrefabName(Index));
            /* Reload, not find: a rebuilt module may have a new layout. */
            if(GameCore::ReloadPrefab(NativePrefabPath(Name).toUtf8().constData())) { Names << Name; }
        }
        Names.sort();
        NativeNames() = Names;
        if(!Names.isEmpty())
        {
            CORE_LOG("[Prefab] native prefabs: " + std::string(Names.join(QStringLiteral(", ")).toUtf8().constData()))
        }
    }

    QStringList GetNativePrefabs()
    {
        return NativeNames();
    }

    QString NativePrefabPath(const QString& InName)
    {
        return QString::fromLatin1(GameCore::NATIVE_PREFAB_PREFIX) + InName;
    }

    bool IsNativePrefabPath(const QString& InPath)
    {
        return InPath.startsWith(QString::fromLatin1(GameCore::NATIVE_PREFAB_PREFIX));
    }

    int ConvertLevelToPrefabs(EditorDocument& InDocument, QWidget* InParent)
    {
        if(InDocument.IsSingleRoot() || InDocument.IsWorldBorrowed())
        {
            return 0;
        }
        GameCore::World& W = InDocument.GetWorld();
        const std::vector<GameCore::EntityID> Loose = InDocument.GetLooseRoots();
        if(Loose.empty())
        {
            QMessageBox::information(InParent, QStringLiteral("Convert Level to Prefabs"),
                                     QStringLiteral("Every entity is already in a prefab instance."));
            return 0;
        }

        /* Grouped by shape, in level order. */
        std::vector<std::pair<QString, std::vector<GameCore::EntityID>>> Groups;
        for(const GameCore::EntityID Root : Loose)
        {
            const QString Signature = TreeSignature(W, Root);
            auto Found = std::find_if(Groups.begin(), Groups.end(), [&](const auto& InGroup) { return InGroup.first == Signature; });
            if(Found == Groups.end()) { Groups.push_back({ Signature, { Root } }); }
            else                      { Found->second.push_back(Root); }
        }

        const QString Folder = GetDefaultFolder();
        if(QMessageBox::question(InParent, QStringLiteral("Convert Level to Prefabs"),
                                 QStringLiteral("Make %1 prefab(s) in %2 from %3 entity tree(s), and put an instance in place of "
                                                "each? Each keeps its own values as overrides. One undo step.")
                                     .arg(Groups.size()).arg(QDir::toNativeSeparators(Folder)).arg(Loose.size()))
           != QMessageBox::Yes)
        {
            return 0;
        }
        QDir().mkpath(Folder);

        std::vector<std::pair<GameCore::EntityID, QString>> Trees;
        for(const auto& Group : Groups)
        {
            const GameCore::EntityID First = Group.second.front();
            QString Name = InDocument.GetDisplayName(First);
            Name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\s]")));
            if(Name.isEmpty()) { Name = QStringLiteral("Prefab"); }
            QString File = QDir(Folder).filePath(Name + PrefabExtension);
            for(int Suffix = 2; QFileInfo::exists(File); ++Suffix)
            {
                File = QDir(Folder).filePath(QStringLiteral("%1%2").arg(Name).arg(Suffix) + PrefabExtension);
            }

            GameCore::PrefabAsset Asset;
            if(!GameCore::CapturePrefab(W, First, Asset)) { continue; }
            Asset.SetName(QFileInfo(File).completeBaseName().toUtf8().constData());
            if(!GameCore::SavePrefabFile(File.toUtf8().constData(), Asset))
            {
                CORE_LOG_ERROR("[Prefab] could not write " + std::string(File.toUtf8().constData()))
                continue;
            }
            Stage(File);
            const QString Runtime = ToRuntimePath(File);
            GameCore::ReloadPrefab(Runtime.toUtf8().constData());
            for(const GameCore::EntityID Root : Group.second) { Trees.emplace_back(Root, Runtime); }
        }

        const int Converted = InDocument.ConvertTreesToPrefabs(Trees);
        CORE_LOG("[Prefab] converted " + std::to_string(Converted) + " entity tree(s) into " + std::to_string(Groups.size()) + " prefab(s)")
        return Converted;
    }

    bool CloseAllPrefabs()
    {
        /* The window asks each prefab in turn; it stays open on a Cancel. */
        PrefabEditorWindow* Window = PrefabEditorWindow::Find();
        return !Window || Window->close();
    }
}

#endif // WITH_EDITOR
