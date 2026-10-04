#if WITH_EDITOR

#include "Public/Editor/OutlinerPanel.h"

#include <functional>
#include <utility>
#include <vector>

#include <QDropEvent>
#include <QSet>
#include <QStyle>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Editor/EditorDocument.h"
#include "Public/GameCore/Prefab.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Private/Editor/EditorEditActions.h"
#include "Private/Editor/EditorLights.h"
#include "Private/Editor/EditorPrimitives.h"

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

OutlinerPanel::OutlinerPanel(EditorDocument& InDocument, QWidget* InParent)
    : QWidget(InParent)
    , Document(InDocument)
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
    Tree->setSelectionMode(QAbstractItemView::ExtendedSelection);   // Ctrl and Shift, as everywhere
    Tree->setEditTriggers(QAbstractItemView::NoEditTriggers);        // F2 renames (RenameSelected)
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
        /* The row last clicked is the primary, if it is selected. */
        std::vector<EntityID> Entities;
        for(const QTreeWidgetItem* Item : Tree->selectedItems())
        {
            Entities.push_back(EntityOf(Item));
        }
        const QTreeWidgetItem* Current = Tree->currentItem();
        Document.SetSelection(Entities, Current && Current->isSelected() ? EntityOf(Current) : GameCore::InvalidEntityID());
    });

    /* An inline rename finished. Rebuild's own writes are not renames. */
    connect(Tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* InItem, const int InColumn)
    {
        const EntityID Entity = EntityOf(InItem);
        if(bSyncing || InColumn != COL_Name || !Document.GetWorld().IsAlive(Entity))
        {
            return;
        }
        const QString Name = InItem->text(COL_Name).trimmed();
        if(!Name.isEmpty() && Name != Document.GetDisplayName(Entity))
        {
            Document.RenameEntity(Entity, Name);
        }
    });

    EditorEditActions::Install(Tree, Document, [this]() { RenameSelected(); });

    connect(Tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* InItem)
    {
        if(!bSyncing) { Collapsed.insert(EntityOf(InItem).Pack()); }
    });
    connect(Tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* InItem)
    {
        if(!bSyncing) { Collapsed.remove(EntityOf(InItem).Pack()); }
    });

    connect(Tree, &QTreeWidget::customContextMenuRequested, this, &OutlinerPanel::ShowContextMenu);

    connect(&Document, &EditorDocument::EntitiesChanged,  this, &OutlinerPanel::Rebuild);
    connect(&Document, &EditorDocument::WorldReset,       this, [this]() { Collapsed.clear(); Rebuild(); });
    connect(&Document, &EditorDocument::SelectionChanged, this, &OutlinerPanel::SyncSelectionFromDocument);

    Rebuild();
}

EntityID OutlinerPanel::EntityOf(const QTreeWidgetItem* InItem)
{
    return InItem ? EntityID::Unpack(InItem->data(COL_Name, EntityRole).toULongLong()) : GameCore::InvalidEntityID();
}

void OutlinerPanel::Rebuild()
{
    GameCore::World& World = Document.GetWorld();

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

    /* Loose trees, badged in a level (a prefab's own entities are not). */
    QSet<qulonglong> LooseRoots;
    if(!Document.IsSingleRoot() && !Document.IsWorldBorrowed())
    {
        for(const EntityID Root : Document.GetLooseRoots()) { LooseRoots.insert(Root.Pack()); }
    }

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
        Item->setFlags(Item->flags() | Qt::ItemIsEditable);
        Item->setText(COL_Name, Document.GetDisplayName(InID));
        Item->setText(COL_Components, Components.join(QStringLiteral(", ")));
        Item->setToolTip(COL_Name, QStringLiteral("#%1 gen %2").arg(InID.Index).arg(InID.Generation));
        Item->setData(COL_Name, EntityRole, QVariant::fromValue<qulonglong>(InID.Pack()));

        /* A prefab instance's root in the prefab colour, bold, its members
         * tinted; the Components column names the prefab. */
        if(E.IsInInstance())
        {
            const GameCore::PrefabInstance Instance = World.GetInstance(E.GetInstance());
            const QString Prefab = QString::fromUtf8(Instance.GetPrefabPath());
            const bool bRoot = Instance.GetRoot().GetID() == InID;
            Item->setForeground(COL_Name, bRoot ? QColor(110, 170, 255) : QColor(160, 195, 240));
            if(bRoot)
            {
                QFont Font = Item->font(COL_Name);
                Font.setBold(true);
                Item->setFont(COL_Name, Font);
                Item->setText(COL_Components, QStringLiteral("Prefab: %1").arg(QFileInfo(Prefab).completeBaseName()));
            }
            Item->setToolTip(COL_Name, QStringLiteral("%1 -- %2 of %3")
                                           .arg(Item->toolTip(COL_Name), bRoot ? QStringLiteral("instance") : QStringLiteral("member"), Prefab));
        }
        else if(!InParentItem && LooseRoots.contains(InID.Pack()))
        {
            /* MIGRATION: a level is made of prefab instances; this is not one. */
            Item->setIcon(COL_Name, style()->standardIcon(QStyle::SP_MessageBoxWarning));
            Item->setToolTip(COL_Name, QStringLiteral("%1 -- not a prefab instance: World > Convert Level to Prefabs, "
                                                      "or right-click > Convert to Prefab").arg(Item->toolTip(COL_Name)));
        }
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
    SyncSelectionFromDocument(Document.GetSelection());
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

void OutlinerPanel::SyncSelectionFromDocument(const EntityID InEntity)
{
    const bool bWasSyncing = bSyncing;
    bSyncing = true;
    Tree->clearSelection();
    for(const EntityID Each : Document.GetSelectedEntities())
    {
        QTreeWidgetItem* Item = Items.value(Each.Pack(), nullptr);
        if(!Item)
        {
            continue;
        }
        /* Opened up to it, so a selection made in the viewport is visible. */
        for(QTreeWidgetItem* Up = Item->parent(); Up; Up = Up->parent())
        {
            Up->setExpanded(true);
            Collapsed.remove(EntityOf(Up).Pack());
        }
        Item->setSelected(true);
    }
    if(QTreeWidgetItem* Primary = InEntity.IsSet() ? Items.value(InEntity.Pack(), nullptr) : nullptr)
    {
        Tree->setCurrentItem(Primary, COL_Name, QItemSelectionModel::NoUpdate);
        Tree->scrollToItem(Primary);
    }
    bSyncing = bWasSyncing;
}

void OutlinerPanel::RenameSelected()
{
    QTreeWidgetItem* Item = Items.value(Document.GetSelection().Pack(), nullptr);
    if(Item && Document.GetSelectedEntities().size() == 1)
    {
        Tree->scrollToItem(Item);
        Tree->editItem(Item, COL_Name);
    }
}

void OutlinerPanel::Reparent(const EntityID InDragged, const EntityID InParent)
{
    if(!InDragged.IsSet() || InDragged == InParent)
    {
        return;
    }
    if(Document.SetEntityParent(InDragged, InParent))
    {
        Document.SetSelection(InDragged);
    }
}

void OutlinerPanel::ShowContextMenu(const QPoint& InPos)
{
    QTreeWidgetItem* Item = Tree->itemAt(InPos);
    const EntityID Target = EntityOf(Item);
    GameCore::World& World = Document.GetWorld();
    const bool bHasParent   = Target.IsSet() && World.GetEntity(Target).HasParent();

    /* Right-clicking a row outside the selection selects it, as a click would. */
    if(Target.IsSet() && !Document.IsSelected(Target))
    {
        Document.SetSelection(Target);
    }

    QMenu Menu(this);
    EditorEditActions::AddToMenu(&Menu, Document, [this]() { RenameSelected(); });
    Menu.addSeparator();
    QAction* Create      = Menu.addAction(QStringLiteral("Create Empty Entity"));
    QAction* CreateChild = Menu.addAction(QStringLiteral("Create Child Entity"));
    CreateChild->setEnabled(Target.IsSet());
    /* At the origin, as Create Empty Entity. */
    const auto AtOrigin = []() { return EngineCore::Vector3(0.0, 0.0, 0.0); };
    EditorPrimitives::AddPrimitiveActions(Menu.addMenu(QStringLiteral("Add Primitive")), Document, AtOrigin);
    EditorLights::AddLightActions(Menu.addMenu(QStringLiteral("Add Light")), Document, AtOrigin);
    Menu.addSeparator();
    QAction* Detach = Menu.addAction(QStringLiteral("Detach from Parent"));
    Detach->setEnabled(bHasParent);

    /* Prefabs: open an instance's, or make one of this subtree. */
    Menu.addSeparator();
    QString PrefabPath;
    if(Target.IsSet() && World.GetEntity(Target).IsInInstance())
    {
        PrefabPath = QString::fromUtf8(World.GetInstance(World.GetEntity(Target).GetInstance()).GetPrefabPath());
    }
    QAction* OpenPrefab = Menu.addAction(QStringLiteral("Open Prefab"));
    OpenPrefab->setEnabled(!PrefabPath.isEmpty());
    QAction* Convert = nullptr;
    if(!Document.IsSingleRoot())
    {
        Convert = Menu.addAction(QStringLiteral("Convert to Prefab..."));
        const QString Why = Target.IsSet() ? EditorPrefabs::WhyNotConvertible(Document, Target) : QStringLiteral("No entity");
        Convert->setEnabled(Why.isEmpty());
        Convert->setToolTip(Why);
    }

    QAction* Chosen = Menu.exec(Tree->viewport()->mapToGlobal(InPos));
    if(Chosen == Create)
    {
        const EntityID NewEntity = Document.CreateEntity(QStringLiteral("Entity"), EngineCore::Vector3(0.0, 0.0, 0.0));
        Document.SetSelection(NewEntity);
    }
    else if(Chosen == CreateChild)
    {
        /* At the parent's origin; one undo step, since the setup runs before
         * the creation is recorded. */
        const EntityID NewEntity = Document.CreateEntity(QStringLiteral("Entity"), EngineCore::Vector3(0.0, 0.0, 0.0),
                                                        [Target](const GameCore::Entity& InEntity)
        {
            InEntity.SetParent(Target, false);
        });
        Document.SetSelection(NewEntity);
    }
    else if(Chosen == Detach)
    {
        Reparent(Target, GameCore::InvalidEntityID());
    }
    else if(Chosen == OpenPrefab)
    {
        EditorPrefabs::OpenPrefab(EditorPrefabs::ToProjectPath(PrefabPath), window());
    }
    else if(Chosen && Chosen == Convert)
    {
        EditorPrefabs::ConvertToPrefab(Document, Target, window());
    }
}

#endif // WITH_EDITOR
