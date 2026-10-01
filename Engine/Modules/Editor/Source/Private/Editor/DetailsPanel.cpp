#if WITH_EDITOR

#include "Public/Editor/DetailsPanel.h"

#include <cstring>

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Editor/EditorContext.h"
#include "Private/Editor/PropertyWidgets.h"

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
}

DetailsPanel::DetailsPanel(EditorContext& InContext, QWidget* InParent)
    : QWidget(InParent)
    , Context(InContext)
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

    connect(&Context, &EditorContext::SelectionChanged, this, [this](EntityID) { Rebuild(); });
    connect(&Context, &EditorContext::WorldReset,       this, [this]() { Rebuild(); });
    connect(&Context, &EditorContext::EntitiesChanged,  this, [this]() { RebuildIfComponentsChanged(); });

    connect(&RefreshTimer, &QTimer::timeout, this, &DetailsPanel::RefreshValues);
    RefreshTimer.start(RefreshIntervalMs);

    Rebuild();
}

std::vector<ComponentTypeID> DetailsPanel::CollectComponentTypes(const EntityID InEntity) const
{
    std::vector<ComponentTypeID> Types;
    GameCore::World& World = Context.GetWorld();
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
    GameCore::World& World = Context.GetWorld();
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
                    .arg(Context.GetDisplayName(ShownEntity).toHtmlEscaped())
                    .arg(ShownEntity.Index).arg(ShownEntity.Generation));
}

void DetailsPanel::RebuildIfComponentsChanged()
{
    if(ShownEntity != Context.GetSelection() || CollectComponentTypes(ShownEntity) != ShownTypes)
    {
        Rebuild();
        return;
    }
    /* Same components -- only the members may have changed. */
    UpdateHeader();
    RefreshEntitySection();
}

void DetailsPanel::Rebuild()
{
    Rows.clear();
    Sections.clear();
    NameEdit     = nullptr;
    ScriptEdit   = nullptr;
    ScriptStatus = nullptr;
    CullCheck    = nullptr;
    ShownEntity = Context.GetSelection();
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
            Context.RenameEntity(InEntity, NameEdit->text());
        }
    });
    Form->addRow(QStringLiteral("Name"), NameEdit);

    // --- Script: a path, typed or browsed -------------------------------------
    auto* ScriptField = new QWidget(Content);
    auto* ScriptRow   = new QHBoxLayout(ScriptField);
    ScriptRow->setContentsMargins(0, 0, 0, 0);
    ScriptRow->setSpacing(2);

    ScriptEdit = new QLineEdit(ScriptField);
    ScriptEdit->setPlaceholderText(QStringLiteral("(none) -- e.g. Scripts/Mover.lua"));
    ScriptEdit->setToolTip(QStringLiteral("A .lua file that returns its class table. Relative paths resolve "
                                          "from the game's working directory."));
    connect(ScriptEdit, &QLineEdit::editingFinished, this, [this, InEntity]()
    {
        if(ScriptEdit)
        {
            Context.SetEntityScript(InEntity, ScriptEdit->text());
        }
    });
    ScriptRow->addWidget(ScriptEdit, 1);

    auto* Browse = new QToolButton(ScriptField);
    Browse->setText(QStringLiteral("..."));
    Browse->setToolTip(QStringLiteral("Choose a Lua script"));
    Browse->setAutoRaise(true);
    connect(Browse, &QToolButton::clicked, this, [this, InEntity]()
    {
        const QString Chosen = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a Lua script"),
                                                            QDir::currentPath(),
                                                            QStringLiteral("Lua scripts (*.lua);;All files (*)"));
        if(Chosen.isEmpty())
        {
            return;
        }
        /* Relative to the working directory, as the game resolves scripts. */
        const QString Relative = QDir::current().relativeFilePath(Chosen);
        Context.SetEntityScript(InEntity, Relative.startsWith(QStringLiteral("..")) ? Chosen : Relative);
        RefreshEntitySection();
    });
    ScriptRow->addWidget(Browse);

    auto* Clear = new QToolButton(ScriptField);
    Clear->setText(QString(QChar(0x2715)));   // multiplication x; code points keep the source ASCII
    Clear->setToolTip(QStringLiteral("Clear the script"));
    Clear->setAutoRaise(true);
    connect(Clear, &QToolButton::clicked, this, [this, InEntity]()
    {
        Context.SetEntityScript(InEntity, QString());
        RefreshEntitySection();
    });
    ScriptRow->addWidget(Clear);

    Form->addRow(QStringLiteral("Script"), ScriptField);

    ScriptStatus = new QLabel(Content);
    ScriptStatus->setStyleSheet(QStringLiteral("color: gray;"));
    Form->addRow(QStringLiteral("Status"), ScriptStatus);

    CullCheck = new QCheckBox(QStringLiteral("Cull outside active region"), Content);
    CullCheck->setToolTip(QStringLiteral("Skip this script's tick while its Bounds are outside the World's "
                                         "active region. The script's class can override this."));
    connect(CullCheck, &QCheckBox::toggled, this, [this, InEntity](const bool bInChecked)
    {
        Context.SetEntityScriptCull(InEntity, bInChecked);
    });
    Form->addRow(QString(), CullCheck);

    return Section;
}

void DetailsPanel::RefreshEntitySection()
{
    if(!NameEdit || !ScriptEdit || !ScriptStatus || !CullCheck)
    {
        return;
    }
    GameCore::World& World = Context.GetWorld();
    const GameCore::Entity E = World.GetEntity(ShownEntity);

    /* Not while the user is typing into it. */
    const QString Name = QString::fromUtf8(E.GetName());
    if(!NameEdit->hasFocus() && NameEdit->text() != Name)
    {
        NameEdit->setText(Name);
    }
    const QString Path = QString::fromUtf8(E.GetScriptPath());
    if(!ScriptEdit->hasFocus() && ScriptEdit->text() != Path)
    {
        ScriptEdit->setText(Path);
    }

    const GameCore::EntityScriptState* Script = E.GetScriptState();
    QString Status;
    if(!Script)
    {
        Status = QStringLiteral("No script");
    }
    else if(!World.HasScriptDriver())
    {
        Status = QStringLiteral("Not running (scripts run in Play)");
    }
    else if(Script->bErrored)
    {
        Status = QStringLiteral("Errored -- see the Console");
    }
    else if(Script->bPendingInstantiate)
    {
        Status = QStringLiteral("Loading on the next tick");
    }
    else
    {
        Status = QStringLiteral("Running in %1").arg(QString::fromLatin1(
            GameCore::TickPhaseName(GameCore::ETickPhase(Script->Phase))));
    }
    ScriptStatus->setText(Status);

    CullCheck->setEnabled(Script != nullptr);
    const bool bCull = Script && Script->bCullOutsideActiveRegion;
    if(CullCheck->isChecked() != bCull)
    {
        const QSignalBlocker Block(CullCheck);
        CullCheck->setChecked(bCull);
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

    auto* Toggle = new QToolButton(Section);
    Toggle->setText(TypeName);
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

    auto* Remove = new QToolButton(Section);
    Remove->setText(QString(QChar(0x2715)));   // multiplication x; code points keep the source ASCII
    Remove->setToolTip(QStringLiteral("Remove %1 from this entity").arg(TypeName));
    Remove->setAutoRaise(true);
    connect(Remove, &QToolButton::clicked, this, [this, InEntity, InType]()
    {
        /* The World's OnComponentRemoved then rebuilds this panel on the
         * next turn, not inside this click. */
        Context.RemoveComponent(InEntity, InType);
    });
    HeaderRow->addWidget(Remove);

    SectionLayout->addLayout(HeaderRow);

    // --- Properties ----------------------------------------------------------
    auto* Content = new QWidget(Section);
    auto* Form    = new QFormLayout(Content);
    Form->setContentsMargins(14, 0, 0, 0);
    Form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    SectionLayout->addWidget(Content);

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
    EditorContext* Ctx = &Context;
    auto Resolve = [Ctx, InEntity, InType]() -> uint8_t*
    {
        GameCore::World& World = Ctx->GetWorld();
        if(!World.IsAlive(InEntity))
        {
            return nullptr;
        }
        return static_cast<uint8_t*>(World.GetEntity(InEntity).GetComponentByType(InType));
    };

    for(size_t Index = 0; Index < Count; ++Index)
    {
        const GameCore::ComponentPropertyEntry& Entry = Entries[Index];

        auto* Field  = new QWidget(Content);
        auto* FieldRow = new QHBoxLayout(Field);
        FieldRow->setContentsMargins(0, 0, 0, 0);
        FieldRow->setSpacing(2);

        PropertyEditor* Editor = CreatePropertyEditor(Entry, Resolve, Field);
        if(!Editor)
        {
            delete Field;
            continue;
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

        Form->addRow(QString::fromLatin1(Entry.Name), Field);
        Rows.push_back(Row);

        connect(Editor, &PropertyEditor::Written, this, [Ctx, InEntity, InType]()
        {
            Ctx->NotifyPropertyEdited(InEntity, InType);
        });
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

    GameCore::World& World = Context.GetWorld();
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
            Context.AddComponent(Target, Type);
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
    Context.NotifyPropertyEdited(ShownEntity, InRow.Type);
    RefreshValues();
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
    Context.NotifyPropertyEdited(ShownEntity, InType);
    RefreshValues();
}

void DetailsPanel::RefreshValues()
{
    if(!isVisible())
    {
        return;
    }

    RefreshEntitySection();

    for(ComponentSection& Section : Sections)
    {
        if(Section.ResetAll)
        {
            Section.ResetAll->setEnabled(false);
        }
    }

    for(const PropertyRow& Row : Rows)
    {
        Row.Editor->Refresh();

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
