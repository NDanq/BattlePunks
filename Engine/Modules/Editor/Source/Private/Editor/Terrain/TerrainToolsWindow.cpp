#if WITH_EDITOR

#include "Private/Editor/Terrain/TerrainToolsWindow.h"

#include <algorithm>
#include <cmath>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/GameCore/World.h"

#include "Private/Editor/EditorIcons.h"
#include "Private/Editor/Terrain/TerrainEditing.h"

namespace
{
    TerrainToolsWindow*& Instance()
    {
        static TerrainToolsWindow* Dock = nullptr;
        return Dock;
    }

    /* The Sculpt buttons' ids: a GameCore::ETerrainTool, or LowerId for Raise inverted. */
    constexpr int LowerId = 100;

    struct ToolEntry
    {
        const char*       Name;      // the console's
        const char*       Label;
        int               Id;
        bool              bPaint;
        EditorIcons::EIcon Icon;
        const char*       Tip;
    };

    const ToolEntry ToolTable[] = {
        { "Raise",       "Raise",        GameCore::TT_Raise,         false, EditorIcons::EIcon::TerrainRaise,
          "Raise the ground under the brush (Shift: lower)" },
        { "Lower",       "Lower",        LowerId,                    false, EditorIcons::EIcon::TerrainLower,
          "Lower the ground under the brush (Shift: raise)" },
        { "Smooth",      "Smooth",       GameCore::TT_Smooth,        false, EditorIcons::EIcon::TerrainSmooth,
          "Even out bumps toward their neighbours' average" },
        { "Flatten",     "Flatten",      GameCore::TT_Flatten,       false, EditorIcons::EIcon::TerrainFlatten,
          "Level toward the height where the stroke began" },
        { "Noise",       "Noise",        GameCore::TT_Noise,         false, EditorIcons::EIcon::TerrainNoise,
          "Roughen with value noise, up and down" },
        { "SetHeight",   "Set Height",   GameCore::TT_SetHeight,     false, EditorIcons::EIcon::TerrainSetHeight,
          "Put the ground at the Target Height" },
        { "Paint",       "Paint",        GameCore::TT_Paint,         true,  EditorIcons::EIcon::TerrainPaint,
          "Paint the chosen layer (Shift: smooth the paint)" },
        { "SmoothPaint", "Smooth Paint", GameCore::TT_SmoothWeights, true,  EditorIcons::EIcon::TerrainSmoothPaint,
          "Blur the layers' edges together" },
    };

    QToolButton* MakeToolButton(const ToolEntry& InEntry, QWidget* InParent)
    {
        auto* Button = new QToolButton(InParent);
        Button->setText(QString::fromLatin1(InEntry.Label));
        Button->setIcon(EditorIcons::Get(InEntry.Icon));
        Button->setIconSize(QSize(24, 24));
        Button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        Button->setCheckable(true);
        Button->setAutoRaise(true);
        Button->setMinimumWidth(72);
        Button->setToolTip(QString::fromLatin1(InEntry.Tip));
        Button->setObjectName(QStringLiteral("TerrainTool_") + QString::fromLatin1(InEntry.Name));
        return Button;
    }
}

TerrainToolsWindow& TerrainToolsWindow::Install(QMainWindow& InHost, QDockWidget* InTabifyWith)
{
    if(Instance()) { return *Instance(); }
    auto* Dock = new TerrainToolsWindow(&InHost);
    Instance() = Dock;
    InHost.addDockWidget(Qt::RightDockWidgetArea, Dock);
    if(InTabifyWith) { InHost.tabifyDockWidget(InTabifyWith, Dock); }
    Dock->hide();
    return *Dock;
}

TerrainToolsWindow* TerrainToolsWindow::Find()
{
    return Instance();
}

void TerrainToolsWindow::Show()
{
    TerrainToolsWindow* Dock = Find();
    if(!Dock) { return; }
    Dock->show();
    Dock->raise();
    if(Dock->Modes->currentIndex() == 0) { Dock->Modes->setCurrentIndex(1); }
    Dock->RefreshTarget();
    Dock->UpdateViewportTool();
}

bool TerrainToolsWindow::SelectTool(const QString& InName)
{
    TerrainToolsWindow* Dock = Find();
    if(!Dock) { return false; }
    if(InName.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0)
    {
        Dock->Modes->setCurrentIndex(0);
        return true;
    }
    for(const ToolEntry& Each : ToolTable)
    {
        if(InName.compare(QString::fromLatin1(Each.Name), Qt::CaseInsensitive) != 0) { continue; }
        Dock->show();
        Dock->raise();
        Dock->Modes->setCurrentIndex(Each.bPaint ? 2 : 1);
        QButtonGroup* Group = Each.bPaint ? Dock->PaintTools : Dock->SculptTools;
        if(QAbstractButton* Button = Group->button(Each.Id)) { Button->setChecked(true); }
        Dock->PullSettings();
        Dock->UpdateViewportTool();
        return true;
    }
    return false;
}

TerrainToolsWindow::TerrainToolsWindow(QWidget* InParent)
    : QDockWidget(QStringLiteral("Terrain"), InParent)
{
    setObjectName(QStringLiteral("TerrainDock"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto* Body   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Body);
    Layout->setContentsMargins(8, 8, 8, 8);
    Layout->setSpacing(8);

    TargetLabel = new QLabel(Body);
    TargetLabel->setWordWrap(true);
    TargetLabel->setTextFormat(Qt::RichText);
    Layout->addWidget(TargetLabel);

    Modes = new QTabWidget(Body);
    Modes->addTab(MakeCreatePage(), EditorIcons::Get(EditorIcons::EIcon::Terrain), QStringLiteral("Create"));
    Modes->addTab(MakeSculptPage(), EditorIcons::Get(EditorIcons::EIcon::TerrainRaise), QStringLiteral("Sculpt"));
    Modes->addTab(MakePaintPage(), EditorIcons::Get(EditorIcons::EIcon::TerrainPaint), QStringLiteral("Paint"));
    Modes->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    Layout->addWidget(Modes);

    BrushPanel = MakeBrushPanel();
    Layout->addWidget(BrushPanel);

    HintLabel = new QLabel(QStringLiteral("Left drag on the terrain to use the brush. Shift inverts. "
                                          "[ and ] or Ctrl+wheel resize it. Esc or a right click cancels a stroke. "
                                          "Each stroke is one undo step; Save writes the terrain with the level."), Body);
    HintLabel->setWordWrap(true);
    HintLabel->setEnabled(false);
    Layout->addWidget(HintLabel);
    Layout->addStretch(1);
    setWidget(Body);

    Tool.OnSettingsChanged = [this]() { PushSettings(); };

    connect(Modes, &QTabWidget::currentChanged, this, [this](int) { PullSettings(); UpdateViewportTool(); });
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool) { RefreshTarget(); UpdateViewportTool(); });

    if(EditorDocument* Document = EditorTerrain::GetHost().Document)
    {
        connect(Document, &EditorDocument::SelectionChanged, this, [this](GameCore::EntityID) { RefreshTarget(); });
        connect(Document, &EditorDocument::EntitiesChanged, this, [this]() { RefreshTarget(); });
        connect(Document, &EditorDocument::WorldReset, this, [this]() { RefreshTarget(); });
    }
    PullSettings();
    RefreshTarget();
    UpdateViewportTool();
}

QWidget* TerrainToolsWindow::MakeCreatePage()
{
    auto* Page   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Page);

    auto* NewBox  = new QGroupBox(QStringLiteral("New terrain"), Page);
    auto* Form    = new QFormLayout(NewBox);
    CreateName    = new QLineEdit(QStringLiteral("Terrain"), NewBox);
    CreateName->setToolTip(QStringLiteral("The entity's name, and its file's: <Level>/Terrain/<Name>.nmterrain"));
    CreateSize    = new QComboBox(NewBox);
    for(const int Samples : { 65, 129, 257, 513, 1025, 2049 })
    {
        CreateSize->addItem(QStringLiteral("%1 x %1 samples").arg(Samples), Samples);
    }
    CreateSize->setCurrentIndex(2);
    CreateSpacing = new QDoubleSpinBox(NewBox);
    CreateSpacing->setRange(10.0, 2000.0);
    CreateSpacing->setDecimals(0);
    CreateSpacing->setValue(100.0);
    CreateSpacing->setSuffix(QStringLiteral(" units"));
    CreateSpacing->setToolTip(QStringLiteral("Between samples, in world units (centimetres in the shipped content)"));
    CreateHeight  = new QDoubleSpinBox(NewBox);
    CreateHeight->setRange(-100000.0, 100000.0);
    CreateHeight->setDecimals(0);
    CreateHeight->setToolTip(QStringLiteral("Every sample's starting height"));
    CreateCollision = new QCheckBox(QStringLiteral("Collision (a Static Rigid Body heightfield)"), NewBox);
    CreateCollision->setChecked(true);
    auto* SizeNote = new QLabel(NewBox);
    SizeNote->setEnabled(false);
    const auto UpdateNote = [this, SizeNote]()
    {
        const double Side = double(CreateSize->currentData().toInt() - 1) * CreateSpacing->value();
        SizeNote->setText(QStringLiteral("%1 x %1 units, centred on the world origin").arg(Side, 0, 'f', 0));
    };
    connect(CreateSize, qOverload<int>(&QComboBox::currentIndexChanged), this, UpdateNote);
    connect(CreateSpacing, qOverload<double>(&QDoubleSpinBox::valueChanged), this, UpdateNote);
    UpdateNote();
    Form->addRow(QStringLiteral("Name"), CreateName);
    Form->addRow(QStringLiteral("Size"), CreateSize);
    Form->addRow(QStringLiteral("Spacing"), CreateSpacing);
    Form->addRow(QStringLiteral("Height"), CreateHeight);
    Form->addRow(QString(), SizeNote);
    Form->addRow(QString(), CreateCollision);
    auto* Create = new QPushButton(EditorIcons::Get(EditorIcons::EIcon::Add), QStringLiteral("Create Terrain"), NewBox);
    Create->setObjectName(QStringLiteral("TerrainCreate"));
    connect(Create, &QPushButton::clicked, this, [this]() { CreateTerrain(); });
    Form->addRow(QString(), Create);
    Layout->addWidget(NewBox);

    auto* MapBox  = new QGroupBox(QStringLiteral("Heightmap"), Page);
    auto* MapForm = new QFormLayout(MapBox);
    ImportLow  = new QDoubleSpinBox(MapBox);
    ImportHigh = new QDoubleSpinBox(MapBox);
    for(QDoubleSpinBox* Each : { ImportLow, ImportHigh })
    {
        Each->setRange(-1000000.0, 1000000.0);
        Each->setDecimals(0);
    }
    ImportHigh->setValue(5000.0);
    ImportLow->setToolTip(QStringLiteral("The height black becomes"));
    ImportHigh->setToolTip(QStringLiteral("The height white becomes"));
    MapForm->addRow(QStringLiteral("Black"), ImportLow);
    MapForm->addRow(QStringLiteral("White"), ImportHigh);
    auto* Buttons = new QHBoxLayout();
    auto* Import  = new QPushButton(QStringLiteral("Import..."), MapBox);
    auto* Export  = new QPushButton(QStringLiteral("Export..."), MapBox);
    Import->setToolTip(QStringLiteral("A greyscale image (16-bit PNG best) as the selected terrain's heights, resampled to fit"));
    Export->setToolTip(QStringLiteral("The selected terrain's heights as a 16-bit greyscale PNG, lowest black, highest white"));
    connect(Import, &QPushButton::clicked, this, [this]() { ImportHeightmap(); });
    connect(Export, &QPushButton::clicked, this, [this]() { ExportHeightmap(); });
    Buttons->addWidget(Import);
    Buttons->addWidget(Export);
    MapForm->addRow(QString(), Buttons);
    Layout->addWidget(MapBox);
    Layout->addStretch(1);
    return Page;
}

QWidget* TerrainToolsWindow::MakeSculptPage()
{
    auto* Page = new QWidget(this);
    auto* Grid = new QGridLayout(Page);
    SculptTools = new QButtonGroup(Page);
    SculptTools->setExclusive(true);
    int Index = 0;
    for(const ToolEntry& Each : ToolTable)
    {
        if(Each.bPaint) { continue; }
        QToolButton* Button = MakeToolButton(Each, Page);
        SculptTools->addButton(Button, Each.Id);
        Grid->addWidget(Button, Index / 3, Index % 3);
        ++Index;
    }
    Grid->setRowStretch((Index + 2) / 3, 1);   // the buttons keep their size; the room goes below them
    SculptTools->button(GameCore::TT_Raise)->setChecked(true);
    connect(SculptTools, &QButtonGroup::idClicked, this, [this](int) { PullSettings(); });
    return Page;
}

QWidget* TerrainToolsWindow::MakePaintPage()
{
    auto* Page   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Page);
    auto* Row    = new QHBoxLayout();
    PaintTools = new QButtonGroup(Page);
    PaintTools->setExclusive(true);
    for(const ToolEntry& Each : ToolTable)
    {
        if(!Each.bPaint) { continue; }
        QToolButton* Button = MakeToolButton(Each, Page);
        PaintTools->addButton(Button, Each.Id);
        Row->addWidget(Button);
    }
    Row->addStretch(1);
    PaintTools->button(GameCore::TT_Paint)->setChecked(true);
    connect(PaintTools, &QButtonGroup::idClicked, this, [this](int) { PullSettings(); });
    Layout->addLayout(Row);

    Layers = new QListWidget(Page);
    Layers->setObjectName(QStringLiteral("TerrainLayers"));
    Layers->setToolTip(QStringLiteral("The layer to paint: the terrain material's four, mixed by the paint (Vertex Color R G B A)"));
    Layers->setMaximumHeight(110);
    for(uint32_t Layer = 0; Layer < GameCore::TERRAIN_LAYERS; ++Layer)
    {
        Layers->addItem(QStringLiteral("Layer %1").arg(Layer));
    }
    Layers->setCurrentRow(0);
    connect(Layers, &QListWidget::currentRowChanged, this, [this](int) { PullSettings(); });
    Layout->addWidget(Layers);
    Layout->addStretch(1);
    return Page;
}

QWidget* TerrainToolsWindow::MakeBrushPanel()
{
    auto* Box  = new QGroupBox(QStringLiteral("Brush"), this);
    auto* Form = new QFormLayout(Box);

    Radius = new QDoubleSpinBox(Box);
    Radius->setObjectName(QStringLiteral("TerrainRadius"));
    Radius->setRange(25.0, 50000.0);
    Radius->setDecimals(0);
    Radius->setSingleStep(50.0);
    Radius->setSuffix(QStringLiteral(" units"));
    Radius->setToolTip(QStringLiteral("World units ( [ and ] or Ctrl+wheel in the viewport)"));

    auto* StrengthRow = new QHBoxLayout();
    Strength = new QSlider(Qt::Horizontal, Box);
    Strength->setObjectName(QStringLiteral("TerrainStrength"));
    Strength->setRange(1, 100);
    StrengthValue = new QLabel(Box);
    StrengthValue->setMinimumWidth(36);
    StrengthRow->addWidget(Strength, 1);
    StrengthRow->addWidget(StrengthValue);

    Falloff = new QComboBox(Box);
    for(uint8_t Each = 0; Each < GameCore::TF_Count; ++Each)
    {
        Falloff->addItem(QString::fromLatin1(GameCore::GetTerrainFalloffName(Each)), int(Each));
    }
    Falloff->setToolTip(QStringLiteral("How the brush fades from its centre to its rim (the inner ring is half strength)"));

    Rate = new QDoubleSpinBox(Box);
    Rate->setRange(1.0, 100000.0);
    Rate->setDecimals(0);
    Rate->setSuffix(QStringLiteral(" units/s"));
    Rate->setToolTip(QStringLiteral("Raise, Lower and Noise: how fast at full strength"));

    TargetHeight = new QDoubleSpinBox(Box);
    TargetHeight->setRange(-1000000.0, 1000000.0);
    TargetHeight->setDecimals(0);
    TargetHeight->setToolTip(QStringLiteral("Set Height: the world height the ground goes to"));

    NoiseScale = new QDoubleSpinBox(Box);
    NoiseScale->setRange(10.0, 100000.0);
    NoiseScale->setDecimals(0);
    NoiseScale->setSuffix(QStringLiteral(" units"));
    NoiseScale->setToolTip(QStringLiteral("Noise: the bumps' size"));

    Form->addRow(QStringLiteral("Radius"), Radius);
    Form->addRow(QStringLiteral("Strength"), StrengthRow);
    Form->addRow(QStringLiteral("Falloff"), Falloff);
    Form->addRow(QStringLiteral("Rate"), Rate);
    Form->addRow(QStringLiteral("Target Height"), TargetHeight);
    Form->addRow(QStringLiteral("Noise Scale"), NoiseScale);

    PushSettings();
    connect(Radius, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { PullSettings(); });
    connect(Strength, &QSlider::valueChanged, this, [this](int) { PullSettings(); });
    connect(Falloff, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { PullSettings(); });
    connect(Rate, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { PullSettings(); });
    connect(TargetHeight, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { PullSettings(); });
    connect(NoiseScale, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { PullSettings(); });
    return Box;
}

void TerrainToolsWindow::PullSettings()
{
    if(bPushing || !Radius || !SculptTools || !PaintTools || !Modes) { return; }
    TerrainTool::Settings& S = Tool.GetSettings();
    const bool bPaint = Modes->currentIndex() == 2;
    const int  Id     = bPaint ? PaintTools->checkedId() : SculptTools->checkedId();
    S.bLower       = Id == LowerId;
    S.Tool         = uint8_t(Id == LowerId ? GameCore::TT_Raise : std::max(Id, 0));
    S.Radius       = float(Radius->value());
    S.Strength     = float(Strength->value()) / 100.0f;
    S.Falloff      = uint8_t(Falloff->currentData().toInt());
    S.Rate         = float(Rate->value());
    S.TargetHeight = float(TargetHeight->value());
    S.NoiseScale   = float(NoiseScale->value());
    S.Layer        = uint8_t(std::clamp(Layers ? Layers->currentRow() : 0, 0, int(GameCore::TERRAIN_LAYERS) - 1));
    StrengthValue->setText(QStringLiteral("%1%").arg(Strength->value()));

    /* Only what the tool in front uses is editable. */
    Rate->setEnabled(!bPaint && (S.Tool == GameCore::TT_Raise || S.Tool == GameCore::TT_Noise));
    TargetHeight->setEnabled(!bPaint && S.Tool == GameCore::TT_SetHeight);
    NoiseScale->setEnabled(!bPaint && S.Tool == GameCore::TT_Noise);
}

void TerrainToolsWindow::PushSettings()
{
    bPushing = true;
    const TerrainTool::Settings& S = Tool.GetSettings();
    Radius->setValue(double(S.Radius));
    Strength->setValue(int(std::lround(S.Strength * 100.0f)));
    StrengthValue->setText(QStringLiteral("%1%").arg(Strength->value()));
    Falloff->setCurrentIndex(Falloff->findData(int(S.Falloff)));
    Rate->setValue(double(S.Rate));
    TargetHeight->setValue(double(S.TargetHeight));
    NoiseScale->setValue(double(S.NoiseScale));
    bPushing = false;
}

void TerrainToolsWindow::UpdateViewportTool()
{
    ViewportWidget* Viewport = EditorTerrain::GetHost().Viewport;
    const bool bBrush = isVisible() && Modes->currentIndex() != 0;
    BrushPanel->setVisible(Modes->currentIndex() != 0);
    HintLabel->setVisible(Modes->currentIndex() != 0);
    if(Viewport) { Viewport->SetTool(bBrush ? &Tool : (Viewport->GetTool() == &Tool ? nullptr : Viewport->GetTool())); }
}

void TerrainToolsWindow::RefreshTarget()
{
    const EditorTerrain::Target Target = EditorTerrain::FindEditTarget();
    EditorDocument* Document = EditorTerrain::GetHost().Document;
    if(!Target.IsValid() || !Document)
    {
        TargetLabel->setText(QStringLiteral("<b>No terrain selected.</b> Select one, or make one under Create. "
                                            "The brushes work on whichever terrain is under the cursor."));
    }
    else
    {
        const GameCore::TerrainData& Data = *Target.Data;
        const QString Name = QString::fromStdString(Document->GetWorld().GetEntity(Target.Entity).GetName());
        TargetLabel->setText(QStringLiteral("<b>%1</b> &mdash; %2 x %2 samples, %3 units apart (%4 a side)%5")
                             .arg(Name.toHtmlEscaped()).arg(Data.GetResolution()).arg(double(Data.GetSpacing()))
                             .arg(double(Data.GetSize())).arg(EditorTerrain::IsDirty(Target.Path) ? QStringLiteral(", unsaved") : QString()));
    }
    for(uint32_t Layer = 0; Layer < GameCore::TERRAIN_LAYERS && Layers; ++Layer)
    {
        const std::string Name = Target.IsValid() ? Target.Data->LayerNames[Layer] : std::string();
        static const char* const Channels[GameCore::TERRAIN_LAYERS] = { "R", "G", "B", "A" };
        Layers->item(int(Layer))->setText(QStringLiteral("%1  (%2)").arg(Name.empty() ? QStringLiteral("Layer %1").arg(Layer)
                                                                                         : QString::fromStdString(Name),
                                                                          QString::fromLatin1(Channels[Layer])));
    }
}

void TerrainToolsWindow::CreateTerrain()
{
    EditorTerrain::CreateSettings Settings;
    Settings.Name       = CreateName->text();
    Settings.Resolution = uint32_t(CreateSize->currentData().toInt());
    Settings.Spacing    = float(CreateSpacing->value());
    Settings.Height     = float(CreateHeight->value());
    Settings.bCollision = CreateCollision->isChecked();
    QString Error;
    if(!EditorTerrain::CreateTerrain(Settings, EngineCore::Vector3(0.0, 0.0, 0.0), Error).IsSet())
    {
        QMessageBox::warning(this, QStringLiteral("Create Terrain"), Error);
        return;
    }
    RefreshTarget();
    Modes->setCurrentIndex(1);
}

void TerrainToolsWindow::ImportHeightmap()
{
    const EditorTerrain::Target Target = EditorTerrain::FindEditTarget();
    if(!Target.IsValid())
    {
        QMessageBox::information(this, QStringLiteral("Import Heightmap"), QStringLiteral("Select the terrain to import into first."));
        return;
    }
    const QString File = QFileDialog::getOpenFileName(this, QStringLiteral("Import Heightmap"), QString(),
                                                      QStringLiteral("Images (*.png *.tif *.tiff *.pgm *.jpg)"));
    if(File.isEmpty()) { return; }
    QString Error;
    if(!EditorTerrain::ImportHeightmap(Target, File, float(ImportLow->value()), float(ImportHigh->value()), Error))
    {
        QMessageBox::warning(this, QStringLiteral("Import Heightmap"), Error);
    }
    RefreshTarget();
}

void TerrainToolsWindow::ExportHeightmap()
{
    const EditorTerrain::Target Target = EditorTerrain::FindEditTarget();
    if(!Target.IsValid())
    {
        QMessageBox::information(this, QStringLiteral("Export Heightmap"), QStringLiteral("Select the terrain to export first."));
        return;
    }
    const QString File = QFileDialog::getSaveFileName(this, QStringLiteral("Export Heightmap"), QStringLiteral("Heightmap.png"),
                                                      QStringLiteral("PNG (*.png)"));
    if(File.isEmpty()) { return; }
    QString Error;
    if(!EditorTerrain::ExportHeightmap(Target, File, Error)) { QMessageBox::warning(this, QStringLiteral("Export Heightmap"), Error); }
}

#endif // WITH_EDITOR
