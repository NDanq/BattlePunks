#if WITH_EDITOR

#include "Public/Editor/DetailsPanel.h"

#include <algorithm>
#include <cstring>

#include <QCheckBox>
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
#include "Public/Editor/EditorContext.h"
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

    /* One past the last element that differs from the default: those in use. */
    size_t CountUsedElements(const GameCore::ComponentPropertyEntry& InEntry, const uint8_t* InComponent, const uint8_t* InDefaults)
    {
        size_t Used = 0;
        for(size_t Index = 0; Index < InEntry.ArrayCount; ++Index)
        {
            const GameCore::ComponentPropertyEntry Element = InEntry.GetElement(Index);
            if(!IsPropertyAtDefault(Element, InComponent + Element.Offset, InDefaults + Element.Offset))
            {
                Used = Index + 1;
            }
        }
        return Used;
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
    if(Context.GetSelection() != ShownEntity)
    {
        ArrayShown.clear();
    }
    const int Scrolled = Scroll->verticalScrollBar()->value();

    Rows.clear();
    Sections.clear();
    Arrays.clear();
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
    ScriptEdit->setToolTip(QStringLiteral("A .lua file that returns its class table, or a script graph "
                                          "(.script.nmgraph), which runs the Lua saved in it. Relative paths "
                                          "resolve from the game's working directory. Drop one here."));
    connect(ScriptEdit, &QLineEdit::editingFinished, this, [this, InEntity]()
    {
        if(!ScriptEdit)
        {
            return;
        }
        const QString Text = ScriptEdit->text().trimmed();
        if(!Text.isEmpty() && !Text.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive)
           && !Text.endsWith(QStringLiteral(".script.nmgraph"), Qt::CaseInsensitive))
        {
            QToolTip::showText(ScriptEdit->mapToGlobal(QPoint(0, ScriptEdit->height())),
                               QStringLiteral("A script is a .lua file or a .script.nmgraph"), ScriptEdit);
            RefreshEntitySection();
            return;
        }
        Context.SetEntityScript(InEntity, Text);
    });
    ScriptEdit->installEventFilter(this);
    ScriptRow->addWidget(ScriptEdit, 1);

    auto* Browse = new QToolButton(ScriptField);
    Browse->setText(QStringLiteral("..."));
    Browse->setToolTip(QStringLiteral("Choose a .lua script or a script graph"));
    Browse->setAutoRaise(true);
    connect(Browse, &QToolButton::clicked, this, [this, InEntity]()
    {
        const QString Chosen = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a script"),
                                                            EditorGraphs::GetProjectRoot(),
                                                            QStringLiteral("Scripts (*.lua *.script.nmgraph);;Lua scripts (*.lua);;"
                                                                           "Script graphs (*.script.nmgraph)"));
        if(!Chosen.isEmpty())
        {
            UseScriptFile(InEntity, Chosen);
        }
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

        FormFor(Entry)->addRow(InLabel, Field);
        Rows.push_back(Row);

        connect(Editor, &PropertyEditor::Written, this, [Ctx, InEntity, InType]()
        {
            Ctx->NotifyPropertyEdited(InEntity, InType);
        });
    };

    for(size_t Index = 0; Index < Count; ++Index)
    {
        const GameCore::ComponentPropertyEntry& Entry = Entries[Index];
        const QString Name = QString::fromLatin1(Entry.Name);
        if(!Entry.IsArray())
        {
            AddRow(Entry, Name, nullptr);
            continue;
        }

        /* An array: the entries in use and any added since, then "+ Add". */
        const uint8_t* Live = Resolve();
        const size_t   Used = Live && State.Defaults ? CountUsedElements(Entry, Live, State.Defaults->data()) : 0;
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
            AddRow(Entry.GetElement(Element), QStringLiteral("    %1").arg(Element), Remove);
        }
    }

    return Section;
}

void DetailsPanel::UseScriptFile(const EntityID InEntity, const QString& InFile)
{
    /* As the game names it -- relative to the project, staged beside the
     * exe so Play finds it -- or, outside the project, as chosen. */
    const QString Runtime = EditorContent::ScriptPathFor(InFile);
    if(!Runtime.isEmpty())
    {
        EditorContent::StageAsset(InFile);
    }
    Context.SetEntityScript(InEntity, Runtime.isEmpty() ? InFile : Runtime);
    RefreshEntitySection();
}

bool DetailsPanel::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    /* The Script field takes a .lua or a .script.nmgraph dropped on it. */
    if(InWatched == ScriptEdit && (InEvent->type() == QEvent::DragEnter || InEvent->type() == QEvent::DragMove
                                   || InEvent->type() == QEvent::Drop))
    {
        auto* Drop = static_cast<QDropEvent*>(InEvent);
        const QString File = EditorContent::SingleDroppedFile(Drop->mimeData());
        if(EditorContent::ScriptPathFor(File).isEmpty())
        {
            Drop->ignore();
            return true;
        }
        Drop->acceptProposedAction();
        if(InEvent->type() == QEvent::Drop)
        {
            const EntityID Target = ShownEntity;
            QTimer::singleShot(0, this, [this, Target, File]() { UseScriptFile(Target, File); });
        }
        return true;
    }
    return QWidget::eventFilter(InWatched, InEvent);
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
    Context.NotifyPropertyEdited(ShownEntity, InType);
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

    /* A list now using more than it shows (an undo, a script): show them all. */
    for(const ArrayList& List : Arrays)
    {
        const ComponentSection* Section = FindSection(List.Type);
        const uint8_t* Component = ResolveComponent(List.Type);
        if(Section && Section->Defaults && Component
           && CountUsedElements(List.Entry, Component, Section->Defaults->data()) > List.Shown)
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
