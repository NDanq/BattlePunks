#ifndef NIMBLEEDITOR_GRAPHDOCUMENT_H
#define NIMBLEEDITOR_GRAPHDOCUMENT_H

#if WITH_EDITOR

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QObject>
#include <QPointF>
#include <QString>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/Schema.h"

class QUndoStack;

/*
 * One open .nmgraph with its own undo history.
 *
 * Every edit stores whole before/after copies (graphs are small); no-op edits
 * push nothing. Changed() follows every edit, undo and redo.
 */
class GraphDocument : public QObject
{
    Q_OBJECT

public:
    GraphDocument(const QString& InPath, NodeGraph::Graph InGraph, QObject* InParent = nullptr);

    const NodeGraph::Graph&  GetGraph() const  { return Model; }
    const NodeGraph::Schema& GetSchema() const { return *SchemaPtr; }
    const QString&           GetPath() const   { return Path; }
    QUndoStack&              GetHistory() const { return *History; }
    bool                     IsDirty() const;

    /* Applies InEdit to a copy of the graph as one undoable step. */
    void Edit(const QString& InLabel, const std::function<void(NodeGraph::Graph&)>& InEdit);

    // --- Queries ---------------------------------------------------------------

    const NodeGraph::NodeDefinition* FindDefinition(const NodeGraph::NodeGuid& InNode) const;
    const NodeGraph::PinDefinition*  FindPin(const NodeGraph::PinRef& InPin) const;

    /* The pin's concrete type in the current graph (the wildcard resolved). */
    std::string GetPinType(const NodeGraph::PinRef& InPin) const;

    bool IsLinked(const NodeGraph::PinRef& InPin) const;

    /* Whether dragging from InA to InB would make a link; why not, if not. */
    bool CanConnect(const NodeGraph::PinRef& InA, const NodeGraph::PinRef& InB, QString* OutWhy = nullptr) const;

    // --- Edits (each one undoable step) --------------------------------------

    /* InConnectFrom, if set, links to the first compatible pin in the same step. */
    NodeGraph::NodeGuid AddNode(const std::string& InType, const QPointF& InPos,
                                const std::map<std::string, std::string>& InProperties = {},
                                const NodeGraph::PinRef* InConnectFrom = nullptr);

    /* InReplacing, if set, is removed in the same step. */
    void Connect(const NodeGraph::PinRef& InA, const NodeGraph::PinRef& InB, const NodeGraph::Link* InReplacing = nullptr);
    void DisconnectPin(const NodeGraph::PinRef& InPin);
    void RemoveLink(const NodeGraph::Link& InLink);
    void RemoveNodes(const std::vector<NodeGraph::NodeGuid>& InNodes, const std::vector<int>& InComments = {});
    void MoveItems(const std::map<NodeGraph::NodeGuid, QPointF>& InNodes, const std::map<int, QPointF>& InComments = {});
    void SetConstant(const NodeGraph::PinRef& InPin, const std::string& InValue);
    void SetProperty(const NodeGraph::NodeGuid& InNode, const std::string& InName, const std::string& InValue);
    void SetWildcardType(const NodeGraph::NodeGuid& InNode, const std::string& InType);
    void SetGraphProperty(const std::string& InName, const std::string& InValue);
    void SetVariables(const std::vector<NodeGraph::Variable>& InVariables);
    void AddComment(const NodeGraph::Comment& InComment);
    void SetCommentText(int InIndex, const QString& InText);

    /* Pastes with the top-left node at InAt; empty if not a valid fragment. */
    std::vector<NodeGraph::NodeGuid> Paste(const std::string& InFragment, const QPointF& InAt);

    // --- File -----------------------------------------------------------------

    /* Writes the graph, then its output. False only if the graph file failed;
     * compile errors go to OutMessage. */
    bool Save(QString& OutError, QString& OutMessage,
              QObject* InContext = nullptr, std::function<void(bool, const QString&)> InStaged = nullptr);

signals:
    void Changed();
    void DirtyChanged(bool bInDirty);
    void Saved();

private:
    friend class GraphSnapshotCommand;
    void Restore(const NodeGraph::Graph& InGraph);

    /* The ordered link and the wildcard types each end would take. */
    struct LinkPlan
    {
        NodeGraph::Link Made;
        std::string     FromWildcard;   // "" = leave the node's type alone
        std::string     ToWildcard;
    };
    bool PlanLink(const NodeGraph::Graph& InGraph, const NodeGraph::PinRef& InA, const NodeGraph::PinRef& InB,
                  LinkPlan& OutPlan, QString* OutWhy) const;
    void ApplyLink(NodeGraph::Graph& InOutGraph, const LinkPlan& InPlan) const;

    QString                  Path;
    NodeGraph::Graph         Model;
    const NodeGraph::Schema* SchemaPtr = nullptr;
    QUndoStack*              History = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHDOCUMENT_H
