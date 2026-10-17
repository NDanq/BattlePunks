#if WITH_EDITOR

#include "Private/Editor/ComponentPicker.h"

#include <algorithm>
#include <map>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScreen>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

namespace
{
    constexpr int IndexRole = Qt::UserRole;

    const QString OtherCategory = QStringLiteral("Other");
}

ComponentPicker::ComponentPicker(GameCore::World& InWorld, const GameCore::EntityID InEntity, QWidget* InParent)
    : QFrame(InParent, Qt::Popup)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    resize(300, 420);

    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(4, 4, 4, 4);
    Layout->setSpacing(4);

    Search = new QLineEdit(this);
    Search->setPlaceholderText(QStringLiteral("Search components..."));
    Search->setClearButtonEnabled(true);
    Search->installEventFilter(this);
    Layout->addWidget(Search);

    Tree = new QTreeWidget(this);
    Tree->setHeaderHidden(true);
    Tree->setRootIsDecorated(true);
    Tree->setUniformRowHeights(true);
    Layout->addWidget(Tree, 1);

    // --- Entries -----------------------------------------------------------------
    const bool bAlive = InWorld.IsAlive(InEntity);
    const size_t TypeCount = GameCore::GetComponentTypeCount();
    for(size_t Index = 0; Index < TypeCount && Index < GameCore::COMPONENT_TYPE_MAX; ++Index)
    {
        const GameCore::ComponentTypeID Type = GameCore::ComponentTypeID(Index);
        const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(Type);
        if(!Info || !GameCore::IsComponentAddable(Type)) { continue; }

        Entry Each;
        Each.Type       = Type;
        Each.Label      = QString::fromLatin1(Info->Name);
        Each.Category   = QString::fromLatin1(GameCore::GetComponentTypeCategory(Type));
        if(Each.Category.isEmpty()) { Each.Category = OtherCategory; }
        Each.SearchText = (Each.Label + QLatin1Char(' ') + Each.Category).toLower();
        Each.bPresent   = bAlive && InWorld.GetEntity(InEntity).HasComponentByType(Type);
        Entries.push_back(Each);
    }

    /* Folders alphabetical with "Other" last; a folder's types alphabetical. */
    std::sort(Entries.begin(), Entries.end(), [](const Entry& A, const Entry& B)
    {
        const bool bAOther = A.Category == OtherCategory;
        const bool bBOther = B.Category == OtherCategory;
        if(bAOther != bBOther) { return bBOther; }
        const int ByCategory = A.Category.compare(B.Category, Qt::CaseInsensitive);
        if(ByCategory != 0) { return ByCategory < 0; }
        return A.Label.compare(B.Label, Qt::CaseInsensitive) < 0;
    });

    connect(Search, &QLineEdit::textChanged, this, [this](const QString& InText) { Refill(InText); });
    connect(Tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* InItem, int) { Choose(InItem); });
    connect(Tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* InItem, int)
    {
        if(InItem->data(0, IndexRole).isValid()) { Choose(InItem); }
    });

    Refill(QString());
}

void ComponentPicker::PopupBelow(const QWidget* InAnchor)
{
    resize((std::max)(width(), InAnchor->width()), height());

    QPoint Pos = InAnchor->mapToGlobal(QPoint(0, InAnchor->height()));
    const QScreen* Screen = InAnchor->screen() ? InAnchor->screen() : QGuiApplication::primaryScreen();
    if(Screen)
    {
        const QRect Available = Screen->availableGeometry();
        if(Pos.y() + height() > Available.bottom())
        {
            Pos.setY((std::max)(Available.top(), InAnchor->mapToGlobal(QPoint(0, 0)).y() - height()));
        }
        Pos.setX((std::max)(Available.left(), (std::min)(Pos.x(), Available.right() - width())));
    }
    move(Pos);
    show();
    Search->setFocus();
}

void ComponentPicker::Refill(const QString& InFilter)
{
    Tree->clear();
    const QStringList Words = InFilter.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);

    std::map<QString, QTreeWidgetItem*> Folders;
    const auto FolderFor = [&](const QString& InCategory) -> QTreeWidgetItem*
    {
        QTreeWidgetItem* Parent = nullptr;
        QString Path;
        for(const QString& Part : InCategory.split(QLatin1Char('/'), Qt::SkipEmptyParts))
        {
            Path += QLatin1Char('/') + Part;
            QTreeWidgetItem*& Folder = Folders[Path];
            if(!Folder)
            {
                Folder = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(Tree);
                Folder->setText(0, Part);
                Folder->setFlags(Qt::ItemIsEnabled);
                QFont Bold = Folder->font(0);
                Bold.setBold(true);
                Folder->setFont(0, Bold);
            }
            Parent = Folder;
        }
        return Parent;
    };

    QTreeWidgetItem* First = nullptr;
    for(size_t Index = 0; Index < Entries.size(); ++Index)
    {
        const Entry& Each = Entries[Index];
        const bool bMatch = std::all_of(Words.begin(), Words.end(), [&](const QString& W) { return Each.SearchText.contains(W); });
        if(!bMatch) { continue; }

        QTreeWidgetItem* Folder = FolderFor(Each.Category);
        auto* Item = Folder ? new QTreeWidgetItem(Folder) : new QTreeWidgetItem(Tree);
        Item->setText(0, Each.Label);
        Item->setData(0, IndexRole, int(Index));
        if(Each.bPresent)
        {
            /* Shown, so a type missing from the list is not a mystery, but not choosable. */
            Item->setToolTip(0, QStringLiteral("Already on this entity"));
            Item->setFlags(Item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
            continue;
        }
        if(!First) { First = Item; }
    }

    if(Tree->topLevelItemCount() == 0)
    {
        auto* None = new QTreeWidgetItem(Tree);
        None->setText(0, QStringLiteral("No matching component"));
        None->setFlags(Qt::NoItemFlags);
    }

    Tree->expandAll();
    if(First && !Words.isEmpty()) { Tree->setCurrentItem(First); }
}

void ComponentPicker::ChooseCurrent()
{
    QTreeWidgetItem* Item = Tree->currentItem();
    if(Item && Item->data(0, IndexRole).isValid()) { Choose(Item); }
}

void ComponentPicker::Choose(QTreeWidgetItem* InItem)
{
    if(!InItem || !InItem->data(0, IndexRole).isValid()) { return; }
    const Entry& Picked = Entries[size_t(InItem->data(0, IndexRole).toInt())];
    if(Picked.bPresent) { return; }
    const GameCore::ComponentTypeID Type = Picked.Type;
    close();
    emit Chosen(Type);
}

bool ComponentPicker::eventFilter(QObject* InWatched, QEvent* InEvent)
{
    /* The search box keeps focus; the arrow keys and Enter drive the list. */
    if(InWatched == Search && InEvent->type() == QEvent::KeyPress)
    {
        auto* Key = static_cast<QKeyEvent*>(InEvent);
        switch(Key->key())
        {
            case Qt::Key_Down:
            case Qt::Key_Up:
            case Qt::Key_PageDown:
            case Qt::Key_PageUp:
                QCoreApplication::sendEvent(Tree, InEvent);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                ChooseCurrent();
                return true;
            case Qt::Key_Escape:
                close();
                return true;
            default:
                break;
        }
    }
    return QFrame::eventFilter(InWatched, InEvent);
}

#endif // WITH_EDITOR
