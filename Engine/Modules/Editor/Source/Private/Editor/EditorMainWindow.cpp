#if WITH_EDITOR

#include "Public/Editor/EditorMainWindow.h"

#include <algorithm>

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QDirIterator>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QUndoStack>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/Editor/ConsolePanel.h"
#include "Public/Editor/ContentBrowserPanel.h"
#include "Public/Editor/DetailsPanel.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/OutlinerPanel.h"
#include "Public/Editor/ViewportPanel.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/NodeGraph/Schema.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Private/Editor/EditorLights.h"
#include "Private/Editor/EditorPrimitives.h"
#include "Private/Editor/AboutDialog.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Private/Editor/Prefab/PrefabEditorTab.h"
#include "Private/Editor/Prefab/PrefabEditorWindow.h"
#include "Private/Editor/Project/NewProjectWizard.h"
#include "Private/Editor/ProjectSettingsDialog.h"
#include "Private/Editor/SDLInputProbe.h"
#include "Private/Editor/SettingsDialog.h"
#include "Private/Editor/SourceBuild.h"

/* Edit > Editor Settings > Build. */
NIMBLE_CONFIG_BOOL(GEditorReloadAfterCompile, "Editor", "ReloadAfterCompile", true, nullptr);

/* The world the editor opens when none is named on its command line: a
 * project path ("Content/Levels/LightLab.nmworld"). Empty, or missing: the Sandbox
 * scene. Set in the project's DefaultEngine.ini. */
NIMBLE_CONFIG_STRING(GEditorStartupWorld, "Editor", "StartupWorld", "", nullptr);

namespace
{
    const char* const WorldFileFilter = "Nimble World (*.nmworld);;All Files (*)";

    /* Where Open and Save As start: the open world, else <project>/Content/Levels. */
    QString WorldDialogPath(const QString& InCurrentPath)
    {
        if(!InCurrentPath.isEmpty()) { return InCurrentPath; }
        const QString Levels = QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content/Levels"));
        return QFileInfo(Levels).isDir() ? Levels : QDir(EditorGraphs::GetProjectRoot()).filePath(QStringLiteral("Content"));
    }

    /* A world saved inside the project, copied beside the executable too --
     * its manifest and its folder of entity and instance files, stale ones removed --
     * so the next Play loads what was saved without a build. */
    void StageWorldPackage(const QString& InPath)
    {
        const QString Staged = EditorGraphs::GetStagedPath(InPath);
        if(Staged.isEmpty())
        {
            return;
        }
        const QFileInfo Source(InPath);
        const QFileInfo Target(Staged);
        QDir().mkpath(Target.absolutePath());
        QFile::remove(Staged);
        QFile::copy(InPath, Staged);

        const QDir SourceEntities(Source.dir().filePath(Source.completeBaseName()));
        QDir TargetEntities(Target.dir().filePath(Target.completeBaseName()));
        TargetEntities.removeRecursively();
        if(SourceEntities.exists())
        {
            QDir().mkpath(TargetEntities.absolutePath());
            for(const QString& Each : SourceEntities.entryList(QDir::Files))
            {
                QFile::copy(SourceEntities.filePath(Each), TargetEntities.filePath(Each));
            }
        }
    }

    const char* BackendName(const RenderCore::ERenderBackend InBackend)
    {
        switch(InBackend)
        {
            case RenderCore::ERenderBackend::Null:   return "Null";
            case RenderCore::ERenderBackend::OpenGL: return "OpenGL";
            case RenderCore::ERenderBackend::Vulkan: return "Vulkan";
            case RenderCore::ERenderBackend::Metal:  return "Metal";
            case RenderCore::ERenderBackend::D3D12:  return "D3D12";
        }
        return "?";
    }
}

EditorMainWindow::EditorMainWindow(EditorContext& InContext, const RenderCore::ERenderBackend InBackend, QWidget* InParent)
    : QMainWindow(InParent)
    , Context(InContext)
    , Level(InContext.GetLevel())
    , Backend(InBackend)
{
    resize(1600, 900);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);

    ViewportArea = new ViewportPanel(Context, Level, this);
    Viewport     = ViewportArea->GetViewport();
    Context.SetPlayInputWidget(Viewport);   // the game reads the viewport's input while playing
    setCentralWidget(ViewportArea);

    auto* OutlinerDock = new QDockWidget(QStringLiteral("Outliner"), this);
    OutlinerDock->setObjectName(QStringLiteral("OutlinerDock"));
    Outliner = new OutlinerPanel(Level, OutlinerDock);
    OutlinerDock->setWidget(Outliner);
    addDockWidget(Qt::RightDockWidgetArea, OutlinerDock);

    auto* DetailsDock = new QDockWidget(QStringLiteral("Details"), this);
    DetailsDock->setObjectName(QStringLiteral("DetailsDock"));
    Details = new DetailsPanel(Level, DetailsDock);
    DetailsDock->setWidget(Details);
    addDockWidget(Qt::RightDockWidgetArea, DetailsDock);

    auto* ConsoleDock = new QDockWidget(QStringLiteral("Console"), this);
    ConsoleDock->setObjectName(QStringLiteral("ConsoleDock"));
    ConsoleArea = new ConsolePanel(Console, ConsoleDock);
    ConsoleDock->setWidget(ConsoleArea);
    addDockWidget(Qt::BottomDockWidgetArea, ConsoleDock);

    /* Beside the Console, in front: the project's files, where imports land. */
    auto* ContentDock = new QDockWidget(QStringLiteral("Content Browser"), this);
    ContentDock->setObjectName(QStringLiteral("ContentBrowserDock"));
    ContentArea = new ContentBrowserPanel(ContentDock);
    ContentDock->setWidget(ContentArea);
    addDockWidget(Qt::BottomDockWidgetArea, ContentDock);
    tabifyDockWidget(ConsoleDock, ContentDock);
    ContentDock->raise();
    connect(ContentArea, &ContentBrowserPanel::OpenWorldRequested, this, &EditorMainWindow::OpenWorldFile);
    connect(ContentArea, &ContentBrowserPanel::StatusMessage, this, [this](const QString& InText)
    {
        statusBar()->showMessage(InText, 6000);
    });

    resizeDocks({ OutlinerDock, DetailsDock }, { 300, 360 }, Qt::Horizontal);
    resizeDocks({ ConsoleDock }, { 240 }, Qt::Vertical);

    RegisterConsoleCommands();

    PlayLabel = new QLabel(this);
    PlayLabel->setStyleSheet(QStringLiteral("QLabel { color: rgb(90, 200, 110); font-weight: bold; }"));
    statusBar()->addPermanentWidget(PlayLabel);

    StatsLabel = new QLabel(this);
    statusBar()->addPermanentWidget(StatsLabel);

    /* What a handle drag has done so far, in the status bar's message area. */
    connect(Viewport, &ViewportWidget::GizmoDragStatus, this, [this](const QString& InText)
    {
        if(InText.isEmpty())
        {
            statusBar()->clearMessage();
        }
        else
        {
            statusBar()->showMessage(InText);
        }
    });

    /* Before anything builds the schemas, so the game's components and script
     * libraries are nodes from the first graph opened. */
    EditorGraphs::RefreshGameDeclarations(false);

    BuildMenus();
    BuildPlayControls();
    BuildBuildMenu();
    BuildHelpMenu();   // last, so Help is the rightmost menu
    connect(&Context, &EditorContext::PlayStateChanged, this, &EditorMainWindow::OnPlayStateChanged);

    connect(&StatsTimer, &QTimer::timeout, this, &EditorMainWindow::UpdateStats);

    /* Material graphs cook for, and reload into, this context's renderer;
     * prefabs draw with it. */
    EditorGraphs::SetEditorContext(&Context);
    EditorPrefabs::SetEditorContext(&Context);

    UpdateTitle();
}

EditorMainWindow::~EditorMainWindow()
{
    /* Normally already done by closeEvent; this covers a window destroyed
     * without being closed. The viewport's native window is still alive here. */
    if(InputProbe)
    {
        InputProbe->Stop();
    }
    EditorGraphs::SetEditorContext(nullptr);
    EditorPrefabs::SetEditorContext(nullptr);
    Context.Shutdown();
}

void EditorMainWindow::BuildMenus()
{
    QMenu* File = menuBar()->addMenu(QStringLiteral("&File"));

    QAction* NewProject = File->addAction(QStringLiteral("New Pro&ject..."));
    NewProject->setToolTip(QStringLiteral("A new game in Games/, from Engine/Templates/Game"));
    connect(NewProject, &QAction::triggered, this, [this]()
    {
        NewProjectWizard Wizard(this);
        Wizard.exec();
    });

    File->addSeparator();

    QAction* New = File->addAction(QStringLiteral("&New Scene"));
    New->setShortcut(QKeySequence::New);
    connect(New, &QAction::triggered, this, [this]()
    {
        Level.NewScene();
        CurrentPath.clear();
        UpdateTitle();
    });

    QAction* SandboxScene = File->addAction(QStringLiteral("New &Sandbox Scene"));
    SandboxScene->setToolTip(QStringLiteral("The Sandbox level as it stands once loaded: Marker, Camera and Scripted"));
    connect(SandboxScene, &QAction::triggered, this, [this]()
    {
        Level.CreateSandboxScene();
        Viewport->ApplySandboxView();
        CurrentPath.clear();
        UpdateTitle();
    });

    File->addSeparator();

    QAction* Open = File->addAction(QStringLiteral("&Open World..."));
    Open->setShortcut(QKeySequence::Open);
    connect(Open, &QAction::triggered, this, &EditorMainWindow::OpenWorld);

    QAction* Save = File->addAction(QStringLiteral("&Save World"));
    Save->setShortcut(QKeySequence::Save);
    connect(Save, &QAction::triggered, this, &EditorMainWindow::SaveWorld);

    QAction* SaveAs = File->addAction(QStringLiteral("Save World &As..."));
    SaveAs->setShortcut(QKeySequence::SaveAs);
    connect(SaveAs, &QAction::triggered, this, &EditorMainWindow::SaveWorldAs);

    /* Save stays: it writes the edited World, which a run never touches. */
    SceneActions = { New, SandboxScene, Open };

    File->addSeparator();

    /* Node graphs open in windows of their own, beside the scene. */
    for(const NodeGraph::Schema* Schema : NodeGraph::GetSchemas())
    {
        QAction* NewGraph = File->addAction(QStringLiteral("New %1 &Graph...").arg(QString::fromStdString(Schema->GetName())));
        NewGraph->setToolTip(QStringLiteral("A %1 graph: generates %2")
                             .arg(QString::fromStdString(Schema->GetName()), QString::fromStdString(Schema->GetOutputExtension())));
        /* By name: a refresh (RefreshGameDeclarations) replaces the schema. */
        const std::string SchemaName = Schema->GetName();
        connect(NewGraph, &QAction::triggered, this, [this, SchemaName]()
        {
            if(const NodeGraph::Schema* Current = NodeGraph::FindSchema(SchemaName)) { EditorGraphs::CreateGraph(*Current, this); }
        });
    }

    QAction* Import = File->addAction(QStringLiteral("&Import..."));
    Import->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    Import->setToolTip(QStringLiteral("glTF meshes and images into engine assets, in the Content Browser's folder"));
    connect(Import, &QAction::triggered, this, [this]() { ContentArea->ImportWithDialog(); });

    QAction* NewPrefab = File->addAction(QStringLiteral("New &Prefab..."));
    NewPrefab->setToolTip(QStringLiteral("A one-entity prefab in the Content Browser's folder, opened in the Prefab Editor"));
    connect(NewPrefab, &QAction::triggered, this, [this]()
    {
        const QString Folder = ContentArea->GetCurrentFolder();
        EditorPrefabs::CreatePrefab(Folder.startsWith(EditorGraphs::GetProjectRoot()) ? Folder : QString(), this);
    });

    QAction* OpenPrefab = File->addAction(QStringLiteral("Open Pre&fab..."));
    connect(OpenPrefab, &QAction::triggered, this, [this]()
    {
        const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Prefab"), EditorPrefabs::GetDefaultFolder(),
                                                          QStringLiteral("Prefabs (*.nmprefab)"));
        if(!Path.isEmpty()) { EditorPrefabs::OpenPrefab(Path, this); }
    });

    QAction* OpenGraph = File->addAction(QStringLiteral("Open Grap&h..."));
    OpenGraph->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(OpenGraph, &QAction::triggered, this, [this]()
    {
        const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Graph"), EditorGraphs::GetProjectRoot(),
                                                          EditorGraphs::GetFileFilter());
        if(!Path.isEmpty()) { EditorGraphs::OpenGraph(Path, this); }
    });

    File->addSeparator();

    QAction* Exit = File->addAction(QStringLiteral("E&xit"));
    Exit->setShortcut(QKeySequence::Quit);
    connect(Exit, &QAction::triggered, this, &QWidget::close);

    QMenu* Edit = menuBar()->addMenu(QStringLiteral("&Edit"));

    /* Through the context, which refuses while playing or mid-drag. Focused
     * text fields still get Ctrl+Z first. */
    UndoAction = Edit->addAction(QStringLiteral("&Undo"));
    UndoAction->setShortcut(QKeySequence::Undo);
    connect(UndoAction, &QAction::triggered, &Level, &EditorDocument::Undo);

    RedoAction = Edit->addAction(QStringLiteral("&Redo"));
    RedoAction->setShortcuts({ QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y) });
    connect(RedoAction, &QAction::triggered, &Level, &EditorDocument::Redo);

    connect(&Level, &EditorDocument::HistoryChanged, this, &EditorMainWindow::UpdateHistoryActions);
    UpdateHistoryActions();

    Edit->addSeparator();
    Edit->addAction(ViewportArea->GetFrameSelectionAction());
    Edit->addAction(ViewportArea->GetFrameAllAction());

    Edit->addSeparator();
    QAction* Settings = Edit->addAction(QStringLiteral("Editor &Settings..."));
    Settings->setMenuRole(QAction::PreferencesRole);
    Settings->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(Settings, &QAction::triggered, this, &EditorMainWindow::OpenSettings);

    QAction* ProjectSettings = Edit->addAction(QStringLiteral("&Project Settings..."));
    ProjectSettings->setToolTip(QStringLiteral("The project's Config/DefaultEngine.ini: the game's and the engine's settings"));
    connect(ProjectSettings, &QAction::triggered, this, &EditorMainWindow::OpenProjectSettings);

    QMenu* World = menuBar()->addMenu(QStringLiteral("&World"));

    QAction* CreateEntity = World->addAction(QStringLiteral("&Create Empty Entity"));
    CreateEntity->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(CreateEntity, &QAction::triggered, this, [this]()
    {
        /* In front of the camera, so the new entity is somewhere visible. */
        const EditorCamera& Camera = Viewport->GetCamera();
        const EngineCore::Vector3 Spot = Camera.Position + Camera.GetForward() * 600.0;
        Level.SetSelection(Level.CreateEntity(QStringLiteral("Entity"), Spot));
    });

    /* Primitives and lights, in front of the camera like a new entity
     * (EditorPrimitives.h, EditorLights.h). */
    const auto InFrontOfCamera = [this]()
    {
        const EditorCamera& Camera = Viewport->GetCamera();
        return Camera.Position + Camera.GetForward() * 500.0;
    };
    EditorPrimitives::AddPrimitiveActions(World->addMenu(QStringLiteral("Add &Primitive")), Level, InFrontOfCamera);
    EditorLights::AddLightActions(World->addMenu(QStringLiteral("Add &Light")), Level, InFrontOfCamera);

    QAction* DeleteEntity = World->addAction(QStringLiteral("&Delete"));
    DeleteEntity->setShortcut(QKeySequence::Delete);
    connect(DeleteEntity, &QAction::triggered, this, [this]() { Level.DestroyEntity(Level.GetSelection()); });

    World->addSeparator();

    /* Rebuilt on opening: the list follows the game module's last load. */
    QMenu* Natives = World->addMenu(QStringLiteral("&Native Prefabs"));
    connect(Natives, &QMenu::aboutToShow, this, [this, Natives]()
    {
        Natives->clear();
        const QStringList Names = EditorPrefabs::GetNativePrefabs();
        if(Names.isEmpty())
        {
            Natives->addAction(QStringLiteral("(none -- the game module declares none, or is not built)"))->setEnabled(false);
            return;
        }
        for(const QString& Name : Names)
        {
            QMenu* Entry = Natives->addMenu(Name);
            const QString Path = EditorPrefabs::NativePrefabPath(Name);
            Entry->addAction(QStringLiteral("Place in Level"), this, [this, Path]()
            {
                const EditorCamera& Camera = Viewport->GetCamera();
                Level.SetSelection(Level.PlacePrefab(Path, Camera.Position + Camera.GetForward() * 600.0));
            });
            Entry->addAction(QStringLiteral("Open"), this, [this, Path]() { EditorPrefabs::OpenPrefab(Path, this); });
        }
    });

    QAction* ConvertLevel = World->addAction(QStringLiteral("Convert &Level to Prefabs..."));
    ConvertLevel->setToolTip(QStringLiteral("Every entity tree not yet in a prefab instance becomes one: a prefab per distinct tree, "
                                            "each keeping its own values"));
    connect(ConvertLevel, &QAction::triggered, this, [this]() { EditorPrefabs::ConvertLevelToPrefabs(Level, this); });

    QAction* ConvertToPrefab = World->addAction(QStringLiteral("Convert Selection to &Prefab..."));
    ConvertToPrefab->setToolTip(QStringLiteral("Write the selected entity and its children as a prefab, and put an instance in their place"));
    connect(ConvertToPrefab, &QAction::triggered, this, [this]()
    {
        EditorPrefabs::ConvertToPrefab(Level, Level.GetSelection(), this);
    });

    QMenu* View = menuBar()->addMenu(QStringLiteral("&View"));
    for(QDockWidget* Dock : findChildren<QDockWidget*>())
    {
        View->addAction(Dock->toggleViewAction());
    }
    QAction* FocusConsole = View->addAction(QStringLiteral("Focus &Console"));
    FocusConsole->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft));
    connect(FocusConsole, &QAction::triggered, this, [this]()
    {
        auto* Dock = static_cast<QDockWidget*>(ConsoleArea->parentWidget());
        Dock->show();
        Dock->raise();
        ConsoleArea->FocusInput();
    });

    View->addSeparator();
    View->addAction(ViewportArea->GetShowAllBoundsAction());
    View->addAction(ViewportArea->GetShowLightGizmosAction());
    QAction* ResetCamera = View->addAction(QStringLiteral("&Reset Camera"));
    connect(ResetCamera, &QAction::triggered, this, [this]() { Viewport->GetCamera() = EditorCamera(); });

    View->addSeparator();
    InputProbeAction = View->addAction(QStringLiteral("SDL Input &Probe"));
    InputProbeAction->setCheckable(true);
    InputProbeAction->setToolTip(QStringLiteral("Route the viewport's input through SDL and report it -- the Play-in-Editor input spike"));
    connect(InputProbeAction, &QAction::toggled, this, &EditorMainWindow::SetSDLInputProbe);
}

void EditorMainWindow::BuildHelpMenu()
{
    QMenu* Help = menuBar()->addMenu(QStringLiteral("&Help"));

    QAction* About = Help->addAction(QStringLiteral("&About NimbleEditor..."));
    About->setMenuRole(QAction::AboutRole);
    connect(About, &QAction::triggered, this, [this]()
    {
        /* Modal; deletes itself on close. */
        auto* Dialog = new AboutDialog(QString::fromLatin1(BackendName(Backend)), this);
        Dialog->open();
    });
}

void EditorMainWindow::BuildPlayControls()
{
    QMenu* PlayMenu = menuBar()->addMenu(QStringLiteral("&Play"));

    PlayAction = PlayMenu->addAction(QStringLiteral("&Play"));
    PlayAction->setShortcut(QKeySequence(Qt::Key_F5));
    PlayAction->setToolTip(QStringLiteral("Run the game ([Game] Module) from its start level in the viewport (F5)"));
    connect(PlayAction, &QAction::triggered, this, &EditorMainWindow::StartPlay);

    StopAction = PlayMenu->addAction(QStringLiteral("&Stop"));
    StopAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    StopAction->setToolTip(QStringLiteral("End the run and return to the edited scene (Shift+F5, or Esc in the viewport)"));
    StopAction->setEnabled(false);
    connect(StopAction, &QAction::triggered, this, [this]() { Context.StopPlay(); });

    PlayMenu->addSeparator();

    EjectAction = PlayMenu->addAction(QStringLiteral("&Eject"));
    EjectAction->setCheckable(true);
    EjectAction->setShortcut(QKeySequence(Qt::Key_F8));
    EjectAction->setToolTip(QStringLiteral("While playing, look around with the editor camera instead of the game's (F8)"));
    EjectAction->setEnabled(false);
    connect(EjectAction, &QAction::toggled, this, [this](const bool bInOn) { Context.SetPlayViewEjected(bInOn); });
    connect(&Context, &EditorContext::PlayViewEjectedChanged, this, [this](const bool bInEjected)
    {
        const QSignalBlocker Block(EjectAction);
        EjectAction->setChecked(bInEjected);
    });

    /* In front of the navigation buttons, where a run is started and ended. */
    QToolBar* Bar = ViewportArea->GetToolbar();
    QAction*  First = Bar->actions().isEmpty() ? nullptr : Bar->actions().front();
    Bar->insertAction(First, PlayAction);
    Bar->insertAction(First, StopAction);
    Bar->insertAction(First, EjectAction);
    Bar->insertSeparator(First);
}

void EditorMainWindow::StartPlay()
{
    /* SDL wraps the viewport once: the game's input needs it, so the probe yields. */
    if(InputProbe && InputProbe->IsRunning())
    {
        InputProbeAction->setChecked(false);   // -> SetSDLInputProbe(false)
        CORE_LOG("[Editor] the SDL input probe stops while playing: the game's input uses the viewport")
    }

    if(!Context.StartPlay())
    {
        statusBar()->showMessage(QStringLiteral("Play failed -- see the Console"), 5000);
        return;
    }
    /* So keys reach the viewport (Esc to stop) rather than the last panel. */
    Viewport->setFocus(Qt::OtherFocusReason);
}

void EditorMainWindow::BuildBuildMenu()
{
    QMenu* BuildMenu = menuBar()->addMenu(QStringLiteral("&Build"));

    Compiler = new SourceBuild(this);
    connect(Compiler, &SourceBuild::Finished, this, [this](const bool bInSucceeded, const QString&)
    {
        OnCompileFinished(bInSucceeded);
    });

    CompileAction = BuildMenu->addAction(QStringLiteral("&Compile Source"));
    CompileAction->setShortcut(QKeySequence(Qt::Key_F7));
    CompileAction->setToolTip(QStringLiteral("Build the game module ([Game] Module) and restart a running game on it (F7)"));
    connect(CompileAction, &QAction::triggered, this, &EditorMainWindow::CompileSource);

    CancelCompileAction = BuildMenu->addAction(QStringLiteral("C&ancel Compile"));
    CancelCompileAction->setEnabled(false);
    connect(CancelCompileAction, &QAction::triggered, Compiler, &SourceBuild::Cancel);

    BuildMenu->addSeparator();

    QAction* Cook = BuildMenu->addAction(QStringLiteral("C&ook Content"));
    Cook->setToolTip(QStringLiteral("Regenerate every node graph's code; cooking the rest of the content is not implemented yet"));
    connect(Cook, &QAction::triggered, this, &EditorMainWindow::CookContent);

    QAction* RefreshNodes = BuildMenu->addAction(QStringLiteral("&Refresh Script Nodes"));
    RefreshNodes->setToolTip(QStringLiteral("Reread the game module's components and script libraries into the script graph nodes "
                                            "(graphs already open keep theirs until reopened)"));
    connect(RefreshNodes, &QAction::triggered, this, [this]()
    {
        const bool bRefreshed = EditorGraphs::RefreshGameDeclarations(Context.IsPlaying());
        statusBar()->showMessage(bRefreshed ? QStringLiteral("Script nodes refreshed -- reopen open graphs to see changes")
                                            : QStringLiteral("Script nodes not refreshed -- see the Console"), 5000);
    });
}

void EditorMainWindow::CompileSource()
{
    if(Compiler->IsRunning())
    {
        statusBar()->showMessage(QStringLiteral("Already compiling %1").arg(Compiler->GetTarget()), 3000);
        return;
    }

    /* The module's name is its target's: nimble_add_hot_module(SandboxGame). */
    const EngineCore::ConfigVar* ModuleVar = EngineCore::ConfigFindVar("Game", "Module");
    const QString Module = ModuleVar ? QString::fromUtf8(ModuleVar->GetString()) : QString();
    if(Module.isEmpty())
    {
        CORE_LOG_ERROR("[Build] no game module to compile -- set [Game] Module in Config/DefaultEngine.ini")
        statusBar()->showMessage(QStringLiteral("Compile failed -- see the Console"), 5000);
        return;
    }

    if(!Compiler->Start(Module))
    {
        statusBar()->showMessage(QStringLiteral("Compile failed -- see the Console"), 5000);
        return;
    }
    if(Compiler->IsRunning())
    {
        CompileAction->setEnabled(false);
        CancelCompileAction->setEnabled(true);
        statusBar()->showMessage(QStringLiteral("Compiling %1...").arg(Module));
    }
}

void EditorMainWindow::OnCompileFinished(const bool bInSucceeded)
{
    CompileAction->setEnabled(true);
    CancelCompileAction->setEnabled(false);

    const QString Module = Compiler->GetTarget();
    if(!bInSucceeded)
    {
        statusBar()->showMessage(QStringLiteral("Compiling %1 failed -- see the Console").arg(Module), 5000);
        return;
    }

    /* The next Play loads the new DLL anyway; restart a running one unless
     * Editor.ReloadAfterCompile says otherwise. Script nodes follow the new
     * build: read now, or when a run still on the old one stops. */
    if(Context.IsPlaying() && !GEditorReloadAfterCompile.GetBool())
    {
        CORE_LOG("[Build] " + std::string(Module.toUtf8().constData()) + " is loaded at the next Play; the run keeps the old one")
        statusBar()->showMessage(QStringLiteral("Compiled %1 -- Stop and Play to load it").arg(Module), 5000);
        bDeclarationsStale = true;
        return;
    }
    if(!Context.IsPlaying())
    {
        CORE_LOG("[Build] " + std::string(Module.toUtf8().constData()) + " is loaded at the next Play")
        statusBar()->showMessage(QStringLiteral("Compiled %1").arg(Module), 5000);
        EditorGraphs::RefreshGameDeclarations(false);
        return;
    }

    CORE_LOG("[Build] reloading " + std::string(Module.toUtf8().constData()) + ": restarting the run")
    Context.StopPlay();
    StartPlay();
    if(Context.IsPlaying())
    {
        statusBar()->showMessage(QStringLiteral("Compiled and reloaded %1").arg(Module), 5000);
    }
    EditorGraphs::RefreshGameDeclarations(Context.IsPlaying());
}

void EditorMainWindow::OpenSettings()
{
    /* One at a time: a second request raises the open one. */
    if(auto* Open = findChild<SettingsDialog*>())
    {
        Open->raise();
        Open->activateWindow();
        return;
    }
    auto* Dialog = new SettingsDialog(this);   // deletes itself on close
    Dialog->show();
}

void EditorMainWindow::OpenProjectSettings()
{
    if(auto* Open = findChild<ProjectSettingsDialog*>())
    {
        Open->raise();
        Open->activateWindow();
        return;
    }
    auto* Dialog = new ProjectSettingsDialog(this);   // deletes itself on close
    Dialog->show();
}

void EditorMainWindow::CookContent()
{
    /* Only graph regeneration so far; the rest of cooking is undefined (the
     * Console's `cook` does one world). */
    QStringList Lines;
    const int Failures = EditorGraphs::RegenerateAll(Lines);
    for(const QString& Line : Lines) { CORE_LOG("[Build] " + std::string(Line.toUtf8().constData())) }
    statusBar()->showMessage(Failures == 0 ? QStringLiteral("Node graphs regenerated")
                                           : QStringLiteral("%1 node graph(s) failed -- see the Console").arg(Failures), 5000);

    CORE_LOG_WARN("[Build] cooking worlds and other content is not implemented yet; 'cook [world] [out]' in the Console cooks a single world")
    QMessageBox::information(this, QStringLiteral("Cook Content"),
                             QStringLiteral("Node graphs: %1 regenerated, %2 failed (details in the Console).\n\n"
                                            "Cooking the rest of the project's content is not implemented yet. "
                                            "The Console's cook command cooks a single world file to binary.")
                             .arg(Lines.size() - Failures).arg(Failures));
}

void EditorMainWindow::OnPlayStateChanged(const bool bInPlaying)
{
    PlayAction->setEnabled(!bInPlaying);
    StopAction->setEnabled(bInPlaying);
    EjectAction->setEnabled(bInPlaying);
    for(QAction* Action : SceneActions)
    {
        Action->setEnabled(!bInPlaying);
    }
    InputProbeAction->setEnabled(!bInPlaying);

    PlayLabel->setText(bInPlaying ? QStringLiteral("Playing %1").arg(Context.DescribePlay()) : QString());
    UpdateTitle();

    /* A compile finished during a run that kept the old module. */
    if(!bInPlaying && bDeclarationsStale)
    {
        bDeclarationsStale = false;
        EditorGraphs::RefreshGameDeclarations(false);
    }
}

void EditorMainWindow::RegisterConsoleCommands()
{
    using GameCore::EntityID;

    /* "#12" is entity index 12; anything else is a name. */
    const auto Resolve = [this](const QString& InWhich) -> EntityID
    {
        GameCore::World& World = Level.GetWorld();
        if(InWhich.startsWith(QLatin1Char('#')))
        {
            bool bOk = false;
            const uint32_t Index = InWhich.mid(1).toUInt(&bOk);
            EntityID Found = GameCore::InvalidEntityID();
            if(bOk)
            {
                struct Search { uint32_t Index; EntityID* Out; } State{ Index, &Found };
                World.ForEachEntity([](const EntityID InEntity, void* InUserData)
                {
                    auto* S = static_cast<Search*>(InUserData);
                    if(InEntity.Index == S->Index)
                    {
                        *S->Out = InEntity;
                    }
                }, &State);
            }
            return Found;
        }
        return World.FindEntityByName(InWhich.toUtf8().constData());
    };

    Console.Register(QStringLiteral("entities"), QStringLiteral("[filter]"),
                     QStringLiteral("List the scene's entities, the selected one marked *"),
                     [this](const QStringList& InArgs)
    {
        std::vector<EntityID> Entities;
        Level.GetWorld().ForEachEntity([](const EntityID InEntity, void* InUserData)
        {
            static_cast<std::vector<EntityID>*>(InUserData)->push_back(InEntity);
        }, &Entities);

        const QString Filter = InArgs.isEmpty() ? QString() : InArgs.front();
        QStringList Lines;
        for(const EntityID ID : Entities)
        {
            const QString Name = Level.GetDisplayName(ID);
            if(!Filter.isEmpty() && !Name.contains(Filter, Qt::CaseInsensitive))
            {
                continue;
            }
            Lines << QStringLiteral("%1 #%2 %3")
                     .arg(ID == Level.GetSelection() ? QLatin1Char('*') : QLatin1Char(' '))
                     .arg(QString::number(ID.Index), -4)
                     .arg(Name);
        }
        Lines << QStringLiteral("%1 of %2 entities").arg(Lines.size()).arg(Entities.size());
        Console.Print(Lines.join(QLatin1Char('\n')));
    });

    Console.Register(QStringLiteral("select"), QStringLiteral("<name | #index | none>"),
                     QStringLiteral("Select an entity"),
                     [this, Resolve](const QStringList& InArgs)
    {
        if(InArgs.size() != 1)
        {
            Console.PrintError(QStringLiteral("Usage: select <name | #index | none>"));
            return;
        }
        if(InArgs.front().compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
        {
            Level.SetSelection(GameCore::InvalidEntityID());
            return;
        }
        const EntityID Found = Resolve(InArgs.front());
        if(!Found.IsSet() || !Level.GetWorld().IsAlive(Found))
        {
            Console.PrintError(QStringLiteral("No entity '%1'").arg(InArgs.front()));
            return;
        }
        Level.SetSelection(Found);
        Console.Print(QStringLiteral("Selected %1 #%2").arg(Level.GetDisplayName(Found)).arg(Found.Index));
    });

    Console.Register(QStringLiteral("create"), QStringLiteral("[name]"),
                     QStringLiteral("Create an empty entity in front of the camera and select it"),
                     [this](const QStringList& InArgs)
    {
        const EditorCamera& Camera = Viewport->GetCamera();
        const QString Name = InArgs.isEmpty() ? QStringLiteral("Entity") : InArgs.join(QLatin1Char(' '));
        const EntityID Created = Level.CreateEntity(Name, Camera.Position + Camera.GetForward() * 600.0);
        Level.SetSelection(Created);
        Console.Print(QStringLiteral("Created %1 #%2").arg(Level.GetDisplayName(Created)).arg(Created.Index));
    });

    Console.Register(QStringLiteral("delete"), QStringLiteral("[name | #index]"),
                     QStringLiteral("Destroy an entity, or the selection"),
                     [this, Resolve](const QStringList& InArgs)
    {
        const EntityID Target = InArgs.isEmpty() ? Level.GetSelection() : Resolve(InArgs.front());
        if(!Target.IsSet() || !Level.GetWorld().IsAlive(Target))
        {
            Console.PrintError(InArgs.isEmpty() ? QStringLiteral("Nothing is selected")
                                                : QStringLiteral("No entity '%1'").arg(InArgs.front()));
            return;
        }
        const QString Name = Level.GetDisplayName(Target);
        Level.DestroyEntity(Target);
        Console.Print(QStringLiteral("Destroyed %1").arg(Name));
    });

    Console.Register(QStringLiteral("frame"), QStringLiteral("[all]"),
                     QStringLiteral("Frame the selection in the viewport, or the whole scene"),
                     [this](const QStringList& InArgs)
    {
        const bool bAll = !InArgs.isEmpty() && InArgs.front().compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0;
        (bAll ? ViewportArea->GetFrameAllAction() : ViewportArea->GetFrameSelectionAction())->trigger();
    });

    Console.Register(QStringLiteral("play"), QString(), QStringLiteral("Run the game in the viewport (F5)"),
                     [this](const QStringList&)
    {
        if(Context.IsPlaying())
        {
            Console.PrintError(QStringLiteral("Already playing %1").arg(Context.DescribePlay()));
            return;
        }
        StartPlay();
        if(Context.IsPlaying())
        {
            Console.Print(QStringLiteral("Playing %1").arg(Context.DescribePlay()));
        }
    });

    Console.Register(QStringLiteral("stop"), QString(), QStringLiteral("End the run (Shift+F5)"),
                     [this](const QStringList&)
    {
        if(!Context.IsPlaying())
        {
            Console.PrintError(QStringLiteral("Not playing"));
            return;
        }
        Context.StopPlay();
    });

    Console.Register(QStringLiteral("eject"), QStringLiteral("[on | off]"),
                     QStringLiteral("While playing, switch between the game's camera and the editor's (F8)"),
                     [this](const QStringList& InArgs)
    {
        if(!Context.IsPlaying())
        {
            Console.PrintError(QStringLiteral("Not playing"));
            return;
        }
        const QString Arg = InArgs.isEmpty() ? QString() : InArgs.front().toLower();
        Context.SetPlayViewEjected(Arg == QStringLiteral("on")  ? true
                                 : Arg == QStringLiteral("off") ? false
                                 : !Context.IsPlayViewEjected());
        Console.Print(Context.IsPlayViewEjected() ? QStringLiteral("Editor camera") : QStringLiteral("Game camera"));
    });

    Console.Register(QStringLiteral("settings"), QString(),
                     QStringLiteral("Open Edit > Editor Settings (Ctrl+,)"),
                     [this](const QStringList&)
    {
        OpenSettings();
        Console.Print(QStringLiteral("Settings are saved to %1")
                      .arg(QDir::toNativeSeparators(SettingsDialog::GetUserSettingsPath())));
    });

    Console.Register(QStringLiteral("projectsettings"), QString(),
                     QStringLiteral("Open Edit > Project Settings"),
                     [this](const QStringList&)
    {
        OpenProjectSettings();
        Console.Print(QStringLiteral("Project settings are saved to %1")
                      .arg(QDir::toNativeSeparators(ProjectSettingsDialog::GetProjectConfigPath())));
    });

    Console.Register(QStringLiteral("compile"), QStringLiteral("[cancel]"),
                     QStringLiteral("Build the game module and reload it into a running game (F7)"),
                     [this](const QStringList& InArgs)
    {
        if(!InArgs.isEmpty() && InArgs.front().compare(QStringLiteral("cancel"), Qt::CaseInsensitive) == 0)
        {
            if(!Compiler->IsRunning())
            {
                Console.PrintError(QStringLiteral("Not compiling"));
                return;
            }
            Compiler->Cancel();
            return;
        }
        if(!InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: compile [cancel]"));
            return;
        }
        CompileSource();
    });

    Console.Register(QStringLiteral("cook"), QStringLiteral("[world | prefab | prefabs] [out]"),
                     QStringLiteral("Cook a world or prefab file to binary (default: the open world, to Cooked/ beside it); "
                                    "'prefabs' cooks every prefab to <project>/Cooked/, which a Client build stages"),
                     [this](const QStringList& InArgs)
    {
        /* Every prefab in the project, to <out>/<its runtime path>. */
        if(!InArgs.isEmpty() && InArgs[0] == QStringLiteral("prefabs"))
        {
            const QString Root = EditorGraphs::GetProjectRoot();
            const QString OutRoot = InArgs.size() >= 2 ? InArgs[1] : QDir(Root).filePath(QStringLiteral("Cooked"));
            int Cooked = 0, Failed = 0;
            QDirIterator It(Root, { QStringLiteral("*.nmprefab") }, QDir::Files, QDirIterator::Subdirectories);
            while(It.hasNext())
            {
                const QString File = It.next();
                const QString Runtime = EditorContent::ToRuntimePath(File);
                const QString Relative = QDir(Root).relativeFilePath(File);
                if(Runtime.isEmpty() || QFileInfo(File).absoluteFilePath().startsWith(QFileInfo(OutRoot).absoluteFilePath() + QLatin1Char('/')))
                {
                    continue;   // outside the project, or a cooked copy
                }
                bool bIgnored = false;
                for(const QString& Part : Relative.split(QLatin1Char('/'))) { bIgnored |= EditorGraphs::IsIgnoredFolder(Part); }
                if(bIgnored) { continue; }

                const QString Out = QDir(OutRoot).filePath(Runtime);
                QDir().mkpath(QFileInfo(Out).absolutePath());
                if(GameCore::CookPrefabFile(File.toUtf8().constData(), Out.toUtf8().constData())) { ++Cooked; }
                else { ++Failed; Console.PrintError(QStringLiteral("Could not cook %1").arg(Runtime)); }
            }
            Console.Print(QStringLiteral("Cooked %1 prefab(s) to %2%3").arg(Cooked).arg(QDir::toNativeSeparators(OutRoot))
                          .arg(Failed ? QStringLiteral("; %1 failed").arg(Failed) : QString()));
            return;
        }
        if(!InArgs.isEmpty() && InArgs[0].endsWith(QStringLiteral(".nmprefab"), Qt::CaseInsensitive))
        {
            const QFileInfo SourceInfo(InArgs[0]);
            const QString Out = InArgs.size() >= 2 ? InArgs[1] : SourceInfo.dir().filePath(QStringLiteral("Cooked/") + SourceInfo.fileName());
            if(QFileInfo(Out).absoluteFilePath() == SourceInfo.absoluteFilePath())
            {
                Console.PrintError(QStringLiteral("Refusing to cook a prefab over itself"));
                return;
            }
            QDir().mkpath(QFileInfo(Out).absolutePath());
            if(GameCore::CookPrefabFile(InArgs[0].toUtf8().constData(), Out.toUtf8().constData()))
            {
                Console.Print(QStringLiteral("Cooked %1 -> %2").arg(InArgs[0], Out));
            }
            else
            {
                Console.PrintError(QStringLiteral("Cook failed; see the log"));
            }
            return;
        }

        /* The file on disk, not the scene in memory: save first to cook edits. */
        const QString Source = InArgs.size() >= 1 ? InArgs[0] : CurrentPath;
        if(Source.isEmpty())
        {
            Console.PrintError(QStringLiteral("No world file is open; save it or give a path"));
            return;
        }
        const QFileInfo SourceInfo(Source);
        const QString Out = InArgs.size() >= 2
                          ? InArgs[1]
                          : SourceInfo.dir().filePath(QStringLiteral("Cooked/") + SourceInfo.fileName());
        if(QFileInfo(Out).absoluteFilePath() == SourceInfo.absoluteFilePath())
        {
            Console.PrintError(QStringLiteral("Refusing to cook a world over itself"));
            return;
        }
        if(GameCore::CookWorldFile(Source.toUtf8().constData(), Out.toUtf8().constData()))
        {
            Console.Print(QStringLiteral("Cooked %1 -> %2").arg(Source, Out));
        }
        else
        {
            Console.PrintError(QStringLiteral("Cook failed; see the log"));
        }
    });

    Console.Register(QStringLiteral("graph"), QStringLiteral("<path>"),
                     QStringLiteral("Open a node graph (*.material.nmgraph, *.script.nmgraph); relative to the project"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: graph <path>"));
            return;
        }
        const QString Given = InArgs.join(QLatin1Char(' '));
        const QString Path = QDir::isAbsolutePath(Given) ? Given : QDir(EditorGraphs::GetProjectRoot()).filePath(Given);
        if(EditorGraphs::OpenGraph(Path, this)) { Console.Print(QStringLiteral("Opened %1").arg(Path)); }
    });

    Console.Register(QStringLiteral("graphs"), QString(),
                     QStringLiteral("Regenerate the code of every node graph in the project"),
                     [this](const QStringList&)
    {
        QStringList Lines;
        const int Failures = EditorGraphs::RegenerateAll(Lines);
        Lines << QStringLiteral("%1 failed").arg(Failures);
        if(Failures > 0) { Console.PrintError(Lines.join(QLatin1Char('\n'))); }
        else             { Console.Print(Lines.join(QLatin1Char('\n'))); }
    });

    /* For scripted runs (--command): "after 3000 prefab close". */
    Console.Register(QStringLiteral("after"), QStringLiteral("<ms> <command...>"),
                     QStringLiteral("Run a console command after a delay"),
                     [this](const QStringList& InArgs)
    {
        bool bOk = false;
        const int Delay = InArgs.isEmpty() ? 0 : InArgs.front().toInt(&bOk);
        if(!bOk || Delay < 0 || InArgs.size() < 2)
        {
            Console.PrintError(QStringLiteral("Usage: after <ms> <command...>"));
            return;
        }
        const QString Line = InArgs.mid(1).join(QLatin1Char(' '));
        QTimer::singleShot(Delay, this, [this, Line]()
        {
            Console.Print(QStringLiteral("> %1").arg(Line));
            Console.Execute(Line);
        });
    });

    Console.Register(QStringLiteral("prefab"), QStringLiteral("<path | new | simulate | stop | close>"),
                     QStringLiteral("Open a .nmprefab in the Prefab Editor, make a new one, or simulate the one in front"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: prefab <path | new | simulate | stop | close>"));
            return;
        }
        if(InArgs.front() == QStringLiteral("new"))
        {
            EditorPrefabs::CreatePrefab(QString(), this);
            return;
        }
        if(InArgs.front() == QStringLiteral("close"))
        {
            EditorPrefabs::CloseAllPrefabs();
            return;
        }
        if(InArgs.front() == QStringLiteral("simulate") || InArgs.front() == QStringLiteral("stop"))
        {
            PrefabEditorWindow* Window = PrefabEditorWindow::Find();
            PrefabEditorTab*    Tab    = Window ? Window->CurrentTab() : nullptr;
            if(!Tab)
            {
                Console.PrintError(QStringLiteral("No prefab is open"));
                return;
            }
            if(InArgs.front() == QStringLiteral("simulate")) { Tab->StartSimulation(); }
            else                                             { Tab->StopSimulation(); }
            return;
        }
        const QString Path = QFileInfo(InArgs.join(QLatin1Char(' '))).isAbsolute()
                           ? InArgs.join(QLatin1Char(' '))
                           : QDir(EditorGraphs::GetProjectRoot()).filePath(InArgs.join(QLatin1Char(' ')));
        if(EditorPrefabs::OpenPrefab(Path, this))
        {
            Console.Print(QStringLiteral("Opened %1").arg(QDir::toNativeSeparators(Path)));
        }
    });

    /* A second window on a World of its own: the multi-window rendering path
     * the Prefab Editor will use, until it exists. */
    Console.Register(QStringLiteral("testwindow"), QString(),
                     QStringLiteral("Open a second viewport on its own World (Sandbox's scene and a large quad)"),
                     [this](const QStringList&)
    {
        if(!Context.IsRendererReady())
        {
            Console.PrintError(QStringLiteral("The renderer is not running"));
            return;
        }
        auto* Window = new QWidget(nullptr, Qt::Window);
        Window->setAttribute(Qt::WA_DeleteOnClose);
        Window->setWindowTitle(QStringLiteral("Test Viewport - Nimble Editor"));
        Window->resize(800, 600);

        auto* Document = new EditorDocument(Window);
        Document->CreateSandboxScene();
        Document->CreateEntity(QStringLiteral("TestQuad"), EngineCore::Vector3(200.0, 200.0, 0.0), [](const GameCore::Entity& InEntity)
        {
            InEntity.GetComponent<GameCore::TransformComponent>()->Scale = EngineCore::Vector3(240.0, 240.0, 1.0);
            InEntity.AddComponent<RenderCore::RenderComponent>();
        });

        auto* Layout = new QVBoxLayout(Window);
        Layout->setContentsMargins(0, 0, 0, 0);
        auto* Panel = new ViewportPanel(Context, *Document, Window);
        Layout->addWidget(Panel);
        Panel->GetViewport()->ApplySandboxView();
        Window->show();

        Console.Print(QStringLiteral("Opened a test viewport on %1 entities").arg(Document->GetWorld().GetEntityCount()));
    });

    Console.Register(QStringLiteral("scene"), QStringLiteral("<new | sandbox>"),
                     QStringLiteral("Replace the scene with an empty one, or Sandbox's"),
                     [this](const QStringList& InArgs)
    {
        if(Context.IsPlaying())
        {
            Console.PrintError(QStringLiteral("Not while playing; 'stop' first"));
            return;
        }
        const QString Which = InArgs.isEmpty() ? QString() : InArgs.front().toLower();
        if(Which == QStringLiteral("new"))
        {
            Level.NewScene();
        }
        else if(Which == QStringLiteral("sandbox"))
        {
            Level.CreateSandboxScene();
            Viewport->ApplySandboxView();
        }
        else
        {
            Console.PrintError(QStringLiteral("Usage: scene <new | sandbox>"));
            return;
        }
        CurrentPath.clear();
        UpdateTitle();
        Console.Print(QStringLiteral("%1 entities").arg(Level.GetWorld().GetEntityCount()));
    });

    Console.Register(QStringLiteral("stats"), QString(), QStringLiteral("Print the renderer's counters"),
                     [this](const QStringList&)
    {
        RenderCore::RenderStats Stats;
        if(!Context.GetRenderStats(Stats))
        {
            Console.PrintError(QStringLiteral("The renderer is not running"));
            return;
        }
        Console.Print(QStringLiteral("%1: %2 frames rendered, %3 proxies live; frame %4 ms, game %5 ms, render %6 ms")
                      .arg(QString::fromLatin1(BackendName(Backend)))
                      .arg(Stats.FramesRendered)
                      .arg(Stats.ProxiesLive)
                      .arg(double(Stats.FrameMs), 0, 'f', 2)
                      .arg(double(Stats.GameThreadMs), 0, 'f', 2)
                      .arg(double(Stats.RenderThreadMs), 0, 'f', 2));
    });

    /* Unreal's spelling: "stat unit" toggles the frame, game and render
     * thread times (Stats.Unit), "stat frame" each stage's GPU time
     * (Stats.Frame); "stat none" hides both. */
    Console.Register(QStringLiteral("stat"), QStringLiteral("unit | frame | none"),
                     QStringLiteral("Show thread times (unit) or each render stage's GPU time (frame) over the viewport"),
                     [this](const QStringList& InArgs)
    {
        EngineCore::ConfigVar* Unit  = EngineCore::ConfigFindVar("Stats", "Unit");
        EngineCore::ConfigVar* Frame = EngineCore::ConfigFindVar("Stats", "Frame");
        const QString Which = InArgs.isEmpty() ? QString() : InArgs.front().toLower();
        if(!Unit || !Frame
           || (Which != QStringLiteral("unit") && Which != QStringLiteral("frame") && Which != QStringLiteral("none")))
        {
            Console.PrintError(QStringLiteral("Usage: stat unit | frame | none"));
            return;
        }
        if(Which == QStringLiteral("none"))
        {
            Unit->SetBool(false);
            Frame->SetBool(false);
            Console.Print(QStringLiteral("stats hidden"));
            return;
        }
        EngineCore::ConfigVar* Var = Which == QStringLiteral("unit") ? Unit : Frame;
        Var->SetBool(!Var->GetBool());
        Console.Print(QStringLiteral("stat %1: %2").arg(Which, Var->GetBool() ? QStringLiteral("on") : QStringLiteral("off")));
    });

    Console.Register(QStringLiteral("undo"), QStringLiteral("[count]"), QStringLiteral("Undo the last change (Ctrl+Z)"),
                     [this](const QStringList& InArgs)
    {
        const int Count = InArgs.isEmpty() ? 1 : std::max(1, InArgs.front().toInt());
        for(int Step = 0; Step < Count && Level.CanUndo(); ++Step)
        {
            Console.Print(QStringLiteral("Undo %1").arg(Level.GetHistory().undoText()));
            Level.Undo();
        }
    });

    Console.Register(QStringLiteral("redo"), QStringLiteral("[count]"), QStringLiteral("Redo an undone change (Ctrl+Y)"),
                     [this](const QStringList& InArgs)
    {
        const int Count = InArgs.isEmpty() ? 1 : std::max(1, InArgs.front().toInt());
        for(int Step = 0; Step < Count && Level.CanRedo(); ++Step)
        {
            Console.Print(QStringLiteral("Redo %1").arg(Level.GetHistory().redoText()));
            Level.Redo();
        }
    });

    Console.Register(QStringLiteral("history"), QString(),
                     QStringLiteral("List the undo history: > marks the current point, * the last save"),
                     [this](const QStringList&)
    {
        const QUndoStack& History = Level.GetHistory();
        QStringList Lines;
        for(int Index = 0; Index < History.count(); ++Index)
        {
            Lines << QStringLiteral("%1%2 %3")
                     .arg(Index + 1 == History.index() ? QLatin1Char('>') : QLatin1Char(' '))
                     .arg(Index + 1 == History.cleanIndex() ? QLatin1Char('*') : QLatin1Char(' '))
                     .arg(History.text(Index));
        }
        Lines << QStringLiteral("%1 entries, %2 undoable").arg(History.count()).arg(History.index());
        Console.Print(Lines.join(QLatin1Char('\n')));
    });

    Console.Register(QStringLiteral("gizmo"), QStringLiteral("[none | move | rotate | scale] [world | local]"),
                     QStringLiteral("Set the viewport's transform handles, or print them (as Q W E R and X)"),
                     [this](const QStringList& InArgs)
    {
        using EMode  = TransformGizmo::EMode;
        using ESpace = TransformGizmo::ESpace;
        static const char* const ModeNames[4] = { "none", "move", "rotate", "scale" };

        for(const QString& Arg : InArgs)
        {
            const QString Word = Arg.toLower();
            if(Word == QStringLiteral("world") || Word == QStringLiteral("local"))
            {
                Viewport->SetGizmoSpace(Word == QStringLiteral("local") ? ESpace::Local : ESpace::World);
                continue;
            }
            int Mode = -1;
            for(int Index = 0; Index < 4; ++Index)
            {
                if(Word == QLatin1String(ModeNames[Index]))
                {
                    Mode = Index;
                }
            }
            if(Mode < 0)
            {
                Console.PrintError(QStringLiteral("Usage: gizmo [none | move | rotate | scale] [world | local]"));
                return;
            }
            Viewport->SetGizmoMode(EMode(Mode));
        }
        Console.Print(QStringLiteral("gizmo %1 %2%3")
                      .arg(QLatin1String(ModeNames[int(Viewport->GetGizmoMode())]))
                      .arg(Viewport->GetGizmoSpace() == ESpace::Local ? QStringLiteral("local") : QStringLiteral("world"))
                      .arg(Viewport->IsGizmoSnapEnabled() ? QStringLiteral(", snapping") : QString()));
    });

    Console.Register(QStringLiteral("snap"), QStringLiteral("[on | off]"),
                     QStringLiteral("Snap handle drags to Editor.SnapTranslate / SnapRotate / SnapScale"),
                     [this](const QStringList& InArgs)
    {
        if(!InArgs.isEmpty())
        {
            const QString Word = InArgs.front().toLower();
            if(Word != QStringLiteral("on") && Word != QStringLiteral("off"))
            {
                Console.PrintError(QStringLiteral("Usage: snap [on | off]"));
                return;
            }
            Viewport->SetGizmoSnap(Word == QStringLiteral("on"));
        }
        Console.Print(QStringLiteral("snap %1").arg(Viewport->IsGizmoSnapEnabled() ? QStringLiteral("on") : QStringLiteral("off")));
    });
}

void EditorMainWindow::showEvent(QShowEvent* InEvent)
{
    QMainWindow::showEvent(InEvent);

    if(!bRendererStarted)
    {
        bRendererStarted = true;
        /* Next turn of the loop: the native windows exist and have their
         * first real size by then. */
        QTimer::singleShot(0, this, &EditorMainWindow::StartRenderer);
    }
}

void EditorMainWindow::StartRenderer()
{
    /* The command line's world, else the project's StartupWorld, else Sandbox's scene. */
    QString StartWorld = InitialWorld;
    const QString Startup = QString::fromUtf8(GEditorStartupWorld.GetString());
    if(StartWorld.isEmpty() && !Startup.isEmpty())
    {
        StartWorld = QDir::isAbsolutePath(Startup) ? Startup : QDir(EditorGraphs::GetProjectRoot()).filePath(Startup);
        if(!QFileInfo::exists(StartWorld))
        {
            CORE_LOG_WARN("[Editor] [Editor] StartupWorld = '" + StartWorld.toStdString() + "' does not exist; opening the Sandbox scene")
            StartWorld.clear();
        }
    }
    if(!StartWorld.isEmpty() && Level.LoadWorld(StartWorld))
    {
        CurrentPath = StartWorld;
        Viewport->ApplyGameCameraView();
    }
    else
    {
        Level.CreateSandboxScene();
        Viewport->ApplySandboxView();
    }
    UpdateTitle();

    if(!Context.InitializeRenderer(Backend, Viewport->GetWindowHandle()))
    {
        statusBar()->showMessage(QStringLiteral("Renderer failed to start (%1) -- the viewport will not draw")
                                 .arg(QString::fromLatin1(BackendName(Backend))));
        return;
    }

    StatsTimer.start(1000);

    if(bStartSDLInputProbe)
    {
        InputProbeAction->setChecked(true);   // -> SetSDLInputProbe(true)
    }
    if(bStartPlay)
    {
        StartPlay();
    }
    for(const QString& Command : StartupCommands)
    {
        Console.Print(QStringLiteral("> %1").arg(Command));
        Console.Execute(Command);
    }
    UpdateStats();
}

void EditorMainWindow::closeEvent(QCloseEvent* InEvent)
{
    /* Open prefabs and graphs first: an unsaved one may cancel the close.
     * Prefabs before the Renderer stops: their viewports' swapchains go now. */
    if(!EditorPrefabs::CloseAllPrefabs() || !EditorGraphs::CloseAllGraphs())
    {
        InEvent->ignore();
        return;
    }

    CORE_LOG(std::string("[Editor] closing -- ") + (InEvent->spontaneous() ? "requested by the window system" : "requested by the editor"))

    /* Before any widget is destroyed: the Renderer releases its swapchain
     * against the viewport's native window, which must still exist. */
    StatsTimer.stop();
    if(InputProbe)
    {
        InputProbe->Stop();   // before the viewport's native window goes: SDL must detach first
    }
    Context.Shutdown();
    InEvent->accept();
}

void EditorMainWindow::SetSDLInputProbe(const bool bInOn)
{
    if(!InputProbe)
    {
        InputProbe = new SDLInputProbe(this);
        connect(InputProbe, &SDLInputProbe::EventReported, this, [this](const QString& InLine)
        {
            statusBar()->showMessage(QStringLiteral("SDL: ") + InLine, 4000);
        });
    }

    if(bInOn)
    {
        Viewport->setFocus(Qt::OtherFocusReason);
        if(!InputProbe->Start(Viewport))
        {
            const QSignalBlocker Block(InputProbeAction);
            InputProbeAction->setChecked(false);
        }
    }
    else
    {
        InputProbe->Stop();
    }
}

void EditorMainWindow::UpdateTitle()
{
    const QString Scene = CurrentPath.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(CurrentPath).fileName();
    const QString Dirty = Level.GetHistory().isClean() ? QString() : QStringLiteral("*");
    const QString Play  = Context.IsPlaying() ? QStringLiteral(" [Playing]") : QString();
    setWindowTitle(QStringLiteral("%1%2%3 - Nimble Editor").arg(Scene, Dirty, Play));

    /* The browser will not move or delete the world that is open. */
    if(ContentArea) { ContentArea->SetOpenWorld(CurrentPath); }
}

void EditorMainWindow::UpdateHistoryActions()
{
    const QUndoStack& History = Level.GetHistory();
    UndoAction->setEnabled(Level.CanUndo());
    RedoAction->setEnabled(Level.CanRedo());
    UndoAction->setText(History.canUndo() ? QStringLiteral("&Undo %1").arg(History.undoText()) : QStringLiteral("&Undo"));
    RedoAction->setText(History.canRedo() ? QStringLiteral("&Redo %1").arg(History.redoText()) : QStringLiteral("&Redo"));
    UpdateTitle();
}

void EditorMainWindow::UpdateStats()
{
    RenderCore::RenderStats Stats;
    if(!Context.GetRenderStats(Stats))
    {
        StatsLabel->setText(QStringLiteral("%1 | not rendering").arg(QString::fromLatin1(BackendName(Backend))));
        return;
    }

    const uint64_t Fps = Stats.FramesRendered - LastFramesRendered;
    LastFramesRendered = Stats.FramesRendered;

    StatsLabel->setText(QStringLiteral("%1 | %2 fps | %3 proxies | %4 px")
                        .arg(QString::fromLatin1(BackendName(Backend)))
                        .arg(Fps)
                        .arg(Stats.ProxiesLive)
                        .arg(QStringLiteral("%1x%2").arg(Viewport->GetPixelSize().Width).arg(Viewport->GetPixelSize().Height)));
}

void EditorMainWindow::OpenWorld()
{
    const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open World"), WorldDialogPath(CurrentPath),
                                                      QString::fromLatin1(WorldFileFilter));
    if(!Path.isEmpty())
    {
        OpenWorldFile(Path);
    }
}

void EditorMainWindow::OpenWorldFile(const QString& Path)
{
    /* As File > Open is: the edited World cannot be replaced mid-run. */
    if(Context.IsPlaying())
    {
        statusBar()->showMessage(QStringLiteral("Stop playing to open another world"), 4000);
        return;
    }
    bool bCooked = false;
    if(!Level.LoadWorld(Path, &bCooked))
    {
        QMessageBox::warning(this, QStringLiteral("Open World"),
                             QStringLiteral("Could not read a world from\n%1").arg(Path));
        CurrentPath.clear();
    }
    else if(bCooked)
    {
        /* No path: Save asks for one, rather than overwriting cooked output
         * with text. */
        CurrentPath.clear();
        QMessageBox::information(this, QStringLiteral("Open World"),
                                 QStringLiteral("%1\nis a cooked world. It is open for viewing; Save will ask "
                                                "where to write it as an editable world.").arg(Path));
    }
    else
    {
        CurrentPath = Path;
    }
    UpdateTitle();
}

bool EditorMainWindow::SaveWorld()
{
    if(CurrentPath.isEmpty())
    {
        return SaveWorldAs();
    }
    if(!Level.SaveWorld(CurrentPath))
    {
        QMessageBox::warning(this, QStringLiteral("Save World"), QStringLiteral("Could not write\n%1").arg(CurrentPath));
        return false;
    }
    StageWorldPackage(CurrentPath);
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(CurrentPath), 3000);
    return true;
}

bool EditorMainWindow::SaveWorldAs()
{
    const QString Path = QFileDialog::getSaveFileName(this, QStringLiteral("Save World As"), WorldDialogPath(CurrentPath),
                                                      QString::fromLatin1(WorldFileFilter));
    if(Path.isEmpty())
    {
        return false;
    }
    CurrentPath = Path;
    UpdateTitle();
    return SaveWorld();
}

#endif // WITH_EDITOR
