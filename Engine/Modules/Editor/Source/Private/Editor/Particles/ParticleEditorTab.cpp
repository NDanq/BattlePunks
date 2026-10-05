#if WITH_EDITOR

#include "Private/Editor/Particles/ParticleEditorTab.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

#include "Public/Editor/EditorCamera.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/ViewportPanel.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/DirectionalLightComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/Components/SkyLightComponent.h"
#include "Public/RenderCore/ParticleSystemAsset.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Particles/ParticleDocument.h"
#include "Private/Editor/Particles/ParticleEmitterPanels.h"

using RenderCore::ParticleEmitterDesc;
using RenderCore::ParticleSystemDesc;

namespace
{
    constexpr const char* FLOOR_MESH   = "Engine/Content/Meshes/Plane.nmmesh";
    constexpr double      FLOOR_SIZE   = 4000.0;
    constexpr double      MOVE_SPEED   = 1.2;     // radians a second round Move Emitter's circle

    /* The Speed box's choices. */
    const float Speeds[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };

    int& PreviewCounter()
    {
        static int Count = 0;
        return Count;
    }

    /*
     * Roughly where InSystem's particles go, for framing: each enabled
     * emitter's mean particle followed through its life (velocity,
     * acceleration and drag, as ParticleSimulate.csh moves it), padded by
     * its radius, spread and size. Tighter than the culling reach, which must
     * hold the worst case.
     */
    GameCore::AABB EstimateBounds(const ParticleSystemDesc& InSystem, const EngineCore::Vector3& InOrigin)
    {
        double Min[3] = {  1e30,  1e30,  1e30 };
        double Max[3] = { -1e30, -1e30, -1e30 };
        const auto Grow = [&](const double (&InAt)[3], const double InPad)
        {
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                Min[Axis] = std::min(Min[Axis], InAt[Axis] - InPad);
                Max[Axis] = std::max(Max[Axis], InAt[Axis] + InPad);
            }
        };

        bool bAny = false;
        for(const ParticleEmitterDesc& Emitter : InSystem.Emitters)
        {
            if(!Emitter.bEnabled) { continue; }
            bAny = true;
            const RenderCore::ParticleEmitterSettings S = Emitter.ToSettings();
            const double Life = S.Lifetime;
            float Largest = std::max(S.StartSize, S.EndSize);
            if(S.bSizeLut) { for(const float Each : S.SizeLut) { Largest = std::max(Largest, Each); } }
            const double Shape = S.Shape == RenderCore::PS_Box ? 0.5 * std::sqrt(double(S.BoxSize[0]) * S.BoxSize[0] + double(S.BoxSize[1]) * S.BoxSize[1]
                                                                                 + double(S.BoxSize[2]) * S.BoxSize[2])
                                                               : double(S.EmitterRadius) + 0.5 * S.RingWidth;
            const double Pad  = Shape + (S.VelocitySpread + std::abs(S.ShapeSpeed)) * std::min(Life, 1.5) * 0.5
                              + 0.5 * Largest + 0.1 * S.Turbulence * Life * Life;
            double P[3] = { InOrigin.GetX() + Emitter.Offset.GetX(), InOrigin.GetY() + Emitter.Offset.GetY(),
                            InOrigin.GetZ() + Emitter.Offset.GetZ() };
            double V[3] = { S.Velocity[0], S.Velocity[1], S.Velocity[2] };
            Grow(P, Pad);
            if(S.Type == RenderCore::PT_Beam)
            {
                const double End[3] = { P[0] + Emitter.BeamEnd.GetX(), P[1] + Emitter.BeamEnd.GetY(), P[2] + Emitter.BeamEnd.GetZ() };
                Grow(End, S.BeamNoise + 0.5 * std::max(S.StartSize, S.EndSize));
                continue;
            }
            const int    Steps = 32;
            const double Dt    = Life / Steps;
            for(int Step = 0; Step < Steps; ++Step)
            {
                const double Keep = std::exp(-double(S.Drag) * Dt);
                for(int Axis = 0; Axis < 3; ++Axis)
                {
                    V[Axis] = (V[Axis] + S.Acceleration[Axis] * Dt) * Keep;
                    P[Axis] += V[Axis] * Dt;
                }
                Grow(P, Pad);
            }
        }
        if(!bAny)
        {
            const double Origin[3] = { InOrigin.GetX(), InOrigin.GetY(), InOrigin.GetZ() };
            Grow(Origin, 100.0);
        }
        /* What falls through the floor is hidden by it: frame what is above. */
        Min[1] = std::max(Min[1], InOrigin.GetY() - 20.0);
        Max[1] = std::max(Max[1], Min[1] + 50.0);
        return GameCore::AABB(EngineCore::Vector3(Min[0], Min[1], Min[2]), EngineCore::Vector3(Max[0], Max[1], Max[2]));
    }
}

ParticleEditorTab::ParticleEditorTab(EditorContext& InContext, QWidget* InParent)
    : QMainWindow(InParent)
    , Context(InContext)
{
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);

    Particles  = new ParticleDocument(this);
    PreviewKey = QStringLiteral("transient://ParticleEditor/%1").arg(++PreviewCounter());

    BuildPreview();
    BuildDocks();
    BuildToolBar();
    BuildActions();

    connect(Particles, &ParticleDocument::Changed, this, &ParticleEditorTab::PushToRenderer);
    connect(Particles, &ParticleDocument::LiveChanged, this, &ParticleEditorTab::PushToRenderer);
    connect(&Particles->GetHistory(), &QUndoStack::cleanChanged, this, &ParticleEditorTab::TitleChanged);
    connect(&Particles->GetHistory(), &QUndoStack::indexChanged, this, &ParticleEditorTab::TitleChanged);

    AnimateTimer = new QTimer(this);
    AnimateTimer->setInterval(16);
    connect(AnimateTimer, &QTimer::timeout, this, &ParticleEditorTab::Animate);
}

ParticleEditorTab::~ParticleEditorTab()
{
    AnimateTimer->stop();
    RenderCore::ParticleSystemCache::Invalidate(PreviewKey.toStdString());

    /* The viewport first, while its native window exists, then the World it shows. */
    delete ViewportArea;
    delete Preview;
}

// === Building ======================================================================

void ParticleEditorTab::BuildPreview()
{
    Preview = new EditorDocument(this);
    GameCore::World& World = Preview->GetEditedWorld();

    /* A floor to judge height and size by, a sun and a sky to light lit materials. */
    {
        const GameCore::Entity Floor = World.CreateEntity("Floor");
        GameCore::TransformComponent Transform;
        Transform.Scale = EngineCore::Vector3(FLOOR_SIZE, 1.0, FLOOR_SIZE);
        Floor.AddComponent(Transform);
        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", FLOOR_MESH);
        Floor.AddComponent(Render);
        FloorEntity = Floor.GetID();
    }
    {
        const GameCore::Entity Sun = World.CreateEntity("Sun");
        GameCore::TransformComponent Transform;
        Transform.Position = EngineCore::Vector3(0.0, 600.0, 0.0);
        Transform.Rotation = EngineCore::Vector3(50.0, 30.0, 0.0);
        Sun.AddComponent(Transform);
        /* Dim: the floor is DefaultLit white, and should sit behind the particles, not glare. */
        RenderCore::DirectionalLightComponent Light;
        Light.Intensity    = 0.9f;
        Light.bCastShadows = 0;
        Sun.AddComponent(Light);
        RenderCore::SkyLightComponent Sky;
        Sky.Intensity = 0.25f;
        Sun.AddComponent(Sky);
    }
    EnsureEmitterEntity();

    ViewportArea = new ViewportPanel(Context, *Preview, this);
    setCentralWidget(ViewportArea);
}

void ParticleEditorTab::BuildDocks()
{
    auto* ListDock = new QDockWidget(QStringLiteral("Emitters"), this);
    ListDock->setObjectName(QStringLiteral("ParticleEmittersDock"));
    EmitterList = new ParticleEmitterListPanel(*Particles, ListDock);
    ListDock->setWidget(EmitterList);
    addDockWidget(Qt::LeftDockWidgetArea, ListDock);

    auto* DetailsDock = new QDockWidget(QStringLiteral("Details"), this);
    DetailsDock->setObjectName(QStringLiteral("ParticleDetailsDock"));
    Details = new ParticleEmitterDetailsPanel(*Particles, DetailsDock);
    DetailsDock->setWidget(Details);
    addDockWidget(Qt::RightDockWidgetArea, DetailsDock);

    /* Once shown: a dock size set before is ignored. Wide enough that a section's Reset and x show. */
    QTimer::singleShot(0, this, [this, ListDock, DetailsDock]()
    {
        resizeDocks({ ListDock, DetailsDock }, { 200, std::max(DetailsDock->widget()->minimumSizeHint().width() + 24, 520) }, Qt::Horizontal);
    });
}

void ParticleEditorTab::BuildToolBar()
{
    QToolBar* Bar = addToolBar(QStringLiteral("Particles"));
    Bar->setObjectName(QStringLiteral("ParticleToolBar"));
    Bar->setMovable(false);

    SaveAction = Bar->addAction(QStringLiteral("Save"), this, &ParticleEditorTab::Save);
    SaveAction->setToolTip(QStringLiteral("Save the particle system (Ctrl+S): every component naming it shows the change"));
    Bar->addSeparator();

    PauseAction = Bar->addAction(QStringLiteral("Pause"));
    PauseAction->setCheckable(true);
    PauseAction->setToolTip(QStringLiteral("Freeze the preview's particles where they are"));
    connect(PauseAction, &QAction::toggled, this, &ParticleEditorTab::SetPaused);
    Bar->addAction(QStringLiteral("Restart"), this, &ParticleEditorTab::Restart)
        ->setToolTip(QStringLiteral("Clear the preview's particles and start emitting again"));

    Bar->addWidget(new QLabel(QStringLiteral(" Speed "), Bar));
    SpeedBox = new QComboBox(Bar);
    for(const float Each : Speeds) { SpeedBox->addItem(QStringLiteral("%1x").arg(double(Each))); }
    SpeedBox->setCurrentIndex(3);
    SpeedBox->setToolTip(QStringLiteral("How fast the preview runs (its Particle System's Time Scale)"));
    connect(SpeedBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](const int InIndex)
    {
        if(InIndex < 0) { return; }
        Speed = Speeds[InIndex];
        ApplyPlayback();
    });
    Bar->addWidget(SpeedBox);
    Bar->addSeparator();

    MoveAction = Bar->addAction(QStringLiteral("Move Emitter"));
    MoveAction->setCheckable(true);
    MoveAction->setToolTip(QStringLiteral("Send the emitter round a circle, to see its particles trail in world space"));
    connect(MoveAction, &QAction::toggled, this, [this](const bool bInOn)
    {
        if(bInOn)
        {
            const GameCore::AABB Box = EstimateBounds(Particles->GetSystem(), EngineCore::Vector3());
            const double Wide = std::max(Box.Max.GetX() - Box.Min.GetX(), Box.Max.GetZ() - Box.Min.GetZ());
            MoveRadius = std::clamp(Wide * 0.5, 50.0, 600.0);
            AnimateClock.restart();
            AnimateTimer->start();
            return;
        }
        AnimateTimer->stop();
        AnimateAngle = 0.0;
        GameCore::World& World = Preview->GetEditedWorld();
        if(World.IsAlive(EmitterEntity))
        {
            if(auto* Transform = World.GetEntity(EmitterEntity).GetComponent<GameCore::TransformComponent>())
            {
                Transform->Position = EngineCore::Vector3(0.0, 0.0, 0.0);
            }
        }
    });

    FloorAction = Bar->addAction(QStringLiteral("Floor"));
    FloorAction->setCheckable(true);
    FloorAction->setChecked(true);
    FloorAction->setToolTip(QStringLiteral("Show the floor under the emitter"));
    connect(FloorAction, &QAction::toggled, this, [this](const bool bInOn)
    {
        GameCore::World& World = Preview->GetEditedWorld();
        if(!World.IsAlive(FloorEntity)) { return; }
        if(auto* Render = World.GetEntity(FloorEntity).GetComponent<RenderCore::RenderComponent>()) { Render->bVisible = bInOn ? 1 : 0; }
    });

    Bar->addAction(QStringLiteral("Frame"), this, &ParticleEditorTab::FrameEffect)
        ->setToolTip(QStringLiteral("Frame where the particles go"));
    Bar->addSeparator();

    LiveAction = Bar->addAction(QStringLiteral("Live in Level"));
    LiveAction->setCheckable(true);
    LiveAction->setChecked(true);
    LiveAction->setToolTip(QStringLiteral("Show every unsaved change in the level too, wherever a component names this system; "
                                          "closing without saving puts the saved one back"));
    connect(LiveAction, &QAction::toggled, this, [this](const bool bInOn)
    {
        if(!bInOn && bLiveStored)
        {
            RenderCore::ParticleSystemCache::Invalidate(GetRuntimePath().toStdString());
            bLiveStored = false;
        }
        PushToRenderer();
    });
    Bar->addSeparator();

    StatsLabel = new QLabel(Bar);
    StatsLabel->setContentsMargins(6, 0, 6, 0);
    Bar->addWidget(StatsLabel);
}

void ParticleEditorTab::BuildActions()
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
    Add(File, QStringLiteral("&Save Particle System"), { QKeySequence::Save }, [this]() { Save(); });
    Add(File, QStringLiteral("&Close"), { QKeySequence::Close }, [this]() { emit CloseRequested(); });

    QMenu* Edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction* Undo = Add(Edit, QStringLiteral("&Undo"), { QKeySequence::Undo }, [this]() { Particles->GetHistory().undo(); });
    QAction* Redo = Add(Edit, QStringLiteral("&Redo"), { QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y) },
                        [this]() { Particles->GetHistory().redo(); });
    const auto UpdateHistory = [this, Undo, Redo]()
    {
        QUndoStack& History = Particles->GetHistory();
        Undo->setEnabled(History.canUndo());
        Redo->setEnabled(History.canRedo());
        Undo->setText(QStringLiteral("&Undo %1").arg(History.undoText()));
        Redo->setText(QStringLiteral("&Redo %1").arg(History.redoText()));
    };
    connect(&Particles->GetHistory(), &QUndoStack::indexChanged, this, UpdateHistory);
    UpdateHistory();

    QMenu* Emitter = menuBar()->addMenu(QStringLiteral("E&mitter"));
    Add(Emitter, QStringLiteral("&Add Emitter"), {}, [this]() { Particles->AddEmitter(); });
    Add(Emitter, QStringLiteral("&Duplicate"), { QKeySequence(Qt::CTRL | Qt::Key_D) },
        [this]() { Particles->DuplicateEmitter(Particles->GetSelected()); });
    Add(Emitter, QStringLiteral("&Remove"), {}, [this]() { Particles->RemoveEmitter(Particles->GetSelected()); });

    QMenu* PreviewMenu = menuBar()->addMenu(QStringLiteral("&Preview"));
    PreviewMenu->addAction(PauseAction);
    Add(PreviewMenu, QStringLiteral("&Restart"), { QKeySequence(Qt::CTRL | Qt::Key_R) }, [this]() { Restart(); });
    PreviewMenu->addAction(MoveAction);
    PreviewMenu->addAction(FloorAction);
    Add(PreviewMenu, QStringLiteral("&Frame Effect"), {}, [this]() { FrameEffect(); });
    PreviewMenu->addSeparator();
    PreviewMenu->addAction(LiveAction);
}

// === Loading and saving ============================================================

bool ParticleEditorTab::Load(const QString& InPath, QString& OutError)
{
    if(!Particles->Load(InPath, OutError))
    {
        return false;
    }
    Path = QFileInfo(InPath).absoluteFilePath();
    PushToRenderer();

    /* Once the viewport has its size. */
    QTimer::singleShot(0, this, [this]() { FrameEffect(); });
    emit TitleChanged();
    return true;
}

QString ParticleEditorTab::GetTabTitle() const
{
    return QFileInfo(Path).fileName() + (IsDirty() ? QStringLiteral("*") : QString());
}

bool ParticleEditorTab::IsDirty() const
{
    return Particles->IsDirty();
}

QString ParticleEditorTab::GetRuntimePath() const
{
    return EditorContent::ParticleSystemPathFor(Path);
}

bool ParticleEditorTab::Save()
{
    if(EditorContent::IsEngineContent(Path))
    {
        QMessageBox::warning(this, QStringLiteral("Save Particle System"), QStringLiteral("The engine's content is read-only here."));
        return false;
    }
    QString Error;
    if(!Particles->Save(Path, Error))
    {
        QMessageBox::warning(this, QStringLiteral("Save Particle System"), Error);
        return false;
    }

    /* Beside the exe, and out of the cache: every component naming it reads the file again. */
    EditorContent::StageAsset(Path);
    bLiveStored = false;
    emit TitleChanged();
    return true;
}

bool ParticleEditorTab::ConfirmClose()
{
    if(IsDirty())
    {
        const auto Answer = QMessageBox::question(this, QStringLiteral("Particle Editor"),
                                                  QStringLiteral("Save changes to %1?").arg(QFileInfo(Path).fileName()),
                                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if(Answer == QMessageBox::Cancel) { return false; }
        if(Answer == QMessageBox::Save && !Save()) { return false; }
    }
    if(bLiveStored)
    {
        RenderCore::ParticleSystemCache::Invalidate(GetRuntimePath().toStdString());
        bLiveStored = false;
    }
    return true;
}

// === The preview ===================================================================

void ParticleEditorTab::EnsureEmitterEntity()
{
    GameCore::World& World = Preview->GetEditedWorld();
    if(World.IsAlive(EmitterEntity) && World.GetEntity(EmitterEntity).HasComponent<RenderCore::ParticleSystemComponent>())
    {
        return;
    }
    if(World.IsAlive(EmitterEntity)) { World.DestroyEntity(EmitterEntity); }

    const GameCore::Entity Emitter = World.CreateEntity("Emitter");
    Emitter.AddComponent<GameCore::TransformComponent>();
    RenderCore::ParticleSystemComponent Component;
    std::snprintf(Component.System, sizeof(Component.System), "%s", PreviewKey.toUtf8().constData());
    Component.TimeScale = bPaused ? 0.0f : Speed;
    Emitter.AddComponent(Component);
    EmitterEntity = Emitter.GetID();
}

void ParticleEditorTab::PushToRenderer()
{
    const auto Copy = std::make_shared<const ParticleSystemDesc>(Particles->GetSystem());
    RenderCore::ParticleSystemCache::Store(PreviewKey.toStdString(), Copy);

    /* Unsaved edits reach the level only once there are some: a clean system is the file's. */
    const QString Runtime = GetRuntimePath();
    if(LiveAction && LiveAction->isChecked() && !Runtime.isEmpty() && (IsDirty() || bLiveStored))
    {
        RenderCore::ParticleSystemCache::Store(Runtime.toStdString(), Copy);
        bLiveStored = true;
    }
    EnsureEmitterEntity();
    UpdateStats();
}

void ParticleEditorTab::UpdateStats()
{
    float Held  = 0.0f;
    int   Count = 0;
    for(const ParticleEmitterDesc& Each : Particles->GetSystem().Emitters)
    {
        if(!Each.bEnabled) { continue; }
        Held += Each.GetSteadyStateCount();
        ++Count;
    }
    StatsLabel->setText(QStringLiteral("About %1 particles from %2 emitter%3%4")
                            .arg(int(std::lround(Held))).arg(Count).arg(Count == 1 ? QString() : QStringLiteral("s"))
                            .arg(bPaused ? QStringLiteral("  (paused)") : QString()));
}

void ParticleEditorTab::ApplyPlayback()
{
    EnsureEmitterEntity();
    GameCore::World& World = Preview->GetEditedWorld();
    if(auto* Component = World.GetEntity(EmitterEntity).GetComponent<RenderCore::ParticleSystemComponent>())
    {
        Component->TimeScale = bPaused ? 0.0f : Speed;
    }
    UpdateStats();
}

void ParticleEditorTab::SetPaused(const bool bInPaused)
{
    bPaused = bInPaused;
    if(PauseAction->isChecked() != bInPaused)
    {
        const QSignalBlocker Block(PauseAction);
        PauseAction->setChecked(bInPaused);
    }
    ApplyPlayback();
}

void ParticleEditorTab::SetSpeed(const float InSpeed)
{
    int Nearest = 0;
    for(int Index = 1; Index < int(std::size(Speeds)); ++Index)
    {
        if(std::abs(Speeds[Index] - InSpeed) < std::abs(Speeds[Nearest] - InSpeed)) { Nearest = Index; }
    }
    {
        const QSignalBlocker Block(SpeedBox);
        SpeedBox->setCurrentIndex(Nearest);
    }
    Speed = std::max(InSpeed, 0.0f);
    ApplyPlayback();
}

void ParticleEditorTab::SetMoving(const bool bInMoving)
{
    MoveAction->setChecked(bInMoving);
}

void ParticleEditorTab::Restart()
{
    /* A new component is a new proxy: its emitters start empty. */
    GameCore::World& World = Preview->GetEditedWorld();
    EngineCore::Vector3 Position;
    if(World.IsAlive(EmitterEntity))
    {
        if(const auto* Transform = World.GetEntity(EmitterEntity).GetComponent<GameCore::TransformComponent>())
        {
            Position = Transform->Position;
        }
        if(Preview->GetSelection() == EmitterEntity) { Preview->SetSelection(GameCore::InvalidEntityID()); }
        World.DestroyEntity(EmitterEntity);
    }
    EnsureEmitterEntity();
    if(auto* Transform = World.GetEntity(EmitterEntity).GetComponent<GameCore::TransformComponent>())
    {
        Transform->Position = Position;
    }
}

void ParticleEditorTab::FrameEffect()
{
    ViewportWidget* Viewport = ViewportArea ? ViewportArea->GetViewport() : nullptr;
    if(!Viewport) { return; }
    /* A little from the side and above, so the floor shows where it rises from. */
    EditorCamera& Camera = Viewport->GetCamera();
    Camera.Yaw   = -25.0f;
    Camera.Pitch = -15.0f;
    Camera.Frame(EstimateBounds(Particles->GetSystem(), EngineCore::Vector3()));
    Viewport->update();
}

void ParticleEditorTab::Animate()
{
    const double Delta = std::min(AnimateClock.restart() / 1000.0, 0.1);
    if(!bPaused) { AnimateAngle += Delta * MOVE_SPEED * double(Speed); }

    EnsureEmitterEntity();
    GameCore::World& World = Preview->GetEditedWorld();
    if(auto* Transform = World.GetEntity(EmitterEntity).GetComponent<GameCore::TransformComponent>())
    {
        Transform->Position = EngineCore::Vector3(MoveRadius * std::cos(AnimateAngle), 0.0, MoveRadius * std::sin(AnimateAngle));
    }
}

uint64_t ParticleEditorTab::RequestCapture()
{
    return Context.RequestSceneCapture(Preview);
}

#endif // WITH_EDITOR
