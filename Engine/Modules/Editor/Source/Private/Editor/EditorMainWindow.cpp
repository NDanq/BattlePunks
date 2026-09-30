#if WITH_EDITOR

#include "Public/Editor/EditorMainWindow.h"

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

#include "Public/GameCore/World.h"
#include "Public/GameCore/WorldPackage.h"
#include "Public/Editor/ConsolePanel.h"
#include "Public/Editor/DetailsPanel.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/OutlinerPanel.h"
#include "Public/Editor/ViewportPanel.h"
#include "Public/Editor/ViewportWidget.h"
#include "Private/Editor/AboutDialog.h"
#include "Private/Editor/SDLInputProbe.h"

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

    QAction* Exit = File->addAction(QStringLiteral("E&xit"));
    Exit->setShortcut(QKeySequence::Quit);
    connect(Exit, &QAction::triggered, this, &QWidget::close);

    QMenu* Edit = menuBar()->addMenu(QStringLiteral("&Edit"));

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
    const QString Play  = Context.IsPlaying() ? QStringLiteral(" [Playing]") : QString();
    setWindowTitle(QStringLiteral("%1%2 - Nimble Editor").arg(Scene, Play));
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
