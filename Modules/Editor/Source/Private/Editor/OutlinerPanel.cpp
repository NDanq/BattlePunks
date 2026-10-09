#if WITH_EDITOR

#include "Public/Editor/OutlinerPanel.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include <QDropEvent>
#include <QKeyEvent>
#include <QSet>
#include <QStyle>
#include <QTimer>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
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
    constexpr int FolderRole = Qt::UserRole + 2;   // a folder row's path

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

        /* A key the edit actions left alone; true if taken. */
        using KeyFn = std::function<bool(const QKeyEvent*)>;

        OutlinerTree(DropFn InOnDrop, KeyFn InOnKey, QWidget* InParent)
            : QTreeWidget(InParent), OnDrop(std::move(InOnDrop)), OnKey(std::move(InOnKey)) {}

    protected:
        void keyPressEvent(QKeyEvent* InEvent) override
        {
            if(OnKey && state() != EditingState && OnKey(InEvent))
            {
                InEvent->accept();
                return;
            }
            QTreeWidget::keyPressEvent(InEvent);
        }

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
        KeyFn  OnKey;
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

    Tree = new OutlinerTree([this](QTreeWidgetItem* InDragged, QTreeWidgetItem* InNewParent) { Drop(InDragged, InNewParent); },
                            [this](const QKeyEvent* InEvent) { return HandleFolderKey(InEvent); }, this);
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
            if(EntityOf(Item).IsSet()) { Entities.push_back(EntityOf(Item)); }   // not a folder's
        }
        const QTreeWidgetItem* Current = Tree->currentItem();
        Document.SetSelection(Entities, Current && Current->isSelected() ? EntityOf(Current) : GameCore::InvalidEntityID());
    });

    /* An inline rename finished. Rebuild's own writes are not renames. */
    connect(Tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* InItem, const int InColumn)
    {
        if(bSyncing || InColumn != COL_Name)
        {
            return;
        }
        if(const QString Folder = FolderOf(InItem); !Folder.isEmpty())
        {
            /* A folder: the same parent, a new last segment. Refused (a "/",
             * or nothing): put back as it was. */
            const QString Name = InItem->text(COL_Name).trimmed();
            const QString Parent = Folder.section(QLatin1Char('/'), 0, -2);
            const bool bRenamed = !Name.isEmpty() && !Name.contains(QLatin1Char('/')) && Name != Folder.section(QLatin1Char('/'), -1)
                               && Document.RenameFolder(Folder, Parent.isEmpty() ? Name : Parent + QLatin1Char('/') + Name);
            if(!bRenamed)
            {
                QueueRebuild();
            }
            return;
        }
        const EntityID Entity = EntityOf(InItem);
        if(!Document.GetWorld().IsAlive(Entity))
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
        if(bSyncing) { return; }
        if(const QString Folder = FolderOf(InItem); !Folder.isEmpty()) { CollapsedFolders.insert(Folder); }
        else { Collapsed.insert(EntityOf(InItem).Pack()); }
    });
    connect(Tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* InItem)
    {
        if(bSyncing) { return; }
        if(const QString Folder = FolderOf(InItem); !Folder.isEmpty()) { CollapsedFolders.remove(Folder); }
        else { Collapsed.remove(EntityOf(InItem).Pack()); }
    });

    connect(Tree, &QTreeWidget::customContextMenuRequested, this, &OutlinerPanel::ShowContextMenu);

    connect(&Document, &EditorDocument::EntitiesChanged,  this, &OutlinerPanel::Rebuild);
    connect(&Document, &EditorDocument::WorldReset,       this, [this]() { Collapsed.clear(); CollapsedFolders.clear(); Rebuild(); });
    connect(&Document, &EditorDocument::WorldSettingsChanged, this, &OutlinerPanel::QueueRebuild);   // folders live there
    connect(&Document, &EditorDocument::SelectionChanged, this, &OutlinerPanel::SyncSelectionFromDocument);

    Rebuild();
}

EntityID OutlinerPanel::EntityOf(const QTreeWidgetItem* InItem)
{
    return InItem && FolderOf(InItem).isEmpty() ? EntityID::Unpack(InItem->data(COL_Name, EntityRole).toULongLong())
                                                : GameCore::InvalidEntityID();
}

QString OutlinerPanel::FolderOf(const QTreeWidgetItem* InItem)
{
    return InItem ? InItem->data(COL_Name, FolderRole).toString() : QString();
}

bool OutlinerPanel::HasFolders() const
{
    return !Document.IsSingleRoot();
}

void OutlinerPanel::QueueRebuild()
{
    /* Deferred: a folder rename finishes inside its row's own signal, and
     * Rebuild deletes every row. */
    if(!bRebuildQueued)
    {
        bRebuildQueued = true;
        QTimer::singleShot(0, this, [this]() { if(bRebuildQueued) { Rebuild(); } });
    }
}

void OutlinerPanel::Rebuild()
{
    bRebuildQueued = false;

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
    FolderItems.clear();

    /* Folders first, each under its parent (GetFolders is sorted, so a parent
     * comes before what is in it), so they list before the entities. */
    if(HasFolders())
    {
        for(const QString& Folder : Document.GetFolders())
        {
            QTreeWidgetItem* ParentItem = FolderItems.value(Folder.section(QLatin1Char('/'), 0, -2), nullptr);
            auto* Item = ParentItem ? new QTreeWidgetItem(ParentItem) : new QTreeWidgetItem(Tree);
            Item->setFlags(Item->flags() | Qt::ItemIsEditable);
            Item->setText(COL_Name, Folder.section(QLatin1Char('/'), -1));
            Item->setIcon(COL_Name, style()->standardIcon(QStyle::SP_DirIcon));
            Item->setToolTip(COL_Name, Folder);
            Item->setData(COL_Name, FolderRole, Folder);
            FolderItems.insert(Folder, Item);
        }
    }

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
        else if(LooseRoots.contains(InID.Pack()))
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
            Pending.emplace_back(ID, HasFolders() ? FolderItems.value(Document.GetEntityFolder(ID), nullptr) : nullptr);
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
    for(auto It = FolderItems.cbegin(); It != FolderItems.cend(); ++It)
    {
        It.value()->setExpanded(!CollapsedFolders.contains(It.key()));
    }

    bSyncing = bWasSyncing;

    Footer->setText(FolderItems.isEmpty() ? QStringLiteral("%1 entities").arg(Entities.size())
                                          : QStringLiteral("%1 entities, %2 folders").arg(Entities.size()).arg(FolderItems.size()));
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

    /* A folder row stays selected while no entity is: selecting one clears
     * the entities, and that must not clear it in turn. */
    if(Document.GetSelectedEntities().empty())
    {
        for(QTreeWidgetItem* Item : std::as_const(Items)) { Item->setSelected(false); }
    }
    else
    {
        Tree->clearSelection();
    }
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
            CollapsedFolders.remove(FolderOf(Up));
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

void OutlinerPanel::Drop(QTreeWidgetItem* InDragged, QTreeWidgetItem* InNewParent)
{
    const QString Target = FolderOf(InNewParent);
    const bool bOntoEntity = InNewParent && Target.isEmpty();

    /* A folder goes inside a folder, or to the top; never under an entity. */
    if(const QString Folder = FolderOf(InDragged); !Folder.isEmpty())
    {
        if(!bOntoEntity)
        {
            const QString Leaf = Folder.section(QLatin1Char('/'), -1);
            Document.RenameFolder(Folder, Target.isEmpty() ? Leaf : Target + QLatin1Char('/') + Leaf);
        }
        return;
    }
    const EntityID Dragged = EntityOf(InDragged);
    if(bOntoEntity || !HasFolders())
    {
        Reparent(Dragged, EntityOf(InNewParent));
        return;
    }

    /* Into a folder, or the top: the selection, if the row dragged is in it. */
    const std::vector<EntityID> Moving = Document.IsSelected(Dragged) ? Document.GetSelectedRoots() : std::vector<EntityID>{ Dragged };
    if(Document.MoveEntitiesToFolder(Moving, Target) > 0)
    {
        Document.SetSelection(Moving, Dragged);
    }
}

std::vector<QString> OutlinerPanel::SelectedFolders() const
{
    std::vector<QString> Folders;
    for(const QTreeWidgetItem* Item : Tree->selectedItems())
    {
        if(const QString Folder = FolderOf(Item); !Folder.isEmpty()) { Folders.push_back(Folder); }
    }
    /* Deepest first: deleting one moves what is inside it, renaming the rest. */
    std::sort(Folders.begin(), Folders.end(), [](const QString& L, const QString& R) { return L.count(QLatin1Char('/')) > R.count(QLatin1Char('/')); });
    return Folders;
}

bool OutlinerPanel::HandleFolderKey(const QKeyEvent* InEvent)
{
    const std::vector<QString> Folders = Document.GetSelectedEntities().empty() ? SelectedFolders() : std::vector<QString>();
    if(Folders.empty())
    {
        return false;
    }
    if(InEvent->key() == Qt::Key_F2 && InEvent->modifiers() == Qt::NoModifier)
    {
        QTreeWidgetItem* Item = Tree->currentItem();
        if(!Item || !Item->isSelected() || FolderOf(Item).isEmpty())
        {
            Item = FolderItems.value(Folders.front(), nullptr);
        }
        if(Item) { Tree->editItem(Item, COL_Name); }
        return true;
    }
    if(InEvent->matches(QKeySequence::Delete))
    {
        for(const QString& Folder : Folders)
        {
            Document.DeleteFolder(Folder);
        }
        return true;
    }
    return false;
}

void OutlinerPanel::CreateFolderIn(const QString& InParent)
{
    const QStringList Existing = Document.GetFolders();
    const auto PathOf = [&InParent](const QString& InName) { return InParent.isEmpty() ? InName : InParent + QLatin1Char('/') + InName; };
    QString Name = QStringLiteral("New Folder");
    for(int Suffix = 2; Existing.contains(PathOf(Name)); ++Suffix)
    {
        Name = QStringLiteral("New Folder %1").arg(Suffix);
    }
    if(!Document.CreateFolder(PathOf(Name)))
    {
        return;
    }
    CollapsedFolders.remove(InParent);
    Rebuild();
    if(QTreeWidgetItem* Item = FolderItems.value(PathOf(Name), nullptr))
    {
        Tree->setCurrentItem(Item, COL_Name);
        Tree->scrollToItem(Item);
        Tree->editItem(Item, COL_Name);
    }
}

void OutlinerPanel::ShowContextMenu(const QPoint& InPos)
{
    QTreeWidgetItem* Item = Tree->itemAt(InPos);

    /* A folder's own menu. */
    if(const QString Folder = FolderOf(Item); !Folder.isEmpty())
    {
        if(!Item->isSelected())
        {
            Tree->setCurrentItem(Item, COL_Name);   // selects it alone, clearing the entities
        }
        QMenu Menu(this);
        QAction* NewFolder = Menu.addAction(QStringLiteral("New Folder"));
        QAction* Rename    = Menu.addAction(QStringLiteral("Rename Folder"));
        Rename->setShortcut(QKeySequence(Qt::Key_F2));
        QAction* Delete    = Menu.addAction(QStringLiteral("Delete Folder"));
        Delete->setShortcut(QKeySequence::Delete);
        Delete->setToolTip(QStringLiteral("What it holds moves up a level"));
        Menu.addSeparator();
        QAction* SelectContents = Menu.addAction(QStringLiteral("Select Contents"));
        SelectContents->setEnabled(!Document.GetEntitiesInFolder(Folder, true).empty());
        QAction* Create = Menu.addAction(QStringLiteral("Create Empty Entity"));

        QAction* Chosen = Menu.exec(Tree->viewport()->mapToGlobal(InPos));
        if(Chosen == NewFolder)
        {
            CreateFolderIn(Folder);
        }
        else if(Chosen == Rename)
        {
            if(QTreeWidgetItem* Row = FolderItems.value(Folder, nullptr)) { Tree->editItem(Row, COL_Name); }
        }
        else if(Chosen == Delete)
        {
            const std::vector<QString> Folders = SelectedFolders();
            for(const QString& Each : Folders.empty() ? std::vector<QString>{ Folder } : Folders)
            {
                Document.DeleteFolder(Each);
            }
        }
        else if(Chosen == SelectContents)
        {
            Document.SetSelection(Document.GetEntitiesInFolder(Folder, true));
        }
        else if(Chosen == Create)
        {
            const QByteArray Path = Folder.toUtf8();
            const EntityID NewEntity = Document.CreateEntity(QStringLiteral("Entity"), EngineCore::Vector3(0.0, 0.0, 0.0),
                                                            [Path](const GameCore::Entity& InEntity) { InEntity.SetFolder(Path.constData()); });
            Document.SetSelection(NewEntity);
        }
        return;
    }

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

    /* Folders: a new one, or the selection into one (data: its path). */
    QAction* NewFolder = nullptr;
    QMenu*   MoveTo    = nullptr;
    QAction* MoveToNew = nullptr;
    if(HasFolders())
    {
        Menu.addSeparator();
        NewFolder = Menu.addAction(QStringLiteral("New Folder"));
        MoveTo = Menu.addMenu(QStringLiteral("Move to Folder"));
        MoveTo->setEnabled(!Document.GetSelectedEntities().empty());
        MoveToNew = MoveTo->addAction(QStringLiteral("New Folder..."));
        MoveTo->addSeparator();
        MoveTo->addAction(QStringLiteral("(No Folder)"))->setData(QString());
        for(const QString& Folder : Document.GetFolders())
        {
            QAction* Into = MoveTo->addAction(QString(Folder).replace(QLatin1Char('/'), QStringLiteral(" / ")));
            Into->setData(Folder);
        }
    }

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
        if(HasFolders())
        {
            /* Into the folder its tree is in, so it stays near where it was. */
            EntityID Top = Target;
            while(World.GetEntity(Top).HasParent()) { Top = World.GetEntity(Top).GetParent(); }
            if(Document.MoveEntitiesToFolder({ Target }, Document.GetEntityFolder(Top)) > 0) { Document.SetSelection(Target); }
        }
        else
        {
            Reparent(Target, GameCore::InvalidEntityID());
        }
    }
    else if(Chosen && Chosen == NewFolder)
    {
        CreateFolderIn(QString());
    }
    else if(Chosen && MoveTo && Chosen->parent() == MoveTo)
    {
        QString Folder = Chosen->data().toString();
        if(Chosen == MoveToNew)
        {
            bool bOk = false;
            Folder = EditorDocument::NormalizeFolder(QInputDialog::getText(window(), QStringLiteral("Move to New Folder"),
                QStringLiteral("Folder (\"A/B\" for B inside A)"), QLineEdit::Normal, QStringLiteral("New Folder"), &bOk));
            if(!bOk || Folder.isEmpty()) { return; }
        }
        const std::vector<EntityID> Moving = Document.GetSelectedRoots();
        if(Document.MoveEntitiesToFolder(Moving, Folder) > 0) { Document.SetSelection(Moving, Document.GetSelection()); }
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
