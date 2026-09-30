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
 * One open .nmgraph: the graph, its file, and its own undo history -- separate
 * from the scene's (EditorContext's), which is about the World.
 *
 * EVERY CHANGE IS A SNAPSHOT. Edit() copies the graph, applies the change to
 * the copy, and pushes a command holding both. Graphs are small, so whole
 * copies cost nothing that matters, and undo can never drift out of step with
 * a hand-written inverse. An edit that changes nothing pushes nothing.
 *
 * Changed() is emitted after every edit, undo and redo. Listeners rebuild from
 * GetGraph() -- the canvas does so queued, so it never deletes an item from
 * inside that item's own mouse handler.
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

    /* The new node's GUID. InConnectFrom, if set, is linked to the new node's
     * first compatible pin in the same step. */
    NodeGraph::NodeGuid AddNode(const std::string& InType, const QPointF& InPos,
                                const std::map<std::string, std::string>& InProperties = {},
                                const NodeGraph::PinRef* InConnectFrom = nullptr);

    /* InReplacing, if set, is removed in the same step (a link dragged off
     * one input onto another). */
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

    /* Pastes a clipboard fragment with its top-left node at InAt; the new
     * nodes' GUIDs, empty if the text was not a fragment for this schema. */
    std::vector<NodeGraph::NodeGuid> Paste(const std::string& InFragment, const QPointF& InAt);

    // --- File -----------------------------------------------------------------

    /* Writes the .nmgraph, then compiles and writes (and stages) its output.
     * False with OutError only if the graph file itself could not be written;
     * a compile failure is reported in OutMessage and leaves the old output. */
    bool Save(QString& OutError, QString& OutMessage,
              QObject* InContext = nullptr, std::function<void(bool, const QString&)> InStaged = nullptr);

signals:
    void Changed();
    void DirtyChanged(bool bInDirty);
    void Saved();

private:
    friend class GraphSnapshotCommand;
    void Restore(const NodeGraph::Graph& InGraph);

    /* How a link between InA and InB would be made: the link in output ->
     * input order, and the wildcard types either end's node would take. */
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
