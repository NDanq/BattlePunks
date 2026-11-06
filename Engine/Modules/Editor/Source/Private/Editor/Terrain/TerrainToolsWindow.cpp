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
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/Editor/EditorDocument.h"
#include "Public/Editor/ViewportWidget.h"
#include "Public/GameCore/World.h"

#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/EditorIcons.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Terrain/FoliageEditing.h"
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
    const bool bFoliagePaint = InName.compare(QStringLiteral("Foliage"), Qt::CaseInsensitive) == 0
                            || InName.compare(QStringLiteral("FoliagePaint"), Qt::CaseInsensitive) == 0;
    if(bFoliagePaint || InName.compare(QStringLiteral("FoliageErase"), Qt::CaseInsensitive) == 0)
    {
        Dock->show();
        Dock->raise();
        Dock->Modes->setCurrentIndex(3);
        if(QAbstractButton* Button = Dock->FoliageTools->button(bFoliagePaint ? GameCore::FT_Paint : GameCore::FT_Erase)) { Button->setChecked(true); }
        Dock->PullSettings();
        Dock->UpdateViewportTool();
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
    Modes->addTab(MakeFoliagePage(), EditorIcons::Get(EditorIcons::EIcon::Foliage), QStringLiteral("Foliage"));
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
        connect(Document, &EditorDocument::HistoryChanged, this, [this]() { if(!bLoadingFoliage) { RefreshFoliage(); } });
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
    S.bFoliage = Modes->currentIndex() == 3;
    if(FoliageTools) { S.FoliageTool = uint8_t(std::max(FoliageTools->checkedId(), 0)); }
    S.FoliageTypes.clear();
    for(int Row = 0; FoliageList && Row < FoliageList->count(); ++Row)
    {
        if(FoliageList->item(Row)->checkState() == Qt::Checked) { S.FoliageTypes.push_back(uint16_t(Row)); }
    }
    if(S.bFoliage && FoliageList && FoliageList->count() > 0 && S.FoliageTypes.empty())
    {
        S.FoliageTypes.push_back(uint16_t(FoliageList->count()));   // none checked: an index of no type, so nothing paints
    }
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
    const bool bSculpt = !bPaint && !S.bFoliage;
    Rate->setEnabled(bSculpt && (S.Tool == GameCore::TT_Raise || S.Tool == GameCore::TT_Noise));
    TargetHeight->setEnabled(bSculpt && S.Tool == GameCore::TT_SetHeight);
    NoiseScale->setEnabled(bSculpt && S.Tool == GameCore::TT_Noise);
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
    RefreshFoliage();
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

// === Foliage =================================================================

QWidget* TerrainToolsWindow::MakeFoliagePage()
{
    auto* Page   = new QWidget(this);
    auto* Layout = new QVBoxLayout(Page);

    auto* Row = new QHBoxLayout();
    FoliageTools = new QButtonGroup(Page);
    FoliageTools->setExclusive(true);
    const struct { const char* Label; int Id; EditorIcons::EIcon Icon; const char* Tip; } Tools[] = {
        { "Paint", GameCore::FT_Paint, EditorIcons::EIcon::Foliage, "Place the checked types under the brush, as close as their Spacing (Shift: erase)" },
        { "Erase", GameCore::FT_Erase, EditorIcons::EIcon::FoliageErase, "Remove the checked types under the brush (Shift: paint)" },
    };
    for(const auto& Each : Tools)
    {
        auto* Button = new QToolButton(Page);
        Button->setText(QString::fromLatin1(Each.Label));
        Button->setIcon(EditorIcons::Get(Each.Icon));
        Button->setIconSize(QSize(24, 24));
        Button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        Button->setCheckable(true);
        Button->setAutoRaise(true);
        Button->setMinimumWidth(72);
        Button->setToolTip(QString::fromLatin1(Each.Tip));
        Button->setObjectName(QStringLiteral("FoliageTool_") + QString::fromLatin1(Each.Label));
        FoliageTools->addButton(Button, Each.Id);
        Row->addWidget(Button);
    }
    FoliageTools->button(GameCore::FT_Paint)->setChecked(true);
    connect(FoliageTools, &QButtonGroup::idClicked, this, [this](int) { PullSettings(); });
    Row->addStretch(1);
    Layout->addLayout(Row);

    FoliageList = new QListWidget(Page);
    FoliageList->setObjectName(QStringLiteral("FoliageTypes"));
    FoliageList->setToolTip(QStringLiteral("The terrain's foliage types: the checked ones are painted or erased; the selected one's settings are below"));
    FoliageList->setMaximumHeight(130);
    connect(FoliageList, &QListWidget::itemChanged, this, [this](QListWidgetItem*) { if(!bLoadingFoliage) { PullSettings(); } });
    connect(FoliageList, &QListWidget::currentRowChanged, this, [this](int) { if(!bLoadingFoliage) { RefreshFoliage(); } });
    Layout->addWidget(FoliageList);

    auto* Buttons = new QHBoxLayout();
    auto* Add     = new QPushButton(EditorIcons::Get(EditorIcons::EIcon::Add), QStringLiteral("Add Mesh..."), Page);
    auto* Remove  = new QPushButton(QStringLiteral("Remove"), Page);
    Add->setToolTip(QStringLiteral("A new type for an .nmmesh in the project (the terrain's foliage file is made on first use)"));
    Remove->setToolTip(QStringLiteral("Remove the selected type and every instance of it"));
    connect(Add, &QPushButton::clicked, this, [this]() { AddFoliageType(); });
    connect(Remove, &QPushButton::clicked, this, [this]() { RemoveFoliageType(); });
    Buttons->addWidget(Add);
    Buttons->addWidget(Remove);
    Buttons->addStretch(1);
    Layout->addLayout(Buttons);

    FoliageStats = new QLabel(Page);
    FoliageStats->setWordWrap(true);
    FoliageStats->setEnabled(false);
    Layout->addWidget(FoliageStats);

    FoliageForm = new QGroupBox(QStringLiteral("Type"), Page);
    auto* Form = new QFormLayout(FoliageForm);
    const auto Spin = [&](const double InMin, const double InMax, const int InDecimals, const double InStep, const QString& InSuffix,
                          const QString& InTip)
    {
        auto* Box = new QDoubleSpinBox(FoliageForm);
        Box->setRange(InMin, InMax);
        Box->setDecimals(InDecimals);
        Box->setSingleStep(InStep);
        Box->setSuffix(InSuffix);
        Box->setToolTip(InTip);
        Box->setKeyboardTracking(false);
        connect(Box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { CommitFoliageType(); });
        return Box;
    };
    const auto Line = [&](const QString& InTip)
    {
        auto* Edit = new QLineEdit(FoliageForm);
        Edit->setToolTip(InTip);
        connect(Edit, &QLineEdit::editingFinished, this, [this]() { CommitFoliageType(); });
        return Edit;
    };
    FoliageName     = Line(QStringLiteral("Its name in the list and in undo steps"));
    FoliageMesh[0]  = Line(QStringLiteral("The mesh drawn near the camera (a runtime .nmmesh path)"));
    FoliageMesh[1]  = Line(QStringLiteral("Its mesh where the terrain draws at LOD 1; empty: LOD 0's"));
    FoliageMesh[2]  = Line(QStringLiteral("Its mesh where the terrain draws at LOD 2; empty: LOD 1's"));
    FoliageMaterial = Line(QStringLiteral("A material graph for every section; empty: the mesh's own"));
    FoliageSpacing  = Spin(1.0, 100000.0, 0, 25.0, QStringLiteral(" units"), QStringLiteral("The distance kept between two of this type"));
    FoliageScaleMin = Spin(0.01, 100.0, 2, 0.05, QString(), QStringLiteral("The smallest an instance is made"));
    FoliageScaleMax = Spin(0.01, 100.0, 2, 0.05, QString(), QStringLiteral("The largest an instance is made"));
    FoliageAlign    = Spin(0.0, 1.0, 2, 0.1, QString(), QStringLiteral("0: upright; 1: leaning with the ground"));
    FoliageSlope    = Spin(0.0, 90.0, 0, 5.0, QStringLiteral(" deg"), QStringLiteral("Not placed on ground steeper than this"));
    FoliageSink     = Spin(-10000.0, 10000.0, 1, 5.0, QStringLiteral(" units"), QStringLiteral("Pushed this far into the ground (times its scale)"));
    FoliageCull     = Spin(0.0, 1000000.0, 0, 500.0, QStringLiteral(" units"), QStringLiteral("Not drawn past this distance from the camera; 0: always"));
    FoliageDensity[0] = Spin(0.0, 1.0, 2, 0.05, QString(), QStringLiteral("The fraction drawn where the terrain draws at LOD 1"));
    FoliageDensity[1] = Spin(0.0, 1.0, 2, 0.05, QString(), QStringLiteral("The fraction drawn where the terrain draws at LOD 2"));
    FoliageColor    = Spin(0.0, 1.0, 2, 0.05, QString(), QStringLiteral("Each instance darkened by up to this"));
    FoliageShadows  = new QCheckBox(QStringLiteral("Cast shadows"), FoliageForm);
    FoliageYaw      = new QCheckBox(QStringLiteral("Random yaw"), FoliageForm);
    connect(FoliageShadows, &QCheckBox::toggled, this, [this](bool) { CommitFoliageType(); });
    connect(FoliageYaw, &QCheckBox::toggled, this, [this](bool) { CommitFoliageType(); });

    auto* Scale = new QHBoxLayout();
    Scale->addWidget(FoliageScaleMin);
    Scale->addWidget(new QLabel(QStringLiteral("to"), FoliageForm));
    Scale->addWidget(FoliageScaleMax);
    auto* Density = new QHBoxLayout();
    Density->addWidget(FoliageDensity[0]);
    Density->addWidget(new QLabel(QStringLiteral("then"), FoliageForm));
    Density->addWidget(FoliageDensity[1]);
    auto* Checks = new QHBoxLayout();
    Checks->addWidget(FoliageShadows);
    Checks->addWidget(FoliageYaw);

    Form->addRow(QStringLiteral("Name"), FoliageName);
    Form->addRow(QStringLiteral("Mesh"), FoliageMesh[0]);
    Form->addRow(QStringLiteral("LOD 1 Mesh"), FoliageMesh[1]);
    Form->addRow(QStringLiteral("LOD 2 Mesh"), FoliageMesh[2]);
    Form->addRow(QStringLiteral("Material"), FoliageMaterial);
    Form->addRow(QStringLiteral("Spacing"), FoliageSpacing);
    Form->addRow(QStringLiteral("Scale"), Scale);
    Form->addRow(QStringLiteral("Align to Ground"), FoliageAlign);
    Form->addRow(QStringLiteral("Max Slope"), FoliageSlope);
    Form->addRow(QStringLiteral("Sink"), FoliageSink);
    Form->addRow(QStringLiteral("Cull Distance"), FoliageCull);
    Form->addRow(QStringLiteral("LOD 1 / 2 Density"), Density);
    Form->addRow(QStringLiteral("Color Variation"), FoliageColor);
    Form->addRow(QString(), Checks);
    Layout->addWidget(FoliageForm);
    Layout->addStretch(1);

    /* The counts while painting, and why a stroke could not start. */
    FoliageTimer = new QTimer(this);
    FoliageTimer->setInterval(400);
    connect(FoliageTimer, &QTimer::timeout, this, [this]()
    {
        if(!isVisible() || Modes->currentIndex() != 3) { return; }
        if(!Tool.LastError.isEmpty())
        {
            FoliageStats->setText(Tool.LastError);
            return;
        }
        const EditorFoliage::Target Foliage = EditorFoliage::FindFoliage(EditorTerrain::FindEditTarget());
        if(!Foliage.IsValid()) { return; }
        std::lock_guard<std::mutex> Guard(Foliage.Data->GetLock());
        FoliageStats->setText(QStringLiteral("%1 instances%2").arg(qulonglong(Foliage.Data->GetInstanceCount()))
                              .arg(EditorTerrain::IsDirty(Foliage.Path) ? QStringLiteral(", unsaved") : QString()));
    });
    FoliageTimer->start();
    return Page;
}

void TerrainToolsWindow::RefreshFoliage()
{
    if(!FoliageList) { return; }
    const EditorFoliage::Target Foliage = EditorFoliage::FindFoliage(EditorTerrain::FindEditTarget());
    std::vector<GameCore::FoliageType> Types;
    size_t Count = 0;
    if(Foliage.IsValid())
    {
        std::lock_guard<std::mutex> Guard(Foliage.Data->GetLock());
        Types = Foliage.Data->GetTypes();
        Count = Foliage.Data->GetInstanceCount();
    }

    bLoadingFoliage = true;
    const int Current = std::clamp(FoliageList->currentRow(), 0, std::max(int(Types.size()) - 1, 0));
    std::vector<bool> Checked;
    for(int Row = 0; Row < FoliageList->count(); ++Row) { Checked.push_back(FoliageList->item(Row)->checkState() == Qt::Checked); }
    FoliageList->clear();
    for(size_t Each = 0; Each < Types.size(); ++Each)
    {
        auto* Item = new QListWidgetItem(QString::fromStdString(Types[Each].Name), FoliageList);
        Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable);
        Item->setCheckState(Each < Checked.size() && !Checked[Each] ? Qt::Unchecked : Qt::Checked);
        Item->setToolTip(QString::fromStdString(Types[Each].Mesh[0]));
    }
    if(!Types.empty()) { FoliageList->setCurrentRow(Current); }

    FoliageForm->setEnabled(!Types.empty());
    FoliageStats->setText(!EditorTerrain::FindEditTarget().IsValid() ? QStringLiteral("Select a terrain first.")
                        : !Foliage.IsValid() ? QStringLiteral("No foliage yet: Add Mesh... makes the terrain's foliage file.")
                        : QStringLiteral("%1 instances").arg(qulonglong(Count)));
    if(!Types.empty())
    {
        const GameCore::FoliageType& Type = Types[size_t(Current)];
        FoliageName->setText(QString::fromStdString(Type.Name));
        for(int Lod = 0; Lod < 3; ++Lod) { FoliageMesh[Lod]->setText(QString::fromStdString(Type.Mesh[Lod])); }
        FoliageMaterial->setText(QString::fromStdString(Type.Material));
        FoliageSpacing->setValue(double(Type.Spacing));
        FoliageScaleMin->setValue(double(Type.ScaleMin));
        FoliageScaleMax->setValue(double(Type.ScaleMax));
        FoliageAlign->setValue(double(Type.AlignToNormal));
        FoliageSlope->setValue(double(Type.MaxSlope));
        FoliageSink->setValue(double(Type.Sink));
        FoliageCull->setValue(double(Type.CullDistance));
        FoliageDensity[0]->setValue(double(Type.LODDensity[1]));
        FoliageDensity[1]->setValue(double(Type.LODDensity[2]));
        FoliageColor->setValue(double(Type.ColorVariation));
        FoliageShadows->setChecked(Type.bCastShadows != 0);
        FoliageYaw->setChecked(Type.bRandomYaw != 0);
    }
    bLoadingFoliage = false;
    PullSettings();
}

void TerrainToolsWindow::CommitFoliageType()
{
    if(bLoadingFoliage || !FoliageList || FoliageList->currentRow() < 0) { return; }
    const EditorFoliage::Target Foliage = EditorFoliage::FindFoliage(EditorTerrain::FindEditTarget());
    if(!Foliage.IsValid()) { return; }
    const uint16_t Index = uint16_t(FoliageList->currentRow());
    GameCore::FoliageType Type;
    {
        std::lock_guard<std::mutex> Guard(Foliage.Data->GetLock());
        if(Index >= Foliage.Data->GetTypes().size()) { return; }
        Type = Foliage.Data->GetTypes()[Index];
    }
    const GameCore::FoliageType Was = Type;
    Type.Name = FoliageName->text().trimmed().isEmpty() ? Type.Name : FoliageName->text().trimmed().toStdString();
    for(int Lod = 0; Lod < 3; ++Lod) { Type.Mesh[Lod] = FoliageMesh[Lod]->text().trimmed().toStdString(); }
    if(Type.Mesh[0].empty()) { Type.Mesh[0] = Was.Mesh[0]; }   // LOD 0 is not optional
    Type.Material       = FoliageMaterial->text().trimmed().toStdString();
    Type.Spacing        = float(FoliageSpacing->value());
    Type.ScaleMin       = float(std::min(FoliageScaleMin->value(), FoliageScaleMax->value()));
    Type.ScaleMax       = float(std::max(FoliageScaleMin->value(), FoliageScaleMax->value()));
    Type.AlignToNormal  = float(FoliageAlign->value());
    Type.MaxSlope       = float(FoliageSlope->value());
    Type.Sink           = float(FoliageSink->value());
    Type.CullDistance   = float(FoliageCull->value());
    Type.LODDensity[1]  = float(FoliageDensity[0]->value());
    Type.LODDensity[2]  = float(FoliageDensity[1]->value());
    Type.ColorVariation = float(FoliageColor->value());
    Type.bCastShadows   = FoliageShadows->isChecked() ? 1 : 0;
    Type.bRandomYaw     = FoliageYaw->isChecked() ? 1 : 0;

    const bool bSame = Type.Name == Was.Name && Type.Material == Was.Material && Type.Spacing == Was.Spacing
                    && Type.ScaleMin == Was.ScaleMin && Type.ScaleMax == Was.ScaleMax && Type.AlignToNormal == Was.AlignToNormal
                    && Type.MaxSlope == Was.MaxSlope && Type.Sink == Was.Sink && Type.CullDistance == Was.CullDistance
                    && Type.LODDensity[1] == Was.LODDensity[1] && Type.LODDensity[2] == Was.LODDensity[2]
                    && Type.ColorVariation == Was.ColorVariation && Type.bCastShadows == Was.bCastShadows
                    && Type.bRandomYaw == Was.bRandomYaw && Type.Mesh[0] == Was.Mesh[0] && Type.Mesh[1] == Was.Mesh[1]
                    && Type.Mesh[2] == Was.Mesh[2];
    if(bSame) { return; }
    bLoadingFoliage = true;   // the history signal would reload the form under the edit
    EditorFoliage::SetType(Foliage, Index, Type);
    bLoadingFoliage = false;
    if(Type.Name != Was.Name && FoliageList->item(Index)) { FoliageList->item(Index)->setText(QString::fromStdString(Type.Name)); }
}

void TerrainToolsWindow::AddFoliageType()
{
    const EditorTerrain::Target Terrain = EditorTerrain::FindEditTarget();
    if(!Terrain.IsValid())
    {
        QMessageBox::information(this, QStringLiteral("Add Foliage"), QStringLiteral("Select the terrain to add foliage to first."));
        return;
    }
    const QString File = QFileDialog::getOpenFileName(this, QStringLiteral("Add Foliage"), EditorGraphs::GetProjectRoot(),
                                                      QStringLiteral("Meshes (*.nmmesh)"));
    if(File.isEmpty()) { return; }
    const QString Runtime = EditorContent::ToRuntimePath(File);
    QString Error;
    const EditorFoliage::Target Foliage = Runtime.isEmpty() ? EditorFoliage::Target() : EditorFoliage::EnsureFoliage(Terrain, Error);
    const int Made = Foliage.IsValid() ? EditorFoliage::AddType(Foliage, Runtime, QString(), Error) : -1;
    if(Made < 0)
    {
        QMessageBox::warning(this, QStringLiteral("Add Foliage"), Runtime.isEmpty() ? QStringLiteral("The mesh must be inside the project.") : Error);
        return;
    }
    RefreshFoliage();
    FoliageList->setCurrentRow(Made);
}

void TerrainToolsWindow::RemoveFoliageType()
{
    const EditorFoliage::Target Foliage = EditorFoliage::FindFoliage(EditorTerrain::FindEditTarget());
    if(!Foliage.IsValid() || !FoliageList || FoliageList->currentRow() < 0) { return; }
    EditorFoliage::RemoveType(Foliage, uint16_t(FoliageList->currentRow()));
    RefreshFoliage();
}

#endif // WITH_EDITOR
