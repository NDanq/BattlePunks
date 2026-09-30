#if WITH_EDITOR

#include "Private/Editor/Graph/GraphScene.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPalette>

#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphItems.h"

using namespace NodeGraph;
using EditorGraphs::ToQ;

namespace
{
    constexpr qreal GridMinor = 20.0;
    constexpr qreal GridMajor = 100.0;
    constexpr qreal SceneExtent = 50000.0;
}

GraphScene::GraphScene(GraphDocument& InDocument, QObject* InParent)
    : QGraphicsScene(InParent)
    , Document(InDocument)
{
    setSceneRect(-SceneExtent, -SceneExtent, 2.0 * SceneExtent, 2.0 * SceneExtent);
    setItemIndexMethod(QGraphicsScene::NoIndex);   // few items, and they move

    /* Queued: an edit made from inside a mouse handler must not delete that
     * handler's item under it. */
    connect(&Document, &GraphDocument::Changed, this, &GraphScene::Rebuild, Qt::QueuedConnection);
    Rebuild();
}

QColor GraphScene::TypeColor(const std::string& InType) const
{
    const PinTypeInfo* Info = Document.GetSchema().FindType(InType);
    const uint32_t Rgb = Info ? Info->Color : 0x9A9A9A;
    return QColor(int((Rgb >> 16) & 0xFF), int((Rgb >> 8) & 0xFF), int(Rgb & 0xFF));
}

void GraphScene::Rebuild()
{
    const std::vector<NodeGuid> Selected = GetSelectedNodes();
    const std::vector<int>      SelectedComments = GetSelectedComments();

    if(DragLink) { EndLinkDrag(); }

    /* Deleting a selected item emits selectionChanged, and its listeners ask
     * this scene what is selected: the maps must be empty before clear(), and
     * the signal quiet until the new items are in (SelectNodes emits once). */
    QSignalBlocker Block(this);
    Nodes.clear();
    Comments.clear();
    clear();

    const Graph& G = Document.GetGraph();

    for(size_t Index = 0; Index < G.Comments.size(); ++Index)
    {
        auto* Item = new CommentItem(int(Index), G.Comments[Index]);
        addItem(Item);
        Comments.push_back(Item);
        Item->setSelected(std::find(SelectedComments.begin(), SelectedComments.end(), int(Index)) != SelectedComments.end());
    }

    for(const Node& Each : G.Nodes)
    {
        auto* Item = new NodeItem(*this, Each, Document.FindDefinition(Each.Id));
        addItem(Item);
        Nodes[Each.Id] = Item;
    }

    for(const Link& Each : G.Links)
    {
        NodeItem* From = FindNodeItem(Each.From.Node);
        NodeItem* To   = FindNodeItem(Each.To.Node);
        if(!From || !To) { continue; }

        const PinDefinition* Pin = Document.FindPin(Each.From);
        const bool bExec = Pin && Pin->IsExec();
        const QColor Color = bExec ? QApplication::palette().color(QPalette::WindowText)
                                   : TypeColor(Document.GetPinType(Each.From));
        auto* Item = new LinkItem(Each, From, To, Color, bExec);
        addItem(Item);
        From->AddLink(Item);
        To->AddLink(Item);
    }

    SetDiagnostics(Diagnostics);
    Block.unblock();
    SelectNodes(Selected);
}

std::vector<NodeGuid> GraphScene::GetSelectedNodes() const
{
    std::vector<NodeGuid> Result;
    for(const auto& Each : Nodes)
    {
        if(Each.second->isSelected()) { Result.push_back(Each.first); }
    }
    return Result;
}

std::vector<int> GraphScene::GetSelectedComments() const
{
    std::vector<int> Result;
    for(const CommentItem* Each : Comments)
    {
        if(Each->isSelected()) { Result.push_back(Each->GetIndex()); }
    }
    return Result;
}

void GraphScene::SelectNodes(const std::vector<NodeGuid>& InNodes)
{
    {
        const QSignalBlocker Block(this);   // one selectionChanged, below, not one per item
        for(const auto& Each : Nodes)
        {
            Each.second->setSelected(std::find(InNodes.begin(), InNodes.end(), Each.first) != InNodes.end());
        }
    }
    emit selectionChanged();
}

NodeItem* GraphScene::FindNodeItem(const NodeGuid& InNode) const
{
    const auto Found = Nodes.find(InNode);
    return Found == Nodes.end() ? nullptr : Found->second;
}

void GraphScene::SetDiagnostics(const CompileResult& InResult)
{
    Diagnostics = InResult;

    std::map<NodeGuid, std::pair<bool, QStringList>> PerNode;
    for(const Diagnostic& Each : InResult.Diagnostics)
    {
        if(!Each.Node.IsSet() || Each.Severity == ESeverity::Info) { continue; }
        auto& Entry = PerNode[Each.Node];
        Entry.first = Entry.first || Each.Severity == ESeverity::Error;
        Entry.second << ToQ(Each.Message);
    }

    for(const auto& Each : Nodes)
    {
        const auto Found = PerNode.find(Each.first);
        if(Found == PerNode.end()) { Each.second->SetMessage(false, QString()); }
        else                       { Each.second->SetMessage(Found->second.first, Found->second.second.join(QLatin1Char('\n'))); }
    }
}

NodeItem* GraphScene::NodeAt(const QPointF& InScenePos) const
{
    /* Pins sit on the node's edge, half outside it: test pins first, over
     * every node, then the body under the cursor. */
    for(const auto& Each : Nodes)
    {
        if(Each.second->PinAt(InScenePos)) { return Each.second; }
    }
    for(QGraphicsItem* Item : items(InScenePos))
    {
        if(Item->type() == NodeItem::Type) { return static_cast<NodeItem*>(Item); }
    }
    return nullptr;
}

// === Link dragging ==============================================================

void GraphScene::BeginLinkDrag(NodeItem* InNode, const std::string& InPin, const bool bInFromOutput)
{
    DragPin = PinRef{ InNode->GetId(), InPin };
    bDragFromOutput = bInFromOutput;

    const PinDefinition* Pin = Document.FindPin(DragPin);
    const bool bExec = Pin && Pin->IsExec();
    const QColor Color = bExec ? QApplication::palette().color(QPalette::WindowText) : TypeColor(Document.GetPinType(DragPin));
    DragLink = new LinkItem(InNode, InPin, bInFromOutput, Color, bExec);
    addItem(DragLink);

    /* Ring every pin the link could land on. */
    for(const auto& Each : Nodes)
    {
        std::vector<std::string> Candidates;
        for(const NodeItem::PinSlot& Slot : Each.second->GetPins())
        {
            const PinRef Target{ Each.first, Slot.Name };
            if(bDragPickedUp && Target == PickedUp.To) { Candidates.push_back(Slot.Name); continue; }
            if(Document.CanConnect(DragPin, Target)) { Candidates.push_back(Slot.Name); }
        }
        Each.second->SetCandidatePins(Candidates);
    }
}

void GraphScene::EndLinkDrag()
{
    for(const auto& Each : Nodes) { Each.second->SetCandidatePins({}); }
    if(DragLink)
    {
        removeItem(DragLink);
        delete DragLink;
        DragLink = nullptr;
    }
    bDragPickedUp = false;
}

void GraphScene::mousePressEvent(QGraphicsSceneMouseEvent* InEvent)
{
    if(InEvent->button() == Qt::LeftButton)
    {
        NodeItem* Node = NodeAt(InEvent->scenePos());
        const NodeItem::PinSlot* Slot = Node ? Node->PinAt(InEvent->scenePos()) : nullptr;
        if(Slot)
        {
            const PinRef Pin{ Node->GetId(), Slot->Name };
            if(InEvent->modifiers() & Qt::AltModifier)
            {
                Document.DisconnectPin(Pin);
                InEvent->accept();
                return;
            }

            /* A linked value input: pick its link up, to move or drop it. */
            const Link* Existing = !Slot->bOutput && !Slot->bExec ? Document.GetGraph().FindLinkTo(Pin) : nullptr;
            if(Existing)
            {
                PickedUp = *Existing;
                bDragPickedUp = true;
                if(NodeItem* Source = FindNodeItem(Existing->From.Node))
                {
                    BeginLinkDrag(Source, Existing->From.Pin, true);
                    DragLink->SetFreeEnd(InEvent->scenePos());
                    InEvent->accept();
                    return;
                }
                bDragPickedUp = false;
            }

            BeginLinkDrag(Node, Slot->Name, Slot->bOutput);
            InEvent->accept();
            return;
        }
    }
    QGraphicsScene::mousePressEvent(InEvent);
}

void GraphScene::mouseMoveEvent(QGraphicsSceneMouseEvent* InEvent)
{
    if(DragLink)
    {
        DragLink->SetFreeEnd(InEvent->scenePos());
        InEvent->accept();
        return;
    }
    QGraphicsScene::mouseMoveEvent(InEvent);
}

void GraphScene::mouseReleaseEvent(QGraphicsSceneMouseEvent* InEvent)
{
    if(DragLink && InEvent->button() == Qt::LeftButton)
    {
        const PinRef From = DragPin;
        const bool bPicked = bDragPickedUp;
        const Link Picked = PickedUp;
        const QPointF At = InEvent->scenePos();

        NodeItem* Node = NodeAt(At);
        const NodeItem::PinSlot* Slot = Node ? Node->PinAt(At) : nullptr;
        EndLinkDrag();

        if(Slot)
        {
            const PinRef Target{ Node->GetId(), Slot->Name };
            if(!(bPicked && Target == Picked.To))   // dropped back where it was: nothing to do
            {
                Document.Connect(From, Target, bPicked ? &Picked : nullptr);
            }
        }
        else if(bPicked)
        {
            Document.RemoveLink(Picked);
        }
        else
        {
            emit PaletteRequested(At, true, From);
        }
        InEvent->accept();
        return;
    }

    QGraphicsScene::mouseReleaseEvent(InEvent);
    if(InEvent->button() == Qt::LeftButton) { CommitMoves(); }
}

/* Nodes and comments the mouse moved, written to the document as one step. */
void GraphScene::CommitMoves()
{
    const Graph& G = Document.GetGraph();
    std::map<NodeGuid, QPointF> MovedNodes;
    std::map<int, QPointF>      MovedComments;

    for(const auto& Each : Nodes)
    {
        const Node* Model = G.FindNode(Each.first);
        const QPointF Pos = Each.second->pos();
        if(Model && (std::abs(Model->X - Pos.x()) > 0.01 || std::abs(Model->Y - Pos.y()) > 0.01))
        {
            /* Whole units: the file stays readable and a nudge is not a diff of noise. */
            MovedNodes[Each.first] = QPointF(std::round(Pos.x()), std::round(Pos.y()));
        }
    }
    for(const CommentItem* Each : Comments)
    {
        const Comment& Model = G.Comments[size_t(Each->GetIndex())];
        const QPointF Pos = Each->pos();
        if(std::abs(Model.X - Pos.x()) > 0.01 || std::abs(Model.Y - Pos.y()) > 0.01)
        {
            MovedComments[Each->GetIndex()] = QPointF(std::round(Pos.x()), std::round(Pos.y()));
        }
    }

    if(!MovedNodes.empty() || !MovedComments.empty()) { Document.MoveItems(MovedNodes, MovedComments); }
}

void GraphScene::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* InEvent)
{
    for(QGraphicsItem* Item : items(InEvent->scenePos()))
    {
        if(Item->type() == NodeItem::Type) { break; }   // a node in front wins
        if(Item->type() == CommentItem::Type)
        {
            emit CommentEditRequested(static_cast<CommentItem*>(Item)->GetIndex());
            InEvent->accept();
            return;
        }
    }
    QGraphicsScene::mouseDoubleClickEvent(InEvent);
}

void GraphScene::contextMenuEvent(QGraphicsSceneContextMenuEvent* InEvent)
{
    NodeItem* Node = NodeAt(InEvent->scenePos());
    const NodeItem::PinSlot* Slot = Node ? Node->PinAt(InEvent->scenePos()) : nullptr;

    if(Slot)
    {
        QMenu Menu;
        const PinRef Pin{ Node->GetId(), Slot->Name };
        QAction* Break = Menu.addAction(QStringLiteral("Break Links"));
        Break->setEnabled(Document.IsLinked(Pin));
        if(Menu.exec(InEvent->screenPos()) == Break) { Document.DisconnectPin(Pin); }
        InEvent->accept();
        return;
    }

    if(Node)
    {
        if(!Node->isSelected())
        {
            clearSelection();
            Node->setSelected(true);
        }
        QMenu Menu;
        QAction* Delete = Menu.addAction(QStringLiteral("Delete"));
        QAction* Break  = Menu.addAction(QStringLiteral("Break All Links"));
        QAction* Chosen = Menu.exec(InEvent->screenPos());
        if(Chosen == Delete) { emit DeleteRequested(); }
        if(Chosen == Break)
        {
            const NodeGuid Id = Node->GetId();
            Document.Edit(QStringLiteral("Break All Links"), [Id](Graph& G)
            {
                G.Links.erase(std::remove_if(G.Links.begin(), G.Links.end(),
                                             [&](const Link& L) { return L.From.Node == Id || L.To.Node == Id; }), G.Links.end());
            });
        }
        InEvent->accept();
        return;
    }

    emit PaletteRequested(InEvent->scenePos(), false, PinRef());
    InEvent->accept();
}

void GraphScene::drawBackground(QPainter* InPainter, const QRectF& InRect)
{
    const bool bDark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    InPainter->fillRect(InRect, bDark ? QColor(0x1E, 0x1F, 0x22) : QColor(0xE9, 0xEA, 0xEE));

    const auto DrawGrid = [&](const qreal InStep, const QColor& InColor)
    {
        QVector<QLineF> Lines;
        for(qreal X = std::floor(InRect.left() / InStep) * InStep; X < InRect.right(); X += InStep)
        {
            Lines.push_back(QLineF(X, InRect.top(), X, InRect.bottom()));
        }
        for(qreal Y = std::floor(InRect.top() / InStep) * InStep; Y < InRect.bottom(); Y += InStep)
        {
            Lines.push_back(QLineF(InRect.left(), Y, InRect.right(), Y));
        }
        InPainter->setPen(QPen(InColor, 0.0));
        InPainter->drawLines(Lines);
    };

    /* The minor grid only when it is not a solid wash. */
    const qreal Scale = InPainter->worldTransform().m11();
    if(Scale > 0.5) { DrawGrid(GridMinor, bDark ? QColor(0x26, 0x27, 0x2B) : QColor(0xDF, 0xE0, 0xE5)); }
    DrawGrid(GridMajor, bDark ? QColor(0x30, 0x31, 0x36) : QColor(0xD2, 0xD3, 0xDA));
}

#endif // WITH_EDITOR
