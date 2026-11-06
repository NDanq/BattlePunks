#if WITH_EDITOR

#include "Private/Editor/UI/UIHierarchyPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>

#include "Public/UI/UIProperty.h"
#include "Private/Editor/UI/UIDesignDocument.h"
#include "Private/Editor/UI/UIPalettePanel.h"

UIHierarchyPanel::UIHierarchyPanel(UIDesignDocument& InDesign, QWidget* InParent)
    : QTreeWidget(InParent)
    , Design(InDesign)
{
    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, &QTreeWidget::itemSelectionChanged, this, [this]()
    {
        if(bSyncing) return;
        const QList<QTreeWidgetItem*> Selected = selectedItems();
        Design.Select(Selected.isEmpty() ? -1 : Selected.front()->data(0, Qt::UserRole).toInt());
    });
    connect(this, &QWidget::customContextMenuRequested, this, &UIHierarchyPanel::ShowMenu);
    connect(&Design, &UIDesignDocument::Changed, this, [this](const bool bInStructure)
    {
        if(bInStructure) { Rebuild(); }
        else
        {
            /* A rename shows; nothing else changes the rows. */
            for(int Index = 0; Index < Items.size() && Index < int(Design.GetAsset().Widgets.size()); ++Index)
            {
                const UI::UIWidgetDesc& Desc = Design.GetAsset().Widgets[size_t(Index)];
                const QString Name = QString::fromStdString(Desc.GetName());
                Items[Index]->setText(0, Name.isEmpty() ? QString::fromStdString(Desc.Type)
                                                        : QStringLiteral("%1  (%2)").arg(Name, QString::fromStdString(Desc.Type)));
            }
        }
    });
    connect(&Design, &UIDesignDocument::SelectionChanged, this, &UIHierarchyPanel::SyncSelection);
    Rebuild();
}

void UIHierarchyPanel::Rebuild()
{
    bSyncing = true;
    clear();
    Items.clear();
    const UI::UIDocumentAsset& Asset = Design.GetAsset();
    for(size_t Index = 0; Index < Asset.Widgets.size(); ++Index)
    {
        const UI::UIWidgetDesc& Desc = Asset.Widgets[Index];
        const QString Name = QString::fromStdString(Desc.GetName());
        const QString Text = Name.isEmpty() ? QString::fromStdString(Desc.Type) : QStringLiteral("%1  (%2)").arg(Name, QString::fromStdString(Desc.Type));
        QTreeWidgetItem* Item = Desc.Parent < 0 ? new QTreeWidgetItem(this, { Text }) : new QTreeWidgetItem(Items[Desc.Parent], { Text });
        Item->setData(0, Qt::UserRole, int(Index));
        Item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (Desc.Parent < 0 ? Qt::NoItemFlags : Qt::ItemIsDragEnabled) | Qt::ItemIsDropEnabled);
        Items.push_back(Item);
    }
    expandAll();
    bSyncing = false;
    SyncSelection();
}

void UIHierarchyPanel::SyncSelection()
{
    bSyncing = true;
    clearSelection();
    const int Selected = Design.GetSelected();
    if(Selected >= 0 && Selected < Items.size())
    {
        Items[Selected]->setSelected(true);
        scrollToItem(Items[Selected]);
    }
    bSyncing = false;
}

bool UIHierarchyPanel::DropTarget(const QPoint& InPos, int& OutParent, int& OutBefore) const
{
    QTreeWidgetItem* Under = itemAt(InPos);
    if(!Under) return false;
    const int Index = Under->data(0, Qt::UserRole).toInt();
    const UI::UIDocumentAsset& Asset = Design.GetAsset();
    const QRect Row = visualItemRect(Under);
    const int   Y   = InPos.y() - Row.top();

    /* The top or bottom quarter of a row: beside it; the middle: into it. */
    if(Index > 0 && Y < Row.height() / 4)
    {
        OutParent = Asset.Widgets[size_t(Index)].Parent;
        OutBefore = Index;
        return true;
    }
    if(Index > 0 && Y > Row.height() * 3 / 4 && !Asset.CanTakeChild(Index))
    {
        OutParent = Asset.Widgets[size_t(Index)].Parent;
        const int After = Asset.GetSubtreeEnd(Index);
        OutBefore = After < int(Asset.Widgets.size()) && Asset.Widgets[size_t(After)].Parent == OutParent ? After : -1;
        return true;
    }
    OutParent = Index;
    OutBefore = -1;
    return true;
}

void UIHierarchyPanel::dragEnterEvent(QDragEnterEvent* InEvent)
{
    if(InEvent->source() == this || InEvent->mimeData()->hasFormat(UIPalettePanel::MIME_TYPE)) InEvent->acceptProposedAction();
}

void UIHierarchyPanel::dragMoveEvent(QDragMoveEvent* InEvent)
{
    QTreeWidget::dragMoveEvent(InEvent);
    int Parent = -1, Before = -1;
    if(DropTarget(InEvent->position().toPoint(), Parent, Before)) InEvent->acceptProposedAction();
    else                                                          InEvent->ignore();
}

void UIHierarchyPanel::dropEvent(QDropEvent* InEvent)
{
    int Parent = -1, Before = -1;
    if(!DropTarget(InEvent->position().toPoint(), Parent, Before))
    {
        InEvent->ignore();
        return;
    }
    if(InEvent->mimeData()->hasFormat(UIPalettePanel::MIME_TYPE))
    {
        const QString Type = QString::fromUtf8(InEvent->mimeData()->data(UIPalettePanel::MIME_TYPE));
        Design.AddWidget(Type, Parent, Before, Type == QStringLiteral("Text") ? std::vector<UI::UIPropertyText>{ { "Text", "Text" } }
                                                                              : std::vector<UI::UIPropertyText>{});
    }
    else if(InEvent->source() == this && Design.GetSelected() > 0)
    {
        Design.MoveWidget(Design.GetSelected(), Parent, Before);
    }
    /* The tree is rebuilt from the document; Qt must not move rows itself. */
    InEvent->setDropAction(Qt::IgnoreAction);
    InEvent->accept();
}

void UIHierarchyPanel::keyPressEvent(QKeyEvent* InEvent)
{
    if(InEvent->key() == Qt::Key_Delete) { Design.DeleteSelected(); return; }
    if(InEvent->matches(QKeySequence::Copy))  { QApplication::clipboard()->setText(Design.CopySelected()); return; }
    if(InEvent->matches(QKeySequence::Paste)) { Design.Paste(QApplication::clipboard()->text()); return; }
    if((InEvent->modifiers() & Qt::ControlModifier) && InEvent->key() == Qt::Key_D) { Design.DuplicateSelected(); return; }
    QTreeWidget::keyPressEvent(InEvent);
}

void UIHierarchyPanel::ShowMenu(const QPoint& InPos)
{
    if(QTreeWidgetItem* Under = itemAt(InPos)) { Design.Select(Under->data(0, Qt::UserRole).toInt()); }
    const bool bChild = Design.GetSelected() > 0;
    QMenu Menu(this);
    Menu.addAction(QStringLiteral("Duplicate"), &Design, &UIDesignDocument::DuplicateSelected)->setEnabled(bChild);
    Menu.addAction(QStringLiteral("Copy"), this, [this]() { QApplication::clipboard()->setText(Design.CopySelected()); })->setEnabled(Design.GetSelected() >= 0);
    Menu.addAction(QStringLiteral("Paste"), this, [this]() { Design.Paste(QApplication::clipboard()->text()); });
    Menu.addSeparator();
    Menu.addAction(QStringLiteral("Delete"), &Design, &UIDesignDocument::DeleteSelected)->setEnabled(bChild);
    Menu.exec(viewport()->mapToGlobal(InPos));
}

#endif // WITH_EDITOR
