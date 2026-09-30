#ifndef NIMBLEEDITOR_GRAPHITEMS_H
#define NIMBLEEDITOR_GRAPHITEMS_H

#if WITH_EDITOR

#include <string>
#include <vector>

#include <QColor>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QString>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/Schema.h"

class GraphScene;
class LinkItem;

/*
 * What the graph canvas draws. Items are views of the document, rebuilt from
 * it after every change (GraphScene::Rebuild) -- they hold copies, never
 * pointers into the graph. Only a drag changes an item ahead of the document,
 * and the drop commits it.
 */

/* One node: a coloured header, then a row per pin -- inputs on the left,
 * outputs on the right, execution pins first. */
class NodeItem final : public QGraphicsItem
{
public:
    enum { Type = UserType + 1 };

    struct PinSlot
    {
        std::string Name;
        bool        bOutput = false;
        bool        bExec   = false;
        std::string TypeName;   // resolved; "" for exec
        QColor      Color;
        QString     Label;      // the name, plus " = value" for an unlinked input's literal
        QPointF     Local;      // the pin's centre, in item coordinates
        bool        bLinked = false;
    };

    NodeItem(GraphScene& InScene, const NodeGraph::Node& InNode, const NodeGraph::NodeDefinition* InDef);

    int    type() const override { return Type; }
    QRectF boundingRect() const override;
    void   paint(QPainter* InPainter, const QStyleOptionGraphicsItem* InOption, QWidget* InWidget) override;

    const NodeGraph::NodeGuid& GetId() const { return Id; }
    const std::vector<PinSlot>& GetPins() const { return Pins; }

    /* The pin under InScenePos (within a generous radius), or nullptr. */
    const PinSlot* PinAt(const QPointF& InScenePos) const;
    QPointF        PinScenePos(const std::string& InPin, bool bInOutput) const;

    /* A compile message on this node: outlined and in the tooltip. */
    void SetMessage(bool bInError, const QString& InText);

    /* Pins a link being dragged could land on, drawn ringed; empty to clear. */
    void SetCandidatePins(const std::vector<std::string>& InPins);

    void AddLink(LinkItem* InLink) { Links.push_back(InLink); }

protected:
    QVariant itemChange(GraphicsItemChange InChange, const QVariant& InValue) override;

private:
    GraphScene&            Scene;
    NodeGraph::NodeGuid    Id;
    QString                Title;
    QString                Subtitle;   // the resolved wildcard type, or "unknown node type"
    QColor                 HeaderColor;
    bool                   bPlaceholder = false;
    std::vector<PinSlot>   Pins;
    std::vector<std::string> Candidates;
    std::vector<LinkItem*> Links;
    qreal                  Width  = 160.0;
    qreal                  Height = 60.0;
    bool                   bHasMessage = false;
    bool                   bMessageIsError = false;
};

/* A link, as a horizontal-tangent bezier from an output to an input. A link
 * being dragged has one free end, set with SetFreeEnd. */
class LinkItem final : public QGraphicsPathItem
{
public:
    enum { Type = UserType + 2 };

    LinkItem(const NodeGraph::Link& InLink, NodeItem* InFrom, NodeItem* InTo, const QColor& InColor, bool bInExec);

    /* A dragged link: anchored at one pin, the other end following the mouse. */
    LinkItem(NodeItem* InAnchor, const std::string& InPin, bool bInFromOutput, const QColor& InColor, bool bInExec);

    int type() const override { return Type; }

    void UpdatePath();
    void SetFreeEnd(const QPointF& InScenePos);

    const NodeGraph::Link& GetLink() const { return Link; }

private:
    NodeGraph::Link Link;
    NodeItem*       From = nullptr;
    NodeItem*       To   = nullptr;
    std::string     AnchorPin;
    bool            bAnchorIsOutput = true;
    QPointF         FreeEnd;
};

/* A comment box behind nodes; index into Graph::Comments. */
class CommentItem final : public QGraphicsRectItem
{
public:
    enum { Type = UserType + 3 };

    CommentItem(int InIndex, const NodeGraph::Comment& InComment);

    int type() const override { return Type; }
    int GetIndex() const { return Index; }

    void paint(QPainter* InPainter, const QStyleOptionGraphicsItem* InOption, QWidget* InWidget) override;

private:
    int     Index;
    QString Text;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHITEMS_H
