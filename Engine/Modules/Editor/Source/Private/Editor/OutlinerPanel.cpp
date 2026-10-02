#if WITH_EDITOR

#include "Public/Editor/OutlinerPanel.h"

#include <functional>
#include <utility>
#include <vector>

#include <QDropEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Editor/EditorContext.h"

using GameCore::EntityID;

namespace
{
    enum EColumn { COL_Name = 0, COL_Components = 1 };

    constexpr int EntityRole = Qt::UserRole + 1;

    /*
     * The tree, with Qt's own drop replaced: a drop asks for a reparent and
     * is then refused, so the view never moves rows itself -- the World
     * changes and the rebuild that follows shows it.
     */
    class OutlinerTree final : public QTreeWidget
    {
    public:
        /* (dragged row, the row to become its parent -- nullptr: a root) */
        using DropFn = std::function<void(QTreeWidgetItem*, QTreeWidgetItem*)>;

        OutlinerTree(DropFn InOnDrop, QWidget* InParent) : QTreeWidget(InParent), OnDrop(std::move(InOnDrop)) {}

    protected:
        void dropEvent(QDropEvent* InEvent) override
        {
            QTreeWidgetItem* Dragged = currentItem();
            QTreeWidgetItem* Target  = itemAt(InEvent->position().toPoint());
            if(InEvent->source() == this && Dragged && OnDrop)
            {
                /* On a row: under it. Between rows: beside it, under its
                 * parent. Empty space: a root. */
                QTreeWidgetItem* NewParent = nullptr;
                switch(dropIndicatorPosition())
                {
                    case OnItem:     NewParent = Target; break;
                    case AboveItem:
                    case BelowItem:  NewParent = Target ? Target->parent() : nullptr; break;
                    case OnViewport: NewParent = nullptr; break;
                }
                OnDrop(Dragged, NewParent);
            }
            InEvent->setDropAction(Qt::IgnoreAction);
            InEvent->ignore();
        }

    private:
        DropFn OnDrop;
    };
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

    Tree = new OutlinerTree([this](QTreeWidgetItem* InDragged, QTreeWidgetItem* InNewParent)
    {
        Reparent(EntityOf(InDragged), EntityOf(InNewParent));
    }, this);
    Tree->setColumnCount(2);
    Tree->setHeaderLabels({ QStringLiteral("Entity"), QStringLiteral("Components") });
    Tree->setRootIsDecorated(true);
    Tree->setUniformRowHeights(true);
    Tree->setSelectionMode(QAbstractItemView::SingleSelection);
    Tree->setContextMenuPolicy(Qt::CustomContextMenu);
    Tree->setDragDropMode(QAbstractItemView::InternalMove);
    Tree->setDragEnabled(true);
    Tree->setAcceptDrops(true);
    Tree->setDropIndicatorShown(true);
    Tree->header()->setSectionResizeMode(COL_Name, QHeaderView::Interactive);
    Tree->header()->setStretchLastSection(true);
    Tree->setColumnWidth(COL_Name, 160);
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

    connect(Tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* InItem)
    {
        if(!bSyncing) { Collapsed.insert(EntityOf(InItem).Pack()); }
    });
    connect(Tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* InItem)
    {
        if(!bSyncing) { Collapsed.remove(EntityOf(InItem).Pack()); }
    });

    connect(Tree, &QTreeWidget::customContextMenuRequested, this, &OutlinerPanel::ShowContextMenu);

    connect(&Context, &EditorContext::EntitiesChanged,  this, &OutlinerPanel::Rebuild);
    connect(&Context, &EditorContext::WorldReset,       this, [this]() { Collapsed.clear(); Rebuild(); });
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
    Items.clear();

    const size_t TypeCount = GameCore::GetComponentTypeCount();
    const auto MakeItem = [&](const EntityID InID, QTreeWidgetItem* InParentItem)
    {
        const GameCore::Entity E = World.GetEntity(InID);

        QStringList Components;
        for(size_t Type = 0; Type < TypeCount && Type < GameCore::COMPONENT_TYPE_MAX; ++Type)
        {
            if(E.HasComponentByType(GameCore::ComponentTypeID(Type)))
            {
                const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(GameCore::ComponentTypeID(Type));
                Components << (Info ? QString::fromLatin1(Info->Name) : QString::number(Type));
            }
        }

        auto* Item = InParentItem ? new QTreeWidgetItem(InParentItem) : new QTreeWidgetItem(Tree);
        Item->setText(COL_Name, Context.GetDisplayName(InID));
        Item->setText(COL_Components, Components.join(QStringLiteral(", ")));
        Item->setToolTip(COL_Name, QStringLiteral("#%1 gen %2").arg(InID.Index).arg(InID.Generation));
        Item->setData(COL_Name, EntityRole, QVariant::fromValue<qulonglong>(InID.Pack()));
        Items.insert(InID.Pack(), Item);
        return Item;
    };

    /* Roots in slot order, each followed by its subtree in child order. */
    std::vector<std::pair<EntityID, QTreeWidgetItem*>> Pending;
    for(const EntityID ID : Entities)
    {
        if(!World.GetEntity(ID).HasParent())
        {
            Pending.emplace_back(ID, nullptr);
        }
    }
    for(size_t Index = 0; Index < Pending.size(); ++Index)
    {
        const auto [ID, ParentItem] = Pending[Index];
        QTreeWidgetItem* Item = MakeItem(ID, ParentItem);
        for(EntityID Child = World.GetEntity(ID).GetFirstChild(); Child.IsSet(); Child = World.GetEntity(Child).GetNextSibling())
        {
            Pending.emplace_back(Child, Item);
        }
    }
    for(QTreeWidgetItem* Item : std::as_const(Items))
    {
        if(Item->childCount() > 0)
        {
            Item->setExpanded(!Collapsed.contains(EntityOf(Item).Pack()));
        }
    }

    bSyncing = bWasSyncing;

    Footer->setText(QStringLiteral("%1 entities").arg(Entities.size()));
    ApplyFilter();
    SyncSelectionFromContext(Context.GetSelection());
}

void OutlinerPanel::ApplyFilter()
{
    const QString Needle = Filter->text().trimmed();

    /* A row shows if it matches or anything below it does, so a match deep
     * in the tree keeps its ancestors visible. */
    const std::function<bool(QTreeWidgetItem*)> Apply = [&](QTreeWidgetItem* InItem)
    {
        bool bVisible = Needle.isEmpty() || InItem->text(COL_Name).contains(Needle, Qt::CaseInsensitive);
        for(int Index = 0; Index < InItem->childCount(); ++Index)
        {
            bVisible = Apply(InItem->child(Index)) || bVisible;
        }
        InItem->setHidden(!bVisible);
        return bVisible;
    };
    for(int Index = 0; Index < Tree->topLevelItemCount(); ++Index)
    {
        Apply(Tree->topLevelItem(Index));
    }
}

void OutlinerPanel::SyncSelectionFromContext(const EntityID InEntity)
{
    bSyncing = true;
    Tree->clearSelection();
    if(QTreeWidgetItem* Item = InEntity.IsSet() ? Items.value(InEntity.Pack(), nullptr) : nullptr)
    {
        /* Opened up to it, so a selection made in the viewport is visible. */
        for(QTreeWidgetItem* Up = Item->parent(); Up; Up = Up->parent())
        {
            Up->setExpanded(true);
            Collapsed.remove(EntityOf(Up).Pack());
        }
        Item->setSelected(true);
        Tree->scrollToItem(Item);
    }
    bSyncing = false;
}

void OutlinerPanel::Reparent(const EntityID InDragged, const EntityID InParent)
{
    if(!InDragged.IsSet() || InDragged == InParent)
    {
        return;
    }
    if(Context.SetEntityParent(InDragged, InParent))
    {
        Context.SetSelection(InDragged);
    }
}

void OutlinerPanel::ShowContextMenu(const QPoint& InPos)
{
    QTreeWidgetItem* Item = Tree->itemAt(InPos);
    const EntityID Target = EntityOf(Item);
    GameCore::World& World = Context.GetWorld();
    const bool bHasChildren = Target.IsSet() && World.GetEntity(Target).GetChildCount() > 0;
    const bool bHasParent   = Target.IsSet() && World.GetEntity(Target).HasParent();


    QMenu Menu(this);
    QAction* Create      = Menu.addAction(QStringLiteral("Create Empty Entity"));
    QAction* CreateChild = Menu.addAction(QStringLiteral("Create Child Entity"));
    CreateChild->setEnabled(Target.IsSet());
    Menu.addSeparator();
    QAction* Detach = Menu.addAction(QStringLiteral("Detach from Parent"));
    Detach->setEnabled(bHasParent);
    Menu.addSeparator();
    QAction* Destroy = Menu.addAction(bHasChildren ? QStringLiteral("Destroy Entity and Children")
                                                   : QStringLiteral("Destroy Entity"));
    Destroy->setEnabled(Target.IsSet());

    QAction* Chosen = Menu.exec(Tree->viewport()->mapToGlobal(InPos));
    if(Chosen == Create)
    {
        const EntityID NewEntity = Context.CreateEntity(QStringLiteral("Entity"), EngineCore::Vector3(0.0, 0.0, 0.0));
        Context.SetSelection(NewEntity);
    }
    else if(Chosen == CreateChild)
    {
        /* At the parent's origin; one undo step, since the setup runs before
         * the creation is recorded. */
        const EntityID NewEntity = Context.CreateEntity(QStringLiteral("Entity"), EngineCore::Vector3(0.0, 0.0, 0.0),
                                                        [Target](const GameCore::Entity& InEntity)
        {
            InEntity.SetParent(Target, false);
        });
        Context.SetSelection(NewEntity);
    }
    else if(Chosen == Detach)
    {
        Reparent(Target, GameCore::InvalidEntityID());
    }
    else if(Chosen == Destroy)
    {
        Context.DestroyEntity(Target);
    }
}

#endif // WITH_EDITOR
