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
 * The graph Details dock: the selected node's settings, wildcard type and
 * unlinked literals, or with nothing selected the graph's settings,
 * variables and functions. Edits commit on finish as one undo step; rebuilt only when its
 * content changes, so half-typed values survive.
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
    void BuildFunctions(QFormLayout* InForm);
    QWidget* MakeLiteralEditor(const NodeGraph::PinRef& InPin, const std::string& InType, const std::string& InValue);

    GraphDocument& Document;
    GraphScene&    Scene;
    std::string    Shown;   // what the current widgets show, to skip needless rebuilds
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHDETAILSPANEL_H
