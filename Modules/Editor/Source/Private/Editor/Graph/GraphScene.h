#ifndef NIMBLEEDITOR_GRAPHSCENE_H
#define NIMBLEEDITOR_GRAPHSCENE_H

#if WITH_EDITOR

#include <map>
#include <string>
#include <vector>

#include <QGraphicsScene>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/GraphCompiler.h"

class CommentItem;
class GraphDocument;
class LinkItem;
class NodeItem;

/*
 * Canvas contents and mouse handling: link dragging (Alt+click breaks; drag
 * from a linked input picks it up), node moves, and palette requests.
 * Rebuilt (queued) after every change; drags commit on release.
 */
class GraphScene : public QGraphicsScene
{
    Q_OBJECT

public:
    explicit GraphScene(GraphDocument& InDocument, QObject* InParent = nullptr);

    GraphDocument& GetDocument() const { return Document; }
    QColor         TypeColor(const std::string& InType) const;

    void Rebuild();

    std::vector<NodeGraph::NodeGuid> GetSelectedNodes() const;
    std::vector<int>                 GetSelectedComments() const;
    void                             SelectNodes(const std::vector<NodeGraph::NodeGuid>& InNodes);
    NodeItem*                        FindNodeItem(const NodeGraph::NodeGuid& InNode) const;

    /* Marks the nodes the last compile complained about. */
    void SetDiagnostics(const NodeGraph::CompileResult& InResult);

signals:
    /* A right-click on empty canvas (bInFromPin false), or a link dropped
     * there: the palette should offer nodes that link to InPin. */
    void PaletteRequested(const QPointF& InScenePos, bool bInFromPin, const NodeGraph::PinRef& InPin);
    void CommentEditRequested(int InIndex);
    void DeleteRequested();

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* InEvent) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* InEvent) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* InEvent) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* InEvent) override;
    void contextMenuEvent(QGraphicsSceneContextMenuEvent* InEvent) override;
    void drawBackground(QPainter* InPainter, const QRectF& InRect) override;

private:
    NodeItem* NodeAt(const QPointF& InScenePos) const;
    void      BeginLinkDrag(NodeItem* InNode, const std::string& InPin, bool bInFromOutput);
    void      EndLinkDrag();
    void      CommitMoves();

    GraphDocument&                          Document;
    std::map<NodeGraph::NodeGuid, NodeItem*> Nodes;
    std::vector<CommentItem*>               Comments;

    LinkItem*          DragLink = nullptr;
    NodeGraph::PinRef  DragPin;
    bool               bDragFromOutput = true;
    bool               bDragPickedUp = false;   // dragging an existing link off an input
    NodeGraph::Link    PickedUp;

    NodeGraph::CompileResult Diagnostics;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHSCENE_H
