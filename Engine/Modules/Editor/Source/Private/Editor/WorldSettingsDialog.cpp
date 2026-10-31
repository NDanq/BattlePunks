#if WITH_EDITOR

#include "Private/Editor/WorldSettingsDialog.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/DetailMode.h"
#include "Public/GameCore/WorldSettings.h"
#include "Public/NimbleEngine/WorldList.h"

namespace
{
    /* World units are centimetres; density reads better per metre. */
    constexpr double UnitsPerMetre = 100.0;

    /* The game mode choice that is not a mode. */
    const char* const NoModeText = "(none)";
}

WorldSettingsDialog::WorldSettingsDialog(EditorDocument& InDocument, QWidget* InParent)
    : QDialog(InParent)
    , Document(InDocument)
{
    setWindowTitle(QStringLiteral("World Settings"));

    /* Modes the game module registers -- loaded for authoring, so known here. */
    const QString Mode = QString::fromStdString(Document.GetWorldSettings().GameMode);
    GameMode = new QComboBox(this);
    GameMode->setEditable(true);
    GameMode->addItem(QString::fromLatin1(NoModeText));
    for(size_t Index = 0; Index < NimbleEngine::GetGameModeCount(); ++Index)
    {
        GameMode->addItem(QString::fromUtf8(NimbleEngine::GetGameModeName(Index)));
    }
    GameMode->setCurrentText(Mode.isEmpty() ? QString::fromLatin1(NoModeText) : Mode);
    GameMode->setToolTip(QStringLiteral("The rules a level of this world runs: a game mode the game module registers "
                                        "(NIMBLE_REGISTER_GAME_MODE). Listed: those the loaded module has"));

    auto* GameGroup = new QGroupBox(QStringLiteral("Game"), this);
    auto* GameForm  = new QFormLayout(GameGroup);
    GameForm->addRow(QStringLiteral("Game mode"), GameMode);

    DetailMode = new QComboBox(this);
    DetailMode->addItem(QStringLiteral("The player's (Scalability.DetailMode)"), int(GameCore::WORLD_DETAIL_USE_GLOBAL));
    for(uint8_t Mode = 0; Mode < GameCore::DETAIL_MODE_COUNT; ++Mode)
    {
        DetailMode->addItem(QStringLiteral("Always %1").arg(QString::fromLatin1(GameCore::DetailModeName(GameCore::EDetailMode(Mode)))), int(Mode));
    }
    DetailMode->setCurrentIndex((std::max)(0, DetailMode->findData(int(Document.GetWorldSettings().DetailMode))));
    DetailMode->setToolTip(QStringLiteral("Which entities' Detail Mode buckets render in this world: the player's tier, "
                                          "or one this world is locked to -- a showcase at Ultra, a test level at Low. "
                                          "The tier's config values still follow the player's"));
    GameForm->addRow(QStringLiteral("Detail mode"), DetailMode);

    const GameCore::WorldLightingSettings& Lighting = Document.GetWorldSettings().Lighting;

    BakedLighting = new QCheckBox(QStringLiteral("Baked lighting"), this);
    BakedLighting->setToolTip(QStringLiteral("Static lights light static entities' meshes through a bake "
                                             "(Build > Bake Lightmaps), not every frame"));
    BakedLighting->setChecked(Lighting.bBakedLighting);

    TexelsPerMetre = new QDoubleSpinBox(this);
    TexelsPerMetre->setRange(0.1, 400.0);
    TexelsPerMetre->setDecimals(1);
    TexelsPerMetre->setSuffix(QStringLiteral(" per metre"));
    TexelsPerMetre->setValue(double(Lighting.TexelsPerUnit) * UnitsPerMetre);
    TexelsPerMetre->setToolTip(QStringLiteral("Lightmap texels across a metre of surface: sharper shadows, a longer bake"));

    Bounces = new QSpinBox(this);
    Bounces->setRange(0, 8);
    Bounces->setValue(Lighting.Bounces);
    Bounces->setToolTip(QStringLiteral("Times light bounces off surfaces after it first lands; 0 is direct light only"));

    SamplesPerTexel = new QSpinBox(this);
    SamplesPerTexel->setRange(1, 4096);
    SamplesPerTexel->setValue(Lighting.SamplesPerTexel);
    SamplesPerTexel->setToolTip(QStringLiteral("Rays per texel for bounced light: less noise, a longer bake"));

    ProbeSpacing = new QDoubleSpinBox(this);
    ProbeSpacing->setRange(10.0, 10000.0);
    ProbeSpacing->setDecimals(0);
    ProbeSpacing->setSuffix(QStringLiteral(" units"));
    ProbeSpacing->setValue(double(Lighting.ProbeSpacing));
    ProbeSpacing->setToolTip(QStringLiteral("Distance between the light probes that light dynamic entities"));

    MaxAtlasSize = new QComboBox(this);
    for(int Size = 128; Size <= 4096; Size *= 2)
    {
        MaxAtlasSize->addItem(QStringLiteral("%1 x %1").arg(Size), Size);
    }
    MaxAtlasSize->setCurrentIndex(std::max(0, MaxAtlasSize->findData(Lighting.MaxAtlasSize)));
    MaxAtlasSize->setToolTip(QStringLiteral("The size of each lightmap texture; a world needing more space gets more of them"));

    auto* Group = new QGroupBox(QStringLiteral("Lighting"), this);
    auto* Form  = new QFormLayout(Group);
    Form->addRow(BakedLighting);
    Form->addRow(QStringLiteral("Lightmap density"), TexelsPerMetre);
    Form->addRow(QStringLiteral("Bounces"), Bounces);
    Form->addRow(QStringLiteral("Samples per texel"), SamplesPerTexel);
    Form->addRow(QStringLiteral("Probe spacing"), ProbeSpacing);
    Form->addRow(QStringLiteral("Lightmap size"), MaxAtlasSize);

    const auto EnableFields = [this](const bool bInOn)
    {
        for(QWidget* Each : { static_cast<QWidget*>(TexelsPerMetre), static_cast<QWidget*>(Bounces),
                              static_cast<QWidget*>(SamplesPerTexel), static_cast<QWidget*>(ProbeSpacing),
                              static_cast<QWidget*>(MaxAtlasSize) })
        {
            Each->setEnabled(bInOn);
        }
    };
    connect(BakedLighting, &QCheckBox::toggled, this, EnableFields);
    EnableFields(BakedLighting->isChecked());

    auto* Note = new QLabel(QStringLiteral("Only static entities and static lights are baked. "
                                           "Saved with the world; Build > Bake Lightmaps makes the bake."), this);
    Note->setWordWrap(true);

    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(Buttons, &QDialogButtonBox::accepted, this, [this]() { Apply(); accept(); });
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* Layout = new QVBoxLayout(this);
    Layout->addWidget(GameGroup);
    Layout->addWidget(Group);
    Layout->addWidget(Note);
    Layout->addWidget(Buttons);
}

void WorldSettingsDialog::Apply()
{
    GameCore::WorldSettings Settings = Document.GetWorldSettings();
    const QString Mode = GameMode->currentText().trimmed();
    Settings.GameMode = Mode == QLatin1String(NoModeText) ? std::string() : Mode.toStdString();
    Settings.DetailMode = uint8_t(DetailMode->currentData().toInt());

    GameCore::WorldLightingSettings& Lighting = Settings.Lighting;
    Lighting.bBakedLighting  = BakedLighting->isChecked();
    Lighting.TexelsPerUnit   = float(TexelsPerMetre->value() / UnitsPerMetre);
    Lighting.Bounces         = Bounces->value();
    Lighting.SamplesPerTexel = SamplesPerTexel->value();
    Lighting.ProbeSpacing    = float(ProbeSpacing->value());
    Lighting.MaxAtlasSize    = MaxAtlasSize->currentData().toInt();
    Document.SetWorldSettings(Settings);
}

#endif // WITH_EDITOR
