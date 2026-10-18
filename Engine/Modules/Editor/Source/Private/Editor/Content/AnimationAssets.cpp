#if WITH_EDITOR

#include "Private/Editor/Content/AnimationAssets.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>

#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RetargetAsset.h"
#include "Private/Editor/Content/AnimStateEditorWindow.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"

namespace EditorAnimation
{
    EngineCore::Archive ReadAssetArchive(const QString& InPath)
    {
        QFile File(InPath);
        if(!File.open(QIODevice::ReadOnly)) { return EngineCore::Archive::ForReading({}); }
        const QByteArray Bytes = File.readAll();
        return EngineCore::Archive::ForReading(std::vector<uint8_t>(Bytes.begin(), Bytes.end()));
    }

    QString FileOfRuntimePath(const QString& InRuntimePath)
    {
        const QString EnginePrefix = QStringLiteral("Engine/Content/");
        if(InRuntimePath.startsWith(EnginePrefix))
        {
            return QDir(EditorContent::GetEngineContentRoot()).absoluteFilePath(InRuntimePath.mid(EnginePrefix.size()));
        }
        return QDir(EditorGraphs::GetProjectRoot()).absoluteFilePath(InRuntimePath);
    }

    std::string SkeletonOfAnimation(const QString& InPath)
    {
        EngineCore::Archive Ar = ReadAssetArchive(InPath);
        if(InPath.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive))
        {
            RenderCore::AnimClipData Clip;
            return RenderCore::SerializeAnimClip(Ar, Clip) ? Clip.Skeleton : std::string();
        }
        RenderCore::AnimStateData State;
        if(!RenderCore::SerializeAnimState(Ar, State)) { return std::string(); }
        for(const RenderCore::AnimLayer& Layer : State.Layers)
        {
            for(const RenderCore::AnimStateNode& Node : Layer.States)
            {
                for(const RenderCore::AnimBlendSample& Sample : Node.Samples)
                {
                    EngineCore::Archive ClipAr = ReadAssetArchive(FileOfRuntimePath(QString::fromStdString(Sample.Clip)));
                    RenderCore::AnimClipData Clip;
                    if(RenderCore::SerializeAnimClip(ClipAr, Clip) && !Clip.Skeleton.empty()) { return Clip.Skeleton; }
                }
            }
        }
        return std::string();
    }

    QString FindMeshOn(const std::string& InSkeleton)
    {
        if(InSkeleton.empty())
        {
            return QString();
        }
        for(const QString& Root : { EditorGraphs::GetProjectRoot(), EditorContent::GetEngineContentRoot() })
        {
            if(Root.isEmpty()) { continue; }
            QDirIterator It(Root, { QStringLiteral("*.nmmesh") }, QDir::Files, QDirIterator::Subdirectories);
            while(It.hasNext())
            {
                const QString File = It.next();
                /* Build trees and staged copies are not the project's own. */
                if(File.contains(QStringLiteral("/Binaries/")) || File.contains(QStringLiteral("/Build/"))
                    || File.contains(QStringLiteral("/cmake-build"))) { continue; }
                EngineCore::Archive Ar = ReadAssetArchive(File);
                RenderCore::MeshData Mesh;
                if(RenderCore::SerializeMesh(Ar, Mesh) && Mesh.IsSkinned() && Mesh.Skeleton == InSkeleton) { return File; }
            }
        }
        return QString();
    }

    QString FindRetargetedMesh(const std::string& InSkeleton, QString& OutRetarget)
    {
        OutRetarget.clear();
        if(InSkeleton.empty()) { return QString(); }
        for(const QString& Each : EditorContent::ListRetargetPaths())
        {
            EngineCore::Archive Ar = ReadAssetArchive(FileOfRuntimePath(Each));
            RenderCore::RetargetData Retarget;
            if(!RenderCore::SerializeRetarget(Ar, Retarget) || !Retarget.FindSource(InSkeleton)) { continue; }
            const QString Mesh = FindMeshOn(Retarget.Target);
            if(!Mesh.isEmpty())
            {
                OutRetarget = Each;
                return Mesh;
            }
        }
        return QString();
    }

    QString CreateAnimState(const QString& InFolder, QWidget* InParent, const bool bInOpen)
    {
        const QString Extension = QString::fromLatin1(RenderCore::ANIM_STATE_EXTENSION);
        const QString Folder = InFolder.isEmpty() ? QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content/Animation")) : InFolder;
        if(EditorContent::IsEngineContent(Folder))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Anim State"), QStringLiteral("The engine's content is read-only here."));
            return QString();
        }

        bool bOk = false;
        QString Name = QInputDialog::getText(InParent, QStringLiteral("New Anim State"), QStringLiteral("Name:"),
                                             QLineEdit::Normal, QStringLiteral("NewAnimState"), &bOk).trimmed();
        if(!bOk || Name.isEmpty())
        {
            return QString();
        }
        if(Name.endsWith(Extension, Qt::CaseInsensitive)) { Name.chop(Extension.size()); }
        if(Name.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Anim State"), QStringLiteral("A name cannot contain \\ / : * ? \" < > |"));
            return QString();
        }
        QDir().mkpath(Folder);
        const QString Path = QDir(Folder).filePath(Name + Extension);
        if(QFileInfo::exists(Path))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Anim State"), QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(Path)));
            return QString();
        }

        /* A start: one layer, one state to give clips to, the parameter most use. */
        RenderCore::AnimStateData State;
        State.Parameters.push_back({ "Speed", RenderCore::EAnimParameterType::Float, 0.0f });
        RenderCore::AnimLayer Base;
        Base.Name = "Base";
        RenderCore::AnimStateNode Idle;
        Idle.Name = "Idle";
        Base.States.push_back(Idle);
        State.Layers.push_back(Base);
        if(!RenderCore::SaveAnimStateFile(Path.toStdString(), State))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Anim State"), QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(Path)));
            return QString();
        }
        EditorContent::StageAsset(Path);
        if(bInOpen)
        {
            OpenAnimState(Path, InParent);
        }
        return Path;
    }

    void OpenAnimState(const QString& InPath, QWidget* InParent)
    {
        AnimStateEditorWindow::Open(InPath, InParent);
    }
}

#endif // WITH_EDITOR
