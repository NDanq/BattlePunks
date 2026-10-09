#if WITH_EDITOR

#include "Private/Editor/Sequencer/SequencerAssets.h"

#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>

#include "Public/Editor/EditorContext.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/Sequencer/SequenceFile.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Sequencer/SequencerTab.h"
#include "Private/Editor/Sequencer/SequencerWindow.h"

namespace
{
    QPointer<EditorContext>& Context()
    {
        static QPointer<EditorContext> Current;
        return Current;
    }

    QPointer<ViewportWidget>& Viewport()
    {
        static QPointer<ViewportWidget> Current;
        return Current;
    }

    QString Canonical(const QString& InPath)
    {
        const QFileInfo Info(InPath);
        const QString Resolved = Info.canonicalFilePath();
        return QDir::cleanPath(Resolved.isEmpty() ? Info.absoluteFilePath() : Resolved);
    }

    const QString Extension = QString::fromLatin1(Sequencer::SEQUENCE_EXTENSION);
}

namespace EditorSequencer
{
    void SetEditorContext(EditorContext* InContext)
    {
        Context() = InContext;
    }

    EditorContext* GetEditorContext()
    {
        return Context();
    }

    void SetLevelViewport(ViewportWidget* InViewport)
    {
        Viewport() = InViewport;
    }

    ViewportWidget* GetLevelViewport()
    {
        return Viewport();
    }

    QString GetDefaultFolder()
    {
        return QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content/Cinematics"));
    }

    SequencerTab* OpenSequence(const QString& InPath, QWidget* InParent)
    {
        if(SequencerWindow* Window = SequencerWindow::Find())
        {
            const QString Key = Canonical(InPath);
            for(SequencerTab* Each : Window->GetTabs())
            {
                if(Canonical(Each->GetPath()) == Key)
                {
                    Window->ShowTab(Each);
                    return Each;
                }
            }
        }

        EditorContext* Ctx = Context();
        if(!Ctx)
        {
            QMessageBox::warning(InParent, QStringLiteral("Open Sequence"), QStringLiteral("The editor is not ready."));
            return nullptr;
        }

        auto* Tab = new SequencerTab(*Ctx);
        QString Error;
        if(!Tab->Load(InPath, Error))
        {
            delete Tab;
            QMessageBox::warning(InParent, QStringLiteral("Open Sequence"),
                                 QStringLiteral("%1 is not a readable sequence: %2").arg(QDir::toNativeSeparators(InPath), Error));
            return nullptr;
        }
        SequencerWindow::Get().AddTab(Tab);
        return Tab;
    }

    QString CreateSequence(const QString& InFolder, QWidget* InParent, const bool bInOpen)
    {
        const QString Folder = InFolder.isEmpty() ? GetDefaultFolder() : InFolder;
        if(EditorContent::IsEngineContent(Folder))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Sequence"), QStringLiteral("The engine's content is read-only here."));
            return QString();
        }

        bool bOk = false;
        QString Name = QInputDialog::getText(InParent, QStringLiteral("New Sequence"), QStringLiteral("Name:"),
                                             QLineEdit::Normal, QStringLiteral("NewSequence"), &bOk).trimmed();
        if(!bOk || Name.isEmpty())
        {
            return QString();
        }
        if(Name.endsWith(Extension, Qt::CaseInsensitive)) { Name.chop(Extension.size()); }
        if(Name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Sequence"), QStringLiteral("A name cannot contain \\ / : * ? \" < > |"));
            return QString();
        }

        QDir().mkpath(Folder);
        const QString Path = QDir(Folder).filePath(Name + Extension);
        if(QFileInfo::exists(Path))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Sequence"), QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(Path)));
            return QString();
        }

        /* Five seconds at 30, a camera cut track waiting for cameras. */
        Sequencer::SequenceAsset Asset;
        Asset.FrameRate = 30;
        Asset.Length    = 150;
        Sequencer::SequenceTrack Cuts;
        Cuts.Type = Sequencer::ESequenceTrackType::CameraCut;
        Asset.Tracks.push_back(Cuts);
        std::string Error;
        if(!Sequencer::SaveSequenceXml(Path.toStdString(), Asset, &Error))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Sequence"), QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(Path)));
            return QString();
        }
        EditorContent::StageAsset(Path);
        if(bInOpen)
        {
            OpenSequence(Path, InParent);
        }
        return Path;
    }

    bool CloseAll()
    {
        SequencerWindow* Window = SequencerWindow::Find();
        return !Window || Window->close();
    }
}

#endif // WITH_EDITOR
