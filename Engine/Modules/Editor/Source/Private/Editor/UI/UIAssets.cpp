#if WITH_EDITOR

#include "Private/Editor/UI/UIAssets.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>

#include "Public/Core/CoreLog.h"
#include "Public/Editor/EditorContext.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UIFile.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Private/Editor/UI/UIEditorTab.h"
#include "Private/Editor/UI/UIEditorWindow.h"

namespace
{
    const QString Extension = QStringLiteral(".nmui");

    QString Canonical(const QString& InPath)
    {
        const QFileInfo Info(InPath);
        return Info.exists() ? Info.canonicalFilePath() : Info.absoluteFilePath();
    }

    /* A new document: the root a screen is laid out in. */
    const char* const NEW_DOCUMENT =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<UI Format=\"1\" ReferenceSize=\"1920,1080\" ScaleMode=\"FitHeight\" InputMode=\"GameAndUI\">\n"
        "  <Canvas Name=\"Root\"/>\n"
        "</UI>\n";
}

namespace EditorUI
{
    QString GetDefaultFolder()
    {
        return QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content/UI"));
    }

    void Stage(const QString& InPath)
    {
        const QString Staged = EditorGraphs::GetStagedPath(InPath);
        if(!Staged.isEmpty())
        {
            QDir().mkpath(QFileInfo(Staged).absolutePath());
            QFile::remove(Staged);
            if(!QFile::copy(InPath, Staged))
            {
                CORE_LOG_WARN("[UI] could not stage " + std::string(Staged.toUtf8().constData()))
            }
        }
        const QString Runtime = EditorContent::ToRuntimePath(InPath);
        if(!Runtime.isEmpty())
        {
            UI::UIAssetCache::Invalidate(Runtime.toStdString());
        }
    }

    UIEditorTab* OpenUI(const QString& InPath, QWidget* InParent)
    {
        if(UIEditorWindow* Window = UIEditorWindow::Find())
        {
            for(UIEditorTab* Each : Window->GetTabs())
            {
                if(Canonical(Each->GetPath()) == Canonical(InPath))
                {
                    Window->ShowTab(Each);
                    return Each;
                }
            }
        }

        EditorContext* Context = EditorPrefabs::GetEditorContext();
        if(!Context || !Context->IsRendererReady())
        {
            QMessageBox::warning(InParent, QStringLiteral("Open UI"), QStringLiteral("The renderer is not running."));
            return nullptr;
        }
        auto* Tab = new UIEditorTab(*Context);
        QString Error;
        if(!Tab->Load(InPath, &Error))
        {
            delete Tab;
            QMessageBox::warning(InParent, QStringLiteral("Open UI"),
                                 QStringLiteral("%1 is not a readable UI document: %2").arg(QDir::toNativeSeparators(InPath), Error));
            return nullptr;
        }
        UIEditorWindow::Get().AddTab(Tab);
        return Tab;
    }

    UIEditorTab* CreateUI(const QString& InFolder, QWidget* InParent)
    {
        const QString Folder = InFolder.isEmpty() ? GetDefaultFolder() : InFolder;
        if(EditorContent::IsEngineContent(Folder))
        {
            QMessageBox::warning(InParent, QStringLiteral("New UI"), QStringLiteral("The engine's content is read-only here."));
            return nullptr;
        }
        bool bOk = false;
        QString Name = QInputDialog::getText(InParent, QStringLiteral("New UI"), QStringLiteral("Name:"),
                                             QLineEdit::Normal, QStringLiteral("NewUI"), &bOk).trimmed();
        if(!bOk || Name.isEmpty()) return nullptr;
        if(Name.endsWith(Extension, Qt::CaseInsensitive)) Name.chop(Extension.size());
        if(Name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))))
        {
            QMessageBox::warning(InParent, QStringLiteral("New UI"), QStringLiteral("A name cannot contain \\ / : * ? \" < > |"));
            return nullptr;
        }
        QDir().mkpath(Folder);
        const QString Path = QDir(Folder).filePath(Name + Extension);
        if(QFileInfo::exists(Path))
        {
            QMessageBox::warning(InParent, QStringLiteral("New UI"), QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(Path)));
            return nullptr;
        }
        QFile File(Path);
        if(!File.open(QIODevice::WriteOnly) || File.write(NEW_DOCUMENT) < 0)
        {
            QMessageBox::warning(InParent, QStringLiteral("New UI"), QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(Path)));
            return nullptr;
        }
        File.close();
        Stage(Path);
        return OpenUI(Path, InParent);
    }

    int CookAll(const QString& InRoot, const QString& InOutRoot, int& OutFailed, QString& OutErrors)
    {
        int Cooked = 0;
        OutFailed = 0;
        QDirIterator It(InRoot, { QStringLiteral("*.nmui") }, QDir::Files, QDirIterator::Subdirectories);
        while(It.hasNext())
        {
            const QString File = It.next();
            const QString Runtime  = EditorContent::ToRuntimePath(File);
            const QString Relative = QDir(InRoot).relativeFilePath(File);
            if(Runtime.isEmpty() || QFileInfo(File).absoluteFilePath().startsWith(QFileInfo(InOutRoot).absoluteFilePath() + QLatin1Char('/')))
            {
                continue;   // outside the project, or a cooked copy
            }
            bool bIgnored = false;
            for(const QString& Part : Relative.split(QLatin1Char('/'))) { bIgnored |= EditorGraphs::IsIgnoredFolder(Part); }
            if(bIgnored) continue;

            std::string Error;
            const QString Out = QDir(InOutRoot).filePath(Runtime);
            if(UI::CookUIFile(File.toUtf8().toStdString(), Out.toUtf8().toStdString(), &Error))
            {
                ++Cooked;
            }
            else
            {
                ++OutFailed;
                OutErrors += QStringLiteral("%1: %2\n").arg(Runtime, QString::fromStdString(Error));
            }
        }
        return Cooked;
    }

    bool CloseAll()
    {
        UIEditorWindow* Window = UIEditorWindow::Find();
        if(!Window) return true;
        for(UIEditorTab* Tab : Window->GetTabs())
        {
            if(!Tab->ConfirmClose()) return false;
        }
        Window->close();
        return true;
    }
}

#endif // WITH_EDITOR
