#if WITH_EDITOR

#include "Public/Editor/EditorMainWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>

#include "Public/Editor/DetailsPanel.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/OutlinerPanel.h"
#include "Public/Editor/ViewportPanel.h"
#include "Public/Editor/ViewportWidget.h"
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

    resizeDocks({ OutlinerDock, DetailsDock }, { 300, 360 }, Qt::Horizontal);

    StatsLabel = new QLabel(this);
    statusBar()->addPermanentWidget(StatsLabel);

    BuildMenus();

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
    UpdateStats();
}

void EditorMainWindow::closeEvent(QCloseEvent* InEvent)
{
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
    setWindowTitle(QStringLiteral("%1 - Nimble Editor").arg(Scene));
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
    if(!Context.LoadWorld(Path))
    {
        QMessageBox::warning(this, QStringLiteral("Open World"),
                             QStringLiteral("Could not read a world from\n%1").arg(Path));
        CurrentPath.clear();
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
