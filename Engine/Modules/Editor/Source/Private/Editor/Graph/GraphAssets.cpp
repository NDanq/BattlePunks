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

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameModule.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/RenderCore/MaterialDescription.h"
#include "Public/RenderCore/PostProcessStack.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/UI/UIDocument.h"
#include "Public/Editor/EditorContext.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Public/Editor/EditorDocument.h"
#include "Private/Editor/Content/ContentPaths.h"
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

    /* A material's stem: its runtime path without the extension
     * ("Materials/Glow"); where its cooked files go. Empty if not a graph path. */
    QString MaterialStem(const QString& InMaterial)
    {
        const QString Extension = QString::fromLatin1(RenderCore::MATERIAL_GRAPH_EXTENSION);
        return InMaterial.endsWith(Extension) ? InMaterial.left(InMaterial.size() - Extension.size()) : QString();
    }

    /* Where a material's function waits to be cooked: the cook names its
     * permutations after the file, and reads <Name>.material beside it, so
     * both go in a folder of their own, by stem. */
    QString CookSourceDir()
    {
        return QDir::temp().filePath(QStringLiteral("NimbleMaterialCook"));
    }

    QString CookSourcePath(const QString& InMaterial)
    {
        return QDir(CookSourceDir()).filePath(MaterialStem(InMaterial) + QStringLiteral(".mtl.glsl"));
    }

    /* The folders the stem is in ("Materials"), which the cook puts its permutations below. */
    QString CookSubdir(const QString& InMaterial)
    {
        const QString Stem = MaterialStem(InMaterial);
        const qsizetype Slash = Stem.lastIndexOf(QLatin1Char('/'));
        return Slash < 0 ? QString() : Stem.left(Slash);
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
                         QStringLiteral("-subdir"), CookSubdir(InName) });
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
        else if(InSchema.GetName() == "UI")
        {
            Add("UI.Event.Open", -300.0f, -120.0f);
        }
        else if(InSchema.GetName() == "PostProcess")
        {
            /* A soft glow and smooth edges: the effects most scenes want. */
            Add("Effect.Bloom", -150.0f, 0.0f);
            Add("Effect.Antialiasing", 150.0f, 0.0f);
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
        const QString Folder = InSchema.GetName() == "Script"      ? QStringLiteral("Scripts")
                             : InSchema.GetName() == "UI"          ? QStringLiteral("Content/UI")
                             : InSchema.GetName() == "PostProcess" ? QStringLiteral("Content/PostProcess")
                                                                   : QStringLiteral("Content/Materials");
        return QDir(GetProjectRoot()).filePath(Folder);
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

    bool CompileAndSave(const QString& InGraphPath, Graph& InOutGraph, CompileResult& OutResult, std::string& OutError)
    {
        CompileGraph(InOutGraph, OutResult);
        StoreGeneratedCode(InOutGraph, OutResult);   // unchanged if it has errors
        return SaveGraphFile(ToStd(InGraphPath), InOutGraph, OutError);
    }

    bool StageOutput(const QString& InGraphPath, const Graph& InGraph, const CompileResult& InResult, QString& OutMessage,
                     QObject* InContext, std::function<void(bool, const QString&)> InStaged)
    {
        const QString Name = QFileInfo(InGraphPath).fileName();
        const Schema* S = FindSchema(InGraph.SchemaName);
        if(!S)
        {
            OutMessage = QStringLiteral("%1: unknown schema").arg(Name);
            return false;
        }
        if(InResult.HasErrors())
        {
            OutMessage = QStringLiteral("Saved %1 -- it has errors, so its generated code was not updated").arg(Name);
            CORE_LOG_WARN("[Graph] " + FormatDiagnostics(InGraph, InResult))
            return false;
        }

        OutMessage = QStringLiteral("Saved %1 with its generated code").arg(Name);

        if(S->GetName() == "Material")
        {
            /* By path, cooked for the viewport; see the header. */
            const QString Material = GetMaterialPath(InGraphPath);
            if(Material.isEmpty())
            {
                OutMessage += QStringLiteral(" (outside the project: not staged)");
            }
            else if(InStaged)
            {
                StageMaterial(Material, InResult, InContext ? InContext : qApp, std::move(InStaged));
            }
            else
            {
                StageMaterial(Material, InResult, qApp, [Material](const bool bInWorked, const QString& InWhat)
                {
                    if(bInWorked) { CORE_LOG("[Graph] material '" + ToStd(Material) + "': " + ToStd(InWhat)) }
                });
            }
            if(!Material.isEmpty()) { OutMessage += QStringLiteral(" (cooking for the viewport)"); }
        }
        else
        {
            /* The graph itself: the runtime runs its <gen_code>. */
            const QString Staged = StagedPathFor(InGraphPath);
            std::string Error;
            if(!Staged.isEmpty())
            {
                if(WriteTextFileIfChanged(ToStd(Staged), WriteGraphText(InGraph), Error)) { OutMessage += QStringLiteral(" (staged for Play)"); }
                else { CORE_LOG_WARN("[Graph] could not stage " + ToStd(Staged) + ": " + Error) }
            }
            /* A post process stack is read as it is used: the viewport's next
             * frame reads the new one. */
            if(S->GetName() == "PostProcess")
            {
                const QString Runtime = EditorContent::ToRuntimePath(InGraphPath);
                if(!Runtime.isEmpty()) { RenderCore::PostProcessStackCache::Invalidate(ToStd(Runtime)); }
            }
            /* A UI document's logic: the document reloads in Play, and with it its logic (UISystem). */
            const std::string Document = InGraph.GetProperty("Document", std::string());
            if(S->GetName() == "UI" && !Document.empty()) { UI::UIAssetCache::Invalidate(Document); }
        }
        CORE_LOG("[Graph] " + ToStd(OutMessage))
        return true;
    }

    // === Materials in the viewport ===========================================

    void SetEditorContext(EditorContext* InContext)
    {
        Context() = InContext;
    }

    QString GetMaterialPath(const QString& InGraphPath)
    {
        const QString Runtime = EditorContent::ToRuntimePath(InGraphPath);
        return MaterialStem(Runtime).isEmpty() ? QString() : Runtime;
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
        const QString Stem = MaterialStem(InName);
        if(Stem.isEmpty())
        {
            Fail(QStringLiteral("'%1' is not a material graph's path").arg(InName));
            return;
        }
        if(InName.toUtf8().size() >= int(RenderCore::RENDER_MATERIAL_PATH_MAX))
        {
            Fail(QStringLiteral("the path '%1' is too long for a primitive to refer to").arg(InName));
            return;
        }

        /* The description, where RenderCore reads it and beside the function
         * for the cook (which picks the permutations from it)... */
        std::string Error;
        for(const auto& Companion : InResult.Companions)
        {
            for(const QString& Folder : { QCoreApplication::applicationDirPath(), CookSourceDir() })
            {
                const QString Staged = QDir(Folder).filePath(Stem + ToQ(Companion.first));
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

        /* What the saved graph's <gen_code> holds; compiled if it has none. */
        CompileResult Result;
        const GeneratedCode* Function = Saved.FindGenCode(".mtl.glsl");
        if(Function)
        {
            Result.Code = Function->Text;
            for(const GeneratedCode& Each : Saved.GenCode)
            {
                if(&Each != Function) { Result.Companions.emplace_back(Each.Extension, Each.Text); }
            }
        }
        else
        {
            CompileGraph(Saved, Result);
        }
        const QString Material = GetMaterialPath(InGraphPath);
        if(!Result.HasErrors() && !Material.isEmpty())
        {
            StageMaterial(Material, Result, qApp, nullptr);
        }
    }

    bool ApplyMaterialToSelection(const QString& InName, QString& OutMessage)
    {
        if(MaterialStem(InName).isEmpty())
        {
            OutMessage = QStringLiteral("'%1' is not a material graph in the project").arg(InName);
            return false;
        }
        /* The level's selection: the one document there is until the Prefab
         * Editor adds its own. */
        EditorDocument* Doc = Context() ? &Context()->GetLevel() : nullptr;
        if(!Doc)
        {
            OutMessage = QStringLiteral("No scene to apply it to");
            return false;
        }
        const GameCore::EntityID Target = Doc->GetSelection();
        if(!Target.IsSet() || !Doc->GetWorld().IsAlive(Target))
        {
            OutMessage = QStringLiteral("Select an entity in the Outliner or the viewport first");
            return false;
        }
        const QByteArray Name = InName.toUtf8();
        if(Name.size() >= int(RenderCore::RENDER_MATERIAL_PATH_MAX))
        {
            OutMessage = QStringLiteral("The material path '%1' is too long").arg(InName);
            return false;
        }

        const GameCore::ComponentTypeID Type = RenderCore::RenderComponent::StaticType().ID;
        const GameCore::Entity Handle = Doc->GetWorld().GetEntity(Target);
        if(!Handle.GetComponent<RenderCore::RenderComponent>() && !Doc->AddComponent(Target, Type))
        {
            OutMessage = QStringLiteral("Could not add a RenderComponent to %1").arg(Doc->GetDisplayName(Target));
            return false;
        }

        /* Written in place, then reported, like a Details edit. */
        RenderCore::RenderComponent* Render = Handle.GetComponent<RenderCore::RenderComponent>();
        /* Slot 0: the quad's, and a mesh's first section's. */
        std::memset(Render->Materials[0], 0, sizeof(Render->Materials[0]));
        std::memcpy(Render->Materials[0], Name.constData(), size_t(Name.size()));
        Doc->NotifyPropertyEdited(Target, Type);

        OutMessage = QStringLiteral("%1 now draws with %2").arg(Doc->GetDisplayName(Target), InName);
        return true;
    }

    bool CanCheckShaders()
    {
        const QString Python = QString::fromUtf8(NIMBLEEDITOR_PYTHON);
        const QString Script = QString::fromUtf8(NIMBLEEDITOR_MATERIAL_CHECK_SCRIPT);
        return !Python.isEmpty() && !Script.isEmpty() && QFileInfo::exists(Script);
    }

    void CheckShader(const QString& InGraphPath, QObject* InContext, std::function<void(bool, const QString&)> InDone)
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
                       { QString::fromUtf8(NIMBLEEDITOR_MATERIAL_CHECK_SCRIPT), QStringLiteral("-check"), InGraphPath });
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
            if(!CompileAndSave(Path, Loaded, Result, Error))
            {
                OutLines << QStringLiteral("FAILED  %1: %2").arg(Path, ToQ(Error));
                ++Failures;
                continue;
            }
            if(!StageOutput(Path, Loaded, Result, Message))
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

    bool RefreshGameDeclarations(const bool bInGameIsLoaded)
    {
        const auto Describe = []()
        {
            std::string Names;
            for(size_t Index = 0; Index < GameCore::GetScriptLibraryCount(); ++Index)
            {
                GameCore::ScriptLibraryInfo Info;
                if(GameCore::GetScriptLibrary(Index, Info)) { Names += (Names.empty() ? "" : ", ") + std::string(Info.Name); }
            }
            return Names.empty() ? std::string("no script libraries") : "script libraries " + Names;
        };

        if(bInGameIsLoaded)
        {
            RefreshSchemas();
            EditorPrefabs::CacheNativePrefabs();
            CORE_LOG("[Graphs] script nodes refreshed from the running game: " + Describe())
            return true;
        }

        const EngineCore::ConfigVar* ModuleVar = EngineCore::ConfigFindVar("Game", "Module");
        if(!ModuleVar || ModuleVar->GetString()[0] == '\0')
        {
            CORE_LOG("[Graphs] no [Game] Module; script graphs offer the engine's nodes only")
            return false;
        }

        /* Its own host, as Play has: loaded for the copy, then gone. */
        NimbleEngine::GameModuleHost Game;
        if(!Game.Load(""))
        {
            CORE_LOG_WARN("[Graphs] could not load the game module for its script declarations; build it (F7), then Build > Refresh Script Nodes")
            return false;
        }
        RefreshSchemas();
        EditorPrefabs::CacheNativePrefabs();
        CORE_LOG("[Graphs] script nodes refreshed from " + Game.GetModuleName() + ": " + Describe())
        Game.Unload();
        return true;
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

    GraphEditorTab* CreateGraph(const Schema& InSchema, QWidget* InParent, const QString& InFolder)
    {
        const QString Folder = InFolder.isEmpty() ? GetDefaultFolder(InSchema) : InFolder;
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

        QString Error;
        if(!CreateGraphFile(InSchema, Path, Error))
        {
            QMessageBox::warning(InParent, QStringLiteral("New Graph"), QStringLiteral("Could not write\n%1\n\n%2").arg(Path, Error));
            return nullptr;
        }
        return OpenGraph(Path, InParent);
    }

    bool CreateGraphFile(const Schema& InSchema, const QString& InPath, QString& OutError,
                         const std::map<std::string, std::string>& InProperties)
    {
        Graph Starter = StarterGraph(InSchema);
        Starter.Name = GetGraphBaseName(ToStd(InPath));
        for(const auto& Each : InProperties) { Starter.Properties[Each.first] = Each.second; }

        std::string Error;
        CompileResult Result;
        if(!CompileAndSave(InPath, Starter, Result, Error))
        {
            OutError = ToQ(Error);
            return false;
        }
        QString Message;
        StageOutput(InPath, Starter, Result, Message);
        return true;
    }

    bool CloseAllGraphs()
    {
        /* The window asks each graph in turn; it stays open on a Cancel. */
        GraphEditorWindow* Window = GraphEditorWindow::Find();
        return !Window || Window->close();
    }
}

#endif // WITH_EDITOR
