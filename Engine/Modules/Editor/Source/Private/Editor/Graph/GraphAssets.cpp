#if WITH_EDITOR

#include "Private/Editor/Graph/GraphAssets.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QList>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QUndoStack>

#include <cstring>
#include <map>
#include <memory>

#include <QTimer>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/Editor/EditorContext.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphEditorTab.h"
#include "Private/Editor/Graph/GraphEditorWindow.h"
#include "Private/Editor/ProjectSettingsDialog.h"

#ifndef NIMBLEEDITOR_PYTHON
#define NIMBLEEDITOR_PYTHON ""
#endif
#ifndef NIMBLEEDITOR_MATERIAL_CHECK_SCRIPT
#define NIMBLEEDITOR_MATERIAL_CHECK_SCRIPT ""
#endif

using namespace NodeGraph;

namespace
{
    /* Every open graph: the graph editor window's tabs. */
    QList<GraphEditorTab*> OpenTabs()
    {
        GraphEditorWindow* Window = GraphEditorWindow::Find();
        return Window ? Window->GetTabs() : QList<GraphEditorTab*>();
    }

    /* SetEditorContext's; a QPointer, so a destroyed context reads as null. */
    QPointer<EditorContext>& Context()
    {
        static QPointer<EditorContext> Current;
        return Current;
    }

    /* One material's cook: running, and whether a newer request waits. */
    struct MaterialCook
    {
        bool                                      bRunning = false;
        bool                                      bPending = false;
        QPointer<QObject>                         PendingContext;
        std::function<void(bool, const QString&)> PendingDone;
    };

    std::map<QString, MaterialCook>& MaterialCooks()
    {
        static std::map<QString, MaterialCook> Cooks;
        return Cooks;
    }

    /* cook_material_graphs.py's -target for a backend; empty for one it
     * cannot cook (Null draws nothing; Metal's cook is not wired up). */
    QString CookTarget(const RenderCore::ERenderBackend InBackend)
    {
        switch(InBackend)
        {
            case RenderCore::ERenderBackend::OpenGL: return QStringLiteral("OpenGL");
            case RenderCore::ERenderBackend::Vulkan: return QStringLiteral("Vulkan");
            case RenderCore::ERenderBackend::D3D12:  return QStringLiteral("D3D12");
            default:                                 return QString();
        }
    }

    /* Where a material's function waits to be cooked: the cook names its
     * permutations after the file, and reads <Name>.material beside it, so
     * both go in a folder of their own. */
    QString CookSourceDir()
    {
        return QDir::temp().filePath(QStringLiteral("NimbleMaterialCook"));
    }

    QString CookSourcePath(const QString& InName)
    {
        return QDir(CookSourceDir()).filePath(InName + QStringLiteral(".mtl.glsl"));
    }

    void RunMaterialCook(const QString& InName, QObject* InContext, std::function<void(bool, const QString&)> InDone);

    /* A cook finished: reload, report, and start the one that waited, if any. */
    void FinishMaterialCook(const QString& InName, const bool bInWorked, const QString& InMessage,
                            const QPointer<QObject>& InContext, const std::function<void(bool, const QString&)>& InDone)
    {
        if(bInWorked && Context()) { Context()->ReloadMaterial(InName); }
        if(InContext && InDone) { InDone(bInWorked, InMessage); }

        MaterialCook& Cook = MaterialCooks()[InName];
        Cook.bRunning = false;
        if(Cook.bPending)
        {
            Cook.bPending = false;
            RunMaterialCook(InName, Cook.PendingContext, std::move(Cook.PendingDone));
        }
    }

    void RunMaterialCook(const QString& InName, QObject* InContext, std::function<void(bool, const QString&)> InDone)
    {
        MaterialCook& Cook = MaterialCooks()[InName];
        Cook.bRunning = true;

        const RenderCore::ERenderBackend Backend = Context() ? Context()->GetRenderBackend() : RenderCore::ERenderBackend::Null;
        const QString Target = CookTarget(Backend);
        const QPointer<QObject> Guard(InContext);

        if(Target.isEmpty())
        {
            /* Nothing to compile for Null; the reload alone re-reads the .material. */
            const bool bNull = Backend == RenderCore::ERenderBackend::Null;
            FinishMaterialCook(InName, bNull, bNull ? QStringLiteral("staged (the Null backend compiles nothing)")
                                                    : QStringLiteral("no shader cook for this backend"), Guard, InDone);
            return;
        }
        if(!EditorGraphs::CanCheckShaders())
        {
            FinishMaterialCook(InName, false, QStringLiteral("cannot cook: the editor was built without Python "
                                                             "(see NodeGraph's CMakeLists)"), Guard, InDone);
            return;
        }

        const QString Out = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Shaders/Cooked"));
        auto* Process = new QProcess(qApp);
        Process->setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(Process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), qApp,
                         [Process, InName, Target, Guard, InDone](const int InCode, const QProcess::ExitStatus InStatus)
        {
            const QString Output = QString::fromUtf8(Process->readAll()).trimmed();
            const bool bWorked = InStatus == QProcess::NormalExit && InCode == 0;
            if(!bWorked)
            {
                CORE_LOG_WARN("[Graph] cooking material '" + EditorGraphs::ToStd(InName) + "' failed:\n" + EditorGraphs::ToStd(Output))
            }
            Process->deleteLater();
            FinishMaterialCook(InName, bWorked, bWorked ? QStringLiteral("cooked for %1").arg(Target) : Output, Guard, InDone);
        });
        QObject::connect(Process, &QProcess::errorOccurred, qApp, [Process, InName, Guard, InDone](const QProcess::ProcessError InError)
        {
            if(InError != QProcess::FailedToStart) { return; }   // finished() reports the rest
            Process->deleteLater();
            FinishMaterialCook(InName, false, QStringLiteral("could not run the shader cook: ") + Process->errorString(), Guard, InDone);
        });

        Process->start(QString::fromUtf8(NIMBLEEDITOR_PYTHON),
                       { QString::fromUtf8(NIMBLEEDITOR_MATERIAL_CHECK_SCRIPT), QStringLiteral("-target"), Target,
                         QStringLiteral("-src"), CookSourcePath(InName), QStringLiteral("-out"), Out,
                         QStringLiteral("-subdir"), QStringLiteral("Materials") });
    }

    QString Canonical(const QString& InPath)
    {
        const QFileInfo Info(InPath);
        const QString Resolved = Info.canonicalFilePath();
        return QDir::cleanPath(Resolved.isEmpty() ? Info.absoluteFilePath() : Resolved);
    }

    /* Build trees, binaries and version control hold no graphs worth
     * regenerating -- and thousands of files to walk. */
    bool IsSkippedFolder(const QString& InName)
    {
        return InName.startsWith(QLatin1Char('.')) || InName.startsWith(QStringLiteral("cmake-build"))
            || InName == QStringLiteral("Binaries") || InName == QStringLiteral("Build") || InName == QStringLiteral("build");
    }

    void FindGraphs(const QDir& InDir, QStringList& OutPaths)
    {
        for(const QFileInfo& Entry : InDir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name))
        {
            if(Entry.isDir())
            {
                if(!IsSkippedFolder(Entry.fileName())) { FindGraphs(QDir(Entry.absoluteFilePath()), OutPaths); }
            }
            else if(Entry.fileName().endsWith(QString::fromLatin1(GRAPH_EXTENSION), Qt::CaseInsensitive))
            {
                OutPaths << Entry.absoluteFilePath();
            }
        }
    }

    /* The runtime copy of a project file: same path relative to the root,
     * under the executable's folder. Empty for a file outside the project. */
    QString StagedPathFor(const QString& InPath)
    {
        const QString Relative = QDir(EditorGraphs::GetProjectRoot()).relativeFilePath(QFileInfo(InPath).absoluteFilePath());
        if(Relative.startsWith(QStringLiteral("..")) || QDir::isAbsolutePath(Relative)) { return QString(); }
        const QString Staged = QDir(QCoreApplication::applicationDirPath()).filePath(Relative);
        return Canonical(Staged) == Canonical(InPath) ? QString() : Staged;
    }

    Graph StarterGraph(const Schema& InSchema)
    {
        Graph G;
        G.SchemaName = InSchema.GetName();

        const auto Add = [&](const char* InType, const float InX, const float InY)
        {
            Node Added;
            Added.Id   = NodeGuid::Generate();
            Added.Type = InType;
            Added.X    = InX;
            Added.Y    = InY;
            G.Nodes.push_back(Added);
        };

        if(InSchema.GetName() == "Script")
        {
            Add("Event.OnStart", -300.0f, -120.0f);
            Add("Event.OnTick", -300.0f, 60.0f);
        }
        else
        {
            Add("Output.Pixel", 200.0f, 0.0f);
        }
        return G;
    }
}

namespace EditorGraphs
{
    QString GetProjectRoot()
    {
        /* <root>/Config/DefaultEngine.ini */
        const QFileInfo Config(ProjectSettingsDialog::GetProjectConfigPath());
        QDir Root = Config.absoluteDir();
        if(Root.dirName().compare(QStringLiteral("Config"), Qt::CaseInsensitive) == 0) { Root.cdUp(); }
        return Root.absolutePath();
    }

    QString GetDefaultFolder(const Schema& InSchema)
    {
        return QDir(GetProjectRoot()).filePath(InSchema.GetName() == "Script" ? QStringLiteral("Scripts") : QStringLiteral("Materials"));
    }

    bool IsIgnoredFolder(const QString& InName)
    {
        return IsSkippedFolder(InName);
    }

    QString GetStagedPath(const QString& InPath)
    {
        return StagedPathFor(InPath);
    }

    bool IsGraphOpen(const QString& InGraphPath)
    {
        const QString Key = Canonical(InGraphPath);
        for(GraphEditorTab* Each : OpenTabs())
        {
            if(Canonical(Each->GetDocument().GetPath()) == Key) { return true; }
        }
        return false;
    }

    QString GetFileFilter(const Schema* InOnly)
    {
        QStringList AllPatterns, Filters;
        for(const Schema* Each : GetSchemas())
        {
            const QString Pattern = QStringLiteral("*.%1%2").arg(ToQ(Each->GetFileTag()), QString::fromLatin1(GRAPH_EXTENSION));
            AllPatterns << Pattern;
            if(!InOnly || InOnly == Each) { Filters << QStringLiteral("%1 Graphs (%2)").arg(ToQ(Each->GetName()), Pattern); }
        }
        if(!InOnly) { Filters.prepend(QStringLiteral("Node Graphs (%1)").arg(AllPatterns.join(QLatin1Char(' ')))); }
        Filters << QStringLiteral("All Files (*)");
        return Filters.join(QStringLiteral(";;"));
    }

    bool WriteOutput(const QString& InGraphPath, const Graph& InGraph, CompileResult& OutResult, QString& OutMessage,
                     QObject* InContext, std::function<void(bool, const QString&)> InStaged)
    {
        const QString Name = QFileInfo(InGraphPath).fileName();
        const Schema* S = FindSchema(InGraph.SchemaName);
        if(!S)
        {
            OutMessage = QStringLiteral("%1: unknown schema").arg(Name);
            return false;
        }

        CompileGraph(InGraph, OutResult);
        if(OutResult.HasErrors())
        {
            OutMessage = QStringLiteral("Saved %1 -- it has errors, so its output was not updated").arg(Name);
            CORE_LOG_WARN("[Graph] " + FormatDiagnostics(InGraph, OutResult))
            return false;
        }

        const QString Output = ToQ(GetGraphOutputPath(ToStd(InGraphPath), *S));
        std::string Error;
        if(!WriteTextFileIfChanged(ToStd(Output), OutResult.Code, Error))
        {
            OutMessage = QStringLiteral("Saved %1, but could not write its output: %2").arg(Name, ToQ(Error));
            CORE_LOG_ERROR("[Graph] " + ToStd(OutMessage))
            return false;
        }

        for(const auto& Companion : OutResult.Companions)
        {
            const std::string CompanionPath = StripGraphExtension(ToStd(InGraphPath)) + Companion.first;
            if(!WriteTextFileIfChanged(CompanionPath, Companion.second, Error))
            {
                OutMessage = QStringLiteral("Saved %1, but could not write %2: %3").arg(Name, ToQ(CompanionPath), ToQ(Error));
                CORE_LOG_ERROR("[Graph] " + ToStd(OutMessage))
                return false;
            }
        }

        OutMessage = QStringLiteral("Saved %1 -> %2").arg(Name, QFileInfo(Output).fileName());

        if(S->GetName() == "Material")
        {
            /* By name, cooked for the viewport; see the header. */
            const QString Material = GetMaterialName(InGraphPath);
            if(InStaged)
            {
                StageMaterial(Material, OutResult, InContext ? InContext : qApp, std::move(InStaged));
            }
            else
            {
                StageMaterial(Material, OutResult, qApp, [Material](const bool bInWorked, const QString& InWhat)
                {
                    if(bInWorked) { CORE_LOG("[Graph] material '" + ToStd(Material) + "': " + ToStd(InWhat)) }
                });
            }
            OutMessage += QStringLiteral(" (cooking for the viewport)");
        }
        else
        {
            const QString Staged = StagedPathFor(Output);
            if(!Staged.isEmpty())
            {
                if(WriteTextFileIfChanged(ToStd(Staged), OutResult.Code, Error)) { OutMessage += QStringLiteral(" (staged for Play)"); }
                else { CORE_LOG_WARN("[Graph] could not stage " + ToStd(Staged) + ": " + Error) }
            }
        }
        CORE_LOG("[Graph] " + ToStd(OutMessage))
        return true;
    }

    // === Materials in the viewport ===========================================

    void SetEditorContext(EditorContext* InContext)
    {
        Context() = InContext;
    }

    QString GetMaterialName(const QString& InGraphPath)
    {
        return ToQ(GetGraphBaseName(ToStd(InGraphPath)));
    }

    void StageMaterial(const QString& InName, const CompileResult& InResult, QObject* InContext,
                       std::function<void(bool, const QString&)> InDone)
    {
        const QPointer<QObject> Guard(InContext);
        const auto Fail = [&](const QString& InWhy) { if(Guard && InDone) { InDone(false, InWhy); } };

        if(InResult.HasErrors() || InResult.Code.empty())
        {
            Fail(QStringLiteral("the graph has errors"));
            return;
        }
        if(InName.toUtf8().size() >= int(RenderCore::RENDER_MATERIAL_NAME_MAX))
        {
            Fail(QStringLiteral("the name '%1' is too long for a primitive to refer to").arg(InName));
            return;
        }

        /* The description, where RenderCore reads it and beside the function
         * for the cook (which picks the permutations from it)... */
        std::string Error;
        for(const auto& Companion : InResult.Companions)
        {
            for(const QString& Folder : { QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Materials")),
                                          CookSourceDir() })
            {
                const QString Staged = QDir(Folder).filePath(InName + ToQ(Companion.first));
                if(!WriteTextFileIfChanged(ToStd(Staged), Companion.second, Error))
                {
                    Fail(QStringLiteral("could not write %1: %2").arg(Staged, ToQ(Error)));
                    return;
                }
            }
        }

        /* ...and the function, where the cook picks it up. */
        const QString Source = CookSourcePath(InName);
        if(!WriteTextFileIfChanged(ToStd(Source), InResult.Code, Error))
        {
            Fail(QStringLiteral("could not write %1: %2").arg(Source, ToQ(Error)));
            return;
        }

        MaterialCook& Cook = MaterialCooks()[InName];
        if(Cook.bRunning)
        {
            /* Replaces any request already waiting: only the newest matters. */
            Cook.bPending       = true;
            Cook.PendingContext = Guard;
            Cook.PendingDone    = std::move(InDone);
            return;
        }
        RunMaterialCook(InName, InContext, std::move(InDone));
    }

    /* Polls the Renderer for InRequest's answer, giving up after a few
     * seconds (a minimised editor renders nothing). */
    static void PollPreview(const uint64_t Request, QObject* InContext, std::function<void(const QImage&, const QString&)> InDone)
    {
        if(Request == 0)
        {
            InDone(QImage(), QStringLiteral("The viewport's renderer is not running"));
            return;
        }

        /* Poll, giving up after a few seconds (a minimised editor renders nothing). */
        auto* Poll = new QTimer(InContext);
        auto  Tries = std::make_shared<int>(0);
        QObject::connect(Poll, &QTimer::timeout, InContext, [Poll, Request, Tries, InDone]()
        {
            QImage  Image;
            QString Error;
            EditorContext* Now = Context();
            if(Now && Now->TakeMaterialPreview(Request, Image, Error))
            {
                Poll->deleteLater();
                InDone(Image, Error);
                return;
            }
            if(!Now || ++*Tries > 150)
            {
                Poll->deleteLater();
                InDone(QImage(), QStringLiteral("No frame was rendered to draw the preview (is the viewport visible?)"));
            }
        });
        Poll->start(20);
    }

    static EngineCore::Color ToColor(const QColor& InColor)
    {
        return EngineCore::Color(uint8_t(InColor.red()), uint8_t(InColor.green()), uint8_t(InColor.blue()), uint8_t(InColor.alpha()));
    }

    void RequestMaterialPreview(const QString& InName, const int InSize, const QColor& InTint, QObject* InContext,
                                std::function<void(const QImage&, const QString&)> InDone)
    {
        EditorContext* Ctx = Context();
        PollPreview(Ctx ? Ctx->RequestMaterialPreview(InName, InSize, ToColor(InTint)) : 0, InContext, std::move(InDone));
    }

    void RequestMeshPreview(const MeshPreviewOptions& InOptions, QObject* InContext,
                            std::function<void(const QImage&, const QString&)> InDone)
    {
        EditorContext* Ctx = Context();
        PollPreview(Ctx ? Ctx->RequestMeshPreview(InOptions.Mesh, InOptions.Material, InOptions.Sections, InOptions.Width,
                                                  InOptions.Height, InOptions.Yaw, InOptions.Pitch, InOptions.Zoom, ToColor(InOptions.Tint))
                        : 0,
                    InContext, std::move(InDone));
    }

    void RestoreMaterial(const QString& InGraphPath)
    {
        Graph Saved;
        std::string Error;
        if(!LoadGraphFile(ToStd(InGraphPath), Saved, Error)) { return; }

        CompileResult Result;
        CompileGraph(Saved, Result);
        if(!Result.HasErrors())
        {
            StageMaterial(GetMaterialName(InGraphPath), Result, qApp, nullptr);
        }
    }

    bool ApplyMaterialToSelection(const QString& InName, QString& OutMessage)
    {
        EditorContext* Ctx = Context();
        if(!Ctx)
        {
            OutMessage = QStringLiteral("No scene to apply it to");
            return false;
        }
        const GameCore::EntityID Target = Ctx->GetSelection();
        if(!Target.IsSet() || !Ctx->GetWorld().IsAlive(Target))
        {
            OutMessage = QStringLiteral("Select an entity in the Outliner or the viewport first");
            return false;
        }
        const QByteArray Name = InName.toUtf8();
        if(Name.size() >= int(RenderCore::RENDER_MATERIAL_NAME_MAX))
        {
            OutMessage = QStringLiteral("The material name '%1' is too long").arg(InName);
            return false;
        }

        const GameCore::ComponentTypeID Type = RenderCore::RenderComponent::StaticType().ID;
        const GameCore::Entity Handle = Ctx->GetWorld().GetEntity(Target);
        if(!Handle.GetComponent<RenderCore::RenderComponent>() && !Ctx->AddComponent(Target, Type))
        {
            OutMessage = QStringLiteral("Could not add a RenderComponent to %1").arg(Ctx->GetDisplayName(Target));
            return false;
        }

        /* Written in place, then reported, like a Details edit. */
        RenderCore::RenderComponent* Render = Handle.GetComponent<RenderCore::RenderComponent>();
        /* Slot 0: the quad's, and a mesh's first section's. */
        std::memset(Render->Materials[0], 0, sizeof(Render->Materials[0]));
        std::memcpy(Render->Materials[0], Name.constData(), size_t(Name.size()));
        Ctx->NotifyPropertyEdited(Target, Type);

        OutMessage = QStringLiteral("%1 now draws with %2").arg(Ctx->GetDisplayName(Target), InName);
        return true;
    }

    bool CanCheckShaders()
    {
        const QString Python = QString::fromUtf8(NIMBLEEDITOR_PYTHON);
        const QString Script = QString::fromUtf8(NIMBLEEDITOR_MATERIAL_CHECK_SCRIPT);
        return !Python.isEmpty() && !Script.isEmpty() && QFileInfo::exists(Script);
    }

    void CheckShader(const QString& InShaderPath, QObject* InContext, std::function<void(bool, const QString&)> InDone)
    {
        if(!CanCheckShaders()) { return; }

        auto* Process = new QProcess(InContext);
        Process->setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(Process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), InContext,
                         [Process, InDone](const int InCode, const QProcess::ExitStatus InStatus)
        {
            const QString Output = QString::fromUtf8(Process->readAll());
            const bool bPassed = InStatus == QProcess::NormalExit && InCode == 0;
            if(!bPassed) { CORE_LOG_WARN("[Graph] shader check failed:\n" + ToStd(Output)) }
            InDone(bPassed, Output);
            Process->deleteLater();
        });
        QObject::connect(Process, &QProcess::errorOccurred, InContext, [Process](const QProcess::ProcessError)
        {
            CORE_LOG_WARN("[Graph] could not run the shader check: " + ToStd(Process->errorString()))
            Process->deleteLater();
        });

        Process->start(QString::fromUtf8(NIMBLEEDITOR_PYTHON),
                       { QString::fromUtf8(NIMBLEEDITOR_MATERIAL_CHECK_SCRIPT), QStringLiteral("-check"), InShaderPath });
    }

    int RegenerateAll(QStringList& OutLines)
    {
        QStringList Paths;
        FindGraphs(QDir(GetProjectRoot()), Paths);

        int Failures = 0;
        for(const QString& Path : Paths)
        {
            Graph Loaded;
            std::string Error;
            std::vector<std::string> Warnings;
            if(!LoadGraphFile(ToStd(Path), Loaded, Error, &Warnings))
            {
                OutLines << QStringLiteral("FAILED  %1: %2").arg(Path, ToQ(Error));
                ++Failures;
                continue;
            }

            CompileResult Result;
            QString Message;
            if(!WriteOutput(Path, Loaded, Result, Message))
            {
                OutLines << QStringLiteral("FAILED  %1").arg(ToQ(FormatDiagnostics(Loaded, Result)));
                ++Failures;
                continue;
            }
            OutLines << QStringLiteral("ok      %1").arg(Message);
        }
        if(Paths.isEmpty()) { OutLines << QStringLiteral("No node graphs under %1").arg(GetProjectRoot()); }
        return Failures;
    }

    GraphEditorTab* OpenGraph(const QString& InPath, QWidget* InParent)
    {
        const QString Key = Canonical(InPath);
        for(GraphEditorTab* Each : OpenTabs())
        {
            if(Canonical(Each->GetDocument().GetPath()) == Key)
            {
                GraphEditorWindow::Get().ShowTab(Each);
                return Each;
            }
        }

        Graph Loaded;
        std::string Error;
        std::vector<std::string> Warnings;
        if(!LoadGraphFile(ToStd(InPath), Loaded, Error, &Warnings))
        {
            QMessageBox::warning(InParent, QStringLiteral("Open Graph"), ToQ(Error));
            return nullptr;
        }
        for(const std::string& Each : Warnings) { CORE_LOG_WARN("[Graph] " + Each) }

        /* A tab of the one graph editor window. */
        auto* Tab = new GraphEditorTab(new GraphDocument(QFileInfo(InPath).absoluteFilePath(), std::move(Loaded)));
        GraphEditorWindow::Get().AddTab(Tab);
        return Tab;
    }

    GraphEditorTab* CreateGraph(const Schema& InSchema, QWidget* InParent)
    {
        const QString Folder = GetDefaultFolder(InSchema);
        QDir().mkpath(Folder);

        QString Path = QFileDialog::getSaveFileName(InParent, QStringLiteral("New %1 Graph").arg(ToQ(InSchema.GetName())),
                                                    QDir(Folder).filePath(ToQ(MakeGraphPath("New" + InSchema.GetName(), InSchema))),
                                                    GetFileFilter(&InSchema));
        if(Path.isEmpty()) { return nullptr; }

        /* Normalize to one compound extension: the Windows dialog may append
         * "script.nmgraph" to a name that already has it. */
        std::string Base = ToStd(Path);
        for(std::string Stripped = StripGraphExtension(Base); Stripped != Base; Stripped = StripGraphExtension(Base))
        {
            Base = Stripped;
        }
        Path = ToQ(MakeGraphPath(Base, InSchema));

        Graph Starter = StarterGraph(InSchema);
        Starter.Name = GetGraphBaseName(ToStd(Path));

        std::string Error;
        if(!SaveGraphFile(ToStd(Path), Starter, Error))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Graph"), QStringLiteral("Could not write\n%1\n\n%2").arg(Path, ToQ(Error)));
            return nullptr;
        }

        CompileResult Result;
        QString Message;
        WriteOutput(Path, Starter, Result, Message);
        return OpenGraph(Path, InParent);
    }

    bool CloseAllGraphs()
    {
        /* The window asks each graph in turn; it stays open on a Cancel. */
        GraphEditorWindow* Window = GraphEditorWindow::Find();
        return !Window || Window->close();
    }
}

#endif // WITH_EDITOR
