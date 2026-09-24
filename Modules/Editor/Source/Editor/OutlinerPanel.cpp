#include "Editor/OutlinerPanel.h"

#include <vector>

#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Editor/EditorContext.h"

using GameCore::EntityID;

namespace
{
    enum EColumn { COL_Name = 0, COL_Components = 1 };

    constexpr int EntityRole = Qt::UserRole + 1;
}

OutlinerPanel::OutlinerPanel(EditorContext& InContext, QWidget* InParent)
    : QWidget(InParent)
    , Context(InContext)
{
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(4, 4, 4, 4);

    Filter = new QLineEdit(this);
    Filter->setPlaceholderText(QStringLiteral("Filter by name..."));
    Filter->setClearButtonEnabled(true);
    Layout->addWidget(Filter);

    Tree = new QTreeWidget(this);
    Tree->setColumnCount(2);
    Tree->setHeaderLabels({ QStringLiteral("Entity"), QStringLiteral("Components") });
    Tree->setRootIsDecorated(false);
    Tree->setUniformRowHeights(true);
    Tree->setSelectionMode(QAbstractItemView::SingleSelection);
    Tree->setContextMenuPolicy(Qt::CustomContextMenu);
    Tree->header()->setSectionResizeMode(COL_Name, QHeaderView::Interactive);
    Tree->header()->setStretchLastSection(true);
    Tree->setColumnWidth(COL_Name, 140);
    Tree->setSortingEnabled(false);
    Layout->addWidget(Tree, 1);

    Footer = new QLabel(this);
    Footer->setStyleSheet(QStringLiteral("color: gray;"));
    Layout->addWidget(Footer);

    connect(Filter, &QLineEdit::textChanged, this, [this]() { ApplyFilter(); });

    connect(Tree, &QTreeWidget::itemSelectionChanged, this, [this]()
    {
        if(bSyncing)
        {
            return;
        }
        const QList<QTreeWidgetItem*> Selected = Tree->selectedItems();
        Context.SetSelection(Selected.isEmpty() ? GameCore::InvalidEntityID() : EntityOf(Selected.front()));
    });

    connect(Tree, &QTreeWidget::customContextMenuRequested, this, &OutlinerPanel::ShowContextMenu);

    connect(&Context, &EditorContext::EntitiesChanged,  this, &OutlinerPanel::Rebuild);
    connect(&Context, &EditorContext::WorldReset,       this, &OutlinerPanel::Rebuild);
    connect(&Context, &EditorContext::SelectionChanged, this, &OutlinerPanel::SyncSelectionFromContext);

    Rebuild();
}

EntityID OutlinerPanel::EntityOf(const QTreeWidgetItem* InItem)
{
    return InItem ? EntityID::Unpack(InItem->data(COL_Name, EntityRole).toULongLong()) : GameCore::InvalidEntityID();
}

void OutlinerPanel::Rebuild()
{
    GameCore::World& World = Context.GetWorld();

    /* Collected first, then turned into items: ForEachEntity's callback must
     * not do anything that could reach back into the World. */
    std::vector<EntityID> Entities;
    World.ForEachEntity([](const EntityID InEntity, void* InUserData)
    {
        static_cast<std::vector<EntityID>*>(InUserData)->push_back(InEntity);
    }, &Entities);

    const bool bWasSyncing = bSyncing;
    bSyncing = true;
    Tree->clear();

    const size_t TypeCount = GameCore::GetComponentTypeCount();
    for(const EntityID ID : Entities)
    {
        const GameCore::Entity E = World.GetEntity(ID);

        QStringList Components;
        for(size_t Type = 0; Type < TypeCount && Type < GameCore::COMPONENT_TYPE_MAX; ++Type)
        {
            if(E.HasComponentByType(GameCore::ComponentTypeID(Type)))
            {
                const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(GameCore::ComponentTypeID(Type));
                Components << (Info ? QString::fromLatin1(Info->Name) : QString::number(Type));
            }
        }

        auto* Item = new QTreeWidgetItem(Tree);
        Item->setText(COL_Name, Context.GetDisplayName(ID));
        Item->setText(COL_Components, Components.join(QStringLiteral(", ")));
        Item->setToolTip(COL_Name, QStringLiteral("#%1 gen %2").arg(ID.Index).arg(ID.Generation));
        Item->setData(COL_Name, EntityRole, QVariant::fromValue<qulonglong>(ID.Pack()));
    }

    bSyncing = bWasSyncing;

    Footer->setText(QStringLiteral("%1 entities").arg(Entities.size()));
    ApplyFilter();
    SyncSelectionFromContext(Context.GetSelection());
}

void OutlinerPanel::ApplyFilter()
{
    const QString Needle = Filter->text().trimmed();
    for(int Index = 0; Index < Tree->topLevelItemCount(); ++Index)
    {
        QTreeWidgetItem* Item = Tree->topLevelItem(Index);
        Item->setHidden(!Needle.isEmpty() && !Item->text(COL_Name).contains(Needle, Qt::CaseInsensitive));
    }
}

void OutlinerPanel::SyncSelectionFromContext(const EntityID InEntity)
{
    bSyncing = true;
    Tree->clearSelection();
    for(int Index = 0; Index < Tree->topLevelItemCount(); ++Index)
    {
        QTreeWidgetItem* Item = Tree->topLevelItem(Index);
        if(InEntity.IsSet() && EntityOf(Item) == InEntity)
        {
            Item->setSelected(true);
            Tree->scrollToItem(Item);
            break;
        }
    }
    bSyncing = false;
}

void OutlinerPanel::ShowContextMenu(const QPoint& InPos)
{
    QTreeWidgetItem* Item = Tree->itemAt(InPos);
    const EntityID Target = EntityOf(Item);

    QMenu Menu(this);
    QAction* Create  = Menu.addAction(QStringLiteral("Create Empty Entity"));
    QAction* Destroy = Menu.addAction(QStringLiteral("Destroy Entity"));
    Destroy->setEnabled(Target.IsSet());

    QAction* Chosen = Menu.exec(Tree->viewport()->mapToGlobal(InPos));
    if(Chosen == Create)
    {
        const EntityID NewEntity = Context.CreateEntity(QStringLiteral("Entity"), EngineCore::Vector3(0.0, 0.0, 0.0));
        Context.SetSelection(NewEntity);
    }
    else if(Chosen == Destroy)
    {
        Context.DestroyEntity(Target);
    }
}
