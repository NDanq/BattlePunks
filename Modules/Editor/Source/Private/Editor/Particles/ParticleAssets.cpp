#if WITH_EDITOR

#include "Private/Editor/Particles/ParticleAssets.h"

#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>

#include "Public/Editor/EditorContext.h"
#include "Public/RenderCore/ParticleSystemAsset.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Particles/ParticleEditorTab.h"
#include "Private/Editor/Particles/ParticleEditorWindow.h"

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

    const QString Extension = QString::fromLatin1(RenderCore::PARTICLE_SYSTEM_EXTENSION);
}

namespace EditorParticles
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
        return QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content/Particles"));
    }

    QString ToProjectPath(const QString& InRuntimePath)
    {
        const QString EnginePrefix = QStringLiteral("Engine/Content/");
        const QString Engine       = EditorContent::GetEngineContentRoot();
        if(InRuntimePath.startsWith(EnginePrefix) && !Engine.isEmpty())
        {
            return QDir(Engine).filePath(InRuntimePath.mid(EnginePrefix.size()));
        }
        return QDir(EditorGraphs::GetProjectRoot()).filePath(InRuntimePath);
    }

    ParticleEditorTab* OpenParticleSystem(const QString& InPath, QWidget* InParent)
    {
        if(ParticleEditorWindow* Window = ParticleEditorWindow::Find())
        {
            const QString Key = Canonical(InPath);
            for(ParticleEditorTab* Each : Window->GetTabs())
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
            QMessageBox::warning(InParent, QStringLiteral("Open Particle System"), QStringLiteral("The renderer is not running."));
            return nullptr;
        }

        auto* Tab = new ParticleEditorTab(*Ctx);
        QString Error;
        if(!Tab->Load(InPath, Error))
        {
            delete Tab;
            QMessageBox::warning(InParent, QStringLiteral("Open Particle System"),
                                 QStringLiteral("%1 is not a readable particle system: %2").arg(QDir::toNativeSeparators(InPath), Error));
            return nullptr;
        }
        ParticleEditorWindow::Get().AddTab(Tab);
        return Tab;
    }

    QString CreateParticleSystem(const QString& InFolder, QWidget* InParent, const bool bInOpen)
    {
        const QString Folder = InFolder.isEmpty() ? GetDefaultFolder() : InFolder;
        if(EditorContent::IsEngineContent(Folder))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Particle System"), QStringLiteral("The engine's content is read-only here."));
            return QString();
        }

        bool bOk = false;
        QString Name = QInputDialog::getText(InParent, QStringLiteral("New Particle System"), QStringLiteral("Name:"),
                                             QLineEdit::Normal, QStringLiteral("NewParticles"), &bOk).trimmed();
        if(!bOk || Name.isEmpty())
        {
            return QString();
        }
        if(Name.endsWith(Extension, Qt::CaseInsensitive)) { Name.chop(Extension.size()); }
        if(Name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Particle System"), QStringLiteral("A name cannot contain \\ / : * ? \" < > |"));
            return QString();
        }

        QDir().mkpath(Folder);
        const QString Path = QDir(Folder).filePath(Name + Extension);
        if(QFileInfo::exists(Path))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Particle System"),
                                 QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(Path)));
            return QString();
        }

        /* One emitter at its defaults, named for the system. */
        RenderCore::ParticleSystemDesc System;
        RenderCore::ParticleEmitterDesc& Emitter = System.Emitters.emplace_back();
        const QByteArray Utf8 = Name.toUtf8().left(int(sizeof(Emitter.Name)) - 1);
        std::snprintf(Emitter.Name, sizeof(Emitter.Name), "%s", Utf8.constData());
        if(!RenderCore::SaveParticleSystemFile(Path.toStdString(), System))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Particle System"),
                                 QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(Path)));
            return QString();
        }
        EditorContent::StageAsset(Path);
        if(bInOpen)
        {
            OpenParticleSystem(Path, InParent);
        }
        return Path;
    }

    bool CloseAll()
    {
        /* The window asks each system in turn; it stays open on a Cancel. */
        ParticleEditorWindow* Window = ParticleEditorWindow::Find();
        return !Window || Window->close();
    }
}

#endif // WITH_EDITOR
