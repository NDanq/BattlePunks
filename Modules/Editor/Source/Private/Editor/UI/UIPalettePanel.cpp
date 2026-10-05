#if WITH_EDITOR

#include "Private/Editor/UI/UIPalettePanel.h"

#include <map>

#include <QMimeData>

#include "Public/UI/UIProperty.h"

UIPalettePanel::UIPalettePanel(QWidget* InParent)
    : QTreeWidget(InParent)
{
    setHeaderHidden(true);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setRootIsDecorated(true);
    connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* InItem, int)
    {
        const QString Type = InItem->data(0, Qt::UserRole).toString();
        if(!Type.isEmpty()) emit AddRequested(Type);
    });
    Refresh();
}

void UIPalettePanel::Refresh()
{
    clear();
    std::map<std::string, QTreeWidgetItem*> Groups;
    for(const UI::UIWidgetType* Type : UI::GetWidgetTypes())
    {
        if(Type->Palette.empty() || Type->IsAbstract()) continue;
        QTreeWidgetItem*& Group = Groups[Type->Palette];
        if(!Group)
        {
            Group = new QTreeWidgetItem(this, { QString::fromStdString(Type->Palette) });
            Group->setFlags(Qt::ItemIsEnabled);
            Group->setExpanded(true);
        }
        auto* Item = new QTreeWidgetItem(Group, { QString::fromStdString(Type->Name) });
        Item->setData(0, Qt::UserRole, QString::fromStdString(Type->Name));
        Item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        Item->setToolTip(0, QStringLiteral("%1 -- %2. Drag onto the canvas, or double-click to add to the selection.")
                             .arg(QString::fromStdString(Type->Name),
                                  Type->MaxChildren < 0 ? QStringLiteral("holds any children")
                                  : Type->MaxChildren == 1 ? QStringLiteral("holds one child") : QStringLiteral("holds no children")));
    }
    expandAll();
}

QStringList UIPalettePanel::mimeTypes() const
{
    return { QString::fromLatin1(MIME_TYPE) };
}

QMimeData* UIPalettePanel::mimeData(const QList<QTreeWidgetItem*>& InItems) const
{
    if(InItems.isEmpty()) return nullptr;
    const QString Type = InItems.front()->data(0, Qt::UserRole).toString();
    if(Type.isEmpty()) return nullptr;
    auto* Data = new QMimeData();
    Data->setData(QString::fromLatin1(MIME_TYPE), Type.toUtf8());
    return Data;
}

#endif // WITH_EDITOR
