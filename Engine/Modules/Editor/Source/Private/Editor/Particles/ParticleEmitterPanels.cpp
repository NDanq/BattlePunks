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
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/GameCore/ComponentProperties.h"
#include "Private/Editor/Particles/ParticleDocument.h"
#include "Private/Editor/PropertyWidgets.h"

using RenderCore::ParticleEmitterDesc;

namespace
{
    /* One setting of ParticleEmitterDesc as a property entry, so the Details
     * panel's widgets edit it; and what its tooltip says. */
    struct EmitterSetting
    {
        GameCore::ComponentPropertyEntry Entry;
        const char*                      Tip = nullptr;
    };

    template<typename TField>
    EmitterSetting Setting(const char* InName, TField ParticleEmitterDesc::* InMember, const char* InCategory,
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
        return Out;
    }

    EmitterSetting Switch(const char* InName, uint8_t ParticleEmitterDesc::* InMember, const char* InCategory, const char* InTip)
    {
        EmitterSetting Out = Setting(InName, InMember, InCategory, InTip);
        Out.Entry.Type = GameCore::EPropertyType::Flag;
        return Out;
    }

    const std::vector<EmitterSetting>& GetSettings()
    {
        static const std::vector<EmitterSetting> Settings = {
            Setting("Mesh",     &ParticleEmitterDesc::Mesh, "Rendering",
                    "What each particle draws as: empty is a 1 x 1 quad (Engine/Content/Meshes/Quad.nmmesh)", GameCore::PF_AssetMesh),
            Setting("Material", &ParticleEmitterDesc::Material, "Rendering",
                    "Its material; the particle's colour reaches it as Vertex Color. Empty is DefaultLit", GameCore::PF_AssetMaterial),
            Switch ("Face Camera", &ParticleEmitterDesc::bFaceCamera, "Rendering",
                    "Turn each particle to the view (a quad: a billboard); off, it stands upright"),
            Setting("Offset",   &ParticleEmitterDesc::Offset, "Rendering",
                    "Where it emits from, relative to the entity, in the entity's own axes"),

            Setting("Max Particles",     &ParticleEmitterDesc::MaxParticles, "Emission",
                    "How many it holds at once (1 to 2048); past that the oldest are replaced"),
            Setting("Spawn Rate",        &ParticleEmitterDesc::SpawnRate, "Emission", "Particles born a second"),
            Setting("Lifetime",          &ParticleEmitterDesc::Lifetime, "Emission", "How long each lives, in seconds"),
            Setting("Lifetime Variance", &ParticleEmitterDesc::LifetimeVariance, "Emission",
                    "0 to 1: each lives Lifetime less up to this much of it, at random"),
            Setting("Emitter Radius",    &ParticleEmitterDesc::EmitterRadius, "Emission",
                    "Each is born anywhere in a sphere this big round the emitter"),
            Setting("Seed",              &ParticleEmitterDesc::Seed, "Emission", "Changes its random numbers"),

            Setting("Velocity",        &ParticleEmitterDesc::Velocity, "Motion", "Each one's speed and direction at birth, in world units a second"),
            Setting("Velocity Spread", &ParticleEmitterDesc::VelocitySpread, "Motion", "Plus up to this speed in a random direction"),
            Setting("Acceleration",    &ParticleEmitterDesc::Acceleration, "Motion", "Buoyancy up, gravity down: world units a second, a second"),
            Setting("Drag",            &ParticleEmitterDesc::Drag, "Motion", "Velocity lost a second (1/s): smoke slows, sparks do not"),
            Setting("Turbulence",      &ParticleEmitterDesc::Turbulence, "Motion", "A swirl's acceleration, pushing each along a moving noise"),

            Setting("Start Size",  &ParticleEmitterDesc::StartSize, "Over Life", "Its size at birth, in world units across"),
            Setting("End Size",    &ParticleEmitterDesc::EndSize, "Over Life", "Its size at death"),
            Setting("Start Color", &ParticleEmitterDesc::StartColor, "Over Life", "Its colour and alpha at birth"),
            Setting("End Color",   &ParticleEmitterDesc::EndColor, "Over Life", "Its colour and alpha at death"),
            Setting("Color Curve", &ParticleEmitterDesc::ColorCurve, "Over Life",
                    "The colour follows age^Curve: under 1 it reaches End Color sooner, over 1 later"),
            Setting("Fade In",     &ParticleEmitterDesc::FadeIn, "Over Life", "0 to 1: the share of its life over which alpha rises from 0"),
            Setting("Fade Out",    &ParticleEmitterDesc::FadeOut, "Over Life", "0 to 1: the share of its life over which alpha falls to 0"),
        };
        return Settings;
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

    const std::vector<EmitterSetting>& Settings = GetSettings();
    for(size_t Index = 0; Index < Settings.size(); ++Index)
    {
        const EmitterSetting& Setting = Settings[Index];
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
    const float Held  = Emitter->GetSteadyStateCount();
    const float Wants = Settings.SpawnRate * Settings.Lifetime * (1.0f - 0.5f * Settings.LifetimeVariance);
    QString Text = QStringLiteral("About <b>%1</b> of %2 particles once it has run %3 s.")
                       .arg(int(std::lround(Held))).arg(Settings.MaxParticles).arg(double(Settings.Lifetime), 0, 'g', 3);
    if(Wants > float(Settings.MaxParticles) * 1.02f)
    {
        Text += QStringLiteral("<br><span style='color:#e0a030'>Full: Spawn Rate x Lifetime wants about %1, so the oldest "
                               "are replaced before they die. Raise Max Particles (up to %2) or lower the rate or lifetime.</span>")
                    .arg(int(std::lround(Wants))).arg(RenderCore::PARTICLE_MAX_PER_SYSTEM);
    }
    Budget->setText(Text);
}

#endif // WITH_EDITOR
