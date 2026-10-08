#if WITH_EDITOR

#include "Public/Editor/DetailsPanel.h"

#include <algorithm>
#include <cstring>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Prefab.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Private/Editor/PropertyWidgets.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"

using GameCore::ComponentTypeID;
using GameCore::EntityID;

namespace
{
    constexpr int RefreshIntervalMs = 100;

    /* The reset arrow: small, flat, and holding its space while hidden so the
     * editors do not shift sideways as values change. */
    QToolButton* MakeResetButton(QWidget* InParent, const QString& InTip)
    {
        auto* Button = new QToolButton(InParent);
        Button->setText(QString(QChar(0x21BA)));   // anticlockwise open circle arrow
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

    uint64_t ArrayKey(const ComponentTypeID InType, const GameCore::ComponentPropertyEntry& InEntry)
    {
        return (uint64_t(InType) << 32) | InEntry.Offset;
    }

    /* A struct array's members, in declaration order (ComponentProperties.h). */
    std::vector<GameCore::ComponentPropertyEntry> CollectMembers(const ComponentTypeID InType,
                                                                 const GameCore::ComponentPropertyEntry& InHead)
    {
        std::vector<GameCore::ComponentPropertyEntry> Members;
        size_t Count = 0;
        const GameCore::ComponentPropertyEntry* Entries = GameCore::FindComponentProperties(InType, Count);
        size_t Head = Count;
        for(size_t Index = 0; Index < Count; ++Index)
        {
            if(Entries[Index].IsStructArray() && Entries[Index].Offset == InHead.Offset && !Entries[Index].IsMember())
            {
                Head = Index;
                break;
            }
        }
        for(size_t Index = Head + 1; Index < Count; ++Index)
        {
            if(Entries[Index].Parent == Head) { Members.push_back(Entries[Index]); }
        }
        return Members;
    }

    /* One past the last element that differs from the default: those in use.
     * A struct element differs when any of its members does (its padding
     * does not count). */
    size_t CountUsedElements(const ComponentTypeID InType, const GameCore::ComponentPropertyEntry& InEntry,
                             const uint8_t* InComponent, const uint8_t* InDefaults)
    {
        const std::vector<GameCore::ComponentPropertyEntry> Members =
            InEntry.IsStructArray() ? CollectMembers(InType, InEntry) : std::vector<GameCore::ComponentPropertyEntry>();
        size_t Used = 0;
        for(size_t Index = 0; Index < InEntry.ArrayCount; ++Index)
        {
            bool bDiffers = false;
            if(InEntry.IsStructArray())
            {
                for(const GameCore::ComponentPropertyEntry& Each : Members)
                {
                    const GameCore::ComponentPropertyEntry Member = InEntry.GetMember(Index, Each);
                    bDiffers = bDiffers || !IsPropertyAtDefault(Member, InComponent + Member.Offset, InDefaults + Member.Offset);
                }
            }
            else
            {
                const GameCore::ComponentPropertyEntry Element = InEntry.GetElement(Index);
                bDiffers = !IsPropertyAtDefault(Element, InComponent + Element.Offset, InDefaults + Element.Offset);
            }
            if(bDiffers)
            {
                Used = Index + 1;
            }
        }
        return Used;
    }
}

DetailsPanel::DetailsPanel(EditorDocument& InDocument, QWidget* InParent)
    : QWidget(InParent)
    , Document(InDocument)
{
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(4, 4, 4, 4);

    Header = new QLabel(this);
    Header->setTextFormat(Qt::RichText);
    Layout->addWidget(Header);

    Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    Layout->addWidget(Scroll, 1);

    connect(&Document, &EditorDocument::SelectionChanged, this, [this](EntityID) { Rebuild(); });
    connect(&Document, &EditorDocument::WorldReset,       this, [this]() { Rebuild(); });
    connect(&Document, &EditorDocument::EntitiesChanged,  this, [this]() { RebuildIfComponentsChanged(); });

    connect(&RefreshTimer, &QTimer::timeout, this, &DetailsPanel::RefreshValues);
    RefreshTimer.start(RefreshIntervalMs);

    Rebuild();
}

std::vector<ComponentTypeID> DetailsPanel::CollectComponentTypes(const EntityID InEntity) const
{
    std::vector<ComponentTypeID> Types;
    GameCore::World& World = Document.GetWorld();
    if(!World.IsAlive(InEntity))
    {
        return Types;
    }

    const GameCore::Entity E = World.GetEntity(InEntity);
    const size_t TypeCount = GameCore::GetComponentTypeCount();
    for(size_t Type = 0; Type < TypeCount && Type < GameCore::COMPONENT_TYPE_MAX; ++Type)
    {
        if(E.HasComponentByType(ComponentTypeID(Type)))
        {
            Types.push_back(ComponentTypeID(Type));
        }
    }
    return Types;
}

uint8_t* DetailsPanel::ResolveComponent(const ComponentTypeID InType) const
{
    GameCore::World& World = Document.GetWorld();
    if(!World.IsAlive(ShownEntity))
    {
        return nullptr;
    }
    return static_cast<uint8_t*>(World.GetEntity(ShownEntity).GetComponentByType(InType));
}

const DetailsPanel::ComponentSection* DetailsPanel::FindSection(const ComponentTypeID InType) const
{
    for(const ComponentSection& Section : Sections)
    {
        if(Section.Type == InType)
        {
            return &Section;
        }
    }
    return nullptr;
}

void DetailsPanel::UpdateHeader()
{
    if(!ShownEntity.IsSet())
    {
        Header->setText(QStringLiteral("<span style='color:gray'>Nothing selected</span>"));
        return;
    }
    Header->setText(QStringLiteral("<b>%1</b> &nbsp;<span style='color:gray'>#%2 gen %3</span>")
                    .arg(Document.GetDisplayName(ShownEntity).toHtmlEscaped())
                    .arg(ShownEntity.Index).arg(ShownEntity.Generation));
}

void DetailsPanel::RebuildIfComponentsChanged()
{
    if(ShownEntity != Document.GetSelection() || CollectComponentTypes(ShownEntity) != ShownTypes)
    {
        Rebuild();
        return;
    }
    /* Same components -- only the members may have changed. */
    UpdateHeader();
    RefreshEntitySection();
}

void DetailsPanel::QueueRebuild()
{
    if(bRebuildQueued)
    {
        return;
    }
    bRebuildQueued = true;
    QTimer::singleShot(0, this, [this]()
    {
        bRebuildQueued = false;
        Rebuild();
    });
}

void DetailsPanel::Rebuild()
{
    /* Another entity's lists start from what it uses. */
    if(Document.GetSelection() != ShownEntity)
    {
        ArrayShown.clear();
    }
    const int Scrolled = Scroll->verticalScrollBar()->value();

    Rows.clear();
    Sections.clear();
    Arrays.clear();
    NameEdit      = nullptr;
    MobilityCombo = nullptr;
    ShownEntity = Document.GetSelection();
    ShownTypes  = CollectComponentTypes(ShownEntity);

    /* A fresh body each time: takeWidget/delete drops every old editor at once. */
    delete Scroll->takeWidget();
    Body       = new QWidget();
    BodyLayout = new QVBoxLayout(Body);
    BodyLayout->setContentsMargins(0, 0, 0, 0);
    BodyLayout->setSpacing(6);

    UpdateHeader();
    if(ShownEntity.IsSet())
    {
        BodyLayout->addWidget(BuildEntitySection(ShownEntity));
        for(const ComponentTypeID Type : ShownTypes)
        {
            BodyLayout->addWidget(BuildComponentSection(ShownEntity, Type));
        }
        BodyLayout->addWidget(BuildAddComponentButton());
    }

    BodyLayout->addStretch(1);
    Scroll->setWidget(Body);

    /* The same entity rebuilt (an entry added, say) stays where it was. */
    QTimer::singleShot(0, this, [this, Scrolled]() { Scroll->verticalScrollBar()->setValue(Scrolled); });

    RefreshValues();
}

QWidget* DetailsPanel::BuildEntitySection(const EntityID InEntity)
{
    auto* Section = new QFrame(Body);
    Section->setFrameShape(QFrame::StyledPanel);
    auto* SectionLayout = new QVBoxLayout(Section);
    SectionLayout->setContentsMargins(4, 2, 4, 4);
    SectionLayout->setSpacing(2);

    auto* Title = new QLabel(QStringLiteral("<b>Entity</b>"), Section);
    SectionLayout->addWidget(Title);

    auto* Content = new QWidget(Section);
    auto* Form    = new QFormLayout(Content);
    Form->setContentsMargins(14, 0, 0, 0);
    Form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    SectionLayout->addWidget(Content);

    // --- Name ----------------------------------------------------------------
    NameEdit = new QLineEdit(Content);
    NameEdit->setPlaceholderText(QStringLiteral("(unnamed)"));
    connect(NameEdit, &QLineEdit::editingFinished, this, [this, InEntity]()
    {
        if(NameEdit)
        {
            Document.RenameEntity(InEntity, NameEdit->text());
        }
    });
    Form->addRow(QStringLiteral("Name"), NameEdit);

    // --- Mobility: Static entities are cached by the renderer, never ticked ---
    MobilityCombo = new QComboBox(Content);
    MobilityCombo->addItem(QStringLiteral("Dynamic"), int(GameCore::EMobility::Dynamic));
    MobilityCombo->addItem(QStringLiteral("Static"),  int(GameCore::EMobility::Static));
    MobilityCombo->addItem(QStringLiteral("Stationary"), int(GameCore::EMobility::Stationary));
    MobilityCombo->setToolTip(QStringLiteral("Static: level geometry that never moves in play. Its draws are kept in the "
                                             "renderer's static lists, its bounds are not refitted, and a prefab instance "
                                             "rooted on it gets no phase ticks and is static throughout; with baked "
                                             "lighting, it is baked. "
                                             "Stationary: a light that stays put but stays live -- only the light it bounces "
                                             "is baked. Applies to every selected entity."));
    MobilityCombo->setFocusPolicy(Qt::StrongFocus);
    connect(MobilityCombo, QOverload<int>::of(&QComboBox::activated), this, [this](const int InIndex)
    {
        if(MobilityCombo)
        {
            Document.SetEntitiesMobility(Document.GetSelectedEntities(),
                                         GameCore::EMobility(MobilityCombo->itemData(InIndex).toInt()));
        }
    });
    Form->addRow(QStringLiteral("Mobility"), MobilityCombo);

    // --- Prefab: what the entity's instance was placed from -------------------
    const GameCore::Entity Shown = Document.GetWorld().GetEntity(InEntity);
    if(Shown.IsInInstance())
    {
        const GameCore::PrefabInstance Instance = Document.GetWorld().GetInstance(Shown.GetInstance());
        const QString PrefabPath = QString::fromUtf8(Instance.GetPrefabPath());

        auto* PrefabField = new QWidget(Content);
        auto* PrefabRow   = new QHBoxLayout(PrefabField);
        PrefabRow->setContentsMargins(0, 0, 0, 0);
        PrefabRow->setSpacing(4);
        auto* PrefabLabel = new QLabel(Instance.GetRoot().GetID() == InEntity
                                           ? PrefabPath
                                           : QStringLiteral("%1 (in %2)").arg(PrefabPath, QString::fromUtf8(Instance.GetRoot().GetName())),
                                       PrefabField);
        PrefabLabel->setToolTip(QStringLiteral("Placed from this prefab. Bold properties override it here; right-click one to revert it."));
        PrefabRow->addWidget(PrefabLabel, 1);
        if(!PrefabPath.isEmpty())
        {
            auto* Open = new QToolButton(PrefabField);
            Open->setText(QStringLiteral("Open"));
            Open->setToolTip(QStringLiteral("Edit the prefab in the Prefab Editor"));
            Open->setAutoRaise(true);
            connect(Open, &QToolButton::clicked, this, [this, PrefabPath]()
            {
                EditorPrefabs::OpenPrefab(EditorPrefabs::ToProjectPath(PrefabPath), window());
            });
            PrefabRow->addWidget(Open);
        }
        Form->addRow(QStringLiteral("Prefab"), PrefabField);
    }

    return Section;
}

void DetailsPanel::RefreshEntitySection()
{
    if(!NameEdit)
    {
        return;
    }
    GameCore::World& World = Document.GetWorld();
    const GameCore::Entity E = World.GetEntity(ShownEntity);

    /* Not while the user is typing into it. */
    const QString Name = QString::fromUtf8(E.GetName());
    if(!NameEdit->hasFocus() && NameEdit->text() != Name)
    {
        NameEdit->setText(Name);
    }
    if(MobilityCombo)
    {
        const int Index = MobilityCombo->findData(int(E.GetMobility()));
        if(Index >= 0 && MobilityCombo->currentIndex() != Index)
        {
            MobilityCombo->setCurrentIndex(Index);
        }
        /* Static from its prefab instance's root: changed there, not here. */
        MobilityCombo->setEnabled(E.GetMobility() == E.GetAuthoredMobility());
    }
}

QWidget* DetailsPanel::BuildComponentSection(const EntityID InEntity, const ComponentTypeID InType)
{
    const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(InType);
    const QString TypeName = Info ? QString::fromLatin1(Info->Name) : QStringLiteral("Component %1").arg(InType);

    /* The type's default-constructed bytes, once per section -- what every
     * reset arrow in it compares against and restores. */
    ComponentSection State;
    State.Type = InType;
    if(Info)
    {
        auto Defaults = std::make_shared<std::vector<uint8_t>>(Info->SizeBytes);
        if(GameCore::GetComponentPropertyDefaults(InType, Defaults->data()))
        {
            State.Defaults = Defaults;
        }
    }

    auto* Section = new QFrame(Body);
    Section->setFrameShape(QFrame::StyledPanel);
    auto* SectionLayout = new QVBoxLayout(Section);
    SectionLayout->setContentsMargins(4, 2, 4, 4);
    SectionLayout->setSpacing(2);

    // --- Header: collapse toggle, Reset, Remove ------------------------------
    auto* HeaderRow = new QHBoxLayout();
    HeaderRow->setContentsMargins(0, 0, 0, 0);

    /* On a prefab instance's entity: what the prefab holds, for overrides. */
    bool bAddedToInstance = false;
    const GameCore::Entity Owner = Document.GetWorld().GetEntity(InEntity);
    if(Info && Owner.IsInInstance())
    {
        const GameCore::PrefabInstance Instance = Document.GetWorld().GetInstance(Owner.GetInstance());
        const bool bPlacement = InType == GameCore::TransformComponent::StaticType().ID && Instance.GetRoot().GetID() == InEntity;
        if(!bPlacement)
        {
            auto Values = std::make_shared<std::vector<uint8_t>>(Info->SizeBytes);
            if(GameCore::ReadPrefabTemplateComponent(Document.GetWorld(), Instance.GetID(), Owner.GetPrefabLocalId(), InType, Values->data()))
            {
                State.PrefabValues = Values;
            }
            else
            {
                bAddedToInstance = true;
            }
        }
    }

    auto* Toggle = new QToolButton(Section);
    Toggle->setText(bAddedToInstance ? QStringLiteral("%1 (added here)").arg(TypeName) : TypeName);
    if(bAddedToInstance)
    {
        Toggle->setToolTip(QStringLiteral("This instance has %1; the prefab does not").arg(TypeName));
    }
    Toggle->setCheckable(true);
    Toggle->setChecked(true);
    Toggle->setArrowType(Qt::DownArrow);
    Toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    Toggle->setAutoRaise(true);
    Toggle->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; }"));
    HeaderRow->addWidget(Toggle);
    HeaderRow->addStretch(1);

    if(State.Defaults)
    {
        State.ResetAll = new QToolButton(Section);
        State.ResetAll->setText(QStringLiteral("Reset"));
        State.ResetAll->setToolTip(QStringLiteral("Reset every property of %1 to its default").arg(TypeName));
        State.ResetAll->setAutoRaise(true);
        State.ResetAll->setEnabled(false);
        connect(State.ResetAll, &QToolButton::clicked, this, [this, InType]() { ResetComponent(InType); });
        HeaderRow->addWidget(State.ResetAll);
    }

    if(State.PrefabValues)
    {
        State.RevertAll = new QToolButton(Section);
        State.RevertAll->setText(QStringLiteral("Revert to Prefab"));
        State.RevertAll->setToolTip(QStringLiteral("Set every overridden property of %1 back to the prefab's").arg(TypeName));
        State.RevertAll->setAutoRaise(true);
        State.RevertAll->setEnabled(false);
        connect(State.RevertAll, &QToolButton::clicked, this, [this, InType]() { RevertComponentToPrefab(InType); });
        HeaderRow->addWidget(State.RevertAll);
    }

    auto* Remove = new QToolButton(Section);
    Remove->setText(QString(QChar(0x2715)));   // multiplication x; code points keep the source ASCII
    Remove->setToolTip(QStringLiteral("Remove %1 from this entity").arg(TypeName));
    Remove->setAutoRaise(true);
    connect(Remove, &QToolButton::clicked, this, [this, InEntity, InType]()
    {
        /* The World's OnComponentRemoved then rebuilds this panel on the
         * next turn, not inside this click. */
        Document.RemoveComponent(InEntity, InType);
    });
    HeaderRow->addWidget(Remove);

    SectionLayout->addLayout(HeaderRow);

    // --- Properties ----------------------------------------------------------
    /* The uncategorised properties' form first, then a titled box per
     * category (Props.Category), in order of first use. */
    auto* Content       = new QWidget(Section);
    auto* ContentLayout = new QVBoxLayout(Content);
    ContentLayout->setContentsMargins(14, 0, 0, 0);
    ContentLayout->setSpacing(4);
    SectionLayout->addWidget(Content);

    const auto MakeForm = []()
    {
        auto* Made = new QFormLayout();
        Made->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        Made->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        return Made;
    };
    QFormLayout* Form = MakeForm();
    Form->setContentsMargins(0, 0, 0, 0);
    ContentLayout->addLayout(Form);

    std::vector<std::pair<QString, QFormLayout*>> CategoryForms;
    const auto FormFor = [&](const GameCore::ComponentPropertyEntry& InEntry) -> QFormLayout*
    {
        if(!InEntry.HasCategory()) { return Form; }

        const QString Category = QString::fromLatin1(InEntry.Category);
        for(const auto& Each : CategoryForms)
        {
            if(Each.first == Category) { return Each.second; }
        }
        auto* Box = new QGroupBox(Category, Content);
        QFormLayout* BoxForm = MakeForm();
        BoxForm->setContentsMargins(6, 4, 0, 4);
        Box->setLayout(BoxForm);
        ContentLayout->addWidget(Box);
        CategoryForms.emplace_back(Category, BoxForm);
        return BoxForm;
    };

    connect(Toggle, &QToolButton::toggled, Content, [Toggle, Content](const bool bInOpen)
    {
        Toggle->setArrowType(bInOpen ? Qt::DownArrow : Qt::RightArrow);
        Content->setVisible(bInOpen);
    });

    Sections.push_back(State);

    size_t Count = 0;
    const GameCore::ComponentPropertyEntry* Entries = GameCore::FindComponentProperties(InType, Count);
    if(Count == 0)
    {
        auto* None = new QLabel(QStringLiteral("No editor-visible properties"), Content);
        None->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
        Form->addRow(None);
        return Section;
    }

    /* Looked up fresh on every read and write -- see PropertyWidgets.h. */
    EditorDocument* Doc = &Document;
    auto Resolve = [Doc, InEntity, InType]() -> uint8_t*
    {
        GameCore::World& World = Doc->GetWorld();
        if(!World.IsAlive(InEntity))
        {
            return nullptr;
        }
        return static_cast<uint8_t*>(World.GetEntity(InEntity).GetComponentByType(InType));
    };

    /* One editor row: the editor, its reset arrow, then InExtra (an array
     * element's remove button). */
    const auto AddRow = [&](const GameCore::ComponentPropertyEntry& Entry, const QString& InLabel, QToolButton* InExtra)
    {
        auto* Field  = new QWidget(Content);
        auto* FieldRow = new QHBoxLayout(Field);
        FieldRow->setContentsMargins(0, 0, 0, 0);
        FieldRow->setSpacing(2);

        PropertyEditor* Editor = CreatePropertyEditor(Entry, Resolve, Field);
        if(!Editor)
        {
            delete Field;
            delete InExtra;
            return;
        }
        FieldRow->addWidget(Editor, 1);

        PropertyRow Row;
        Row.Editor = Editor;
        Row.Type   = InType;
        Row.Entry  = Entry;

        if(Entry.IsReadOnly())
        {
            Editor->setToolTip(QStringLiteral("Read-only"));
            /* Keeps read-only editors aligned with writable ones. */
            FieldRow->addSpacing(24);
        }
        else if(State.Defaults)
        {
            Row.Reset = MakeResetButton(Field, QStringLiteral("Changed from the default -- click to reset %1")
                                                .arg(QString::fromLatin1(Entry.Name)));
            FieldRow->addWidget(Row.Reset);
            const size_t RowIndex = Rows.size();
            connect(Row.Reset, &QToolButton::clicked, this, [this, RowIndex]()
            {
                if(RowIndex < Rows.size())
                {
                    ResetProperty(Rows[RowIndex]);
                }
            });
        }

        if(InExtra)
        {
            InExtra->setParent(Field);
            FieldRow->addWidget(InExtra);
        }

        auto* Label = new QLabel(InLabel, Content);
        Row.Label = Label;
        if(State.PrefabValues && !Entry.IsReadOnly())
        {
            Label->setContextMenuPolicy(Qt::CustomContextMenu);
            const size_t RowIndex = Rows.size();
            connect(Label, &QLabel::customContextMenuRequested, this, [this, RowIndex, Label](const QPoint& InAt)
            {
                if(RowIndex >= Rows.size()) { return; }
                QMenu Menu(this);
                QAction* Revert = Menu.addAction(QStringLiteral("Revert to Prefab"));
                Revert->setEnabled(IsOverridden(Rows[RowIndex]));
                if(Menu.exec(Label->mapToGlobal(InAt)) == Revert)
                {
                    RevertPropertyToPrefab(Rows[RowIndex]);
                }
            });
        }
        FormFor(Entry)->addRow(Label, Field);
        Rows.push_back(Row);

        connect(Editor, &PropertyEditor::Written, this, [Doc, InEntity, InType]()
        {
            Doc->NotifyPropertyEdited(InEntity, InType);
        });
    };

    for(size_t Index = 0; Index < Count; ++Index)
    {
        const GameCore::ComponentPropertyEntry& Entry = Entries[Index];
        const QString Name = QString::fromLatin1(Entry.Name);
        if(Entry.IsMember())
        {
            continue;   // shown inside its struct array's elements, below
        }
        if(!Entry.IsArray())
        {
            AddRow(Entry, Name, nullptr);
            continue;
        }

        /* An array: the entries in use and any added since, then "+ Add". */
        const uint8_t* Live = Resolve();
        const size_t   Used = Live && State.Defaults ? CountUsedElements(InType, Entry, Live, State.Defaults->data()) : 0;
        size_t& Shown = ArrayShown[ArrayKey(InType, Entry)];
        Shown = std::min<size_t>(std::max(Shown, Used), Entry.ArrayCount);
        Arrays.push_back(ArrayList{ InType, Entry, Shown });

        auto* Heading    = new QWidget(Content);
        auto* HeadingRow = new QHBoxLayout(Heading);
        HeadingRow->setContentsMargins(0, 0, 0, 0);
        auto* Summary = new QLabel(Shown == 0 ? QStringLiteral("Empty") : Shown == 1 ? QStringLiteral("1 entry")
                                                                        : QStringLiteral("%1 entries").arg(Shown), Heading);
        Summary->setStyleSheet(QStringLiteral("color: gray;"));
        HeadingRow->addWidget(Summary, 1);
        if(!Entry.IsReadOnly())
        {
            auto* Add = new QToolButton(Heading);
            Add->setText(QStringLiteral("+ Add"));
            Add->setAutoRaise(true);
            Add->setEnabled(Shown < Entry.ArrayCount);
            Add->setToolTip(Shown < Entry.ArrayCount ? QStringLiteral("Add an entry to %1").arg(Name)
                                                     : QStringLiteral("%1 holds at most %2").arg(Name).arg(Entry.ArrayCount));
            const uint64_t Key = ArrayKey(InType, Entry);
            connect(Add, &QToolButton::clicked, this, [this, Key]()
            {
                ++ArrayShown[Key];
                QueueRebuild();
            });
            HeadingRow->addWidget(Add);
        }
        FormFor(Entry)->addRow(Name, Heading);

        for(size_t Element = 0; Element < Shown; ++Element)
        {
            QToolButton* Remove = nullptr;
            if(!Entry.IsReadOnly())
            {
                Remove = new QToolButton();
                Remove->setText(QString(QChar(0x2715)));   // multiplication x
                Remove->setAutoRaise(true);
                Remove->setFixedSize(22, 22);
                Remove->setToolTip(QStringLiteral("Remove entry %1 (the ones after it move up)").arg(Element));
                connect(Remove, &QToolButton::clicked, this, [this, InType, Entry, Element]()
                {
                    RemoveArrayElement(InType, Entry, Element);
                });
            }
            if(!Entry.IsStructArray())
            {
                AddRow(Entry.GetElement(Element), QStringLiteral("    %1").arg(Element), Remove);
                continue;
            }

            /* A struct element: a heading with its remove button, then one
             * row per member. */
            auto* ElementHeading    = new QWidget(Content);
            auto* ElementHeadingRow = new QHBoxLayout(ElementHeading);
            ElementHeadingRow->setContentsMargins(0, 0, 0, 0);
            ElementHeadingRow->addStretch(1);
            if(Remove)
            {
                Remove->setParent(ElementHeading);
                ElementHeadingRow->addWidget(Remove);
            }
            auto* ElementLabel = new QLabel(QStringLiteral("    %1").arg(Element), Content);
            ElementLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
            FormFor(Entry)->addRow(ElementLabel, ElementHeading);

            for(const GameCore::ComponentPropertyEntry& Member : CollectMembers(InType, Entry))
            {
                AddRow(Entry.GetMember(Element, Member), QStringLiteral("        %1").arg(QString::fromLatin1(Member.Name)), nullptr);
            }
        }
    }

    return Section;
}

QWidget* DetailsPanel::BuildAddComponentButton()
{
    auto* Button = new QToolButton(Body);
    Button->setText(QStringLiteral("+ Add Component"));
    Button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    Button->setPopupMode(QToolButton::InstantPopup);
    Button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    Button->setMinimumHeight(28);

    /* Filled when opened, not now: another panel (or a script) may add or
     * remove a component between this rebuild and the click. */
    auto* Menu = new QMenu(Button);
    connect(Menu, &QMenu::aboutToShow, this, [this, Menu]() { PopulateAddMenu(Menu); });
    Button->setMenu(Menu);
    return Button;
}

void DetailsPanel::PopulateAddMenu(QMenu* InMenu)
{
    InMenu->clear();

    GameCore::World& World = Document.GetWorld();
    if(!World.IsAlive(ShownEntity))
    {
        return;
    }
    const GameCore::Entity E = World.GetEntity(ShownEntity);

    const size_t TypeCount = GameCore::GetComponentTypeCount();
    for(size_t Index = 0; Index < TypeCount && Index < GameCore::COMPONENT_TYPE_MAX; ++Index)
    {
        const ComponentTypeID Type = ComponentTypeID(Index);
        const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(Type);
        if(!Info || !GameCore::IsComponentAddable(Type) || E.HasComponentByType(Type))
        {
            continue;
        }

        QAction* Action = InMenu->addAction(QString::fromLatin1(Info->Name));
        const EntityID Target = ShownEntity;
        connect(Action, &QAction::triggered, this, [this, Target, Type]()
        {
            Document.AddComponent(Target, Type);
        });
    }

    if(InMenu->isEmpty())
    {
        InMenu->addAction(QStringLiteral("(every addable component is present)"))->setEnabled(false);
    }
}

void DetailsPanel::ResetProperty(const PropertyRow& InRow)
{
    const ComponentSection* Section = FindSection(InRow.Type);
    uint8_t* Component = ResolveComponent(InRow.Type);
    if(!Section || !Section->Defaults || !Component || InRow.Entry.IsReadOnly())
    {
        return;
    }

    std::memcpy(Component + InRow.Entry.Offset, Section->Defaults->data() + InRow.Entry.Offset, InRow.Entry.Size);
    InRow.Editor->Refresh(true);
    Document.NotifyPropertyEdited(ShownEntity, InRow.Type);
    RefreshValues();
}

void DetailsPanel::RemoveArrayElement(const ComponentTypeID InType, const GameCore::ComponentPropertyEntry& InEntry,
                                      const size_t InIndex)
{
    const ComponentSection* Section = FindSection(InType);
    uint8_t* Component = ResolveComponent(InType);
    if(!Section || !Component || InEntry.IsReadOnly() || InIndex >= InEntry.ArrayCount)
    {
        return;
    }

    const size_t Stride = InEntry.Stride;
    const size_t Last   = InEntry.ArrayCount - 1;
    uint8_t* Base = Component + InEntry.Offset;
    std::memmove(Base + InIndex * Stride, Base + (InIndex + 1) * Stride, (Last - InIndex) * Stride);
    if(Section->Defaults)
    {
        std::memcpy(Base + Last * Stride, Section->Defaults->data() + InEntry.Offset + Last * Stride, InEntry.Size);
    }
    else
    {
        std::memset(Base + Last * Stride, 0, InEntry.Size);
    }

    size_t& Shown = ArrayShown[ArrayKey(InType, InEntry)];
    if(Shown > 0)
    {
        --Shown;
    }
    Document.NotifyPropertyEdited(ShownEntity, InType);
    QueueRebuild();
}

void DetailsPanel::ResetComponent(const ComponentTypeID InType)
{
    /* Per property: the default bytes also hold per-run state to keep. */
    const ComponentSection* Section = FindSection(InType);
    uint8_t* Component = ResolveComponent(InType);
    if(!Section || !Section->Defaults || !Component)
    {
        return;
    }

    for(const PropertyRow& Row : Rows)
    {
        if(Row.Type == InType && !Row.Entry.IsReadOnly())
        {
            std::memcpy(Component + Row.Entry.Offset, Section->Defaults->data() + Row.Entry.Offset, Row.Entry.Size);
            Row.Editor->Refresh(true);
        }
    }
    Document.NotifyPropertyEdited(ShownEntity, InType);
    RefreshValues();
}

bool DetailsPanel::IsOverridden(const PropertyRow& InRow) const
{
    const ComponentSection* Section = FindSection(InRow.Type);
    const uint8_t* Component = ResolveComponent(InRow.Type);
    return Section && Section->PrefabValues && Component && !InRow.Entry.IsReadOnly()
        && !IsPropertyAtDefault(InRow.Entry, Component + InRow.Entry.Offset, Section->PrefabValues->data() + InRow.Entry.Offset);
}

void DetailsPanel::RevertPropertyToPrefab(const PropertyRow& InRow)
{
    const ComponentSection* Section = FindSection(InRow.Type);
    uint8_t* Component = ResolveComponent(InRow.Type);
    if(!Section || !Section->PrefabValues || !Component || InRow.Entry.IsReadOnly())
    {
        return;
    }
    std::memcpy(Component + InRow.Entry.Offset, Section->PrefabValues->data() + InRow.Entry.Offset, InRow.Entry.Size);
    InRow.Editor->Refresh(true);
    Document.NotifyPropertyEdited(ShownEntity, InRow.Type);
    RefreshValues();
}

void DetailsPanel::RevertComponentToPrefab(const ComponentTypeID InType)
{
    const ComponentSection* Section = FindSection(InType);
    uint8_t* Component = ResolveComponent(InType);
    if(!Section || !Section->PrefabValues || !Component)
    {
        return;
    }
    for(const PropertyRow& Row : Rows)
    {
        if(Row.Type == InType && !Row.Entry.IsReadOnly())
        {
            std::memcpy(Component + Row.Entry.Offset, Section->PrefabValues->data() + Row.Entry.Offset, Row.Entry.Size);
            Row.Editor->Refresh(true);
        }
    }
    Document.NotifyPropertyEdited(ShownEntity, InType);
    RefreshValues();
}

void DetailsPanel::RefreshValues()
{
    if(!isVisible())
    {
        return;
    }

    RefreshEntitySection();

    /* A list now using more than it shows (an undo, a script): show them all. */
    for(const ArrayList& List : Arrays)
    {
        const ComponentSection* Section = FindSection(List.Type);
        const uint8_t* Component = ResolveComponent(List.Type);
        if(Section && Section->Defaults && Component
           && CountUsedElements(List.Type, List.Entry, Component, Section->Defaults->data()) > List.Shown)
        {
            QueueRebuild();
            return;
        }
    }

    for(ComponentSection& Section : Sections)
    {
        if(Section.ResetAll)
        {
            Section.ResetAll->setEnabled(false);
        }
        if(Section.RevertAll)
        {
            Section.RevertAll->setEnabled(false);
        }
    }

    for(const PropertyRow& Row : Rows)
    {
        Row.Editor->Refresh();

        const bool bOverridden = IsOverridden(Row);
        if(Row.Label && Row.Label->font().bold() != bOverridden)
        {
            QFont Font = Row.Label->font();
            Font.setBold(bOverridden);
            Row.Label->setFont(Font);
            Row.Label->setToolTip(bOverridden ? QStringLiteral("Overrides the prefab -- right-click to revert") : QString());
        }
        if(bOverridden)
        {
            if(const ComponentSection* Section = FindSection(Row.Type); Section && Section->RevertAll)
            {
                Section->RevertAll->setEnabled(true);
            }
        }

        if(!Row.Reset)
        {
            continue;
        }
        const ComponentSection* Section = FindSection(Row.Type);
        const uint8_t* Component = ResolveComponent(Row.Type);
        const bool bChanged = Section && Section->Defaults && Component
                           && !IsPropertyAtDefault(Row.Entry, Component + Row.Entry.Offset,
                                                   Section->Defaults->data() + Row.Entry.Offset);
        Row.Reset->setVisible(bChanged);
        if(bChanged && Section->ResetAll)
        {
            Section->ResetAll->setEnabled(true);
        }
    }
}

#endif // WITH_EDITOR
