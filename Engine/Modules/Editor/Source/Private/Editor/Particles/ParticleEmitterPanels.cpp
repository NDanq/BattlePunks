#if WITH_EDITOR

#include "Private/Editor/Particles/ParticleEmitterPanels.h"

#include <cmath>
#include <cstring>

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/GameCore/ComponentProperties.h"
#include "Private/Editor/Particles/ParticleDocument.h"
#include "Private/Editor/PropertyWidgets.h"

using RenderCore::ParticleEmitterDesc;

namespace
{
    /* Which emitter types a setting applies to (1 << EParticleType). */
    enum : uint8_t
    {
        FOR_SPRITE = 1 << RenderCore::PT_Sprite,
        FOR_MESH   = 1 << RenderCore::PT_Mesh,
        FOR_RIBBON = 1 << RenderCore::PT_Ribbon,
        FOR_BEAM   = 1 << RenderCore::PT_Beam,
        FOR_ALL    = FOR_SPRITE | FOR_MESH | FOR_RIBBON | FOR_BEAM,
        FOR_SPAWNS = FOR_SPRITE | FOR_MESH | FOR_RIBBON,   // everything that spawns particles
    };

    /* One setting of ParticleEmitterDesc as a property entry, so the Details
     * panel's widgets edit it; its tooltip; and when it is shown: for which
     * types, and (SpawnMode >= 0) only in that spawn mode. */
    struct EmitterSetting
    {
        GameCore::ComponentPropertyEntry Entry;
        const char*                      Tip       = nullptr;
        uint8_t                          Types     = FOR_ALL;
        int                              SpawnMode = -1;
    };

    template<typename TField>
    EmitterSetting Setting(const char* InName, TField ParticleEmitterDesc::* InMember, const char* InCategory, const uint8_t InTypes,
                           const char* InTip, const uint8_t InFlags = GameCore::PF_None)
    {
        /* Measured on a default-constructed one, as ComponentPropertyBinder does. */
        static const ParticleEmitterDesc Probe{};
        EmitterSetting Out;
        std::strncpy(Out.Entry.Name, InName, sizeof(Out.Entry.Name) - 1);
        std::strncpy(Out.Entry.Category, InCategory, sizeof(Out.Entry.Category) - 1);
        Out.Entry.Type   = GameCore::ScriptFieldTypeOf<TField>::Value;
        Out.Entry.Flags  = InFlags;
        Out.Entry.Offset = uint32_t(reinterpret_cast<const char*>(&(Probe.*InMember)) - reinterpret_cast<const char*>(&Probe));
        Out.Entry.Size   = uint32_t(sizeof(TField));
        Out.Tip          = InTip;
        Out.Types        = InTypes;
        return Out;
    }

    EmitterSetting Switch(const char* InName, uint8_t ParticleEmitterDesc::* InMember, const char* InCategory, const uint8_t InTypes,
                          const char* InTip)
    {
        EmitterSetting Out = Setting(InName, InMember, InCategory, InTypes, InTip);
        Out.Entry.Type = GameCore::EPropertyType::Flag;
        return Out;
    }

    /* A dropdown: InLabels name its values 0, 1, 2..., '|'-separated. */
    EmitterSetting Choice(const char* InName, uint8_t ParticleEmitterDesc::* InMember, const char* InCategory, const uint8_t InTypes,
                          const char* InLabels, const char* InTip)
    {
        EmitterSetting Out = Setting(InName, InMember, InCategory, InTypes, InTip);
        std::strncpy(Out.Entry.Choices, InLabels, sizeof(Out.Entry.Choices) - 1);
        return Out;
    }

    EmitterSetting InSpawnMode(EmitterSetting InSetting, const int InMode)
    {
        InSetting.SpawnMode = InMode;
        return InSetting;
    }

    const std::vector<EmitterSetting>& GetSettings()
    {
        using namespace RenderCore;
        static const std::vector<EmitterSetting> Settings = {
            Choice("Type", &ParticleEmitterDesc::Type, "Rendering", FOR_ALL, "Sprite|Mesh|Ribbon|Beam",
                    "What its particles are drawn as. Sprite: a quad each, turned to the view as Billboard says. Mesh: a mesh each, "
                    "turned as Mesh Orientation says. Ribbon: one strip through its particles in the order they were born, a trail. "
                    "Beam: one strip from the emitter to Beam End"),
            Setting("Mesh",     &ParticleEmitterDesc::Mesh, "Rendering", FOR_SPRITE | FOR_MESH,
                    "What each particle draws as: empty is a 1 x 1 quad for a Sprite, a 1-unit box for a Mesh", GameCore::PF_AssetMesh),
            Setting("Material", &ParticleEmitterDesc::Material, "Rendering", FOR_ALL,
                    "Its material; the particle's colour reaches it as Vertex Color. A ribbon's or beam's U runs across it, "
                    "V along each segment. Empty is DefaultLit", GameCore::PF_AssetMaterial),
            Setting("Offset",   &ParticleEmitterDesc::Offset, "Rendering", FOR_ALL,
                    "Where it emits from, relative to the entity, in the entity's own axes"),
            Choice ("Billboard", &ParticleEmitterDesc::Billboard, "Rendering", FOR_SPRITE,
                    "Face Camera|Vertical|Velocity Aligned|Fixed",
                    "How each sprite is turned. Face Camera: square to the view. Vertical: upright, turned about world up "
                    "toward the eye (flames, foliage). Velocity Aligned: along its motion, stretched by speed (sparks, rain). "
                    "Fixed: the quad's own axes"),
            Setting("Velocity Stretch", &ParticleEmitterDesc::VelocityStretch, "Rendering", FOR_SPRITE,
                    "Velocity Aligned: seconds of travel added to its length -- 0.05 at 400 units a second adds 20"),
            Choice ("Mesh Orientation", &ParticleEmitterDesc::MeshOrientation, "Rendering", FOR_MESH,
                    "Fixed|Random Spin|Align to Velocity",
                    "How each mesh is turned. Fixed: its own axes, spinning about +Y at Rotation Rate. Random Spin: about an "
                    "axis of its own (debris). Align to Velocity: its +Z along its motion (arrows)"),
            Switch ("Random Rotation", &ParticleEmitterDesc::bRandomRotation, "Rendering", FOR_SPRITE | FOR_MESH,
                    "Each starts at a random angle and spins either way"),
            Setting("Rotation Rate", &ParticleEmitterDesc::RotationRate, "Rendering", FOR_SPRITE | FOR_MESH,
                    "Degrees a second: a sprite in its own plane, a mesh about its axis"),

            Choice("Spawn Mode", &ParticleEmitterDesc::SpawnMode, "Emission", FOR_SPAWNS, "Continuous|Burst",
                    "Continuous: Spawn Rate a second while it emits. Burst: Burst Count at once, then again every Burst "
                    "Interval. Switching Emitting off and on starts the bursts over"),
            Setting("Max Particles",     &ParticleEmitterDesc::MaxParticles, "Emission", FOR_SPAWNS,
                    "How many it holds at once (1 to 2048); past that the oldest are replaced"),
            InSpawnMode(Setting("Spawn Rate", &ParticleEmitterDesc::SpawnRate, "Emission", FOR_SPAWNS, "Particles born a second"),
                        RenderCore::PSM_Continuous),
            InSpawnMode(Setting("Burst Count", &ParticleEmitterDesc::BurstCount, "Emission", FOR_SPAWNS,
                                "Particles born at once in each burst"), RenderCore::PSM_Burst),
            InSpawnMode(Setting("Burst Interval", &ParticleEmitterDesc::BurstInterval, "Emission", FOR_SPAWNS,
                                "Seconds from one burst to the next; 0: a single burst"), RenderCore::PSM_Burst),
            InSpawnMode(Setting("Burst Cycles", &ParticleEmitterDesc::BurstCycles, "Emission", FOR_SPAWNS,
                                "How many bursts, with an interval; 0: no end"), RenderCore::PSM_Burst),
            Setting("Lifetime",          &ParticleEmitterDesc::Lifetime, "Emission", FOR_SPAWNS, "How long each lives, in seconds"),
            Setting("Lifetime Variance", &ParticleEmitterDesc::LifetimeVariance, "Emission", FOR_SPAWNS,
                    "0 to 1: each lives Lifetime less up to this much of it, at random (keep a ribbon's at 0: it breaks where one dies early)"),
            Setting("Emitter Radius",    &ParticleEmitterDesc::EmitterRadius, "Emission", FOR_SPAWNS,
                    "Each is born anywhere in a sphere this big round the emitter"),
            Setting("Seed",              &ParticleEmitterDesc::Seed, "Emission", FOR_ALL, "Changes its random numbers (a beam's jitter)"),

            Setting("Velocity",        &ParticleEmitterDesc::Velocity, "Motion", FOR_SPAWNS,
                    "Each one's speed and direction at birth, in world units a second"),
            Setting("Velocity Spread", &ParticleEmitterDesc::VelocitySpread, "Motion", FOR_SPAWNS, "Plus up to this speed in a random direction"),
            Setting("Acceleration",    &ParticleEmitterDesc::Acceleration, "Motion", FOR_SPAWNS,
                    "Buoyancy up, gravity down: world units a second, a second"),
            Setting("Drag",            &ParticleEmitterDesc::Drag, "Motion", FOR_SPAWNS, "Velocity lost a second (1/s): smoke slows, sparks do not"),
            Setting("Turbulence",      &ParticleEmitterDesc::Turbulence, "Motion", FOR_SPAWNS,
                    "A swirl's acceleration, pushing each along a moving noise"),

            Setting("Beam End",         &ParticleEmitterDesc::BeamEnd, "Beam", FOR_BEAM,
                    "Where the beam ends, relative to the emitter in the entity's own axes (a component's Beam Target overrides it)"),
            Setting("Beam Segments",    &ParticleEmitterDesc::BeamSegments, "Beam", FOR_BEAM,
                    "How many pieces it is drawn in (1 to 128): more bends more smoothly"),
            Setting("Beam Noise",       &ParticleEmitterDesc::BeamNoise, "Beam", FOR_BEAM,
                    "How far its middle strays from the straight line, in world units; its ends stay put"),
            Setting("Beam Noise Speed", &ParticleEmitterDesc::BeamNoiseSpeed, "Beam", FOR_BEAM, "How fast the jitter changes"),

            Setting("Start Size",  &ParticleEmitterDesc::StartSize, "Over Life", FOR_ALL,
                    "Its size at birth, in world units across (a ribbon's width at its newest point, a beam's at the emitter)"),
            Setting("End Size",    &ParticleEmitterDesc::EndSize, "Over Life", FOR_ALL, "Its size at death (a beam's at its end)"),
            Setting("Start Color", &ParticleEmitterDesc::StartColor, "Over Life", FOR_ALL, "Its colour and alpha at birth (a beam's at the emitter)"),
            Setting("End Color",   &ParticleEmitterDesc::EndColor, "Over Life", FOR_ALL, "Its colour and alpha at death (a beam's at its end)"),
            Setting("Color Curve", &ParticleEmitterDesc::ColorCurve, "Over Life", FOR_ALL,
                    "The colour follows age^Curve: under 1 it reaches End Color sooner, over 1 later"),
            Setting("Fade In",     &ParticleEmitterDesc::FadeIn, "Over Life", FOR_ALL, "0 to 1: the share of its life over which alpha rises from 0"),
            Setting("Fade Out",    &ParticleEmitterDesc::FadeOut, "Over Life", FOR_ALL, "0 to 1: the share of its life over which alpha falls to 0"),
        };
        return Settings;
    }

    /* Whether InSetting applies to InEmitter, as its type and spawn mode are. */
    bool Applies(const EmitterSetting& InSetting, const ParticleEmitterDesc& InEmitter)
    {
        const uint8_t Type = InEmitter.Type < RenderCore::PT_Count ? InEmitter.Type : uint8_t(RenderCore::PT_Sprite);
        return (InSetting.Types & (1u << Type)) != 0 && (InSetting.SpawnMode < 0 || InSetting.SpawnMode == InEmitter.SpawnMode);
    }

    QToolButton* MakeButton(QWidget* InParent, const QString& InText, const QString& InTip)
    {
        auto* Button = new QToolButton(InParent);
        Button->setText(InText);
        Button->setToolTip(InTip);
        Button->setAutoRaise(true);
        return Button;
    }

    /* The reset arrow, as the Details panel draws it. */
    QToolButton* MakeResetButton(QWidget* InParent, const QString& InTip)
    {
        auto* Button = new QToolButton(InParent);
        Button->setText(QString(QChar(0x21BA)));
        Button->setToolTip(InTip);
        Button->setAutoRaise(true);
        Button->setFixedSize(22, 22);
        Button->setStyleSheet(QStringLiteral("QToolButton { color: #ffa51e; font-weight: bold; }"));
        QSizePolicy Policy = Button->sizePolicy();
        Policy.setRetainSizeWhenHidden(true);
        Button->setSizePolicy(Policy);
        Button->setVisible(false);
        return Button;
    }
}

// === Emitters ======================================================================

ParticleEmitterListPanel::ParticleEmitterListPanel(ParticleDocument& InDocument, QWidget* InParent)
    : QWidget(InParent)
    , Document(InDocument)
{
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(4, 4, 4, 4);

    List = new QListWidget(this);
    List->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    List->setContextMenuPolicy(Qt::CustomContextMenu);
    List->setToolTip(QStringLiteral("Each emitter runs on its own and is drawn with its own mesh and material. "
                                    "The check box turns one off; F2 or a double-click renames it."));
    Layout->addWidget(List, 1);

    auto* Buttons = new QHBoxLayout();
    Buttons->setSpacing(2);
    AddButton  = MakeButton(this, QStringLiteral("Add"), QStringLiteral("Add an emitter at its defaults"));
    DupButton  = MakeButton(this, QStringLiteral("Duplicate"), QStringLiteral("Copy the selected emitter, seeded apart"));
    DelButton  = MakeButton(this, QStringLiteral("Remove"), QStringLiteral("Remove the selected emitter"));
    UpButton   = MakeButton(this, QString(QChar(0x2191)), QStringLiteral("Move up: drawn earlier"));
    DownButton = MakeButton(this, QString(QChar(0x2193)), QStringLiteral("Move down: drawn later"));
    for(QToolButton* Each : { AddButton, DupButton, DelButton, UpButton, DownButton }) { Buttons->addWidget(Each); }
    Buttons->addStretch(1);
    Layout->addLayout(Buttons);

    connect(AddButton,  &QToolButton::clicked, this, [this]() { Document.AddEmitter(); });
    connect(DupButton,  &QToolButton::clicked, this, [this]() { Document.DuplicateEmitter(Document.GetSelected()); });
    connect(DelButton,  &QToolButton::clicked, this, [this]() { Document.RemoveEmitter(Document.GetSelected()); });
    connect(UpButton,   &QToolButton::clicked, this, [this]() { Document.MoveEmitter(Document.GetSelected(), -1); });
    connect(DownButton, &QToolButton::clicked, this, [this]() { Document.MoveEmitter(Document.GetSelected(), 1); });

    connect(List, &QListWidget::currentRowChanged, this, [this](const int InRow)
    {
        if(!bRebuilding && InRow >= 0) { Document.SetSelected(InRow); }
    });
    connect(List, &QListWidget::itemChanged, this, [this](QListWidgetItem* InItem)
    {
        if(bRebuilding) { return; }
        const int Row = List->row(InItem);
        const ParticleEmitterDesc* Emitter = Document.GetEmitter(Row);
        if(!Emitter) { return; }
        const bool bEnabled = InItem->checkState() == Qt::Checked;
        if(bEnabled != (Emitter->bEnabled != 0))
        {
            Document.SetEmitterEnabled(Row, bEnabled);
        }
        else
        {
            Document.RenameEmitter(Row, InItem->text());
            if(InItem->text() != QString::fromUtf8(Emitter->Name)) { Rebuild(); }   // refused: show the name it has
        }
    });
    connect(List, &QListWidget::customContextMenuRequested, this, [this](const QPoint& InAt)
    {
        QMenu Menu(this);
        Menu.addAction(QStringLiteral("Add Emitter"), this, [this]() { Document.AddEmitter(); })
            ->setEnabled(AddButton->isEnabled());
        Menu.addAction(QStringLiteral("Duplicate"), this, [this]() { Document.DuplicateEmitter(Document.GetSelected()); })
            ->setEnabled(DupButton->isEnabled());
        Menu.addAction(QStringLiteral("Rename"), this, [this]()
        {
            if(QListWidgetItem* Item = List->currentItem()) { List->editItem(Item); }
        })->setEnabled(List->currentItem() != nullptr);
        Menu.addAction(QStringLiteral("Remove"), this, [this]() { Document.RemoveEmitter(Document.GetSelected()); })
            ->setEnabled(DelButton->isEnabled());
        Menu.exec(List->viewport()->mapToGlobal(InAt));
    });

    connect(&Document, &ParticleDocument::EmittersChanged, this, &ParticleEmitterListPanel::Rebuild);
    connect(&Document, &ParticleDocument::SelectionChanged, this, [this](const int InIndex)
    {
        const QSignalBlocker Block(List);
        List->setCurrentRow(InIndex);
        UpdateButtons();
    });
    Rebuild();
}

void ParticleEmitterListPanel::Rebuild()
{
    bRebuilding = true;
    List->clear();
    for(int Index = 0; Index < Document.GetEmitterCount(); ++Index)
    {
        const ParticleEmitterDesc& Emitter = *Document.GetEmitter(Index);
        auto* Item = new QListWidgetItem(QString::fromUtf8(Emitter.Name), List);
        Item->setFlags(Item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        Item->setCheckState(Emitter.bEnabled ? Qt::Checked : Qt::Unchecked);
    }
    List->setCurrentRow(Document.GetSelected());
    bRebuilding = false;
    UpdateButtons();
}

void ParticleEmitterListPanel::UpdateButtons()
{
    const int  Selected = Document.GetSelected();
    const int  Count    = Document.GetEmitterCount();
    const bool bRoom    = Count < int(RenderCore::PARTICLE_SYSTEM_EMITTERS_MAX);
    AddButton->setEnabled(bRoom);
    DupButton->setEnabled(bRoom && Selected >= 0);
    DelButton->setEnabled(Selected >= 0);
    UpButton->setEnabled(Selected > 0);
    DownButton->setEnabled(Selected >= 0 && Selected + 1 < Count);
}

// === Details =======================================================================

ParticleEmitterDetailsPanel::ParticleEmitterDetailsPanel(ParticleDocument& InDocument, QWidget* InParent)
    : QWidget(InParent)
    , Document(InDocument)
{
    auto* Outer = new QVBoxLayout(this);
    Outer->setContentsMargins(0, 0, 0, 0);

    auto* Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    Outer->addWidget(Scroll, 1);

    Content = new QWidget(Scroll);
    ContentLayout = new QVBoxLayout(Content);
    ContentLayout->setContentsMargins(6, 6, 6, 6);
    Scroll->setWidget(Content);

    Budget = new QLabel(this);
    Budget->setWordWrap(true);
    Budget->setContentsMargins(8, 4, 8, 6);
    Outer->addWidget(Budget);

    connect(&Document, &ParticleDocument::SelectionChanged, this, &ParticleEmitterDetailsPanel::Rebuild);
    connect(&Document, &ParticleDocument::Changed, this, &ParticleEmitterDetailsPanel::Refresh);
    Rebuild();
}

void ParticleEmitterDetailsPanel::Rebuild()
{
    Rows.clear();
    while(QLayoutItem* Item = ContentLayout->takeAt(0))
    {
        delete Item->widget();
        delete Item;
    }

    const int Selected = Document.GetSelected();
    if(!Document.GetEmitter(Selected))
    {
        auto* None = new QLabel(Document.GetEmitterCount() == 0 ? QStringLiteral("No emitters: Add one in the Emitters list")
                                                                : QStringLiteral("Select an emitter"), Content);
        None->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        ContentLayout->addWidget(None);
        ContentLayout->addStretch(1);
        Refresh();
        return;
    }

    /* Looked up fresh on every read and write: the selected emitter's bytes. */
    ParticleDocument* Doc = &Document;
    const auto Resolve = [Doc, Selected]() -> uint8_t* { return reinterpret_cast<uint8_t*>(Doc->GetEmitter(Selected)); };

    std::vector<std::pair<QString, QFormLayout*>> Forms;
    const auto FormFor = [&](const QString& InCategory) -> QFormLayout*
    {
        for(const auto& Each : Forms)
        {
            if(Each.first == InCategory) { return Each.second; }
        }
        auto* Box  = new QGroupBox(InCategory, Content);
        auto* Form = new QFormLayout(Box);
        Form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        Form->setContentsMargins(6, 4, 0, 4);
        ContentLayout->addWidget(Box);
        Forms.emplace_back(InCategory, Form);
        return Form;
    };

    const ParticleEmitterDesc& Shown = *Document.GetEmitter(Selected);
    ShownType      = Shown.Type;
    ShownSpawnMode = Shown.SpawnMode;

    const std::vector<EmitterSetting>& Settings = GetSettings();
    for(size_t Index = 0; Index < Settings.size(); ++Index)
    {
        const EmitterSetting& Setting = Settings[Index];
        if(!Applies(Setting, Shown)) { continue; }
        auto* Field    = new QWidget(Content);
        auto* FieldRow = new QHBoxLayout(Field);
        FieldRow->setContentsMargins(0, 0, 0, 0);
        FieldRow->setSpacing(2);

        PropertyEditor* Editor = CreatePropertyEditor(Setting.Entry, Resolve, Field);
        if(!Editor)
        {
            delete Field;
            continue;
        }
        FieldRow->addWidget(Editor, 1);
        const QString Name = QString::fromLatin1(Setting.Entry.Name);
        QToolButton* Reset = MakeResetButton(Field, QStringLiteral("Changed from the default -- click to reset %1").arg(Name));
        FieldRow->addWidget(Reset);

        auto* Label = new QLabel(Name, Content);
        Label->setToolTip(QString::fromLatin1(Setting.Tip));
        Editor->setToolTip(QString::fromLatin1(Setting.Tip));
        FormFor(QString::fromLatin1(Setting.Entry.Category))->addRow(Label, Field);

        connect(Editor, &PropertyEditor::Written, this, [this, Name]()
        {
            Document.CommitLiveEdit(QStringLiteral("Edit %1").arg(Name));
        });
        connect(Reset, &QToolButton::clicked, this, [this, Index, Name]()
        {
            const GameCore::ComponentPropertyEntry& Entry = GetSettings()[Index].Entry;
            const int At = Document.GetSelected();
            static const ParticleEmitterDesc Defaults{};
            Document.Edit(QStringLiteral("Reset %1").arg(Name), [&Entry, At](RenderCore::ParticleSystemDesc& InOut)
            {
                std::memcpy(reinterpret_cast<uint8_t*>(&InOut.Emitters[size_t(At)]) + Entry.Offset,
                            reinterpret_cast<const uint8_t*>(&Defaults) + Entry.Offset, Entry.Size);
            });
        });

        Rows.push_back({ Editor, Reset, Index });
    }
    ContentLayout->addStretch(1);
    Refresh();
}

void ParticleEmitterDetailsPanel::Refresh()
{
    const ParticleEmitterDesc* Emitter = Document.GetEmitter(Document.GetSelected());

    /* A new type or spawn mode shows other settings: rebuilt once this edit's signals are done with the widgets. */
    if(Emitter && !Rows.empty() && (Emitter->Type != ShownType || Emitter->SpawnMode != ShownSpawnMode))
    {
        if(!bRebuildQueued)
        {
            bRebuildQueued = true;
            QTimer::singleShot(0, this, [this]()
            {
                bRebuildQueued = false;
                Rebuild();
            });
        }
        return;
    }
    static const ParticleEmitterDesc Defaults{};
    for(const Row& Each : Rows)
    {
        Each.Editor->Refresh(true);
        if(Emitter)
        {
            const GameCore::ComponentPropertyEntry& Entry = GetSettings()[Each.Entry].Entry;
            Each.Reset->setVisible(!IsPropertyAtDefault(Entry, reinterpret_cast<const uint8_t*>(Emitter) + Entry.Offset,
                                                        reinterpret_cast<const uint8_t*>(&Defaults) + Entry.Offset));
        }
    }

    if(!Emitter)
    {
        Budget->clear();
        return;
    }
    const RenderCore::ParticleEmitterSettings Settings = Emitter->ToSettings();
    if(Settings.Type == RenderCore::PT_Beam)
    {
        Budget->setText(QStringLiteral("A beam of <b>%1</b> segments: %2 points, redrawn every frame.")
                            .arg(Settings.BeamSegments).arg(Settings.BeamSegments + 1));
        return;
    }
    const float Held  = Emitter->GetSteadyStateCount();
    /* What it would hold with no cap: the rate times a mean life, or the bursts alive at once. */
    const float MeanLife = Settings.Lifetime * (1.0f - 0.5f * Settings.LifetimeVariance);
    float Wants = Settings.SpawnRate * MeanLife;
    if(Settings.SpawnMode == RenderCore::PSM_Burst)
    {
        float Overlap = Settings.BurstInterval > 0.0f ? std::max(1.0f, std::ceil(MeanLife / Settings.BurstInterval)) : 1.0f;
        if(Settings.BurstInterval > 0.0f && Settings.BurstCycles > 0) { Overlap = std::min(Overlap, float(Settings.BurstCycles)); }
        Wants = float(Settings.BurstCount) * Overlap;
    }
    QString Text = QStringLiteral("About <b>%1</b> of %2 particles once it has run %3 s.")
                       .arg(int(std::lround(Held))).arg(Settings.MaxParticles).arg(double(Settings.Lifetime), 0, 'g', 3);
    if(Wants > float(Settings.MaxParticles) * 1.02f)
    {
        Text += QStringLiteral("<br><span style='color:#e0a030'>Full: it would hold about %1, so the oldest are replaced "
                               "before they die. Raise Max Particles (up to %2), or spawn fewer or shorter-lived ones.</span>")
                    .arg(int(std::lround(Wants))).arg(RenderCore::PARTICLE_MAX_PER_SYSTEM);
    }
    Budget->setText(Text);
}

#endif // WITH_EDITOR
