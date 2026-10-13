#include "Private/Editor/AI/NavigationPanel.h"

#if WITH_EDITOR && WITH_AICORE

#include <cstdio>
#include <cstring>

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "Private/Editor/AI/EditorAI.h"
#include "Public/AICore/AIWorld.h"
#include "Public/AICore/Components/NavMeshVolumeComponent.h"
#include "Public/AICore/Components/NavModifierComponent.h"
#include "Public/AICore/Components/NavObstacleComponent.h"
#include "Public/Editor/EditorCamera.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/* NavigationPanel.h has the dock's layout. */
namespace
{
    NavigationPanel*& Instance()
    {
        static NavigationPanel* Panel = nullptr;
        return Panel;
    }

    AICore::NavMeshVolumeComponent* FindVolume(GameCore::World& InWorld, GameCore::EntityID* OutEntity = nullptr)
    {
        AICore::NavMeshVolumeComponent* Found = nullptr;
        InWorld.Each<AICore::NavMeshVolumeComponent>([&](const GameCore::Entity& InEntity, AICore::NavMeshVolumeComponent& InVolume)
        {
            if(!Found) { Found = &InVolume; if(OutEntity) { *OutEntity = InEntity.GetID(); } }
        });
        return Found;
    }

    EngineCore::Vector3 InFront()
    {
        ViewportWidget* Viewport = EditorAI::GetHost().Viewport;
        if(!Viewport) { return EngineCore::Vector3(); }
        const EditorCamera& Camera = Viewport->GetCamera();
        return Camera.Position + Camera.GetForward() * 600.0;
    }
}

NavigationPanel& NavigationPanel::Install(QMainWindow& InHost, QDockWidget* InTabifyWith)
{
    if(Instance()) { return *Instance(); }
    auto* Dock = new NavigationPanel(&InHost);
    Instance() = Dock;
    InHost.addDockWidget(Qt::RightDockWidgetArea, Dock);
    if(InTabifyWith) { InHost.tabifyDockWidget(InTabifyWith, Dock); }
    Dock->hide();
    return *Dock;
}

NavigationPanel* NavigationPanel::Find() { return Instance(); }

void NavigationPanel::Show()
{
    NavigationPanel* Dock = Find();
    if(!Dock) { return; }
    Dock->show();
    Dock->raise();
    Dock->Refresh();
}

NavigationPanel::NavigationPanel(QWidget* InParent) : QDockWidget(QStringLiteral("Navigation"), InParent)
{
    setObjectName(QStringLiteral("NavigationDock"));
    EditorAI::InstallOverlay();
    EditorAI::OverlaySettings& Overlay = EditorAI::GetOverlaySettings();

    auto* Body = new QWidget(this);
    auto* Layout = new QVBoxLayout(Body);

    Status = new QLabel(Body);
    Status->setWordWrap(true);
    Status->setStyleSheet(QStringLiteral("font-weight: 600;"));
    Layout->addWidget(Status);
    BuildButton = new QPushButton(QStringLiteral("Build navigation"), Body);
    BuildButton->setToolTip(QStringLiteral("Build the level's navmesh from its Nav Mesh Volume and write its .nmnav"));
    Layout->addWidget(BuildButton);
    connect(BuildButton, &QPushButton::clicked, this, [this]() { Build(true); });
    Stats = new QLabel(Body);
    Stats->setWordWrap(true);
    Stats->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    Layout->addWidget(Stats);

    auto* Options = new QGroupBox(QStringLiteral("Options"), Body);
    auto* OptionsLayout = new QVBoxLayout(Options);
    AutoRebuild = new QCheckBox(QStringLiteral("Rebuild on change"), Options);
    AutoRebuild->setChecked(true);
    AutoRebuild->setToolTip(QStringLiteral("Rebuild a moment after the level changes; written with the level's next save"));
    OptionsLayout->addWidget(AutoRebuild);
    const auto Toggle = [&](const QString& InLabel, bool& InFlag)
    {
        auto* Box = new QCheckBox(InLabel, Options);
        Box->setChecked(InFlag);
        connect(Box, &QCheckBox::toggled, this, [&InFlag](const bool bInOn) { InFlag = bInOn; });
        OptionsLayout->addWidget(Box);
        Synced.emplace_back(Box, &InFlag);
        return Box;
    };
    Toggle(QStringLiteral("Show navmesh"), Overlay.bNavMesh);
    Toggle(QStringLiteral("Show tile bounds"), Overlay.bTileBounds);
    Toggle(QStringLiteral("Show obstacles"), Overlay.bObstacles);
    Toggle(QStringLiteral("Show modifiers and volume"), Overlay.bModifiers);
    Layout->addWidget(Options);

    auto* Tools = new QGroupBox(QStringLiteral("Tools"), Body);
    auto* ToolsLayout = new QVBoxLayout(Tools);
    PathTester = new QCheckBox(QStringLiteral("Path tester"), Tools);
    PathTester->setToolTip(QStringLiteral("Click the level for a path's start, again for its end (Shift+click moves the end)"));
    ToolsLayout->addWidget(PathTester);
    connect(PathTester, &QCheckBox::toggled, this, [this]() { UpdateTool(); });
    PathLine = new QLabel(Tools);
    PathLine->setWordWrap(true);
    ToolsLayout->addWidget(PathLine);
    auto* AddRow = new QHBoxLayout();
    auto* AddVolumeButton = new QPushButton(QStringLiteral("Add volume"), Tools);
    auto* AddModifierButton = new QPushButton(QStringLiteral("Add modifier"), Tools);
    auto* AddObstacleButton = new QPushButton(QStringLiteral("Add obstacle"), Tools);
    AddRow->addWidget(AddVolumeButton);
    AddRow->addWidget(AddModifierButton);
    AddRow->addWidget(AddObstacleButton);
    ToolsLayout->addLayout(AddRow);
    connect(AddVolumeButton, &QPushButton::clicked, this, [this]() { AddVolume(); });
    connect(AddModifierButton, &QPushButton::clicked, this, [this]() { AddModifier(); });
    connect(AddObstacleButton, &QPushButton::clicked, this, [this]() { AddObstacle(); });
    Layout->addWidget(Tools);
    Layout->addStretch(1);
    setWidget(Body);

    Tool.OnChanged = [this]() { UpdatePathLine(); };

    RebuildTimer = new QTimer(this);
    RebuildTimer->setSingleShot(true);
    RebuildTimer->setInterval(700);
    connect(RebuildTimer, &QTimer::timeout, this, [this]() { Build(false); });
    RefreshTimer = new QTimer(this);
    RefreshTimer->setSingleShot(true);
    RefreshTimer->setInterval(300);
    connect(RefreshTimer, &QTimer::timeout, this, [this]() { Refresh(); });

    if(EditorDocument* Document = EditorAI::GetHost().Document)
    {
        connect(Document, &EditorDocument::EntitiesChanged, this, [this]() { ScheduleRebuild(); });
        connect(Document, &EditorDocument::PropertyEdited, this, [this](GameCore::EntityID, GameCore::ComponentTypeID) { ScheduleRebuild(); });
        connect(Document, &EditorDocument::WorldReset, this, [this]()
        {
            EditorAI::GetTestPath() = EditorAI::TestPath();
            bUnsavedBake = false;
            RefreshTimer->start();
        });
        connect(Document, &EditorDocument::AboutToSaveWorld, this, [this]()
        {
            /* A bake made by Rebuild on change goes to disk with the level. */
            if(bUnsavedBake && !EditorAI::IsPlaying()) { Build(true); }
        });
    }
    connect(this, &QDockWidget::visibilityChanged, this, [this](const bool bInVisible)
    {
        if(bInVisible) { EditorAI::GetOverlaySettings().bNavMesh = true; Refresh(); }
        UpdateTool();
    });
    Refresh();
}

void NavigationPanel::ScheduleRebuild()
{
    if(!isVisible() || EditorAI::IsPlaying()) { return; }
    AICore::AIWorld* AI = EditorAI::GetAI();
    if(AutoRebuild->isChecked() && AI && AI->GetNav().HasNavMesh()) { RebuildTimer->start(); }
    else { RefreshTimer->start(); }
}

bool NavigationPanel::Build(const bool bInSave)
{
    AICore::AIWorld* AI = EditorAI::GetAI();
    if(!AI)
    {
        Status->setText(QStringLiteral("No level to build for"));
        return false;
    }
    if(EditorAI::IsPlaying())
    {
        Status->setText(QStringLiteral("Stop Play to build the level's navigation"));
        return false;
    }
    GameCore::EntityID VolumeEntity;
    AICore::NavMeshVolumeComponent* Volume = FindVolume(AI->GetWorld(), &VolumeEntity);
    if(!Volume)
    {
        Status->setText(QStringLiteral("No Nav Mesh Volume: Add volume first"));
        return false;
    }
    Status->setText(QStringLiteral("Building..."));
    Status->repaint();

    AICore::NavBuildStats Result;
    std::string Error;
    if(!AI->GetNav().Build(&Result, &Error))
    {
        Status->setText(QStringLiteral("Build failed: %1").arg(QString::fromStdString(Error)));
        return false;
    }
    EditorAI::UpdateTestPath();
    UpdatePathLine();

    if(bInSave)
    {
        QString Runtime, Disk;
        if(!EditorAI::GetNavMeshPaths(Runtime, Disk))
        {
            Status->setText(QStringLiteral("Built; save the level to give its navmesh a file"));
            bUnsavedBake = true;
            Refresh();
            return true;
        }
        if(QString::fromUtf8(Volume->NavMesh) != Runtime)
        {
            /* The volume names the file now: one undo step. */
            EditorDocument* Document = EditorAI::GetHost().Document;
            Document->BeginEditGesture(QStringLiteral("Set Nav Mesh"));
            std::snprintf(Volume->NavMesh, sizeof(Volume->NavMesh), "%s", Runtime.toUtf8().constData());
            Document->NotifyPropertyEdited(VolumeEntity, AICore::NavMeshVolumeComponent::StaticType().ID);
            Document->EndEditGesture();
        }
        QDir().mkpath(QFileInfo(Disk).absolutePath());
        if(!AI->GetNav().SaveFile(Disk.toStdString(), &Error))
        {
            Status->setText(QStringLiteral("Built, but %1 %2").arg(Runtime, QString::fromStdString(Error)));
            return false;
        }
        bUnsavedBake = false;
    }
    else
    {
        bUnsavedBake = true;
    }
    Refresh();
    return true;
}

void NavigationPanel::Refresh()
{
    for(const auto& Each : Synced)
    {
        const QSignalBlocker Quiet(Each.first);
        Each.first->setChecked(*Each.second);
    }
    AICore::AIWorld* AI = EditorAI::GetAI();
    BuildButton->setEnabled(!EditorAI::IsPlaying());
    if(!AI)
    {
        Status->setText(QStringLiteral("No level"));
        Stats->clear();
        return;
    }
    const bool bVolume = FindVolume(AI->GetWorld()) != nullptr;
    AICore::NavWorld& Nav = AI->GetNav();
    if(EditorAI::IsPlaying())
    {
        Status->setText(Nav.HasNavMesh() ? QStringLiteral("Playing: the game's navmesh") : QStringLiteral("Playing: no navmesh"));
    }
    else if(!bVolume)
    {
        Status->setText(QStringLiteral("No Nav Mesh Volume"));
    }
    else if(!Nav.HasNavMesh())
    {
        /* A baked file the level names: load it to show it. */
        QString Runtime, Disk;
        if(EditorAI::GetNavMeshPaths(Runtime, Disk) && QFileInfo::exists(Disk) && Nav.LoadFile(Disk.toStdString()))
        {
            Status->setText(Nav.IsUpToDate() ? QStringLiteral("Up to date") : QStringLiteral("Out of date: build again"));
        }
        else
        {
            Status->setText(QStringLiteral("Not built"));
        }
    }
    else
    {
        Status->setText(bUnsavedBake ? QStringLiteral("Rebuilt: saved with the level") : QStringLiteral("Up to date"));
    }
    if(Nav.HasNavMesh())
    {
        const AICore::NavBuildStats S = Nav.GetNavMesh().GetStats();
        Stats->setText(QStringLiteral("%1 tiles (%2 x %3), %4 polygons from %5 triangles; %6 ms, %7 KB")
                       .arg(S.Tiles).arg(S.TilesX).arg(S.TilesZ).arg(S.Polygons).arg(S.Triangles)
                       .arg(int(S.Milliseconds)).arg(double(S.Bytes) / 1024.0, 0, 'f', 1));
    }
    else
    {
        Stats->clear();
    }
    UpdatePathLine();
}

void NavigationPanel::UpdatePathLine()
{
    const EditorAI::TestPath& Path = EditorAI::GetTestPath();
    if(!PathTester->isChecked() && !Path.bHasStart) { PathLine->clear(); return; }
    if(!Path.bHasStart) { PathLine->setText(QStringLiteral("Click the level for the start")); return; }
    if(!Path.bHasEnd)   { PathLine->setText(QStringLiteral("Click again for the end")); return; }
    const char* Names[] = { "no path", "complete", "partial: the end cannot be reached", "failed: off the navmesh" };
    PathLine->setText(QStringLiteral("%1 corners, %2 m, %3").arg(Path.Points.size()).arg(double(Path.Length) / 100.0, 0, 'f', 1)
                      .arg(QString::fromLatin1(Names[std::min(Path.Status, 3)])));
}

void NavigationPanel::UpdateTool()
{
    ViewportWidget* Viewport = EditorAI::GetHost().Viewport;
    if(!Viewport) { return; }
    const bool bOn = PathTester->isChecked() && isVisible();
    if(bOn) { Viewport->SetTool(&Tool); }
    else if(Viewport->GetTool() == &Tool) { Viewport->SetTool(nullptr); }
    if(!bOn) { EditorAI::GetTestPath() = EditorAI::TestPath(); }
    UpdatePathLine();
}

void NavigationPanel::AddVolume()
{
    EditorDocument* Document = EditorAI::GetHost().Document;
    if(!Document) { return; }
    /* Over everything the level has, a little taller. */
    const GameCore::AABB Scene = Document->GetSceneBounds();
    const EngineCore::Vector3 Centre = Scene.IsEmpty() ? EngineCore::Vector3() : (Scene.Min + Scene.Max) * 0.5f;
    const EngineCore::Vector3 Half = Scene.IsEmpty() ? EngineCore::Vector3(5000.0, 1000.0, 5000.0)
                                                     : (Scene.Max - Scene.Min) * 0.5f + EngineCore::Vector3(100.0, 200.0, 100.0);
    const GameCore::EntityID Made = Document->CreateEntity(QStringLiteral("NavMeshVolume"), Centre, [&Half](const GameCore::Entity& InEntity)
    {
        AICore::NavMeshVolumeComponent Volume;
        Volume.HalfExtents = Half;
        InEntity.AddComponent(Volume);
    });
    Document->SetSelection(Made);
    Refresh();
}

void NavigationPanel::AddModifier()
{
    EditorDocument* Document = EditorAI::GetHost().Document;
    if(!Document) { return; }
    const GameCore::EntityID Made = Document->CreateEntity(QStringLiteral("NavModifier"), InFront(), [](const GameCore::Entity& InEntity)
    {
        InEntity.AddComponent<AICore::NavModifierComponent>();
    });
    Document->SetSelection(Made);
}

void NavigationPanel::AddObstacle()
{
    EditorDocument* Document = EditorAI::GetHost().Document;
    if(!Document) { return; }
    const GameCore::EntityID Made = Document->CreateEntity(QStringLiteral("NavObstacle"), InFront(), [](const GameCore::Entity& InEntity)
    {
        InEntity.AddComponent<AICore::NavObstacleComponent>();
    });
    Document->SetSelection(Made);
}

#endif // WITH_EDITOR && WITH_AICORE
