#ifndef NIMBLEEDITOR_NODEPALETTE_H
#define NIMBLEEDITOR_NODEPALETTE_H

#if WITH_EDITOR

#include <map>
#include <string>
#include <vector>

#include <QFrame>

#include "Public/NodeGraph/Graph.h"

class GraphDocument;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * The "add node" popup: a search box over the schema's nodes, by category.
 * Opened from a link dropped on empty canvas, it lists only nodes with a pin
 * that link could join. A script graph's variables appear as their own
 * Get/Set entries. Deletes itself when closed.
 */
class NodePalette : public QFrame
{
    Q_OBJECT

public:
    NodePalette(const GraphDocument& InDocument, bool bInFromPin, const NodeGraph::PinRef& InFromPin, QWidget* InParent);

    void Popup(const QPoint& InGlobalPos);

signals:
    void Chosen(const std::string& InType, const std::map<std::string, std::string>& InProperties);

protected:
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    struct Entry
    {
        std::string                        Type;
        std::map<std::string, std::string> Properties;
        QString                            Label;
        QString                            Category;
        QString                            SearchText;   // lower-case: label, category, keywords
        QString                            Tooltip;
    };

    void Refill(const QString& InFilter);
    void ChooseCurrent();
    void Choose(QTreeWidgetItem* InItem);

    std::vector<Entry> Entries;
    QLineEdit*         Search = nullptr;
    QTreeWidget*       Tree   = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_NODEPALETTE_H
