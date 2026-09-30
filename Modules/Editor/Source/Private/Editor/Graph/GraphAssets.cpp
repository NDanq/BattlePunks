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

#include "Public/Core/CoreLog.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Private/Editor/Graph/GraphDocument.h"
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
    /* Every open graph window; QPointer, so a closed one reads as null. */
    QList<QPointer<GraphEditorWindow>>& OpenWindows()
    {
        static QList<QPointer<GraphEditorWindow>> Windows;
        return Windows;
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
        return QDir(GetProjectRoot()).filePath(InSchema.GetName() == "Script" ? QStringLiteral("Scripts") : QStringLiteral("Shaders"));
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

    bool WriteOutput(const QString& InGraphPath, const Graph& InGraph, CompileResult& OutResult, QString& OutMessage)
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

        OutMessage = QStringLiteral("Saved %1 -> %2").arg(Name, QFileInfo(Output).fileName());

        const QString Staged = StagedPathFor(Output);
        if(!Staged.isEmpty())
        {
            if(WriteTextFileIfChanged(ToStd(Staged), OutResult.Code, Error)) { OutMessage += QStringLiteral(" (staged for Play)"); }
            else { CORE_LOG_WARN("[Graph] could not stage " + ToStd(Staged) + ": " + Error) }
        }
        CORE_LOG("[Graph] " + ToStd(OutMessage))
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

    GraphEditorWindow* OpenGraph(const QString& InPath, QWidget* InParent)
    {
        const QString Key = Canonical(InPath);
        for(const QPointer<GraphEditorWindow>& Each : OpenWindows())
        {
            if(Each && Canonical(Each->GetDocument().GetPath()) == Key)
            {
                Each->show();
                Each->raise();
                Each->activateWindow();
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

        /* Top-level and unowned: a graph window may sit behind the main one. */
        auto* Window = new GraphEditorWindow(new GraphDocument(QFileInfo(InPath).absoluteFilePath(), std::move(Loaded)));
        OpenWindows().removeAll(nullptr);
        OpenWindows() << Window;
        Window->show();
        return Window;
    }

    GraphEditorWindow* CreateGraph(const Schema& InSchema, QWidget* InParent)
    {
        const QString Folder = GetDefaultFolder(InSchema);
        QDir().mkpath(Folder);

        QString Path = QFileDialog::getSaveFileName(InParent, QStringLiteral("New %1 Graph").arg(ToQ(InSchema.GetName())),
                                                    QDir(Folder).filePath(ToQ(MakeGraphPath("New" + InSchema.GetName(), InSchema))),
                                                    GetFileFilter(&InSchema));
        if(Path.isEmpty()) { return nullptr; }

        /* Whatever was typed, the file gets this schema's compound extension,
         * once. The Windows dialog sees only ".nmgraph" as the extension, so it
         * appends the filter's whole "script.nmgraph" to a name that already
         * ends in it -- every graph extension comes off before one goes back on. */
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
        for(const QPointer<GraphEditorWindow>& Each : OpenWindows())
        {
            if(Each && !Each->ConfirmClose()) { return false; }
        }
        for(const QPointer<GraphEditorWindow>& Each : OpenWindows())
        {
            if(Each)
            {
                Each->GetDocument().GetHistory().setClean();   // already confirmed; do not ask twice
                Each->close();
            }
        }
        OpenWindows().clear();
        return true;
    }
}

#endif // WITH_EDITOR
