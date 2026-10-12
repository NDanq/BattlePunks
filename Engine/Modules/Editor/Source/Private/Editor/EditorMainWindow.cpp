/**
* VIBECODE CERTIFIED
*/
#if WITH_EDITOR

#include "Public/Editor/EditorMainWindow.h"

#include <algorithm>
#include <iterator>

#include <QPainter>
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QDirIterator>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QCoreApplication>
#include <QProcess>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
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
#include "Private/Editor/EditorIcons.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Private/Editor/Content/RenderTargetAssets.h"
#include "Private/Editor/EditorEditActions.h"
#include "Private/Editor/EditorLights.h"
#include "Private/Editor/EditorPrimitives.h"
#include "Private/Editor/NotificationWidgets.h"
#include "Private/Editor/SourceControl/SourceControlWindow.h"
#include "Public/Editor/EditorNotifications.h"
#include "Private/Editor/AboutDialog.h"
#include "Private/Editor/DocumentationWindow.h"
#include "Private/Editor/Content/AssetImport.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Content/AnimPreviewWidget.h"
#include "Private/Editor/Content/AnimStateEditorWindow.h"
#include "Private/Editor/Content/AnimationAssets.h"
#include "Private/Editor/Content/MeshViewerWindow.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Graph/GraphEditorTab.h"
#include "Private/Editor/Graph/GraphEditorWindow.h"
#include "Private/Editor/Particles/ParticleAssets.h"
#include "Private/Editor/Sequencer/SequencerAssets.h"
#include "Private/Editor/Sequencer/SequencerDocument.h"
#include "Private/Editor/Sequencer/SequencerTab.h"
#include "Private/Editor/Sequencer/SequencerTimeline.h"
#include "Private/Editor/Sequencer/SequencerWindow.h"
#include "Private/Editor/Terrain/FoliageEditing.h"
#include "Private/Editor/Terrain/TerrainEditing.h"
#include "Private/Editor/Terrain/TerrainToolsWindow.h"
#include "Private/Editor/Particles/ParticleDocument.h"
#include "Private/Editor/Particles/ParticleEditorTab.h"
#include "Private/Editor/Particles/ParticleEditorWindow.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Private/Editor/UI/UIAssets.h"
#include "Private/Editor/UI/UIDesignDocument.h"
#include "Private/Editor/UI/UIDesignerViewport.h"
#include "Private/Editor/UI/UIEditorTab.h"
#include "Private/Editor/UI/UIEditorWindow.h"
#include "Public/UI/UIFile.h"
#include "Private/Editor/Prefab/PrefabEditorTab.h"
#include "Private/Editor/Prefab/PrefabEditorWindow.h"
#include "Private/Editor/Project/NewProjectWizard.h"
#include "Private/Editor/ProjectSettingsDialog.h"
#include "Private/Editor/Build/ContentCook.h"
#include "Private/Editor/Build/ProjectBuildDialog.h"
#include "Private/Editor/WorldSettingsDialog.h"
#include "Private/Editor/Lighting/BakeLightmaps.h"
#include "Private/Editor/Lighting/LightBakeWorld.h"
#include "Public/RenderCore/BakedLightingAsset.h"
#include "Private/Editor/SDLInputProbe.h"
#include "Private/Editor/SettingsDialog.h"
#include "Private/Editor/SourceBuild.h"
#include "Private/Editor/PlayProcesses.h"
#include "Private/Editor/Collab/CollabSessionWindow.h"
#include "Private/Editor/Collab/EditorCollabSession.h"
#include "Public/Collab/CollabProtocol.h"
#include "Public/Editor/EditorContentEvents.h"
#if WITH_NETCORE
#include "Public/NetCore/NetSession.h"
#endif

/* Edit > Editor Settings > Build. */
NIMBLE_CONFIG_BOOL(GEditorReloadAfterCompile, "Editor", "ReloadAfterCompile", true, nullptr);
/* Compile Source also builds the game's launcher and dedicated server (Editor Settings > Build). */
NIMBLE_CONFIG_BOOL(GEditorBuildGameExecutables, "Editor", "BuildGameExecutables", true, nullptr);

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
     * its manifest, its folder of entity and instance files and its baked
     * lighting (Lighting/), stale ones removed -- so the next Play loads what
     * was saved without a build. */
    /* InFrom's files and folders, recursively, into InTo (made if need be). */
    void CopyFolder(const QDir& InFrom, const QString& InTo)
    {
        QDir().mkpath(InTo);
        for(const QString& Each : InFrom.entryList(QDir::Files))
        {
            QFile::copy(InFrom.filePath(Each), QDir(InTo).filePath(Each));
        }
        for(const QString& Each : InFrom.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            CopyFolder(QDir(InFrom.filePath(Each)), QDir(InTo).filePath(Each));
        }
    }

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

        /* The level's folder, whole: its entities and what lives beside them --
         * Lighting/ (a bake), Terrain/ (heightfields and foliage) -- so a save
         * never leaves the staged copy without a file the level names. */
        const QDir SourceEntities(Source.dir().filePath(Source.completeBaseName()));
        QDir TargetEntities(Target.dir().filePath(Target.completeBaseName()));
        TargetEntities.removeRecursively();
        if(SourceEntities.exists())
        {
            CopyFolder(SourceEntities, TargetEntities.absolutePath());
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

    /* Terrain mode, hidden until World > Terrain: tabbed with the Details.
     * Edited terrains are written with the level, and dropped unsaved when
     * another level replaces it (TerrainEditing.h). */
    EditorTerrain::Host TerrainHost;
    TerrainHost.Context      = &Context;
    TerrainHost.Document     = &Level;
    TerrainHost.Viewport     = Viewport;
    TerrainHost.GetWorldFile = [this]() { return CurrentPath; };
    EditorTerrain::SetHost(TerrainHost);
    TerrainToolsWindow::Install(*this, DetailsDock);
    DetailsDock->raise();
    connect(&Level, &EditorDocument::AboutToSaveWorld, this, [this]()
    {
        QStringList Errors;
        if(!EditorTerrain::SaveDirty(&Errors))
        {
            for(const QString& Each : Errors) { Console.PrintError(Each); }
        }
    });
    connect(&Level, &EditorDocument::EditedWorldAboutToBeDestroyed, this, []() { EditorTerrain::DiscardUnsaved(); });

    /* The Sequencer, hidden until a sequence opens: tabbed with them. */
    SequencerWindow::Install(*this, ContentDock);

    /* Notifications: the history, hidden until the status bar's bell, tabbed
     * with them too; the toasts over the viewport's corner. */
    EditorNotifications::Install();   // EditorMain's already; a no-op then
    Notifications = new NotificationCenter(this);
    addDockWidget(Qt::BottomDockWidgetArea, Notifications);
    tabifyDockWidget(ConsoleDock, Notifications);
    Notifications->hide();
    Toasts = new NotificationToasts(*this);

    /* Source Control, hidden until View > Source Control: the project's git,
     * wide along the bottom for its diff. */
    auto* SourceControl = new SourceControlWindow(EditorGraphs::GetProjectRoot(), ProjectSettingsDialog::GetProjectConfigPath(), this);
    addDockWidget(Qt::BottomDockWidgetArea, SourceControl);
    tabifyDockWidget(ConsoleDock, SourceControl);
    SourceControl->hide();
    connect(&Level, &EditorDocument::WorldSaved, SourceControl, &SourceControlWindow::RequestRefresh);

    /* The collaboration session, and its dock beside them: who else is working
     * and on what; their changed files badged in the Content Browser. */
    CollabSession = new EditorCollabSession(EditorGraphs::GetProjectRoot(), ProjectSettingsDialog::GetProjectConfigPath(), this);
    auto* SessionDock = new CollabSessionWindow(*CollabSession, this);
    addDockWidget(Qt::BottomDockWidgetArea, SessionDock);
    tabifyDockWidget(ConsoleDock, SessionDock);
    SessionDock->hide();
    connect(&Level, &EditorDocument::WorldSaved, CollabSession, &EditorCollabSession::RequestDirtyRefresh);
    connect(CollabSession, &EditorCollabSession::RosterChanged, this, [this]() { ContentArea->SetRemoteChanges(CollabSession->GetOthersDirty()); });

    /* The branch moving: this editor's pushes told to the others, and theirs
     * pulled and reloaded here (Pull & Reload). */
    connect(SourceControl, &SourceControlWindow::Pushed, CollabSession, &EditorCollabSession::AnnouncePush);
    connect(CollabSession, &EditorCollabSession::PullAndReloadFinished, SourceControl, &SourceControlWindow::RequestRefresh);
    EditorCollabSession::ReloadHooks Reload;
    Reload.ConfirmDiscardLevel = [this]()
    {
        return QMessageBox::question(this, QStringLiteral("Pull & Reload"),
                                     QStringLiteral("%1 changed upstream, and you have unsaved edits to it.\n\n"
                                                    "Reload it and lose your edits?").arg(QFileInfo(CurrentPath).fileName()),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    };
    Reload.ReloadLevel = [this]()
    {
        /* A run plays the old one: stop it, as File > Open would ask. */
        if(Context.IsPlaying())
        {
            Context.StopPlay();
        }
        if(!CurrentPath.isEmpty())
        {
            OpenWorldFile(CurrentPath);
        }
    };
    Reload.CompileGameModule = [this]() { CompileSource(); };
    Reload.ReloadConfig = [this]() { ReloadProjectConfig(); };
    CollabSession->SetReloadHooks(Reload);

    /* Play Together: this editor's game build checked against the server's,
     * and Play started as a client of the session's game server. */
    EditorCollabSession::PlayHooks Together;
    Together.LocalSchemaHash = [this]() -> uint64_t
    {
#if WITH_NETCORE
        return Context.IsAuthoringGameLoaded() || Context.IsPlaying() ? NetCore::ComputeSchemaHash() : 0;
#else
        return 0;
#endif
    };
    Together.JoinRun = [this](const QString& InLevel, const QString& InHost, const uint16_t InPort)
    {
        if(Context.IsPlaying())
        {
            Context.StopPlay();
        }
        if(!Context.StartPlayTogether(InLevel, InHost, InPort))
        {
            return false;
        }
        Viewport->setFocus(Qt::OtherFocusReason);
        return true;
    };
    Together.LeaveRun = [this]() { Context.StopPlay(); };
    CollabSession->SetPlayHooks(Together);

    /* Files changed under the editor (a pull): staged again, so the viewport
     * and Play see the new ones. */
    connect(&EditorContentEvents::Get(), &EditorContentEvents::ContentChanged, this, &EditorMainWindow::OnContentChanged);
    ContentDock->raise();
    connect(ContentArea, &ContentBrowserPanel::OpenWorldRequested, this, &EditorMainWindow::OpenWorldFile);

    /* The eye button on a Details asset field: its folder, selected, and opened. */
    EditorContent::SetRevealHandler([this, ContentDock](const QString& InFile)
    {
        ContentDock->show();
        ContentDock->raise();
        ContentArea->RevealAndOpen(InFile);
    });
    connect(ViewportArea, &ViewportPanel::StatusMessage, this, [this](const QString& InText) { statusBar()->showMessage(InText, 6000); });
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

    BakeLabel = new QLabel(this);
    BakeLabel->setStyleSheet(QStringLiteral("QLabel { color: rgb(230, 170, 60); }"));
    BakeLabel->setToolTip(QStringLiteral("A static entity or light changed since Build > Bake Lightmaps"));
    statusBar()->addPermanentWidget(BakeLabel);

    /* Rightmost: Do Not Disturb and the bell. */
    statusBar()->addPermanentWidget(new NotificationStatusItem(*Notifications, this));

    /* Re-gathering the static scene costs a little: once edits settle. */
    BakeCheckTimer.setSingleShot(true);
    BakeCheckTimer.setInterval(1000);
    connect(&BakeCheckTimer, &QTimer::timeout, this, &EditorMainWindow::CheckBake);
    const auto QueueBakeCheck = [this]() { BakeCheckTimer.start(); };
    connect(&Level, &EditorDocument::WorldReset, this, QueueBakeCheck);
    connect(&Level, &EditorDocument::EntitiesChanged, this, QueueBakeCheck);
    connect(&Level, &EditorDocument::PropertyEdited, this, QueueBakeCheck);
    connect(&Level, &EditorDocument::WorldSettingsChanged, this, QueueBakeCheck);
    connect(&Level, &EditorDocument::HistoryChanged, this, QueueBakeCheck);

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

    /* The game module, kept loaded for editing (EditorContext::ReloadAuthoringGame),
     * before any world opens -- its components open with it -- and before
     * anything builds the schemas, so its components and script libraries are
     * nodes from the first graph opened. */
    EditorGraphs::RefreshGameDeclarations(Context.ReloadAuthoringGame());

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
    EditorParticles::SetEditorContext(&Context);
    EditorSequencer::SetEditorContext(&Context);
    EditorSequencer::SetLevelViewport(Viewport);

    /* Editor Settings > Shaders > ShaderPDB: the build cooks without debug
     * information, so it is written here, in the background. */
    EditorGraphs::CookShaderPDBs(Backend);

    UpdateTitle();
}

EditorMainWindow::~EditorMainWindow()
{
    EditorContent::SetRevealHandler(nullptr);

    /* Normally already done by closeEvent; this covers a window destroyed
     * without being closed. The viewport's native window is still alive here. */
    if(InputProbe)
    {
        InputProbe->Stop();
    }
    EditorGraphs::SetEditorContext(nullptr);
    EditorPrefabs::SetEditorContext(nullptr);
    EditorParticles::SetEditorContext(nullptr);
    EditorSequencer::SetEditorContext(nullptr);
    EditorSequencer::SetLevelViewport(nullptr);
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

    QAction* New = File->addAction(QStringLiteral("&New World"));
    New->setShortcut(QKeySequence::New);
    connect(New, &QAction::triggered, this, [this]()
    {
        Level.NewWorld();
        CurrentPath.clear();
        UpdateTitle();
    });

    File->addSeparator();

    QAction* Open = File->addAction(QStringLiteral("&Open World..."));
    Open->setShortcut(QKeySequence::Open);
    connect(Open, &QAction::triggered, this, &EditorMainWindow::OpenWorld);

    QAction* Save = File->addAction(EditorIcons::Get(EditorIcons::EIcon::Save), QStringLiteral("&Save World"));
    Save->setShortcut(QKeySequence::Save);
    connect(Save, &QAction::triggered, this, &EditorMainWindow::SaveWorld);

    QAction* SaveAs = File->addAction(QStringLiteral("Save World &As..."));
    SaveAs->setShortcut(QKeySequence::SaveAs);
    connect(SaveAs, &QAction::triggered, this, &EditorMainWindow::SaveWorldAs);

    /* Save stays: it writes the edited World, which a run never touches. */
    SceneActions = { New, Open };

    File->addSeparator();

    QAction* Import = File->addAction(QStringLiteral("&Import..."));
    Import->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    Import->setToolTip(QStringLiteral("glTF meshes and images into engine assets, in the Content Browser's folder"));
    connect(Import, &QAction::triggered, this, [this]() { ContentArea->ImportWithDialog(); });

    QAction* OpenPrefab = File->addAction(QStringLiteral("Open Pre&fab..."));
    connect(OpenPrefab, &QAction::triggered, this, [this]()
    {
        const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Prefab"), EditorPrefabs::GetDefaultFolder(),
                                                          QStringLiteral("Prefabs (*.nmprefab)"));
        if(!Path.isEmpty()) { EditorPrefabs::OpenPrefab(Path, this); }
    });

    QAction* OpenParticles = File->addAction(QStringLiteral("Open Particle S&ystem..."));
    connect(OpenParticles, &QAction::triggered, this, [this]()
    {
        const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Particle System"), EditorParticles::GetDefaultFolder(),
                                                          QStringLiteral("Particle Systems (*.nmparticle)"));
        if(!Path.isEmpty()) { EditorParticles::OpenParticleSystem(Path, this); }
    });

    QAction* OpenSequence = File->addAction(QStringLiteral("Open Se&quence..."));
    connect(OpenSequence, &QAction::triggered, this, [this]()
    {
        const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Sequence"), EditorSequencer::GetDefaultFolder(),
                                                          QStringLiteral("Sequences (*.nmseq)"));
        if(!Path.isEmpty()) { EditorSequencer::OpenSequence(Path, this); }
    });
    QAction* NewSequence = File->addAction(QStringLiteral("New Sequence..."));
    connect(NewSequence, &QAction::triggered, this, [this]() { EditorSequencer::CreateSequence(QString(), this); });

    QAction* OpenGraph = File->addAction(QStringLiteral("Open Grap&h..."));
    OpenGraph->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(OpenGraph, &QAction::triggered, this, [this]()
    {
        const QString Path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Graph"), EditorGraphs::GetProjectRoot(),
                                                          EditorGraphs::GetFileFilter());
        if(!Path.isEmpty()) { EditorGraphs::OpenGraph(Path, this); }
    });

    File->addSeparator();

    /* The collaboration session: the Session dock, and leaving it. */
    QAction* JoinSession = File->addAction(QStringLiteral("Collaboration &Session..."));
    JoinSession->setToolTip(QStringLiteral("Join others working on this project through NimbleEditorServer (the Session dock)"));
    connect(JoinSession, &QAction::triggered, this, [this]()
    {
        if(auto* Dock = findChild<CollabSessionWindow*>())
        {
            Dock->show();
            Dock->raise();
            Dock->GetAddressEdit()->setFocus();
        }
    });
    QAction* LeaveSession = File->addAction(QStringLiteral("&Leave Session"));
    connect(LeaveSession, &QAction::triggered, this, [this]() { CollabSession->Leave(); });
    connect(File, &QMenu::aboutToShow, this, [this, LeaveSession]()
    {
        LeaveSession->setEnabled(CollabSession->GetState() != ::Collab::CollabClient::EState::Idle);
    });

    File->addSeparator();

    QAction* Exit = File->addAction(QStringLiteral("E&xit"));
    Exit->setShortcut(QKeySequence::Quit);
    connect(Exit, &QAction::triggered, this, &QWidget::close);

    QMenu* Edit = menuBar()->addMenu(QStringLiteral("&Edit"));

    /* Through the context, which refuses while playing or mid-drag. Focused
     * text fields still get Ctrl+Z first. */
    UndoAction = Edit->addAction(EditorIcons::Get(EditorIcons::EIcon::Undo), QStringLiteral("&Undo"));
    UndoAction->setShortcut(QKeySequence::Undo);
    connect(UndoAction, &QAction::triggered, &Level, &EditorDocument::Undo);

    RedoAction = Edit->addAction(EditorIcons::Get(EditorIcons::EIcon::Redo), QStringLiteral("&Redo"));
    RedoAction->setShortcuts({ QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y) });
    connect(RedoAction, &QAction::triggered, &Level, &EditorDocument::Redo);

    connect(&Level, &EditorDocument::HistoryChanged, this, &EditorMainWindow::UpdateHistoryActions);
    UpdateHistoryActions();

    /* Cut/Copy/Paste/Duplicate/Rename/Delete on the level's selection. Their
     * keys belong to the viewport and the Outliner (EditorEditActions.h);
     * here they are shown. */
    Edit->addSeparator();
    const std::vector<QAction*> EntityItems =
        EditorEditActions::AddToMenu(Edit, Level, [this]() { EditorEditActions::PromptRename(this, Level); });
    connect(Edit, &QMenu::aboutToShow, this, [this, EntityItems]() { EditorEditActions::Refresh(EntityItems, Level); });

    Edit->addSeparator();
    Edit->addAction(ViewportArea->GetFrameSelectionAction());
    Edit->addAction(ViewportArea->GetFrameAllAction());

    Edit->addSeparator();
    QAction* Settings = Edit->addAction(QStringLiteral("Editor &Settings..."));
    Settings->setMenuRole(QAction::PreferencesRole);
    Settings->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(Settings, &QAction::triggered, this, [this]() { OpenSettings(); });

    QAction* ProjectSettings = Edit->addAction(QStringLiteral("&Project Settings..."));
    ProjectSettings->setToolTip(QStringLiteral("The project's Config/DefaultEngine.ini: the game's and the engine's settings"));
    connect(ProjectSettings, &QAction::triggered, this, [this]() { OpenProjectSettings(); });

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

    /* Terrain mode (TerrainToolsWindow.h): make one, then sculpt and paint it. */
    QAction* TerrainAction = World->addAction(EditorIcons::Get(EditorIcons::EIcon::Terrain), QStringLiteral("&Terrain..."));
    TerrainAction->setToolTip(QStringLiteral("Make, sculpt and paint terrain"));
    connect(TerrainAction, &QAction::triggered, this, []() { TerrainToolsWindow::Show(); });

    /* Del itself belongs to the viewport and the Outliner (EditorEditActions.h). */
    QAction* DeleteEntity = World->addAction(QStringLiteral("&Delete"));
    DeleteEntity->setShortcut(QKeySequence::Delete);
    DeleteEntity->setShortcutContext(Qt::WidgetShortcut);
    connect(DeleteEntity, &QAction::triggered, this, [this]() { Level.DeleteSelection(); });

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

    World->addSeparator();
    QAction* BakeCaptures = World->addAction(QStringLiteral("&Bake Scene Captures"));
    BakeCaptures->setToolTip(QStringLiteral("Every Scene Capture Probe captures now, and its render target file keeps it as its bake: "
                                            "what materials sample before (or without) a live capture"));
    connect(BakeCaptures, &QAction::triggered, this, [this]()
    {
        EditorRenderTargets::BakeSceneCaptures(Level, Context, this, [this](const QString& InText) { statusBar()->showMessage(InText, 8000); });
    });

    World->addSeparator();
    QAction* WorldSettings = World->addAction(QStringLiteral("World &Settings..."));
    WorldSettings->setToolTip(QStringLiteral("What this world saves besides its entities: baked lighting"));
    connect(WorldSettings, &QAction::triggered, this, [this]()
    {
        WorldSettingsDialog Dialog(Level, this);
        Dialog.exec();
    });
    SceneActions.push_back(WorldSettings);

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

    QAction* Docs = Help->addAction(QStringLiteral("&Documentation"));
    Docs->setShortcut(QKeySequence::HelpContents);
    Docs->setToolTip(QStringLiteral("The engine's documentation (Engine/Docs), rendered (F1)"));
    connect(Docs, &QAction::triggered, this, []() { DocumentationWindow::Open(); });

    Help->addSeparator();

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

    PlayAction = PlayMenu->addAction(EditorIcons::Get(EditorIcons::EIcon::Play), QStringLiteral("&Play"));
    PlayAction->setShortcut(QKeySequence(Qt::Key_F5));
    PlayAction->setToolTip(QStringLiteral("Run the game ([Game] Module) from its start level in the viewport (F5)"));
    connect(PlayAction, &QAction::triggered, this, &EditorMainWindow::StartPlay);

    StopAction = PlayMenu->addAction(EditorIcons::Get(EditorIcons::EIcon::Stop), QStringLiteral("&Stop"));
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

    PlayMenu->addSeparator();

    /* A second (third, ...) player: the game's own window, joining the run. */
    ClientWindowAction = PlayMenu->addAction(QStringLiteral("New &Client Window"));
    ClientWindowAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6));
    ClientWindowAction->setToolTip(QStringLiteral("While a networked run plays (Editor Settings > Play), open the game in a "
                                                  "window of its own as another client of it (Shift+F6)"));
    ClientWindowAction->setEnabled(false);
    connect(ClientWindowAction, &QAction::triggered, this, [this]()
    {
        statusBar()->showMessage(Context.LaunchPlayClientWindow() ? QStringLiteral("Client window started")
                                                                  : QStringLiteral("No client window -- see the Console"), 5000);
    });

    QAction* PlaySettings = PlayMenu->addAction(QStringLiteral("Network &Settings..."));
    PlaySettings->setToolTip(QStringLiteral("How Play runs: standalone, as a listen server, or as a client (Editor Settings > Play)"));
    connect(PlaySettings, &QAction::triggered, this, [this]() { OpenSettings(QStringLiteral("Play")); });

    /* A session server on this machine, for Play Together without hosting one by hand. */
    QMenu*   NetMenu = PlayMenu->addMenu(QStringLiteral("&Net"));
    QAction* LocalServer = NetMenu->addAction(QStringLiteral("Launch &Local Server"));
    LocalServer->setToolTip(QStringLiteral("Start NimbleEditorServer on this machine for this project's checkout; join it "
                                           "with File > Collaboration Session. It stops when this editor closes."));
    connect(LocalServer, &QAction::triggered, this, &EditorMainWindow::ToggleLocalSessionServer);
    connect(NetMenu, &QMenu::aboutToShow, this, [this, LocalServer]()
    {
        const bool bRunning = LocalSessionServer && LocalSessionServer->state() != QProcess::NotRunning;
        LocalServer->setText(bRunning ? QStringLiteral("Stop &Local Server") : QStringLiteral("Launch &Local Server"));
    });

    /* Play Together: everyone in the collaboration session, on the server
     * host's game server (EditorCollabSession). */
    PlayMenu->addSeparator();
    QAction* PlayTogether = PlayMenu->addAction(QStringLiteral("Play &Together"));
    PlayTogether->setToolTip(QStringLiteral("Start a run of the open level on the session server's game server and invite everyone "
                                            "in the session (or join the one that's going)"));
    connect(PlayTogether, &QAction::triggered, this, [this]()
    {
        CollabSession->RequestPlayTogether(CurrentPath.isEmpty() ? QString() : EditorContent::ToRuntimePath(CurrentPath));
    });
    QAction* StopTogether = PlayMenu->addAction(QStringLiteral("Stop Play T&ogether"));
    StopTogether->setToolTip(QStringLiteral("End the shared run you started, for everyone"));
    connect(StopTogether, &QAction::triggered, this, [this]() { CollabSession->StopPlayTogether(); });
    connect(PlayMenu, &QMenu::aboutToShow, this, [this, PlayTogether, StopTogether]()
    {
        PlayTogether->setEnabled(CollabSession->IsJoined());
        PlayTogether->setText(CollabSession->IsRunActive() ? QStringLiteral("Join Play &Together") : QStringLiteral("Play &Together"));
        StopTogether->setEnabled(CollabSession->IsRunStarter());
    });

    /* In front of the navigation buttons, where a run is started and ended. */
    QToolBar* Bar = ViewportArea->GetToolbar();
    QAction*  First = Bar->actions().isEmpty() ? nullptr : Bar->actions().front();
    /* Undo and Redo first: the level's, as Edit's. */
    Bar->insertAction(First, UndoAction);
    Bar->insertAction(First, RedoAction);
    Bar->insertSeparator(First);
    Bar->insertAction(First, PlayAction);
    Bar->insertAction(First, StopAction);
    Bar->insertAction(First, EjectAction);
    Bar->insertSeparator(First);

    /* The viewport toolbar is text-only; these show their icons (the tooltip names them). */
    for(QAction* Each : { UndoAction, RedoAction, PlayAction, StopAction })
    {
        if(auto* Button = qobject_cast<QToolButton*>(Bar->widgetForAction(Each)))
        {
            Button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        }
    }
}

void EditorMainWindow::StartPlay()
{
    /* SDL wraps the viewport once: the game's input needs it, so the probe yields. */
    if(InputProbe && InputProbe->IsRunning())
    {
        InputProbeAction->setChecked(false);   // -> SetSDLInputProbe(false)
        CORE_LOG("[Editor] the SDL input probe stops while playing: the game's input uses the viewport")
    }

    /* The open world's level, if the project lists it (Project Settings > Worlds). */
    if(!Context.StartPlay(CurrentPath.isEmpty() ? QString() : EditorContent::ToRuntimePath(CurrentPath)))
    {
        EditorNotifications::Notification Failed;
        Failed.Severity   = EditorNotifications::ESeverity::Error;
        Failed.Title      = QStringLiteral("Play failed");
        Failed.Body       = QStringLiteral("The Console says why.");
        Failed.Source     = QStringLiteral("Play");
        Failed.ReplaceKey = QStringLiteral("play.failed");
        Failed.Actions    = { { QStringLiteral("Show Log"), [this]() { ShowConsole(); } } };
        EditorNotifications::Get()->Post(std::move(Failed));
        return;
    }
    /* So keys reach the viewport (Esc to stop) rather than the last panel. */
    Viewport->setFocus(Qt::OtherFocusReason);
}

void EditorMainWindow::ToggleLocalSessionServer()
{
    if(LocalSessionServer && LocalSessionServer->state() != QProcess::NotRunning)
    {
        /* A console program: killed, not asked. Its workers and game servers go with it. */
        LocalSessionServer->setProperty("Stopping", true);
        LocalSessionServer->kill();
        LocalSessionServer->waitForFinished(5000);
        statusBar()->showMessage(QStringLiteral("Local session server stopped"), 5000);
        return;
    }

    const auto Fail = [](const QString& InTitle, const QString& InBody)
    {
        EditorNotifications::Notification Failed;
        Failed.Severity   = EditorNotifications::ESeverity::Error;
        Failed.Title      = InTitle;
        Failed.Body       = InBody;
        Failed.Source     = QStringLiteral("Session");
        Failed.ReplaceKey = QStringLiteral("session.localserver");
        EditorNotifications::Get()->Post(std::move(Failed));
    };

    /* NimbleEditorServer is built beside the editor. */
#if defined(_WIN32)
    const QString Executable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("NimbleEditorServer.exe"));
#else
    const QString Executable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("NimbleEditorServer"));
#endif
    if(!QFileInfo::exists(Executable))
    {
        Fail(QStringLiteral("No session server"), QStringLiteral("NimbleEditorServer isn't built beside the editor. Build its target, then try again."));
        return;
    }

    /* This project's checkout hosts the content worker, which builds and checks
     * each session's commit and runs Play Together's game server. */
    const QDir    Project(EditorGraphs::GetProjectRoot());
    QStringList   Args = { QStringLiteral("--logs"), Project.filePath(QStringLiteral("Saved/Logs")) };
    const QString Repo = CollabSession->GetRepositoryRoot();
    if(!Repo.isEmpty())
    {
        Args << QStringLiteral("--repo") << Repo;   // worktrees: the server's short per-user folder
    }

    if(!LocalSessionServer)
    {
        LocalSessionServer = new QProcess(this);   // killed with the editor
        LocalSessionServer->setProcessChannelMode(QProcess::MergedChannels);
        connect(LocalSessionServer, &QProcess::readyRead, this, [this]()
        {
            while(LocalSessionServer->canReadLine())
            {
                const QString Line = QString::fromUtf8(LocalSessionServer->readLine()).trimmed();
                if(!Line.isEmpty()) { CORE_LOG("[Session server] " + Line.toStdString()) }
            }
        });
        connect(LocalSessionServer, &QProcess::finished, this, [this, Fail](const int InExitCode, const QProcess::ExitStatus)
        {
            if(LocalSessionServer->property("Stopping").toBool()) { return; }
            Fail(QStringLiteral("Local session server stopped"),
                 QStringLiteral("It exited with code %1; the Console says why (is another server on UDP %2?).")
                     .arg(InExitCode).arg(Collab::DefaultPort));
        });
    }
    LocalSessionServer->setProperty("Stopping", false);
    LocalSessionServer->setWorkingDirectory(QFileInfo(Executable).absolutePath());
    LocalSessionServer->start(Executable, Args);
    if(!LocalSessionServer->waitForStarted(5000))
    {
        Fail(QStringLiteral("No session server"), QStringLiteral("NimbleEditorServer didn't start: ") + LocalSessionServer->errorString());
        return;
    }

    EditorNotifications::Notification Started;
    Started.Severity   = EditorNotifications::ESeverity::Success;
    Started.Title      = QStringLiteral("Local session server started");
    Started.Body       = QStringLiteral("Listening on UDP %1. Join it at localhost; others join at this machine's address.")
                             .arg(Collab::DefaultPort);
    Started.Source     = QStringLiteral("Session");
    Started.ReplaceKey = QStringLiteral("session.localserver");
    Started.Actions    = { { QStringLiteral("Join..."), [this]()
    {
        if(auto* Dock = findChild<CollabSessionWindow*>())
        {
            Dock->show();
            Dock->raise();
            Dock->GetAddressEdit()->setText(QStringLiteral("localhost"));
            Dock->GetAddressEdit()->setFocus();
        }
    } } };
    EditorNotifications::Get()->Post(std::move(Started));
}

void EditorMainWindow::BuildBuildMenu()
{
    QMenu* BuildMenu = menuBar()->addMenu(QStringLiteral("&Build"));

    Compiler = new SourceBuild(this);
    connect(Compiler, &SourceBuild::Finished, this, [this](const bool bInSucceeded, const QString&)
    {
        OnCompileFinished(bInSucceeded);
    });
    /* The first compiler error, for the failure's toast. Ninja's "FAILED:"
     * comes before it, so only while there is nothing better. */
    connect(Compiler, &SourceBuild::OutputLine, this, [this](const QString& InLine)
    {
        if(!SourceBuild::IsErrorLine(InLine))
        {
            return;
        }
        const bool bNinja = InLine.startsWith(QStringLiteral("FAILED:"));
        if(FirstCompileError.isEmpty() || (FirstCompileError.startsWith(QStringLiteral("FAILED:")) && !bNinja))
        {
            FirstCompileError = InLine.trimmed();
        }
    });

    CompileAction = BuildMenu->addAction(QStringLiteral("&Compile Source"));
    CompileAction->setShortcut(QKeySequence(Qt::Key_F7));
    CompileAction->setToolTip(QStringLiteral("Build the game module ([Game] Module) and restart a running game on it (F7)"));
    connect(CompileAction, &QAction::triggered, this, &EditorMainWindow::CompileSource);

    CancelCompileAction = BuildMenu->addAction(QStringLiteral("C&ancel Compile"));
    CancelCompileAction->setEnabled(false);
    connect(CancelCompileAction, &QAction::triggered, Compiler, &SourceBuild::Cancel);

    BuildMenu->addSeparator();

    QAction* BuildProject = BuildMenu->addAction(QStringLiteral("&Build Project..."));
    BuildProject->setToolTip(QStringLiteral("Cook the project's content and compile the game as a packaged build "
                                            "(Debug, RelWithDebInfo or Shipping), then open its folder"));
    connect(BuildProject, &QAction::triggered, this, [this]() { OpenBuildProject(); });

    QAction* Cook = BuildMenu->addAction(QStringLiteral("C&ook Content"));
    Cook->setToolTip(QStringLiteral("Cook the worlds Project Settings > Worlds ticks, every prefab and UI document, and "
                                    "regenerate the node graphs, into <project>/Saved/Cooked; shows the cook's stats"));
    connect(Cook, &QAction::triggered, this, &EditorMainWindow::CookContent);

    QAction* BakeLightmaps = BuildMenu->addAction(QStringLiteral("Bake &Lightmaps"));
    BakeLightmaps->setToolTip(QStringLiteral("Light the static entities from the static lights into lightmaps and light probes, "
                                             "beside the world (World > World Settings turns baked lighting on)"));
    connect(BakeLightmaps, &QAction::triggered, this, [this]()
    {
        QString Outcome;
        const bool bBaked = EditorLighting::BakeLightmaps(Level, Context, CurrentPath, this,
                                                          [&Outcome](const QString& InText) { Outcome = InText; });
        if(!Outcome.isEmpty())
        {
            EditorNotifications::Notify(bBaked ? EditorNotifications::ESeverity::Success : EditorNotifications::ESeverity::Warning,
                                        bBaked ? QStringLiteral("Lightmaps baked") : QStringLiteral("Lightmaps not baked"),
                                        Outcome, QStringLiteral("Bake"), QStringLiteral("bake.lightmaps"));
        }
    });
    SceneActions.push_back(BakeLightmaps);

    QAction* RefreshNodes = BuildMenu->addAction(QStringLiteral("&Refresh Script Nodes"));
    RefreshNodes->setToolTip(QStringLiteral("Reread the game module's components and script libraries into the script graph nodes "
                                            "(graphs already open keep theirs until reopened)"));
    connect(RefreshNodes, &QAction::triggered, this, [this]()
    {
        /* Not playing: the editor's own copy, reloaded, is what is read. */
        const bool bLoaded    = Context.IsPlaying() || Context.ReloadAuthoringGame();
        const bool bRefreshed = EditorGraphs::RefreshGameDeclarations(bLoaded);
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
        PostCompileResult(EditorNotifications::ESeverity::Error, QStringLiteral("Nothing to compile"),
                          QStringLiteral("Set [Game] Module in Config/DefaultEngine.ini."));
        return;
    }

    /* The module, then what runs it: the game's launcher, and with networking
     * its dedicated server -- what Play's client windows and server run. Not
     * while they are running: their executables are locked. */
    QStringList Targets = { Module };
    if(GEditorBuildGameExecutables.GetBool())
    {
        if(Context.IsPlayNetworked())
        {
            CORE_LOG_WARN("[Build] the game's client and server are running with Play; building " + std::string(Module.toUtf8().constData()) + " only")
        }
        else
        {
            Targets << GameExecutables::FindTargets(Module, QStringLiteral("client"))
                    << GameExecutables::FindTargets(Module, QStringLiteral("server"));
        }
    }

    FirstCompileError.clear();
    if(!Compiler->Start(Targets))
    {
        PostCompileResult(EditorNotifications::ESeverity::Error, QStringLiteral("Compile didn't start"),
                          QStringLiteral("The Console says why."));
        return;
    }
    if(Compiler->IsRunning())
    {
        CompileAction->setEnabled(false);
        CancelCompileAction->setEnabled(true);

        EditorNotifications::Notification Running;
        Running.Title      = QStringLiteral("Compiling %1…").arg(Module);
        Running.Source     = QStringLiteral("Build");
        Running.ReplaceKey = QStringLiteral("build.compile");
        Running.Progress   = EditorNotifications::Indeterminate;
        Running.Actions    = { { QStringLiteral("Cancel"), [this]() { Compiler->Cancel(); } } };
        EditorNotifications::Get()->Post(std::move(Running));
    }
}

void EditorMainWindow::PostCompileResult(const EditorNotifications::ESeverity InSeverity, const QString& InTitle, const QString& InBody)
{
    EditorNotifications::Notification Result;
    Result.Severity   = InSeverity;
    Result.Title      = InTitle;
    Result.Body       = InBody;
    Result.Source     = QStringLiteral("Build");
    Result.ReplaceKey = QStringLiteral("build.compile");
    if(InSeverity == EditorNotifications::ESeverity::Error)
    {
        Result.Actions = { { QStringLiteral("Show Log"), [this]() { ShowConsole(); } } };
    }
    EditorNotifications::Get()->Post(std::move(Result));
}

void EditorMainWindow::ReloadProjectConfig()
{
    /* The project's own files (a pull changed them, not the staged copies),
     * then the user's choices over them again, as at startup. */
    const QString EngineIni = ProjectSettingsDialog::GetProjectConfigPath();
    const QString GameIni   = QFileInfo(EngineIni).dir().absoluteFilePath(QStringLiteral("DefaultGame.ini"));
    for(const QString& Each : { EngineIni, GameIni })
    {
        if(QFileInfo::exists(Each))
        {
            EngineCore::LoadConfigFile(Each.toUtf8().toStdString());
        }
    }
    SettingsDialog::LoadUserSettings();
    EditorNotifications::Notify(EditorNotifications::ESeverity::Info, QStringLiteral("Config reloaded"),
                                QStringLiteral("Settings read only at startup -- the renderer, the modules -- apply after a restart."),
                                QStringLiteral("Collab"), QStringLiteral("collab.config"));
}

void EditorMainWindow::OnContentChanged(const QStringList& InFiles)
{
    QStringList OpenGraphs;
    for(const QString& File : InFiles)
    {
        /* Only what the engine loads: the project's content and graphs. */
        if(File.endsWith(QStringLiteral(".nmgraph"), Qt::CaseInsensitive))
        {
            if(QFileInfo::exists(File))
            {
                EditorGraphs::RestageGraph(File);
            }
            if(EditorGraphs::IsGraphOpen(File))
            {
                OpenGraphs << QFileInfo(File).fileName();
            }
        }
        else if(!EditorContent::ToRuntimePath(File).isEmpty())
        {
            if(QFileInfo::exists(File))
            {
                EditorContent::StageAsset(File);
            }
            else
            {
                EditorContent::UnstageAsset(File);
            }
        }
    }
    /* An open graph keeps its own copy until reopened. */
    if(!OpenGraphs.isEmpty())
    {
        EditorNotifications::Notify(EditorNotifications::ESeverity::Warning, QStringLiteral("Open graphs changed upstream"),
                                    QStringLiteral("%1 changed in the pull. Close and reopen to see the new version; "
                                                   "saving now would overwrite it.").arg(OpenGraphs.join(QStringLiteral(", "))),
                                    QStringLiteral("Collab"), QStringLiteral("collab.graphs"));
    }
}

void EditorMainWindow::ShowConsole()
{
    auto* Dock = static_cast<QDockWidget*>(ConsoleArea->parentWidget());
    Dock->show();
    Dock->raise();
}

void EditorMainWindow::OnCompileFinished(const bool bInSucceeded)
{
    CompileAction->setEnabled(true);
    CancelCompileAction->setEnabled(false);

    const QString Module = Compiler->GetTarget();
    using ESeverity = EditorNotifications::ESeverity;
    if(!bInSucceeded && Compiler->WasCancelled())
    {
        PostCompileResult(ESeverity::Info, QStringLiteral("Compile of %1 cancelled").arg(Module));
        return;
    }
    if(!bInSucceeded)
    {
        PostCompileResult(ESeverity::Error, QStringLiteral("Compiling %1 failed").arg(Module),
                          FirstCompileError.isEmpty() ? QStringLiteral("The Console has the build's output.") : FirstCompileError);
        return;
    }

    /* The next Play loads the new DLL anyway; restart a running one unless
     * Editor.ReloadAfterCompile says otherwise. Script nodes follow the new
     * build: read now, or when a run still on the old one stops. */
    if(Context.IsPlaying() && !GEditorReloadAfterCompile.GetBool())
    {
        CORE_LOG("[Build] " + std::string(Module.toUtf8().constData()) + " is loaded at the next Play; the run keeps the old one")
        PostCompileResult(ESeverity::Success, QStringLiteral("Compiled %1").arg(Module),
                          QStringLiteral("The run keeps the old build: Stop and Play to load it."));
        bDeclarationsStale = true;
        return;
    }
    if(!Context.IsPlaying())
    {
        CORE_LOG("[Build] " + std::string(Module.toUtf8().constData()) + " reloaded for editing, and loaded at the next Play")
        const bool bReloaded = Context.ReloadAuthoringGame();
        PostCompileResult(bReloaded ? ESeverity::Success : ESeverity::Warning, QStringLiteral("Compiled %1").arg(Module),
                          bReloaded ? QStringLiteral("Reloaded for editing.")
                                    : QStringLiteral("It didn't reload for editing; the Console says why."));
        EditorGraphs::RefreshGameDeclarations(bReloaded);
        return;
    }

    CORE_LOG("[Build] reloading " + std::string(Module.toUtf8().constData()) + ": restarting the run")
    Context.StopPlay();
    StartPlay();
    if(Context.IsPlaying())
    {
        PostCompileResult(ESeverity::Success, QStringLiteral("Compiled %1").arg(Module), QStringLiteral("The run restarted on it."));
    }
    else
    {
        /* StartPlay posted why; this one replaces the progress. */
        PostCompileResult(ESeverity::Warning, QStringLiteral("Compiled %1").arg(Module), QStringLiteral("The run didn't restart."));
    }
    EditorGraphs::RefreshGameDeclarations(Context.IsPlaying());
}

void EditorMainWindow::OpenSettings(const QString& InPage)
{
    /* One at a time: a second request raises the open one. */
    if(auto* Open = findChild<SettingsDialog*>())
    {
        if(!InPage.isEmpty()) { Open->ShowPage(InPage); }
        Open->raise();
        Open->activateWindow();
        return;
    }
    auto* Dialog = new SettingsDialog(this);   // deletes itself on close
    if(!InPage.isEmpty()) { Dialog->ShowPage(InPage); }
    Dialog->show();
}

void EditorMainWindow::OpenProjectSettings(const bool bInWorlds)
{
    if(auto* Open = findChild<ProjectSettingsDialog*>())
    {
        if(bInWorlds) { Open->ShowWorlds(); }
        Open->raise();
        Open->activateWindow();
        return;
    }
    auto* Dialog = new ProjectSettingsDialog(this);   // deletes itself on close
    Dialog->SetOpenWorld(CurrentPath.isEmpty() ? QString() : EditorContent::ToRuntimePath(CurrentPath));
    if(bInWorlds) { Dialog->ShowWorlds(); }
    Dialog->show();
}

void EditorMainWindow::CookContent()
{
    if(ProjectBuildDialog* Dialog = OpenBuildProject())
    {
        Dialog->RunCookOnly();
    }
}

ProjectBuildDialog* EditorMainWindow::OpenBuildProject()
{
    /* One at a time: a second request raises the open one. */
    if(auto* Open = findChild<ProjectBuildDialog*>())
    {
        Open->raise();
        Open->activateWindow();
        return Open;
    }
    auto* Dialog = new ProjectBuildDialog(this, [this]() { OpenProjectSettings(true); });   // deletes itself on close
    Dialog->show();
    return Dialog;
}

void EditorMainWindow::OnPlayStateChanged(const bool bInPlaying)
{
    PlayAction->setEnabled(!bInPlaying);
    StopAction->setEnabled(bInPlaying);
    EjectAction->setEnabled(bInPlaying);
    ClientWindowAction->setEnabled(bInPlaying && Context.IsPlayNetworked());
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
        EditorGraphs::RefreshGameDeclarations(Context.IsAuthoringGameLoaded());
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
        if(InArgs.isEmpty() && Level.GetSelectedEntities().size() > 1)
        {
            Console.Print(QStringLiteral("Destroyed %1 entities").arg(Level.DeleteSelection()));
            return;
        }
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

    Console.Register(QStringLiteral("camera"), QStringLiteral("[<x> <y> <z> [yaw] [pitch]]"),
                     QStringLiteral("Put the viewport's camera at a position, turned yaw degrees about Y and pitched up; "
                                    "with nothing, print where it is"),
                     [this](const QStringList& InArgs)
    {
        EditorCamera& Camera = Viewport->GetCamera();
        if(InArgs.isEmpty())
        {
            Console.Print(QStringLiteral("camera %1 %2 %3 %4 %5").arg(Camera.Position.GetX()).arg(Camera.Position.GetY())
                          .arg(Camera.Position.GetZ()).arg(double(Camera.Yaw)).arg(double(Camera.Pitch)));
            return;
        }
        if(InArgs.size() < 3)
        {
            Console.PrintError(QStringLiteral("Usage: camera [<x> <y> <z> [yaw] [pitch]]"));
            return;
        }
        Camera.Position = EngineCore::Vector3(InArgs[0].toDouble(), InArgs[1].toDouble(), InArgs[2].toDouble());
        Camera.Yaw   = 0.0f;
        Camera.Pitch = 0.0f;
        Camera.AddYawPitch(InArgs.value(3).toFloat(), InArgs.value(4).toFloat());
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

    Console.Register(QStringLiteral("lighting"), QStringLiteral("<bake | baked | live>"),
                     QStringLiteral("Bake the static lights into the static entities (Build > Bake Lightmaps), or draw with the bake or live (Show > Lighting)"),
                     [this](const QStringList& InArgs)
    {
        const QString Word = InArgs.isEmpty() ? QString() : InArgs[0];
        if(Word == QStringLiteral("bake"))
        {
            EditorLighting::BakeLightmaps(Level, Context, CurrentPath, this, [this](const QString& InText) { Console.Print(InText); });
        }
        else if(Word == QStringLiteral("baked") || Word == QStringLiteral("live"))
        {
            ViewportArea->SetBakedLighting(Word == QStringLiteral("baked"));
            Console.Print(QStringLiteral("Lighting: %1").arg(Word));
        }
        else
        {
            Console.PrintError(QStringLiteral("Usage: lighting <bake | baked | live>"));
        }
    });

    /* The Content Browser's import, by hand or from --command: one file at the
     * importer's default options, into a project folder (default the
     * project's Content). Mesh, image or sound, by its extension. */
    Console.Register(QStringLiteral("import"), QStringLiteral("<source file> [project folder]"),
                     QStringLiteral("Import a .gltf/.glb, an image or a sound into the project (Content, or the folder given), "
                                    "with the importer's default options"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: import <source file> [project folder]"));
            return;
        }
        const QString Root   = EditorGraphs::GetProjectRoot();
        const QString Source = QFileInfo(QDir(Root), InArgs[0]).absoluteFilePath();
        const QString Folder = QDir(Root).absoluteFilePath(InArgs.size() > 1 ? InArgs[1] : QStringLiteral("Content"));
        const QString Base   = QFileInfo(Source).completeBaseName();
        QDir().mkpath(Folder);

        EditorContent::ImportResult Result;
        switch(EditorContent::ClassifySource(Source))
        {
            case EditorContent::EImportKind::Mesh:
                Result = EditorContent::ImportMesh(Source, Folder, Base, EditorContent::MeshImportOptions(),
                                                   EditorContent::TextureImportOptions(), Root);
                break;
            case EditorContent::EImportKind::Texture:
                Result = EditorContent::ImportTexture(Source, Folder, Base, EditorContent::TextureImportOptions(), Root);
                break;
            case EditorContent::EImportKind::Sound:
                Result = EditorContent::ImportSound(Source, Folder, Base, Root);
                break;
            case EditorContent::EImportKind::None:
                Console.PrintError(QStringLiteral("import: %1 is not a mesh, image or sound this editor imports").arg(InArgs[0]));
                return;
        }
        for(const QString& Written : Result.Written) { EditorContent::StageAsset(Written); }
        for(const QString& Warning : Result.Warnings) { Console.Print(QStringLiteral("  ") + Warning); }
        if(Result.bOK) { Console.Print(QStringLiteral("import: ") + Result.Message); }
        else           { Console.PrintError(QStringLiteral("import: ") + Result.Message); }
    });

    Console.Register(QStringLiteral("viewmode"), QStringLiteral("<lit | unlit | target [name]>"),
                     QStringLiteral("The viewport's Show > View Mode; \"target\" alone lists the render targets it can show"),
                     [this](const QStringList& InArgs)
    {
        const QString Mode = InArgs.isEmpty() ? QString() : InArgs[0];
        if(Mode == QStringLiteral("lit"))        { Context.SetViewMode(RenderCore::ERenderViewMode::Lit); }
        else if(Mode == QStringLiteral("unlit")) { Context.SetViewMode(RenderCore::ERenderViewMode::Unlit); }
        else if(Mode == QStringLiteral("target"))
        {
            const QStringList Targets = Context.GetViewableTargets();
            const QString Name = InArgs.mid(1).join(QLatin1Char(' '));
            if(Name.isEmpty() || !Targets.contains(Name))
            {
                if(!Name.isEmpty()) { Console.PrintError(QStringLiteral("No render target named \"%1\"").arg(Name)); }
                Console.Print(QStringLiteral("Render targets: %1").arg(Targets.isEmpty() ? QStringLiteral("none yet") : Targets.join(QStringLiteral(", "))));
                return;
            }
            Context.SetViewMode(RenderCore::ERenderViewMode::RenderTarget, Name);
        }
        else
        {
            Console.PrintError(QStringLiteral("Usage: viewmode <lit | unlit | target [name]>"));
            return;
        }
        ViewportArea->RefreshViewMode();
    });

    Console.Register(QStringLiteral("save"), QString(), QStringLiteral("File > Save World (Ctrl+S)"),
                     [this](const QStringList&)
    {
        if(CurrentPath.isEmpty())
        {
            Console.PrintError(QStringLiteral("The world has no file yet: File > Save World As"));
            return;
        }
        if(SaveWorld()) { Console.Print(QStringLiteral("Saved %1").arg(CurrentPath)); }
    });

    Console.Register(QStringLiteral("screenshot"), QStringLiteral("<file.png>"),
                     QStringLiteral("Save the viewport's next frame (no gizmos or debug lines) as an image"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: screenshot <file.png>"));
            return;
        }
        const QString Path = QFileInfo(InArgs.join(QLatin1Char(' '))).absoluteFilePath();
        const uint64_t Request = Context.RequestSceneCapture();
        if(Request == 0)
        {
            Console.PrintError(QStringLiteral("The viewport's renderer is not running"));
            return;
        }
        SaveCaptureWhenReady(Request, Path);
    });

    Console.Register(QStringLiteral("windowshot"), QStringLiteral("<file.png>"),
                     QStringLiteral("Save the whole editor window, panels and docks included, as an image"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: windowshot <file.png>"));
            return;
        }
        const QString File = QFileInfo(InArgs.join(QLatin1Char(' '))).absoluteFilePath();
        const uint64_t Request = Viewport && Viewport->isVisible() ? Context.RequestSceneCapture() : 0;
        if(Request != 0)
        {
            SaveCaptureWhenReady(Request, File, true);
            return;
        }
        if(!grab().save(File)) { Console.PrintError(QStringLiteral("Could not write %1").arg(File)); }
        else                   { Console.Print(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(File))); }
    });

    Console.Register(QStringLiteral("quit"), QString(), QStringLiteral("File > Exit (asks to save unsaved changes)"),
                     [this](const QStringList&)
    {
        QMetaObject::invokeMethod(this, &QWidget::close, Qt::QueuedConnection);
    });

    Console.Register(QStringLiteral("projectsettings"), QStringLiteral("[page]"),
                     QStringLiteral("Open Edit > Project Settings, on a page if named (Net, Shadows, Worlds...)"),
                     [this](const QStringList& InArgs)
    {
        OpenProjectSettings();
        if(!InArgs.isEmpty())
        {
            auto* Dialog = findChild<ProjectSettingsDialog*>();
            if(!Dialog || !Dialog->ShowPage(InArgs.join(QLatin1Char(' '))))
            {
                Console.PrintError(QStringLiteral("No Project Settings page named %1").arg(InArgs.join(QLatin1Char(' '))));
            }
        }
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

    Console.Register(QStringLiteral("cook"), QStringLiteral("[project | world | prefab | prefabs | ui | doc.nmui] [out]"),
                     QStringLiteral("Cook to binary, into <project>/Saved/Cooked/<its path>, which a game build stages: 'project' is "
                                    "Build > Cook Content (the listed worlds, every prefab and UI document, with stats); "
                                    "'prefabs' and 'ui' every prefab or UI document; else one file (default: the open world)"),
                     [this](const QStringList& InArgs)
    {
        /* A project file's cooked copy: the mirror Build > Cook Content writes. */
        const auto DefaultOut = [](const QString& InSource)
        {
            const QString Runtime = EditorContent::ToRuntimePath(InSource);
            if(!Runtime.isEmpty() && !Runtime.startsWith(QStringLiteral("Engine/")))
            {
                return EditorCook::GetCookedPath(EditorGraphs::GetProjectRoot(), Runtime);
            }
            const QFileInfo Info(InSource);
            return Info.dir().filePath(QStringLiteral("Cooked/") + Info.fileName());
        };

        if(!InArgs.isEmpty() && InArgs[0] == QStringLiteral("project"))
        {
            const QString Root = EditorGraphs::GetProjectRoot();
            QStringList Lines = { EditorCook::LogHeader(QStringLiteral("Cook")) };
            const auto Stamped = [&Lines](const QString& InText) { Lines << EditorCook::LogStamp() + InText; };
            const EditorCook::CookStats Stats = EditorCook::CookProject(Root, Stamped);
            for(const QString& Line : EditorCook::DescribeStats(Stats)) { Console.Print(Line); Stamped(Line); }
            for(const QString& Failure : Stats.Failures) { Console.PrintError(Failure); Stamped(QStringLiteral("FAILED: ") + Failure); }

            /* As Build > Cook Content's: this run's log in Saved/Logs. */
            QFile RunLog(EditorCook::NewLogPath(Root, QStringLiteral("Cook")));
            if(RunLog.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                RunLog.write((Lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
                Console.Print(QStringLiteral("Log: %1").arg(QDir::toNativeSeparators(RunLog.fileName())));
            }
            return;
        }

        /* Every UI document in the project, to <out>/<its runtime path>. */
        if(!InArgs.isEmpty() && InArgs[0] == QStringLiteral("ui"))
        {
            const QString Root    = EditorGraphs::GetProjectRoot();
            const QString OutRoot = InArgs.size() >= 2 ? InArgs[1] : EditorCook::GetCookedRoot(Root);
            int Failed = 0;
            QString Errors;
            const int Cooked = EditorUI::CookAll(Root, OutRoot, Failed, Errors);
            if(!Errors.isEmpty()) Console.PrintError(Errors.trimmed());
            Console.Print(QStringLiteral("Cooked %1 UI document(s) to %2%3").arg(Cooked).arg(QDir::toNativeSeparators(OutRoot))
                          .arg(Failed ? QStringLiteral("; %1 failed").arg(Failed) : QString()));
            return;
        }
        if(!InArgs.isEmpty() && InArgs[0].endsWith(QStringLiteral(".nmui"), Qt::CaseInsensitive))
        {
            const QFileInfo SourceInfo(InArgs[0]);
            const QString Out = InArgs.size() >= 2 ? InArgs[1] : DefaultOut(InArgs[0]);
            if(QFileInfo(Out).absoluteFilePath() == SourceInfo.absoluteFilePath())
            {
                Console.PrintError(QStringLiteral("Refusing to cook a UI document over itself"));
                return;
            }
            std::string Error;
            if(UI::CookUIFile(InArgs[0].toUtf8().toStdString(), Out.toUtf8().toStdString(), &Error))
            {
                Console.Print(QStringLiteral("Cooked %1 -> %2").arg(InArgs[0], Out));
            }
            else
            {
                Console.PrintError(QStringLiteral("Cook failed: %1").arg(QString::fromStdString(Error)));
            }
            return;
        }

        /* Every prefab in the project, to <out>/<its runtime path>. */
        if(!InArgs.isEmpty() && InArgs[0] == QStringLiteral("prefabs"))
        {
            const QString Root = EditorGraphs::GetProjectRoot();
            const QString OutRoot = InArgs.size() >= 2 ? InArgs[1] : EditorCook::GetCookedRoot(Root);
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
            const QString Out = InArgs.size() >= 2 ? InArgs[1] : DefaultOut(InArgs[0]);
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
        const QString Out = InArgs.size() >= 2 ? InArgs[1] : DefaultOut(Source);
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

    Console.Register(QStringLiteral("graph"), QStringLiteral("<path> | windowshot <file.png> | canvasshot <file.png> [scale]"),
                     QStringLiteral("Open a node graph (*.material.nmgraph, *.script.nmgraph; relative to the project), "
                                    "save the graph editor's window as an image, or draw the whole graph at a scale (default 2)"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: graph <path> | windowshot <file.png> | canvasshot <file.png> [scale]"));
            return;
        }
        if(InArgs.front() == QStringLiteral("canvasshot"))
        {
            GraphEditorWindow* Window = GraphEditorWindow::Find();
            GraphEditorTab*    Tab    = Window ? Window->CurrentTab() : nullptr;
            if(!Tab || InArgs.size() < 2)
            {
                Console.PrintError(Tab ? QStringLiteral("Usage: graph canvasshot <file.png> [scale]") : QStringLiteral("No graph is open"));
                return;
            }
            const QString File  = QFileInfo(InArgs[1]).absoluteFilePath();
            const double  Scale = InArgs.size() > 2 ? InArgs[2].toDouble() : 2.0;
            QRectF Area;
            if(!Tab->SaveCanvasImage(File, Scale, Area)) { Console.PrintError(QStringLiteral("Could not write %1 (too large at that scale?)").arg(File)); }
            else
            {
                Console.Print(QStringLiteral("Saved %1: scene %2,%3 to %4,%5 at x%6").arg(QDir::toNativeSeparators(File))
                              .arg(Area.left()).arg(Area.top()).arg(Area.right()).arg(Area.bottom()).arg(Scale));
            }
            return;
        }
        if(InArgs.front() == QStringLiteral("windowshot"))
        {
            GraphEditorWindow* Window = GraphEditorWindow::Find();
            if(!Window)
            {
                Console.PrintError(QStringLiteral("No graph is open"));
                return;
            }
            const QString File = QFileInfo(InArgs.mid(1).join(QLatin1Char(' '))).absoluteFilePath();
            if(!Window->grab().save(File)) { Console.PrintError(QStringLiteral("Could not write %1").arg(File)); }
            else                           { Console.Print(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(File))); }
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

    Console.Register(QStringLiteral("ui"), QStringLiteral("<path | new | close | select <name> | resolution <w>x<h> | preview <on|off> | screenshot <file.png>>"),
                     QStringLiteral("Open a .nmui in the UI Designer, make a new one, or drive the one in front (for scripts and tests)"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: ui <path | new | close | select <name> | resolution <w>x<h> | preview <on|off>>"));
            return;
        }
        const QString Verb = InArgs.front();
        if(Verb == QStringLiteral("new"))   { EditorUI::CreateUI(QString(), this); return; }
        if(Verb == QStringLiteral("close")) { EditorUI::CloseAll(); return; }

        UIEditorWindow* Window = UIEditorWindow::Find();
        UIEditorTab*    Tab    = Window ? Window->CurrentTab() : nullptr;
        if(Verb == QStringLiteral("select") || Verb == QStringLiteral("resolution") || Verb == QStringLiteral("preview"))
        {
            if(!Tab || InArgs.size() < 2)
            {
                Console.PrintError(QStringLiteral("No UI document is open, or the command needs an argument"));
                return;
            }
            if(Verb == QStringLiteral("select"))
            {
                const int Index = Tab->GetDesign().GetAsset().FindWidget(InArgs[1].toStdString());
                if(Index < 0) { Console.PrintError(QStringLiteral("No widget '%1'").arg(InArgs[1])); return; }
                Tab->GetDesign().Select(Index);
            }
            else if(Verb == QStringLiteral("resolution"))
            {
                const QStringList Parts = InArgs[1].split(QLatin1Char('x'));
                if(Parts.size() != 2 || Parts[0].toInt() <= 0 || Parts[1].toInt() <= 0) { Console.PrintError(QStringLiteral("Usage: ui resolution 1920x1080")); return; }
                Tab->GetViewport()->SetDesignSize(QSize(Parts[0].toInt(), Parts[1].toInt()));
                Tab->GetViewport()->FitToView();
            }
            else
            {
                Tab->GetViewport()->SetPreview(InArgs[1] == QStringLiteral("on"));
            }
            return;
        }
        if(Verb == QStringLiteral("screenshot"))
        {
            if(!Tab || InArgs.size() < 2 || Tab->GetViewport()->GetScene() == RenderCore::RENDER_INVALID_SCENE)
            {
                Console.PrintError(QStringLiteral("Usage: ui screenshot <file.png>, with a document open and drawn"));
                return;
            }
            const uint64_t Request = Context.RequestSceneCapture(Tab->GetViewport()->GetScene());
            if(Request == 0) { Console.PrintError(QStringLiteral("The renderer is not running")); return; }
            SaveCaptureWhenReady(Request, QFileInfo(InArgs.mid(1).join(QLatin1Char(' '))).absoluteFilePath());
            return;
        }
        const QString Path = QFileInfo(Verb).isAbsolute() ? Verb : QDir(EditorGraphs::GetProjectRoot()).filePath(Verb);
        if(!EditorUI::OpenUI(Path, this)) { Console.PrintError(QStringLiteral("Could not open %1").arg(Path)); }
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

    Console.Register(QStringLiteral("terrain"), EditorTerrain::GetConsoleUsage(),
                     QStringLiteral("Make, sculpt, paint, import or export the level's terrain (for scripts and tests)"),
                     [this](const QStringList& InArgs) { EditorTerrain::RunConsoleCommand(Console, InArgs); });

    Console.Register(QStringLiteral("foliage"), EditorFoliage::GetConsoleUsage(),
                     QStringLiteral("Add, edit, paint or erase the selected terrain's foliage (for scripts and tests)"),
                     [this](const QStringList& InArgs) { EditorFoliage::RunConsoleCommand(Console, InArgs); });

    Console.Register(QStringLiteral("sequence"),
                     QStringLiteral("<path | new | close | seek <seconds | marker> | play | pause | cuts <on|off> | autokey <on|off> | sound <on|off> | scrub <on|off> | height <px> | collapse <on|off> | select <track> [section] | windowshot <file.png>>"),
                     QStringLiteral("Open a .nmseq in the Sequencer, make a new one, or drive the one in front (for scripts and tests)"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: sequence <path | new | close | seek <seconds | marker> | play | pause | cuts <on|off> | autokey <on|off> | sound <on|off> | scrub <on|off> | height <px> | collapse <on|off> | select <track> [section] | windowshot <file.png>>"));
            return;
        }
        const QString Verb = InArgs.front();
        if(Verb == QStringLiteral("new"))   { EditorSequencer::CreateSequence(QString(), this); return; }
        if(Verb == QStringLiteral("close")) { EditorSequencer::CloseAll(); return; }
        if(Verb == QStringLiteral("height"))
        {
            /* The dock's height in pixels, for a scripted run that wants room. */
            SequencerWindow* Window = SequencerWindow::Find();
            bool bOk = false;
            const int Height = InArgs.value(1).toInt(&bOk);
            if(!Window || !bOk || Height <= 0)
            {
                Console.PrintError(QStringLiteral("Usage: sequence height <pixels> (with the Sequencer open)"));
                return;
            }
            resizeDocks({ Window }, { Height }, Qt::Vertical);
            return;
        }
        if(Verb == QStringLiteral("windowshot"))
        {
            SequencerWindow* Window = SequencerWindow::Find();
            if(!Window || !Window->isVisible())
            {
                Console.PrintError(QStringLiteral("The Sequencer is not open"));
                return;
            }
            const QString File = QFileInfo(InArgs.mid(1).join(QLatin1Char(' '))).absoluteFilePath();
            if(!Window->grab().save(File)) { Console.PrintError(QStringLiteral("Could not write %1").arg(File)); }
            else                           { Console.Print(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(File))); }
            return;
        }

        const QStringList Drives = { QStringLiteral("seek"), QStringLiteral("play"), QStringLiteral("pause"), QStringLiteral("cuts"),
                                     QStringLiteral("autokey"), QStringLiteral("sound"), QStringLiteral("scrub"),
                                     QStringLiteral("collapse"), QStringLiteral("select") };
        if(Drives.contains(Verb))
        {
            SequencerWindow* Window = SequencerWindow::Find();
            SequencerTab*    Tab    = Window ? Window->CurrentTab() : nullptr;
            if(!Tab)
            {
                Console.PrintError(QStringLiteral("No sequence is open"));
                return;
            }
            const QString Argument = InArgs.mid(1).join(QLatin1Char(' '));
            const bool    bOn      = Argument == QStringLiteral("on");
            if(Verb == QStringLiteral("play"))         { Tab->SetPlaying(true); }
            else if(Verb == QStringLiteral("pause"))   { Tab->SetPlaying(false); }
            else if(Verb == QStringLiteral("cuts"))    { Tab->SetViewLocked(bOn); }
            else if(Verb == QStringLiteral("autokey")) { Tab->SetAutoKey(bOn); }
            else if(Verb == QStringLiteral("sound"))   { Tab->SetSound(bOn); }
            else if(Verb == QStringLiteral("scrub"))   { Tab->SetScrubAudio(bOn); }
            else if(Verb == QStringLiteral("collapse"))
            {
                /* Every binding and track that folds, folded (or opened). */
                std::vector<QString> Keys;
                for(const SequencerTimeline::Row& Row : Tab->GetTimeline().GetRows())
                {
                    if(!Row.CollapseKey.isEmpty()) { Keys.push_back(Row.CollapseKey); }
                }
                for(const QString& Key : Keys) { Tab->GetDocument().SetCollapsed(Key, bOn); }
            }
            else if(Verb == QStringLiteral("select"))
            {
                /* A track by name (or type), and one of its sections: shown in Details. */
                QStringList Words = InArgs.mid(1);
                bool bIndex = false;
                const int Section = Words.size() > 1 ? Words.last().toInt(&bIndex) : 0;
                if(bIndex) { Words.removeLast(); }
                const std::string Name = Words.join(QLatin1Char(' ')).toStdString();
                const Sequencer::SequenceAsset& Asset = Tab->GetDocument().GetAsset();
                SequencerTrackRef Found;
                const auto Match = [&Name](const Sequencer::SequenceTrack& InTrack)
                {
                    return InTrack.Name == Name || (InTrack.Name.empty() && Name == Sequencer::TrackTypeName(InTrack.Type));
                };
                for(size_t T = 0; T < Asset.Tracks.size() && !Found.IsValid(); ++T)
                {
                    if(Match(Asset.Tracks[T])) { Found = { -1, int(T) }; }
                }
                for(size_t B = 0; B < Asset.Bindings.size() && !Found.IsValid(); ++B)
                {
                    for(size_t T = 0; T < Asset.Bindings[B].Tracks.size() && !Found.IsValid(); ++T)
                    {
                        if(Match(Asset.Bindings[B].Tracks[T])) { Found = { int(B), int(T) }; }
                    }
                }
                const Sequencer::SequenceTrack* Track = Found.IsValid() ? Tab->GetDocument().FindTrack(Found) : nullptr;
                if(!Track)
                {
                    Console.PrintError(QStringLiteral("No track named %1").arg(QString::fromStdString(Name)));
                    return;
                }
                Tab->GetDocument().SetCurrent(Found.Binding, Found);
                if(Section >= 0 && size_t(Section) < Track->Sections.size())
                {
                    Tab->GetDocument().SetSelection({ { SequencerItem::EKind::Section, Found, 0, Section } });
                }
                else
                {
                    Tab->GetDocument().ClearSelection();
                }
                Tab->GetTimeline().ScrollToTrack(Found);
            }
            else
            {
                /* Seconds, or a marker by name. */
                const Sequencer::SequenceAsset& Asset = Tab->GetDocument().GetAsset();
                bool bNumber = false;
                const double Seconds = Argument.toDouble(&bNumber);
                const int32_t Marker = bNumber ? -1 : Asset.FindMarker(Argument.toStdString());
                if(!bNumber && Marker < 0)
                {
                    Console.PrintError(QStringLiteral("No marker named %1").arg(Argument));
                    return;
                }
                Tab->SetPlaying(false);
                Tab->GetDocument().SetPlayhead(bNumber ? Asset.SecondsToFrames(Seconds) : double(Asset.Markers[size_t(Marker)].Frame));
            }
            return;
        }
        const QString Path = QFileInfo(InArgs.join(QLatin1Char(' '))).isAbsolute()
                           ? InArgs.join(QLatin1Char(' '))
                           : QDir(EditorGraphs::GetProjectRoot()).filePath(InArgs.join(QLatin1Char(' ')));
        if(EditorSequencer::OpenSequence(Path, this))
        {
            Console.Print(QStringLiteral("Opened %1").arg(QDir::toNativeSeparators(Path)));
        }
    });

    Console.Register(QStringLiteral("particles"),
                     QStringLiteral("<path | new | close | pause | play | restart | speed <x> | move <on|off> | select <n> | screenshot <file.png>>"),
                     QStringLiteral("Open a .nmparticle in the Particle Editor, make a new one, or drive the one in front (for scripts and tests)"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: particles <path | new | close | pause | play | restart | speed <x> | move <on|off> | "
                                              "select <n> | screenshot <file.png>>"));
            return;
        }
        const QString Verb = InArgs.front();
        if(Verb == QStringLiteral("new"))   { EditorParticles::CreateParticleSystem(QString(), this); return; }
        if(Verb == QStringLiteral("close")) { EditorParticles::CloseAll(); return; }

        const QStringList Drives = { QStringLiteral("pause"), QStringLiteral("play"), QStringLiteral("restart"), QStringLiteral("speed"),
                                     QStringLiteral("move"), QStringLiteral("select"), QStringLiteral("screenshot") };
        if(Drives.contains(Verb))
        {
            ParticleEditorWindow* Window = ParticleEditorWindow::Find();
            ParticleEditorTab*    Tab    = Window ? Window->CurrentTab() : nullptr;
            if(!Tab)
            {
                Console.PrintError(QStringLiteral("No particle system is open"));
                return;
            }
            const QString Argument = InArgs.mid(1).join(QLatin1Char(' '));
            if(Verb == QStringLiteral("pause"))        { Tab->SetPaused(true); }
            else if(Verb == QStringLiteral("play"))    { Tab->SetPaused(false); }
            else if(Verb == QStringLiteral("restart")) { Tab->Restart(); }
            else if(Verb == QStringLiteral("speed"))   { Tab->SetSpeed(Argument.toFloat()); }
            else if(Verb == QStringLiteral("move"))    { Tab->SetMoving(Argument == QStringLiteral("on")); }
            else if(Verb == QStringLiteral("select"))  { Tab->GetDocument().SetSelected(Argument.toInt()); }
            else
            {
                const uint64_t Request = Argument.isEmpty() ? 0 : Tab->RequestCapture();
                if(Request == 0)
                {
                    Console.PrintError(QStringLiteral("Usage: particles screenshot <file.png>, with a system open and drawn"));
                    return;
                }
                SaveCaptureWhenReady(Request, QFileInfo(Argument).absoluteFilePath());
            }
            return;
        }
        const QString Path = QFileInfo(InArgs.join(QLatin1Char(' '))).isAbsolute()
                           ? InArgs.join(QLatin1Char(' '))
                           : QDir(EditorGraphs::GetProjectRoot()).filePath(InArgs.join(QLatin1Char(' ')));
        if(EditorParticles::OpenParticleSystem(Path, this))
        {
            Console.Print(QStringLiteral("Opened %1").arg(QDir::toNativeSeparators(Path)));
        }
    });

    /* The mesh viewer, and its animation preview, for scripts and tests. */
    Console.Register(QStringLiteral("mesh"),
                     QStringLiteral("<path> | play | pause | seek <s> | param <name> <value> | bones <on|off> | windowshot <file.png>"),
                     QStringLiteral("Open a .nmmesh, .nmanim or .nmanimstate in the mesh viewer, or drive the one in front"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: mesh <path> | play | pause | seek <s> | param <name> <value> | bones <on|off> | windowshot <file.png>"));
            return;
        }
        const QString Verb = InArgs.front();
        const QStringList Drives = { QStringLiteral("play"), QStringLiteral("pause"), QStringLiteral("seek"), QStringLiteral("param"),
                                     QStringLiteral("bones"), QStringLiteral("windowshot") };
        if(Drives.contains(Verb))
        {
            MeshViewerWindow* Viewer = MeshViewerWindow::FindFront();
            if(!Viewer)
            {
                Console.PrintError(QStringLiteral("No mesh viewer is open"));
                return;
            }
            AnimPreviewWidget* Anim = Viewer->GetAnimPreview();
            if(Verb == QStringLiteral("windowshot"))
            {
                const QString File = QFileInfo(InArgs.mid(1).join(QLatin1Char(' '))).absoluteFilePath();
                if(!Viewer->grab().save(File)) { Console.PrintError(QStringLiteral("Could not write %1").arg(File)); }
                else                           { Console.Print(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(File))); }
                return;
            }
            if(!Anim)
            {
                Console.PrintError(QStringLiteral("The mesh in front is not skinned"));
                return;
            }
            if(Verb == QStringLiteral("play"))       { Anim->SetPlaying(true); }
            else if(Verb == QStringLiteral("pause")) { Anim->SetPlaying(false); }
            else if(Verb == QStringLiteral("seek"))  { Anim->SetPlaying(false); Anim->Seek(InArgs.value(1).toFloat()); }
            else if(Verb == QStringLiteral("bones")) { Anim->SetBonesShown(InArgs.value(1) != QStringLiteral("off")); }
            else
            {
                if(!Anim->GetInstance().SetParameter(InArgs.value(1).toStdString(), InArgs.value(2).toFloat()))
                {
                    Console.PrintError(QStringLiteral("The anim state playing has no parameter %1").arg(InArgs.value(1)));
                }
                if(!Anim->IsPlaying()) { Anim->Repose(); }
            }
            return;
        }
        const QString Joined = InArgs.join(QLatin1Char(' '));
        const QString Path = QFileInfo(Joined).isAbsolute() ? Joined : QDir(EditorGraphs::GetProjectRoot()).filePath(Joined);
        const bool bAnimation = Path.endsWith(QStringLiteral(".nmanim"), Qt::CaseInsensitive)
                             || Path.endsWith(QStringLiteral(".nmanimstate"), Qt::CaseInsensitive);
        if(bAnimation ? MeshViewerWindow::OpenAnimation(Path, this) != nullptr : MeshViewerWindow::Open(Path, this) != nullptr)
        {
            Console.Print(QStringLiteral("Opened %1").arg(QDir::toNativeSeparators(Path)));
        }
    });

    /* The anim state editor, for scripts and tests. */
    Console.Register(QStringLiteral("animstate"), QStringLiteral("<path> | new | save | windowshot <file.png>"),
                     QStringLiteral("Open an .nmanimstate in the anim state editor, make one, or drive the one in front"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: animstate <path> | new | save | windowshot <file.png>"));
            return;
        }
        const QString Verb = InArgs.front();
        if(Verb == QStringLiteral("new")) { EditorAnimation::CreateAnimState(QString(), this); return; }
        if(Verb == QStringLiteral("save") || Verb == QStringLiteral("windowshot"))
        {
            AnimStateEditorWindow* Editor = AnimStateEditorWindow::FindFront();
            if(!Editor)
            {
                Console.PrintError(QStringLiteral("No anim state is open"));
                return;
            }
            if(Verb == QStringLiteral("save")) { Editor->Save(); return; }
            const QString File = QFileInfo(InArgs.mid(1).join(QLatin1Char(' '))).absoluteFilePath();
            if(!Editor->grab().save(File)) { Console.PrintError(QStringLiteral("Could not write %1").arg(File)); }
            else                           { Console.Print(QStringLiteral("Saved %1").arg(QDir::toNativeSeparators(File))); }
            return;
        }
        const QString Joined = InArgs.join(QLatin1Char(' '));
        const QString Path = QFileInfo(Joined).isAbsolute() ? Joined : QDir(EditorGraphs::GetProjectRoot()).filePath(Joined);
        if(AnimStateEditorWindow::Open(Path, this)) { Console.Print(QStringLiteral("Opened %1").arg(QDir::toNativeSeparators(Path))); }
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
        Document->NewWorld();
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
            Level.NewWorld();
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
     * (Stats.Frame), "stat physics" the playing level's simulation
     * (Stats.Physics, GameSession), "stat audio" the playing level's sound
     * (Stats.Audio), "stat streaming" texture streaming (Stats.Streaming);
     * "stat none" hides them all. */
    Console.Register(QStringLiteral("stat"), QStringLiteral("unit | frame | physics | audio | streaming | none"),
                     QStringLiteral("Show thread times (unit), each render stage's GPU time (frame), the "
                                    "physics simulation (physics), the sound (audio) while playing, or texture streaming (streaming) over the viewport"),
                     [this](const QStringList& InArgs)
    {
        static const char* const Names[] = { "unit", "frame", "physics", "audio", "streaming" };
        static const char* const Vars[]  = { "Unit", "Frame", "Physics", "Audio", "Streaming" };
        const QString Which = InArgs.isEmpty() ? QString() : InArgs.front().toLower();
        if(Which == QStringLiteral("none"))
        {
            for(const char* Var : Vars)
            {
                if(EngineCore::ConfigVar* Found = EngineCore::ConfigFindVar("Stats", Var)) { Found->SetBool(false); }
            }
            Console.Print(QStringLiteral("stats hidden"));
            return;
        }
        for(size_t Index = 0; Index < std::size(Names); ++Index)
        {
            if(Which != QLatin1String(Names[Index])) { continue; }
            EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Stats", Vars[Index]);
            if(!Var) { break; }
            Var->SetBool(!Var->GetBool());
            Console.Print(QStringLiteral("stat %1: %2").arg(Which, Var->GetBool() ? QStringLiteral("on") : QStringLiteral("off")));
            if(Which == QStringLiteral("physics") && Var->GetBool() && !Context.IsPlaying())
            {
                Console.Print(QStringLiteral("(shown while playing: the edited world is not simulated)"));
            }
            if(Which == QStringLiteral("audio") && Var->GetBool() && !Context.IsPlaying())
            {
                Console.Print(QStringLiteral("(shown while playing: the edited world is not heard)"));
            }
            return;
        }
        Console.PrintError(QStringLiteral("Usage: stat unit | frame | physics | audio | none"));
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

    Console.Register(QStringLiteral("play.client"), QString(),
                     QStringLiteral("Open another client of the networked run in a window of its own (Play > New Client Window)"),
                     [this](const QStringList&)
    {
        if(Context.LaunchPlayClientWindow()) { Console.Print(QStringLiteral("Client window started")); }
        else                                 { Console.PrintError(QStringLiteral("No client window -- see above")); }
    });


    /* The collaboration session (EditorCollabSession.h; the Session dock does the same). */
    Console.Register(QStringLiteral("collab.connect"), QStringLiteral("host[:port] [name] [password]"),
                     QStringLiteral("Join the collaboration session on NimbleEditorServer at host (UDP 7788 by default)"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.isEmpty())
        {
            Console.PrintError(QStringLiteral("Usage: collab.connect host[:port] [name] [password]"));
            return;
        }
        CollabSession->Connect(InArgs.value(0), InArgs.value(1), InArgs.value(2));
        Console.Print(QStringLiteral("Joining %1 -- the notification says how it went").arg(InArgs.value(0)));
    });
    Console.Register(QStringLiteral("collab.leave"), QString(), QStringLiteral("Leave the collaboration session"),
                     [this](const QStringList&)
    {
        CollabSession->Leave();
    });
    Console.Register(QStringLiteral("collab.play"), QStringLiteral("[stop]"),
                     QStringLiteral("Play Together: start a shared run of the open level on the session server (Play > Play Together), "
                                    "or stop the one you started"),
                     [this](const QStringList& InArgs)
    {
        if(InArgs.value(0) == QStringLiteral("stop"))
        {
            CollabSession->StopPlayTogether();
            return;
        }
        CollabSession->RequestPlayTogether(CurrentPath.isEmpty() ? QString() : EditorContent::ToRuntimePath(CurrentPath));
    });
    Console.Register(QStringLiteral("collab.join"), QString(), QStringLiteral("Join the Play Together run that's going"),
                     [this](const QStringList&)
    {
        if(!CollabSession->IsRunActive())
        {
            Console.Print(QStringLiteral("No run is going"));
            return;
        }
        if(!CollabSession->GetJoinBlocker().isEmpty())
        {
            Console.PrintError(CollabSession->GetJoinBlocker());
            return;
        }
        CollabSession->JoinPlayTogether();
    });
    Console.Register(QStringLiteral("collab.status"), QString(), QStringLiteral("Who is in the collaboration session, and what they're doing"),
                     [this](const QStringList&)
    {
        if(!CollabSession->IsJoined())
        {
            Console.Print(CollabSession->GetState() == ::Collab::CollabClient::EState::Connecting ? QStringLiteral("Joining %1").arg(CollabSession->GetServerAddress())
                                                                                         : QStringLiteral("Not in a session"));
            return;
        }
        Console.Print(QStringLiteral("In the session at %1:").arg(CollabSession->GetServerAddress()));
        for(const QString& Line : CollabSession->DescribeRoster())
        {
            Console.Print(QStringLiteral("  ") + Line);
        }
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
        Level.NewWorld();
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
    /* Open prefabs, particle systems and graphs first: an unsaved one may cancel the close.
     * Prefabs and particle systems before the Renderer stops: their viewports' swapchains go now. */
    if(!EditorSequencer::CloseAll() || !EditorPrefabs::CloseAllPrefabs() || !EditorParticles::CloseAll() || !EditorUI::CloseAll()
       || !EditorGraphs::CloseAllGraphs())
    {
        InEvent->ignore();
        return;
    }

    CORE_LOG(std::string("[Editor] closing -- ") + (InEvent->spontaneous() ? "requested by the window system" : "requested by the editor"))

    DocumentationWindow::CloseIfOpen();

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

    /* What the session sees of this editor. */
    if(CollabSession)
    {
        CollabSession->SetOpenLevel(CurrentPath);
        CollabSession->SetLevelUnsaved(!Level.GetHistory().isClean());
        CollabSession->SetPlaying(Context.IsPlaying());
        if(!Context.IsPlaying())
        {
            CollabSession->SetPlayingTogether(false);   // Stop Play leaves the shared run
        }
    }
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

void EditorMainWindow::CheckBake()
{
    const GameCore::WorldLightingSettings& Lighting = Level.GetWorldSettings().Lighting;
    const std::shared_ptr<const RenderCore::BakedLightingData> Bake =
        Lighting.bBakedLighting ? RenderCore::BakedLightingCache::Find(Lighting.BakedData) : nullptr;
    if(!Bake || &Level.GetWorld() != &Level.GetEditedWorld())
    {
        BakeLabel->clear();
        return;
    }
    BakeLabel->setText(LightBake::HashWorld(Level.GetEditedWorld()) == Bake->InputHash ? QString()
                                                                                     : QStringLiteral("Lighting needs to be rebuilt"));
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


void EditorMainWindow::SaveCaptureWhenReady(const uint64_t InRequest, const QString& InPath, const bool bInWholeWindow)
{
    const uint64_t Request = InRequest;
    const QString  Path    = InPath;

    /* Answered after a frame renders; a few seconds, then given up. */
    auto* Poll  = new QTimer(this);
    auto  Tries = std::make_shared<int>(0);
    connect(Poll, &QTimer::timeout, this, [this, Poll, Request, Tries, Path, bInWholeWindow]()
    {
        QImage  Image;
        QString Error;
        if(Context.TakeMaterialPreview(Request, Image, Error))
        {
            Poll->deleteLater();
            if(bInWholeWindow && !Image.isNull() && Viewport)
            {
                QPixmap Window = grab();
                QPainter Painter(&Window);
                Painter.drawImage(QRect(Viewport->mapTo(this, QPoint(0, 0)), Viewport->size()), Image);
                Painter.end();
                Image = Window.toImage();
            }
            if(Image.isNull() || !Image.save(Path))
            {
                Console.PrintError(QStringLiteral("No screenshot: %1").arg(Image.isNull() ? Error : QStringLiteral("cannot write ") + Path));
                return;
            }
            Console.Print(QStringLiteral("Saved %1 (%2 x %3)").arg(Path).arg(Image.width()).arg(Image.height()));
            return;
        }
        if(++*Tries > 250)
        {
            Poll->deleteLater();
            Console.PrintError(QStringLiteral("No frame rendered for the screenshot (is the viewport visible?)"));
        }
    });
    Poll->start(20);
}

#endif // WITH_EDITOR
