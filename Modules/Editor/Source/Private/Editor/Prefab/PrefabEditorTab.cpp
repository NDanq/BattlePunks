#if WITH_EDITOR

#include "Private/Editor/EditorEditActions.h"
#include "Private/Editor/Prefab/PrefabEditorTab.h"

#include <algorithm>
#include <cstdio>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemory.h"
#include "Public/Editor/DetailsPanel.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/OutlinerPanel.h"
#include "Public/Editor/ViewportPanel.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/NodeGraph/Schema.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Private/Editor/Prefab/PrefabScriptPanel.h"

namespace
{
    /* The Tick Mode box's order. */
    const GameCore::EPrefabTickMode TickModes[] = {
        GameCore::EPrefabTickMode::Tick, GameCore::EPrefabTickMode::EventsOnly, GameCore::EPrefabTickMode::Off };

    QString GraphScriptSuffix()
    {
        return QStringLiteral(".script") + QString::fromLatin1(NodeGraph::GRAPH_EXTENSION);
    }
}

PrefabEditorTab::PrefabEditorTab(EditorContext& InContext, QWidget* InParent)
    : QMainWindow(InParent)
    , Context(InContext)
{
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);

    Document = new EditorDocument(this);
    Document->SetSingleRoot(true);

    ViewportArea = new ViewportPanel(Context, *Document, this);
    setCentralWidget(ViewportArea);

    BuildDocks();
    BuildToolBar();
    BuildActions();

    connect(Document, &EditorDocument::HistoryChanged, this, &PrefabEditorTab::TitleChanged);
    connect(ScriptPanel, &PrefabScriptPanel::ModifiedChanged, this, &PrefabEditorTab::TitleChanged);
    UpdateSimulationControls();
}

PrefabEditorTab::~PrefabEditorTab()
{
    StopSimulation();

    /* The panels first, then the document they show: the viewport releases
     * its swapchain while its native window exists, and the document's scene
     * goes with the document. */
    delete ViewportArea;
    delete Hierarchy;
    delete Details;
    delete Document;
}

void PrefabEditorTab::BuildDocks()
{
    auto* HierarchyDock = new QDockWidget(QStringLiteral("Hierarchy"), this);
    HierarchyDock->setObjectName(QStringLiteral("PrefabHierarchyDock"));
    Hierarchy = new OutlinerPanel(*Document, HierarchyDock);
    HierarchyDock->setWidget(Hierarchy);
    addDockWidget(Qt::LeftDockWidgetArea, HierarchyDock);

    auto* DetailsDock = new QDockWidget(QStringLiteral("Details"), this);
    DetailsDock->setObjectName(QStringLiteral("PrefabDetailsDock"));
    Details = new DetailsPanel(*Document, DetailsDock);
    DetailsDock->setWidget(Details);
    addDockWidget(Qt::RightDockWidgetArea, DetailsDock);

    auto* ScriptDock = new QDockWidget(QStringLiteral("Script"), this);
    ScriptDock->setObjectName(QStringLiteral("PrefabScriptDock"));
    ScriptPanel = new PrefabScriptPanel(ScriptDock);
    ScriptDock->setWidget(ScriptPanel);
    addDockWidget(Qt::BottomDockWidgetArea, ScriptDock);

    /* Room for property editors and a few lines of script; the viewport takes the rest. */
    resizeDocks({ HierarchyDock, DetailsDock }, { 260, 340 }, Qt::Horizontal);
    resizeDocks({ ScriptDock }, { 240 }, Qt::Vertical);
}

void PrefabEditorTab::BuildToolBar()
{
    QToolBar* Bar = addToolBar(QStringLiteral("Prefab"));
    Bar->setObjectName(QStringLiteral("PrefabToolBar"));
    Bar->setMovable(false);

    SaveAction = Bar->addAction(QStringLiteral("Save"), this, &PrefabEditorTab::Save);
    SaveAction->setToolTip(QStringLiteral("Save the prefab and its Lua script (Ctrl+S)"));
    Bar->addSeparator();

    SimulateAction = Bar->addAction(QStringLiteral("Simulate"), this, &PrefabEditorTab::StartSimulation);
    SimulateAction->setToolTip(QStringLiteral("Run the prefab and its script in a World of its own; Stop drops it"));
    StopAction = Bar->addAction(QStringLiteral("Stop"), this, &PrefabEditorTab::StopSimulation);
    SimulateLabel = new QLabel(Bar);
    SimulateLabel->setStyleSheet(QStringLiteral("color: #4caf50; padding: 0 6px;"));
    Bar->addWidget(SimulateLabel);
    Bar->addSeparator();

    Bar->addWidget(new QLabel(QStringLiteral(" Tick Mode "), Bar));
    TickModeBox = new QComboBox(Bar);
    for(const GameCore::EPrefabTickMode Mode : TickModes)
    {
        TickModeBox->addItem(QString::fromLatin1(GameCore::PrefabTickModeName(Mode)));
    }
    TickModeBox->setToolTip(QStringLiteral("Tick: phase functions run. EventsOnly: only events. Off: no script, static geometry."));
    connect(TickModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](const int InIndex)
    {
        if(bSyncing || InIndex < 0) { return; }
        Asset.SetTickMode(TickModes[InIndex]);
        MarkSettingsDirty();
    });
    Bar->addWidget(TickModeBox);
    Bar->addSeparator();

    Bar->addWidget(new QLabel(QStringLiteral(" Script "), Bar));
    ScriptBox = new QComboBox(Bar);
    ScriptBox->addItems({ QStringLiteral("None"), QStringLiteral("Lua"), QStringLiteral("Graph") });
    ScriptBox->setToolTip(QStringLiteral("Lua or Graph makes <Prefab>.lua or <Prefab>.script.nmgraph beside the prefab, if missing"));
    connect(ScriptBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](const int InIndex)
    {
        if(!bSyncing && InIndex >= 0) { SetScriptKind(EScriptKind(InIndex)); }
    });
    Bar->addWidget(ScriptBox);
    ChooseAction = Bar->addAction(QStringLiteral("Choose..."), this, &PrefabEditorTab::ChooseScript);
    ChooseAction->setToolTip(QStringLiteral("Use an existing Lua script or script graph from the project"));
}

void PrefabEditorTab::BuildActions()
{
    /* Shortcuts for this tab only: the main window has its own. */
    const auto Add = [this](QMenu* InMenu, const QString& InText, const QList<QKeySequence>& InKeys, auto&& InSlot)
    {
        QAction* Action = InMenu->addAction(InText);
        Action->setShortcuts(InKeys);
        Action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(Action, &QAction::triggered, this, InSlot);
        addAction(Action);
        return Action;
    };

    QMenu* File = menuBar()->addMenu(QStringLiteral("&File"));
    Add(File, QStringLiteral("&Save Prefab"), { QKeySequence::Save }, [this]() { Save(); });
    Add(File, QStringLiteral("&Close Prefab"), { QKeySequence::Close }, [this]() { emit CloseRequested(); });

    QMenu* Edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction* Undo = Add(Edit, QStringLiteral("&Undo"), { QKeySequence::Undo }, [this]() { Document->Undo(); });
    QAction* Redo = Add(Edit, QStringLiteral("&Redo"), { QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y) },
                        [this]() { Document->Redo(); });
    /* Cut/Copy/Paste/Duplicate/Rename/Delete: keys owned by this tab's
     * viewport and Outliner (EditorEditActions.h), shown here. */
    Edit->addSeparator();
    const std::vector<QAction*> EntityItems =
        EditorEditActions::AddToMenu(Edit, *Document, [this]() { EditorEditActions::PromptRename(this, *Document); });
    connect(Edit, &QMenu::aboutToShow, this, [this, EntityItems]() { EditorEditActions::Refresh(EntityItems, *Document); });

    const auto UpdateHistory = [this, Undo, Redo]()
    {
        Undo->setEnabled(Document->CanUndo());
        Redo->setEnabled(Document->CanRedo());
        Undo->setText(QStringLiteral("&Undo %1").arg(Document->GetHistory().undoText()));
        Redo->setText(QStringLiteral("&Redo %1").arg(Document->GetHistory().redoText()));
    };
    connect(Document, &EditorDocument::HistoryChanged, this, UpdateHistory);
    UpdateHistory();
}

// === Loading and saving ========================================================

bool PrefabEditorTab::Load(const QString& InPath)
{
    if(!Document->LoadPrefab(InPath, Asset))
    {
        return false;
    }
    bNative = EditorPrefabs::IsNativePrefabPath(InPath);
    Path    = bNative ? InPath : QFileInfo(InPath).absoluteFilePath();
    bSettingsDirty = false;
    if(bNative)
    {
        SaveAction->setText(QStringLiteral("Save As Asset..."));
        SaveAction->setToolTip(QStringLiteral("This prefab is defined in code. Save its layout as an editable .nmprefab "
                                              "that keeps the code's callbacks"));
        UpdateSimulationControls();
    }
    SyncSettingsControls();

    const std::vector<GameCore::EntityID> Roots = Document->GetRootEntities();
    if(!Roots.empty()) { Document->SetSelection(Roots.front()); }

    /* Once the viewport has its size. */
    QTimer::singleShot(0, this, [this]() { ViewportArea->GetViewport()->FrameAll(); });
    emit TitleChanged();
    return true;
}

QString PrefabEditorTab::GetTabTitle() const
{
    return QFileInfo(Path).fileName() + (IsDirty() ? QStringLiteral("*") : QString());
}

bool PrefabEditorTab::IsDirty() const
{
    return bSettingsDirty || !Document->GetHistory().isClean() || ScriptPanel->IsModified();
}

bool PrefabEditorTab::Save()
{
    if(IsSimulating())
    {
        StopSimulation();
    }
    if(bNative)
    {
        return SaveAsAsset();
    }

    QString Error;
    if(!Document->SavePrefab(Path, Asset, Error) || !ScriptPanel->Save(Error))
    {
        QMessageBox::warning(this, QStringLiteral("Save Prefab"), Error);
        return false;
    }
    bSettingsDirty = false;

    /* Beside the exe, and in the registry, so the next Play spawns this. */
    EditorPrefabs::Stage(Path);
    const QString Runtime = EditorPrefabs::ToRuntimePath(Path);
    if(!Runtime.isEmpty())
    {
        GameCore::ReloadPrefab(Runtime.toUtf8().constData());

        /* Instances placed in the level pick the change up, overrides kept. */
        const int Refreshed = Context.GetLevel().RefreshPrefabInstances(Runtime);
        if(Refreshed > 0)
        {
            CORE_LOG("[Prefab] refreshed " + std::to_string(Refreshed) + " instance(s) in the level")
        }
    }

    CORE_LOG("[Prefab] saved " + std::string(Path.toUtf8().constData()))
    emit TitleChanged();
    return true;
}

bool PrefabEditorTab::SaveAsAsset()
{
    if(IsSimulating())
    {
        StopSimulation();
    }
    const QString Name = Path.section(QLatin1Char('.'), -1);   // "Sandbox.Marker" -> "Marker"
    QDir().mkpath(EditorPrefabs::GetDefaultFolder());
    const QString File = QFileDialog::getSaveFileName(this, QStringLiteral("Save As Asset"),
                                                      QDir(EditorPrefabs::GetDefaultFolder()).filePath(Name + QStringLiteral(".nmprefab")),
                                                      QStringLiteral("Prefabs (*.nmprefab)"));
    if(File.isEmpty())
    {
        return false;
    }
    if(EditorPrefabs::ToRuntimePath(File).isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Save As Asset"), QStringLiteral("Save the prefab inside the project."));
        return false;
    }

    /* A copy: this tab stays the native prefab. Its callbacks come along by name. */
    GameCore::PrefabAsset Fork = Asset;
    Fork.SetName(QFileInfo(File).completeBaseName().toUtf8().constData());
    QString Error;
    if(!Document->SavePrefab(File, Fork, Error))
    {
        QMessageBox::warning(this, QStringLiteral("Save As Asset"), Error);
        return false;
    }
    EditorPrefabs::Stage(File);
    emit TitleChanged();
    EditorPrefabs::OpenPrefab(File, window());
    return true;
}

bool PrefabEditorTab::ConfirmClose()
{
    StopSimulation();
    if(!IsDirty())
    {
        return true;
    }
    const QMessageBox::StandardButton Answer = QMessageBox::question(
        this, QStringLiteral("Close Prefab"), QStringLiteral("Save changes to %1?").arg(QFileInfo(Path).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if(Answer == QMessageBox::Save)    { return Save(); }
    return Answer == QMessageBox::Discard;
}

// === Settings ==================================================================

void PrefabEditorTab::SyncSettingsControls()
{
    bSyncing = true;
    for(int Index = 0; Index < int(std::size(TickModes)); ++Index)
    {
        if(TickModes[Index] == Asset.GetTickMode()) { TickModeBox->setCurrentIndex(Index); }
    }

    const QString Script = QString::fromUtf8(Asset.GetScriptPath());
    const EScriptKind Kind = Script.isEmpty()                                       ? EScriptKind::None
                           : Script.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive) ? EScriptKind::Lua
                                                                                    : EScriptKind::Graph;
    ScriptBox->setCurrentIndex(int(Kind));
    ScriptPanel->SetScript(Script);
    bSyncing = false;
}

void PrefabEditorTab::MarkSettingsDirty()
{
    bSettingsDirty = true;
    emit TitleChanged();
}

void PrefabEditorTab::SetScriptKind(const EScriptKind InKind)
{
    const QString Current = QString::fromUtf8(Asset.GetScriptPath());

    /* Switching scripts drops the text on screen: offer to keep it. */
    if(ScriptPanel->IsModified())
    {
        const QMessageBox::StandardButton Answer = QMessageBox::question(
            this, QStringLiteral("Change Script"), QStringLiteral("Save changes to %1 first?").arg(Current),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        QString Error;
        if(Answer == QMessageBox::Cancel || (Answer == QMessageBox::Save && !ScriptPanel->Save(Error)))
        {
            if(!Error.isEmpty()) { QMessageBox::warning(this, QStringLiteral("Change Script"), Error); }
            SyncSettingsControls();
            return;
        }
    }

    if(InKind == EScriptKind::None)
    {
        SetScriptPath(QString());
        return;
    }

    /* Keep a script of the chosen kind it already has. */
    const bool bIsLua = Current.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive);
    if(!Current.isEmpty() && bIsLua == (InKind == EScriptKind::Lua))
    {
        SyncSettingsControls();
        return;
    }

    /* <Prefab>.lua or <Prefab>.script.nmgraph beside the prefab. */
    const QFileInfo Prefab(Path);
    const QString   Stem = Prefab.completeBaseName();
    const QString   File = Prefab.absoluteDir().filePath(Stem + (InKind == EScriptKind::Lua ? QStringLiteral(".lua") : GraphScriptSuffix()));
    const QString   Runtime = EditorPrefabs::ToRuntimePath(File);
    if(Runtime.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Script"), QStringLiteral("The prefab is outside the project, so its script would be too."));
        SyncSettingsControls();
        return;
    }

    if(!QFileInfo::exists(File))
    {
        QString Error;
        if(InKind == EScriptKind::Lua)
        {
            QFile Out(File);
            if(!Out.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                Error = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(File));
            }
            else
            {
                Out.write(PrefabScriptPanel::StarterLua(Stem).toUtf8());
                Out.close();
                EditorPrefabs::Stage(File);
            }
        }
        else if(const NodeGraph::Schema* Schema = NodeGraph::FindSchema("Script"))
        {
            EditorGraphs::CreateGraphFile(*Schema, File, Error);
        }
        else
        {
            Error = QStringLiteral("There is no Script graph schema");
        }
        if(!Error.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("Script"), Error);
            SyncSettingsControls();
            return;
        }
    }
    SetScriptPath(Runtime);
}

void PrefabEditorTab::ChooseScript()
{
    const QString Picked = QFileDialog::getOpenFileName(this, QStringLiteral("Choose Script"), QFileInfo(Path).absolutePath(),
                                                        QStringLiteral("Scripts (*.lua *%1)").arg(GraphScriptSuffix()));
    if(Picked.isEmpty())
    {
        return;
    }
    const QString Runtime = EditorPrefabs::ToRuntimePath(Picked);
    if(Runtime.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Choose Script"), QStringLiteral("Choose a script inside the project."));
        return;
    }
    SetScriptPath(Runtime);
}

void PrefabEditorTab::SetScriptPath(const QString& InRuntimePath)
{
    if(InRuntimePath == QString::fromUtf8(Asset.GetScriptPath()))
    {
        SyncSettingsControls();
        return;
    }
    Asset.SetScriptPath(InRuntimePath.toUtf8().constData());
    SyncSettingsControls();
    MarkSettingsDirty();
}

// === Simulate ==================================================================

namespace
{
    /* LowestPoint's walk: every entity's box (its Bounds, else its Render
     * mesh's) through its world pose. */
    struct LowestWalk
    {
        GameCore::World* World  = nullptr;
        double           Lowest = 0.0;
        bool             bFound = false;
    };

    /* The lowest point anything in InWorld reaches, or 0 if nothing has a box. */
    double LowestPoint(GameCore::World& InWorld)
    {
        LowestWalk Walk;
        Walk.World = &InWorld;
        InWorld.ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
        {
            auto& Walk = *static_cast<LowestWalk*>(InUserData);
            const GameCore::Entity Entity = Walk.World->GetEntity(InEntity);
            GameCore::AABB Local;
            if(const auto* Bounds = Entity.GetComponent<GameCore::BoundsComponent>())
            {
                Local = Bounds->Local;
            }
            else
            {
                const GameCore::CollisionMeshProvider* Provider = GameCore::GetCollisionMeshProvider();
                const char* Mesh = Provider ? Provider->GetMeshPath(*Walk.World, InEntity, Provider->UserData) : nullptr;
                if(!Mesh || !Provider->GetMeshBounds(Mesh, Local, Provider->UserData)) { return; }
            }
            if(Local.IsEmpty()) { return; }

            const GameCore::TransformPose Pose = Entity.GetWorldPose();
            const GameCore::Rotation3 Turn = GameCore::TransformMath::EulerToRotation(Pose.Rotation);
            for(int Corner = 0; Corner < 8; ++Corner)
            {
                const EngineCore::Vector3 Point(double((Corner & 1) ? Local.Max.GetX() : Local.Min.GetX()) * Pose.Scale.GetX(),
                                                double((Corner & 2) ? Local.Max.GetY() : Local.Min.GetY()) * Pose.Scale.GetY(),
                                                double((Corner & 4) ? Local.Max.GetZ() : Local.Min.GetZ()) * Pose.Scale.GetZ());
                const double Y = double((Pose.Position + Turn * Point).GetY());
                Walk.Lowest = Walk.bFound ? std::min(Walk.Lowest, Y) : Y;
                Walk.bFound = true;
            }
        }, &Walk);
        return Walk.bFound ? Walk.Lowest : 0.0;
    }

    /* Something for the prefab's bodies to land on: a large static Plane at
     * the prefab's lowest point, drawn so it is clear what they rest on.
     * Simulate's World only: never part of the prefab. */
    void SpawnSimulationGround(GameCore::World& InWorld)
    {
        const GameCore::Entity Ground = InWorld.CreateEntity("Simulation Ground");
        GameCore::TransformComponent Transform;
        Transform.Position = EngineCore::Vector3(0.0, LowestPoint(InWorld), 0.0);
        Transform.Scale    = EngineCore::Vector3(4000.0, 1.0, 4000.0);
        Ground.AddComponent(Transform);

        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Plane.nmmesh");
        Render.bCastShadows = 0;
        Ground.AddComponent(Render);

        GameCore::RigidBodyComponent Body;
        Body.Motion = GameCore::RBM_Static;
        Ground.AddComponent(Body);
        Ground.SetMobility(GameCore::EMobility::Static);
    }

    bool HasRigidBodies(GameCore::World& InWorld)
    {
        bool bAny = false;
        InWorld.Each<GameCore::RigidBodyComponent>([&bAny](GameCore::Entity, GameCore::RigidBodyComponent&) { bAny = true; });
        return bAny;
    }
}

void PrefabEditorTab::StartSimulation()
{
    if(IsSimulating())
    {
        return;
    }
    if(!Context.IsRendererReady())
    {
        QMessageBox::warning(this, QStringLiteral("Simulate"), QStringLiteral("The renderer is not running."));
        return;
    }
    const std::vector<GameCore::EntityID> Roots = Document->GetRootEntities();
    if(Roots.size() != 1)
    {
        QMessageBox::warning(this, QStringLiteral("Simulate"), QStringLiteral("A prefab simulates with exactly one root entity."));
        return;
    }

    /* The prefab as it is on screen, script included, saved or not. */
    ScriptPanel->StageForRun();
    GameCore::PrefabAsset Snapshot = Asset;
    if(!GameCore::CapturePrefab(Document->GetEditedWorld(), Roots.front(), Snapshot))
    {
        return;
    }

    SimWorld   = EngineCore::Memory::Alloc<GameCore::World>();
    SimScripts = new GameCore::ScriptEngine();
    if(SimScripts->Initialize())
    {
        SimScripts->BindWorld(SimWorld);
    }
    SimWorld->SpawnPrefab(Snapshot);

    /* Rigid Bodies simulate here as they would in a level, on a ground of
     * their own. Without the physics module they stay put. */
    bSimPhysics = false;
    if(HasRigidBodies(*SimWorld))
    {
        if(NimbleEngine::IsPhysicsAvailable())
        {
            SpawnSimulationGround(*SimWorld);
            bSimPhysics = NimbleEngine::StartSimulating(*SimWorld);
        }
        else
        {
            CORE_LOG_WARN("[Prefab] its Rigid Bodies will not move: JoltPhysics is not compiled in (Project Settings > Modules)")
        }
    }

    Document->BorrowWorld(SimWorld);
    Context.BindDocumentScene(*Document);
    connect(&Context, &EditorContext::BeforeFrame, this, [this](const double InDeltaSeconds)
    {
        if(SimWorld) { SimWorld->Tick(float(InDeltaSeconds)); }
    });

    CORE_LOG("[Prefab] simulating " + std::string(QFileInfo(Path).fileName().toUtf8().constData())
             + (bSimPhysics ? " with physics" : ""))
    UpdateSimulationControls();
}

void PrefabEditorTab::StopSimulation()
{
    if(!IsSimulating())
    {
        return;
    }
    disconnect(&Context, &EditorContext::BeforeFrame, this, nullptr);

    /* Let go of the simulation's World before it is freed: the document's
     * hooks, then the scene's proxies. */
    Document->UnhookBorrowedWorld();
    Context.UnbindDocumentScene(*Document);
    Document->DropBorrowedWorld();

    if(bSimPhysics)
    {
        NimbleEngine::StopSimulating(*SimWorld);
        bSimPhysics = false;
    }
    SimScripts->UnbindWorld();
    SimScripts->Shutdown();
    delete SimScripts;
    SimScripts = nullptr;
    EngineCore::Memory::Dealloc(SimWorld);
    SimWorld = nullptr;

    Document->ReturnToEditedWorld();
    Context.BindDocumentScene(*Document);
    UpdateSimulationControls();
}

void PrefabEditorTab::UpdateSimulationControls()
{
    const bool bSimulating = IsSimulating();
    SimulateAction->setEnabled(!bSimulating);
    StopAction->setEnabled(bSimulating);
    /* A native prefab's script and tick mode are its code's. */
    TickModeBox->setEnabled(!bSimulating && !bNative);
    ScriptBox->setEnabled(!bSimulating && !bNative);
    ChooseAction->setEnabled(!bSimulating && !bNative);
    SimulateLabel->setText(bSimulating ? QStringLiteral("Simulating") : QString());
}

#endif // WITH_EDITOR
