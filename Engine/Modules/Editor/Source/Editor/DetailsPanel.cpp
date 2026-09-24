#include "Editor/DetailsPanel.h"

#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Editor/EditorContext.h"
#include "Editor/PropertyWidgets.h"

using GameCore::ComponentTypeID;
using GameCore::EntityID;

namespace
{
    constexpr int RefreshIntervalMs = 100;
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

void DetailsPanel::RebuildIfComponentsChanged()
{
    if(ShownEntity != Context.GetSelection() || CollectComponentTypes(ShownEntity) != ShownTypes)
    {
        Rebuild();
        return;
    }
    /* Same components -- only the name may have changed. */
    if(ShownEntity.IsSet())
    {
        Header->setText(QStringLiteral("<b>%1</b> &nbsp;<span style='color:gray'>#%2 gen %3</span>")
                        .arg(Context.GetDisplayName(ShownEntity).toHtmlEscaped())
                        .arg(ShownEntity.Index).arg(ShownEntity.Generation));
    }
}

void DetailsPanel::Rebuild()
{
    Editors.clear();
    ShownEntity = Context.GetSelection();
    ShownTypes  = CollectComponentTypes(ShownEntity);

    /* A fresh body each time: takeWidget/delete drops every old editor at once. */
    delete Scroll->takeWidget();
    Body       = new QWidget();
    BodyLayout = new QVBoxLayout(Body);
    BodyLayout->setContentsMargins(0, 0, 0, 0);
    BodyLayout->setSpacing(6);

    if(!ShownEntity.IsSet())
    {
        Header->setText(QStringLiteral("<span style='color:gray'>Nothing selected</span>"));
    }
    else
    {
        Header->setText(QStringLiteral("<b>%1</b> &nbsp;<span style='color:gray'>#%2 gen %3</span>")
                        .arg(Context.GetDisplayName(ShownEntity).toHtmlEscaped())
                        .arg(ShownEntity.Index).arg(ShownEntity.Generation));

        for(const ComponentTypeID Type : ShownTypes)
        {
            BodyLayout->addWidget(BuildComponentSection(ShownEntity, Type));
        }
    }

    BodyLayout->addStretch(1);
    Scroll->setWidget(Body);

    RefreshValues();
}

QWidget* DetailsPanel::BuildComponentSection(const EntityID InEntity, const ComponentTypeID InType)
{
    const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(InType);
    const QString TypeName = Info ? QString::fromLatin1(Info->Name) : QStringLiteral("Component %1").arg(InType);

    auto* Section = new QFrame(Body);
    Section->setFrameShape(QFrame::StyledPanel);
    auto* SectionLayout = new QVBoxLayout(Section);
    SectionLayout->setContentsMargins(4, 2, 4, 4);
    SectionLayout->setSpacing(2);

    auto* Toggle = new QToolButton(Section);
    Toggle->setText(TypeName);
    Toggle->setCheckable(true);
    Toggle->setChecked(true);
    Toggle->setArrowType(Qt::DownArrow);
    Toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    Toggle->setAutoRaise(true);
    Toggle->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; }"));
    SectionLayout->addWidget(Toggle);

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
        PropertyEditor* Editor = CreatePropertyEditor(Entries[Index], Resolve, Content);
        if(!Editor)
        {
            continue;
        }
        QString Label = QString::fromLatin1(Entries[Index].Name);
        if(Entries[Index].IsReadOnly())
        {
            Editor->setToolTip(QStringLiteral("Read-only"));
        }
        Form->addRow(Label, Editor);
        Editors.push_back(Editor);

        connect(Editor, &PropertyEditor::Written, this, [Ctx, InEntity, InType]()
        {
            Ctx->NotifyPropertyEdited(InEntity, InType);
        });
    }

    return Section;
}

void DetailsPanel::RefreshValues()
{
    if(!isVisible())
    {
        return;
    }
    for(PropertyEditor* Editor : Editors)
    {
        Editor->Refresh();
    }
}
