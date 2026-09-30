#if WITH_EDITOR

#include "Public/Editor/EditorMainWindow.h"

#include <algorithm>

#include <QAction>
#include <QCloseEvent>
#include <QDir>
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

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/Editor/ConsolePanel.h"
#include "Public/Editor/DetailsPanel.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/OutlinerPanel.h"
#include "Public/Editor/ViewportPanel.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/NodeGraph/Schema.h"
#include "Private/Editor/AboutDialog.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/ProjectSettingsDialog.h"
#include "Private/Editor/SDLInputProbe.h"
#include "Private/Editor/SettingsDialog.h"
#include "Private/Editor/SourceBuild.h"

/* Edit > Editor Settings > Build. */
NIMBLE_CONFIG_BOOL(GEditorReloadAfterCompile, "Editor", "ReloadAfterCompile", true, nullptr);

namespace
{
    const char* const WorldFileFilter = "Nimble World (*.nmworld);;All Files (*)";

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
    , Backend(InBackend)
{
    resize(1600, 900);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);

    ViewportArea = new ViewportPanel(Context, this);
    Viewport     = ViewportArea->GetViewport();
    setCentralWidget(ViewportArea);

    auto* OutlinerDock = new QDockWidget(QStringLiteral("Outliner"), this);
    OutlinerDock->setObjectName(QStringLiteral("OutlinerDock"));
    Outliner = new OutlinerPanel(Context, OutlinerDock);
    OutlinerDock->setWidget(Outliner);
    addDockWidget(Qt::LeftDockWidgetArea, OutlinerDock);

    auto* DetailsDock = new QDockWidget(QStringLiteral("Details"), this);
    DetailsDock->setObjectName(QStringLiteral("DetailsDock"));
    Details = new DetailsPanel(Context, DetailsDock);
    DetailsDock->setWidget(Details);
    addDockWidget(Qt::RightDockWidgetArea, DetailsDock);

    auto* ConsoleDock = new QDockWidget(QStringLiteral("Console"), this);
    ConsoleDock->setObjectName(QStringLiteral("ConsoleDock"));
    ConsoleArea = new ConsolePanel(Console, ConsoleDock);
    ConsoleDock->setWidget(ConsoleArea);
    addDockWidget(Qt::BottomDockWidgetArea, ConsoleDock);

    resizeDocks({ OutlinerDock, DetailsDock }, { 300, 360 }, Qt::Horizontal);
    resizeDocks({ ConsoleDock }, { 200 }, Qt::Vertical);

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

    BuildMenus();
    BuildPlayControls();
    BuildBuildMenu();
    BuildHelpMenu();   // last, so Help is the rightmost menu
    connect(&Context, &EditorContext::PlayStateChanged, this, &EditorMainWindow::OnPlayStateChanged);

    connect(&StatsTimer, &QTimer::timeout, this, &EditorMainWindow::UpdateStats);

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
    Context.Shutdown();
}

void EditorMainWindow::BuildMenus()
{
    QMenu* File = menuBar()->addMenu(QStringLiteral("&File"));

    QAction* New = File->addAction(QStringLiteral("&New Scene"));
    New->setShortcut(QKeySequence::New);
    connect(New, &QAction::triggered, this, [this]()
    {
        Context.NewScene();
        CurrentPath.clear();
        UpdateTitle();
    });

    QAction* SandboxScene = File->addAction(QStringLiteral("New &Sandbox Scene"));
    SandboxScene->setToolTip(QStringLiteral("The Sandbox level as it stands once loaded: Marker, Camera and Scripted"));
    connect(SandboxScene, &QAction::triggered, this, [this]()
    {
        Context.CreateSandboxScene();
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
        connect(NewGraph, &QAction::triggered, this, [this, Schema]() { EditorGraphs::CreateGraph(*Schema, this); });
    }

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

    /* Not QUndoStack::createUndoAction: these go through the context, which
     * refuses while playing or mid-drag and keeps its own state in step. A
     * text field with focus still gets Ctrl+Z for its own text first. */
    UndoAction = Edit->addAction(QStringLiteral("&Undo"));
    UndoAction->setShortcut(QKeySequence::Undo);
    connect(UndoAction, &QAction::triggered, &Context, &EditorContext::Undo);

    RedoAction = Edit->addAction(QStringLiteral("&Redo"));
    RedoAction->setShortcuts({ QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y) });
    connect(RedoAction, &QAction::triggered, &Context, &EditorContext::Redo);

    connect(&Context, &EditorContext::HistoryChanged, this, &EditorMainWindow::UpdateHistoryActions);
    UpdateHistoryActions();

    Edit->addSeparator();

    QAction* Create = Edit->addAction(QStringLiteral("&Create Empty Entity"));
    Create->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(Create, &QAction::triggered, this, [this]()
    {
        /* In front of the camera, so the new entity is somewhere visible. */
        const EditorCamera& Camera = Viewport->GetCamera();
        const EngineCore::Vector3 Spot = Camera.Position + Camera.GetForward() * 600.0;
        Context.SetSelection(Context.CreateEntity(QStringLiteral("Entity"), Spot));
    });

    QAction* Delete = Edit->addAction(QStringLiteral("&Delete"));
    Delete->setShortcut(QKeySequence::Delete);
    connect(Delete, &QAction::triggered, this, [this]() { Context.DestroyEntity(Context.GetSelection()); });

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

    /* Nothing holds the module between runs, so the next Play loads the new
     * DLL by itself. A run in progress still has the old one: restart it,
     * unless Editor.ReloadAfterCompile says to leave it running. */
    if(Context.IsPlaying() && !GEditorReloadAfterCompile.GetBool())
    {
        CORE_LOG("[Build] " + std::string(Module.toUtf8().constData()) + " is loaded at the next Play; the run keeps the old one")
        statusBar()->showMessage(QStringLiteral("Compiled %1 -- Stop and Play to load it").arg(Module), 5000);
        return;
    }
    if(!Context.IsPlaying())
    {
        CORE_LOG("[Build] " + std::string(Module.toUtf8().constData()) + " is loaded at the next Play")
        statusBar()->showMessage(QStringLiteral("Compiled %1").arg(Module), 5000);
        return;
    }

    CORE_LOG("[Build] reloading " + std::string(Module.toUtf8().constData()) + ": restarting the run")
    Context.StopPlay();
    StartPlay();
    if(Context.IsPlaying())
    {
        statusBar()->showMessage(QStringLiteral("Compiled and reloaded %1").arg(Module), 5000);
    }
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
    /* The node graphs' generated code, which is defined: every .nmgraph under
     * the project, compiled, its output rewritten where it changed. The rest
     * is a stub until cooking has a definition -- which content, from where,
     * to where, for which target. Until then the Console's `cook` does one world. */
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

    PlayLabel->setText(bInPlaying ? QStringLiteral("Playing %1").arg(Context.DescribePlay()) : QString());
    UpdateTitle();
}

void EditorMainWindow::RegisterConsoleCommands()
{
    using GameCore::EntityID;

    /* "#12" is entity index 12; anything else is a name. */
    const auto Resolve = [this](const QString& InWhich) -> EntityID
    {
        GameCore::World& World = Context.GetWorld();
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
        Context.GetWorld().ForEachEntity([](const EntityID InEntity, void* InUserData)
        {
            static_cast<std::vector<EntityID>*>(InUserData)->push_back(InEntity);
        }, &Entities);

        const QString Filter = InArgs.isEmpty() ? QString() : InArgs.front();
        QStringList Lines;
        for(const EntityID ID : Entities)
        {
            const QString Name = Context.GetDisplayName(ID);
            if(!Filter.isEmpty() && !Name.contains(Filter, Qt::CaseInsensitive))
            {
                continue;
            }
            Lines << QStringLiteral("%1 #%2 %3")
                     .arg(ID == Context.GetSelection() ? QLatin1Char('*') : QLatin1Char(' '))
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
            Context.SetSelection(GameCore::InvalidEntityID());
            return;
        }
        const EntityID Found = Resolve(InArgs.front());
        if(!Found.IsSet() || !Context.GetWorld().IsAlive(Found))
        {
            Console.PrintError(QStringLiteral("No entity '%1'").arg(InArgs.front()));
            return;
        }
        Context.SetSelection(Found);
        Console.Print(QStringLiteral("Selected %1 #%2").arg(Context.GetDisplayName(Found)).arg(Found.Index));
    });

    Console.Register(QStringLiteral("create"), QStringLiteral("[name]"),
                     QStringLiteral("Create an empty entity in front of the camera and select it"),
                     [this](const QStringList& InArgs)
    {
        const EditorCamera& Camera = Viewport->GetCamera();
        const QString Name = InArgs.isEmpty() ? QStringLiteral("Entity") : InArgs.join(QLatin1Char(' '));
        const EntityID Created = Context.CreateEntity(Name, Camera.Position + Camera.GetForward() * 600.0);
        Context.SetSelection(Created);
        Console.Print(QStringLiteral("Created %1 #%2").arg(Context.GetDisplayName(Created)).arg(Created.Index));
    });

    Console.Register(QStringLiteral("delete"), QStringLiteral("[name | #index]"),
                     QStringLiteral("Destroy an entity, or the selection"),
                     [this, Resolve](const QStringList& InArgs)
    {
        const EntityID Target = InArgs.isEmpty() ? Context.GetSelection() : Resolve(InArgs.front());
        if(!Target.IsSet() || !Context.GetWorld().IsAlive(Target))
        {
            Console.PrintError(InArgs.isEmpty() ? QStringLiteral("Nothing is selected")
                                                : QStringLiteral("No entity '%1'").arg(InArgs.front()));
            return;
        }
        const QString Name = Context.GetDisplayName(Target);
        Context.DestroyEntity(Target);
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

    Console.Register(QStringLiteral("cook"), QStringLiteral("[world] [out]"),
                     QStringLiteral("Cook a world file to binary (default: the open world, to Cooked/ beside it)"),
                     [this](const QStringList& InArgs)
    {
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
            Context.NewScene();
        }
        else if(Which == QStringLiteral("sandbox"))
        {
            Context.CreateSandboxScene();
            Viewport->ApplySandboxView();
        }
        else
        {
            Console.PrintError(QStringLiteral("Usage: scene <new | sandbox>"));
            return;
        }
        CurrentPath.clear();
        UpdateTitle();
        Console.Print(QStringLiteral("%1 entities").arg(Context.GetWorld().GetEntityCount()));
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
        Console.Print(QStringLiteral("%1: %2 frames rendered, %3 proxies live")
                      .arg(QString::fromLatin1(BackendName(Backend)))
                      .arg(Stats.FramesRendered)
                      .arg(Stats.ProxiesLive));
    });

    Console.Register(QStringLiteral("undo"), QStringLiteral("[count]"), QStringLiteral("Undo the last change (Ctrl+Z)"),
                     [this](const QStringList& InArgs)
    {
        const int Count = InArgs.isEmpty() ? 1 : std::max(1, InArgs.front().toInt());
        for(int Step = 0; Step < Count && Context.CanUndo(); ++Step)
        {
            Console.Print(QStringLiteral("Undo %1").arg(Context.GetHistory().undoText()));
            Context.Undo();
        }
    });

    Console.Register(QStringLiteral("redo"), QStringLiteral("[count]"), QStringLiteral("Redo an undone change (Ctrl+Y)"),
                     [this](const QStringList& InArgs)
    {
        const int Count = InArgs.isEmpty() ? 1 : std::max(1, InArgs.front().toInt());
        for(int Step = 0; Step < Count && Context.CanRedo(); ++Step)
        {
            Console.Print(QStringLiteral("Redo %1").arg(Context.GetHistory().redoText()));
            Context.Redo();
        }
    });

    Console.Register(QStringLiteral("history"), QString(),
                     QStringLiteral("List the undo history: > marks the current point, * the last save"),
                     [this](const QStringList&)
    {
        const QUndoStack& History = Context.GetHistory();
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
    if(!InitialWorld.isEmpty() && Context.LoadWorld(InitialWorld))
    {
        CurrentPath = InitialWorld;
    }
    else
    {
        Context.CreateSandboxScene();
        Viewport->ApplySandboxView();
    }
    UpdateTitle();

    if(!Context.InitializeRenderer(Backend, Viewport->GetWindowHandle()))
    {
        statusBar()->showMessage(QStringLiteral("Renderer failed to start (%1) -- the viewport will not draw")
                                 .arg(QString::fromLatin1(BackendName(Backend))));
        return;
    }

    Viewport->StartRendering();
    StatsTimer.start(1000);

    if(bStartSDLInputProbe)
    {
        InputProbeAction->setChecked(true);   // -> SetSDLInputProbe(true)
    }
    if(bStartPlay)
    {
        StartPlay();
    }
    UpdateStats();
}

void EditorMainWindow::closeEvent(QCloseEvent* InEvent)
{
    /* Spontaneous: the window system asked (the close button, Alt+F4, a
     * WM_CLOSE from outside); otherwise something in the editor called close(). */
    /* Open graphs first: an unsaved one may cancel the close. */
    if(!EditorGraphs::CloseAllGraphs())
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
    const QString Dirty = Context.GetHistory().isClean() ? QString() : QStringLiteral("*");
    const QString Play  = Context.IsPlaying() ? QStringLiteral(" [Playing]") : QString();
    setWindowTitle(QStringLiteral("%1%2%3 - Nimble Editor").arg(Scene, Dirty, Play));
}

void EditorMainWindow::UpdateHistoryActions()
{
    const QUndoStack& History = Context.GetHistory();
    UndoAction->setEnabled(Context.CanUndo());
    RedoAction->setEnabled(Context.CanRedo());
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
    const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open World"), CurrentPath,
                                                      QString::fromLatin1(WorldFileFilter));
    if(Path.isEmpty())
    {
        return;
    }
    bool bCooked = false;
    if(!Context.LoadWorld(Path, &bCooked))
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
    if(!Context.SaveWorld(CurrentPath))
    {
        QMessageBox::warning(this, QStringLiteral("Save World"), QStringLiteral("Could not write\n%1").arg(CurrentPath));
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(CurrentPath), 3000);
    return true;
}

bool EditorMainWindow::SaveWorldAs()
{
    const QString Path = QFileDialog::getSaveFileName(this, QStringLiteral("Save World As"), CurrentPath,
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
