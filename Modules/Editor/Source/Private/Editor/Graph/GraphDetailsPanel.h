#ifndef NIMBLEEDITOR_GRAPHDETAILSPANEL_H
#define NIMBLEEDITOR_GRAPHDETAILSPANEL_H

#if WITH_EDITOR

#include <string>

#include <QScrollArea>
#include <QString>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/Schema.h"

class GraphDocument;
class GraphScene;
class QFormLayout;

/*
 * The graph window's Details dock. With one node selected: its settings, its
 * wildcard type, and the literal value of each unlinked input. With nothing
 * selected: the graph's own settings and (for a script) its variables.
 *
 * Every edit is committed when it is finished (Enter, focus leaving, a pick)
 * as one undoable step. Rebuilt only when what it shows actually changed, so
 * an unrelated edit elsewhere does not throw away a half-typed value.
 */
class GraphDetailsPanel : public QScrollArea
{
    Q_OBJECT

public:
    GraphDetailsPanel(GraphDocument& InDocument, GraphScene& InScene, QWidget* InParent = nullptr);

    void Refresh();

private:
    void BuildNode(QFormLayout* InForm, const NodeGraph::Node& InNode);
    void BuildGraph(QFormLayout* InForm);
    QWidget* MakeLiteralEditor(const NodeGraph::PinRef& InPin, const std::string& InType, const std::string& InValue);

    GraphDocument& Document;
    GraphScene&    Scene;
    std::string    Shown;   // what the current widgets show, to skip needless rebuilds
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHDETAILSPANEL_H
