#if WITH_EDITOR

#include "Private/Editor/Content/AnimStateGraph.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPathStroker>
#include <QTimer>
#include <QVBoxLayout>

#include "Private/Editor/Graph/GraphView.h"

namespace AnimStateGraphItems
{
    constexpr qreal BOX_W = 170.0;
    constexpr qreal BOX_H = 52.0;
    constexpr qreal GRID  = 10.0;

    const QColor Fill(45, 48, 56);
    const QColor AnyFill(38, 72, 78);
    const QColor Edge(84, 90, 102);
    const QColor Text(222, 224, 230);
    const QColor Dim(150, 155, 165);
    const QColor Entry(224, 154, 62);
    const QColor Live(95, 207, 106);
    const QColor Picked(106, 168, 255);
    const QColor Arrow(170, 174, 184);

    /* A state, or Any State (Index -1). */
    class StateItem final : public QGraphicsItem
    {
    public:
        StateItem(AnimStateGraph* InGraph, const int InIndex, QString InTitle, QString InSubtitle, const bool bInEntry, const bool bInMovable)
            : Graph(InGraph), Index(InIndex), Title(std::move(InTitle)), Subtitle(std::move(InSubtitle)), bEntry(bInEntry)
        {
            setFlag(ItemIsSelectable);
            setFlag(ItemIsMovable, bInMovable);
            setFlag(ItemSendsGeometryChanges);
            setZValue(1.0);
            setAcceptHoverEvents(true);
        }

        QRectF Box() const      { return QRectF(-BOX_W * 0.5, -BOX_H * 0.5, BOX_W, BOX_H); }
        QRectF SceneBox() const { return Box().translated(pos()); }
        QRectF boundingRect() const override { return Box().adjusted(-4.0, -4.0, 4.0, 4.0); }

        void paint(QPainter* InPainter, const QStyleOptionGraphicsItem*, QWidget*) override
        {
            InPainter->setRenderHint(QPainter::Antialiasing);
            const QColor Outline = isSelected() ? Picked : bLive ? Live : Edge;
            QPen Pen(Outline, isSelected() || bLive ? 2.4 : 1.2);
            if(bLive && bBlending && !isSelected()) { Pen.setStyle(Qt::DashLine); }
            InPainter->setPen(Pen);
            InPainter->setBrush(Index < 0 ? AnyFill : Fill);
            InPainter->drawRoundedRect(Box(), 7.0, 7.0);
            if(bEntry)
            {
                /* The entry state: a bar down its left side. */
                InPainter->setPen(Qt::NoPen);
                InPainter->setBrush(Entry);
                InPainter->drawRoundedRect(QRectF(Box().left() + 3.0, Box().top() + 6.0, 4.0, BOX_H - 12.0), 2.0, 2.0);
            }

            QFont Font = InPainter->font();
            Font.setBold(true);
            InPainter->setFont(Font);
            InPainter->setPen(Text);
            const QRectF TitleRect = Subtitle.isEmpty() ? Box() : Box().adjusted(12.0, 6.0, -8.0, -BOX_H * 0.5);
            InPainter->drawText(TitleRect, Qt::AlignCenter | Qt::TextSingleLine,
                                InPainter->fontMetrics().elidedText(Title, Qt::ElideRight, int(TitleRect.width())));
            if(!Subtitle.isEmpty())
            {
                Font.setBold(false);
                Font.setPointSizeF(Font.pointSizeF() * 0.85);
                InPainter->setFont(Font);
                InPainter->setPen(Dim);
                const QRectF SubRect = Box().adjusted(12.0, BOX_H * 0.5, -8.0, -6.0);
                InPainter->drawText(SubRect, Qt::AlignCenter | Qt::TextSingleLine,
                                    InPainter->fontMetrics().elidedText(Subtitle, Qt::ElideMiddle, int(SubRect.width())));
            }
        }

        AnimStateGraph* Graph;
        int             Index;
        QString         Title, Subtitle;
        bool            bEntry    = false;
        bool            bLive     = false;
        bool            bBlending = false;

    protected:
        QVariant itemChange(const GraphicsItemChange InChange, const QVariant& InValue) override
        {
            if(InChange == ItemPositionChange)
            {
                const QPointF At = InValue.toPointF();
                return QPointF(std::round(At.x() / GRID) * GRID, std::round(At.y() / GRID) * GRID);
            }
            if(InChange == ItemPositionHasChanged && Graph)
            {
                Graph->UpdateArrows();
            }
            return QGraphicsItem::itemChange(InChange, InValue);
        }

        void mousePressEvent(QGraphicsSceneMouseEvent* InEvent) override
        {
            Pressed = pos();
            QGraphicsItem::mousePressEvent(InEvent);
        }

        void mouseReleaseEvent(QGraphicsSceneMouseEvent* InEvent) override
        {
            QGraphicsItem::mouseReleaseEvent(InEvent);
            if(pos() != Pressed && Graph) { emit Graph->StateMoved(Index, pos()); }
        }

    private:
        QPointF Pressed;
    };

    /* A transition: a line from box to box, its arrowhead half way. */
    class TransitionItem final : public QGraphicsItem
    {
    public:
        TransitionItem(const int InIndex, const bool bInAtEnd, QString InTip) : Index(InIndex), bAtEnd(bInAtEnd)
        {
            setFlag(ItemIsSelectable);
            setZValue(0.5);
            setToolTip(InTip);
        }

        /* A straight run, or (InLoop) a loop over the top of a box: a state to itself. */
        void Route(const QPointF& InFrom, const QPointF& InTo, const bool bInLoop)
        {
            prepareGeometryChange();
            Path = QPainterPath();
            if(bInLoop)
            {
                Path.moveTo(InFrom);
                Path.cubicTo(InFrom + QPointF(-30.0, -60.0), InTo + QPointF(30.0, -60.0), InTo);
            }
            else
            {
                Path.moveTo(InFrom);
                Path.lineTo(InTo);
            }
            Head = Path.pointAtPercent(0.5);
            const QPointF Ahead = Path.pointAtPercent(0.52);
            Angle = std::atan2(Ahead.y() - Head.y(), Ahead.x() - Head.x());
        }

        QRectF boundingRect() const override { return Path.boundingRect().adjusted(-12.0, -12.0, 12.0, 12.0); }

        QPainterPath shape() const override
        {
            QPainterPathStroker Stroker;
            Stroker.setWidth(12.0);
            QPainterPath Out = Stroker.createStroke(Path);
            Out.addEllipse(Head, 9.0, 9.0);
            return Out;
        }

        void paint(QPainter* InPainter, const QStyleOptionGraphicsItem*, QWidget*) override
        {
            InPainter->setRenderHint(QPainter::Antialiasing);
            const QColor Colour = isSelected() ? Picked : Arrow;
            QPen Pen(Colour, isSelected() ? 2.4 : 1.5);
            if(bAtEnd) { Pen.setStyle(Qt::DashLine); }   // waits for the clip's end
            InPainter->setPen(Pen);
            InPainter->setBrush(Qt::NoBrush);
            InPainter->drawPath(Path);

            const qreal Size = 9.0;
            const QPointF Tip  = Head + QPointF(std::cos(Angle), std::sin(Angle)) * Size * 0.6;
            const QPointF Left = Head + QPointF(std::cos(Angle + 2.5), std::sin(Angle + 2.5)) * Size;
            const QPointF Right= Head + QPointF(std::cos(Angle - 2.5), std::sin(Angle - 2.5)) * Size;
            InPainter->setPen(Qt::NoPen);
            InPainter->setBrush(Colour);
            InPainter->drawPolygon(QPolygonF({ Tip, Left, Right }));
        }

        int Index;

    private:
        bool         bAtEnd;
        QPainterPath Path;
        QPointF      Head;
        qreal        Angle = 0.0;
    };

    /* The canvas: a grid behind; drawing a transition; menus and Delete. */
    class StateScene final : public QGraphicsScene
    {
    public:
        explicit StateScene(AnimStateGraph* InGraph) : QGraphicsScene(InGraph), Graph(InGraph)
        {
            setSceneRect(-6000.0, -6000.0, 12000.0, 12000.0);
        }

    protected:
        void drawBackground(QPainter* InPainter, const QRectF& InRect) override
        {
            InPainter->fillRect(InRect, QColor(30, 32, 37));
            const qreal Step = 40.0;
            InPainter->setPen(QPen(QColor(44, 47, 54), 1.0));
            for(qreal X = std::floor(InRect.left() / Step) * Step; X < InRect.right(); X += Step) { InPainter->drawLine(QPointF(X, InRect.top()), QPointF(X, InRect.bottom())); }
            for(qreal Y = std::floor(InRect.top() / Step) * Step; Y < InRect.bottom(); Y += Step) { InPainter->drawLine(QPointF(InRect.left(), Y), QPointF(InRect.right(), Y)); }
        }

        void mousePressEvent(QGraphicsSceneMouseEvent* InEvent) override
        {
            if(Graph->IsLinking())
            {
                if(InEvent->button() == Qt::LeftButton) { Graph->EndLink(InEvent->scenePos()); }
                else                                    { Graph->CancelLink(); }
                InEvent->accept();
                return;
            }
            QGraphicsScene::mousePressEvent(InEvent);
        }

        void mouseMoveEvent(QGraphicsSceneMouseEvent* InEvent) override
        {
            if(Graph->IsLinking())
            {
                Graph->MoveLink(InEvent->scenePos());
                InEvent->accept();
                return;
            }
            QGraphicsScene::mouseMoveEvent(InEvent);
        }

        void contextMenuEvent(QGraphicsSceneContextMenuEvent* InEvent) override
        {
            if(Graph->IsLinking()) { Graph->CancelLink(); InEvent->accept(); return; }
            Graph->ContextMenu(InEvent->scenePos(), InEvent->screenPos());
            InEvent->accept();
        }

        void keyPressEvent(QKeyEvent* InEvent) override
        {
            if(InEvent->key() == Qt::Key_Escape && Graph->IsLinking()) { Graph->CancelLink(); InEvent->accept(); return; }
            if(InEvent->key() == Qt::Key_Delete || InEvent->key() == Qt::Key_Backspace) { Graph->DeleteSelected(); InEvent->accept(); return; }
            QGraphicsScene::keyPressEvent(InEvent);
        }

    private:
        AnimStateGraph* Graph;
    };

    /* Where a ray from InInside (in InBox) along InDirection leaves the box. */
    QPointF LeaveBox(const QRectF& InBox, const QPointF& InInside, const QPointF& InDirection)
    {
        qreal T = 1e9;
        if(std::fabs(InDirection.x()) > 1e-6) { T = std::min(T, (InDirection.x() > 0.0 ? InBox.right() - InInside.x() : InInside.x() - InBox.left()) / std::fabs(InDirection.x())); }
        if(std::fabs(InDirection.y()) > 1e-6) { T = std::min(T, (InDirection.y() > 0.0 ? InBox.bottom() - InInside.y() : InInside.y() - InBox.top()) / std::fabs(InDirection.y())); }
        return T < 1e8 ? InInside + InDirection * std::max(T, 0.0) : InInside;
    }

    /* A state's second line: its clip, or what it blends on. */
    QString Describe(const RenderCore::AnimStateNode& InState)
    {
        if(InState.Samples.empty()) { return QStringLiteral("no clip"); }
        const QString Clips = QStringLiteral("%1 clips").arg(InState.Samples.size());
        if(InState.Samples.size() > 1 && !InState.BlendParameter.empty())
        {
            if(!InState.BlendParameterY.empty())
            {
                return QStringLiteral("2D: %1 x %2, %3").arg(QString::fromStdString(InState.BlendParameter),
                                                            QString::fromStdString(InState.BlendParameterY), Clips);
            }
            return QStringLiteral("blend: %1, %2").arg(QString::fromStdString(InState.BlendParameter), Clips);
        }
        const QString Clip = QFileInfo(QString::fromStdString(InState.Samples.front().Clip)).completeBaseName();
        return Clip.isEmpty() ? QStringLiteral("no clip") : Clip;
    }
}

using namespace AnimStateGraphItems;

AnimStateGraph::AnimStateGraph(QWidget* InParent) : QWidget(InParent)
{
    Scene = new StateScene(this);
    View  = new GraphView(Scene, this);
    View->setMinimumSize(320, 240);
    Hint = new QLabel(this);
    Hint->setWordWrap(true);
    Hint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->addWidget(View, 1);
    Layout->addWidget(Hint);

    connect(Scene, &QGraphicsScene::selectionChanged, this, &AnimStateGraph::OnSelectionChanged);
}

void AnimStateGraph::SetLayer(const RenderCore::AnimLayer* InLayer, const bool bInReadOnly)
{
    bReadOnly = bInReadOnly;
    bQuiet    = true;
    CancelLink();
    Scene->clear();
    States.clear();
    Arrows.clear();
    Any = nullptr;

    Hint->setText(bReadOnly ? QStringLiteral("Read-only.")
                            : QStringLiteral("Right-click: add a state, make a transition, set the entry. Drag a state to move it; "
                                             "Delete removes what is selected. Dashed arrows wait for the clip's end."));
    if(!InLayer)
    {
        bQuiet = false;
        return;
    }

    Any = new StateItem(this, -1, QStringLiteral("Any State"), QString(), false, !bReadOnly);
    Any->setPos(InLayer->AnyEditorX, InLayer->AnyEditorY);
    Scene->addItem(Any);
    for(size_t Index = 0; Index < InLayer->States.size(); ++Index)
    {
        const RenderCore::AnimStateNode& State = InLayer->States[Index];
        auto* Item = new StateItem(this, int(Index), QString::fromStdString(State.Name), Describe(State),
                                   int32_t(Index) == InLayer->EntryState, !bReadOnly);
        Item->setPos(State.EditorX, State.EditorY);
        Scene->addItem(Item);
        States.push_back(Item);
    }

    for(size_t Index = 0; Index < InLayer->Transitions.size(); ++Index)
    {
        const RenderCore::AnimTransition& Transition = InLayer->Transitions[Index];
        QString Tip = QStringLiteral("%1 -> %2").arg(Transition.From < 0 ? QStringLiteral("Any")
                                                     : size_t(Transition.From) < InLayer->States.size() ? QString::fromStdString(InLayer->States[size_t(Transition.From)].Name) : QStringLiteral("?"),
                                                     size_t(Transition.To) < InLayer->States.size() ? QString::fromStdString(InLayer->States[size_t(Transition.To)].Name) : QStringLiteral("?"));
        Tip += Transition.Conditions.empty() ? QString() : QStringLiteral(", %1 condition(s)").arg(Transition.Conditions.size());
        if(Transition.bAtEnd) { Tip += QStringLiteral(", at the clip's end"); }
        auto* Item = new TransitionItem(int(Index), Transition.bAtEnd, Tip);
        Scene->addItem(Item);
        Arrows.push_back(Item);
    }

    /* The arrows' ends, by index: kept for UpdateArrows. */
    LinkEnds.clear();
    for(const RenderCore::AnimTransition& Transition : InLayer->Transitions) { LinkEnds.emplace_back(Transition.From, Transition.To); }
    UpdateArrows();
    bQuiet = false;

    if(!bFramed)
    {
        bFramed = true;
        QTimer::singleShot(0, this, &AnimStateGraph::FrameAll);   // once laid out: the view has its size
    }
}

void AnimStateGraph::UpdateArrows()
{
    /* Same way, same pair: side by side. Opposite ways part on their own --
     * each is offset to the right of its own direction. */
    std::map<std::pair<int, int>, int> Seen;
    for(size_t Index = 0; Index < Arrows.size() && Index < LinkEnds.size(); ++Index)
    {
        const int From = LinkEnds[Index].first, To = LinkEnds[Index].second;
        StateItem* A = From < 0 ? Any : size_t(From) < States.size() ? States[size_t(From)] : nullptr;
        StateItem* B = To >= 0 && size_t(To) < States.size() ? States[size_t(To)] : nullptr;
        if(!A || !B)
        {
            Arrows[Index]->setVisible(false);
            continue;
        }
        Arrows[Index]->setVisible(true);
        const int Nth = Seen[{ From, To }]++;
        if(A == B)
        {
            const QRectF Box = A->SceneBox();
            const qreal  Spread = 20.0 + 14.0 * Nth;
            Arrows[Index]->Route(QPointF(Box.center().x() - Spread, Box.top()), QPointF(Box.center().x() + Spread, Box.top()), true);
            continue;
        }
        const QPointF CA = A->pos(), CB = B->pos();
        QPointF Direction = CB - CA;
        const qreal Length = std::hypot(Direction.x(), Direction.y());
        if(Length < 1e-3) { Arrows[Index]->setVisible(false); continue; }
        Direction /= Length;
        const QPointF Right(-Direction.y(), Direction.x());
        const QPointF Offset = Right * (9.0 + 12.0 * Nth);
        Arrows[Index]->Route(LeaveBox(A->SceneBox(), CA + Offset, Direction), LeaveBox(B->SceneBox(), CB + Offset, -Direction), false);
    }
}

void AnimStateGraph::SetSelection(const int InState, const int InTransition)
{
    bQuiet = true;
    Scene->clearSelection();
    if(InState >= 0 && size_t(InState) < States.size())                { States[size_t(InState)]->setSelected(true); }
    else if(InTransition >= 0 && size_t(InTransition) < Arrows.size()) { Arrows[size_t(InTransition)]->setSelected(true); }
    bQuiet = false;
}

void AnimStateGraph::SetLive(const int InState, const bool bInBlending)
{
    for(StateItem* Each : States)
    {
        const bool bLive = Each->Index == InState;
        if(Each->bLive != bLive || Each->bBlending != (bLive && bInBlending))
        {
            Each->bLive     = bLive;
            Each->bBlending = bLive && bInBlending;
            Each->update();
        }
    }
}

void AnimStateGraph::FrameAll()
{
    QRectF Bounds;
    for(QGraphicsItem* Each : Scene->items()) { Bounds |= Each->sceneBoundingRect(); }
    View->Frame(Bounds);
}

void AnimStateGraph::LayOut(RenderCore::AnimLayer& InOutLayer)
{
    /* Entry first, then the rest in order, in rows of a near-square grid. */
    std::vector<size_t> Order;
    if(InOutLayer.EntryState >= 0 && size_t(InOutLayer.EntryState) < InOutLayer.States.size()) { Order.push_back(size_t(InOutLayer.EntryState)); }
    for(size_t Index = 0; Index < InOutLayer.States.size(); ++Index)
    {
        if(int32_t(Index) != InOutLayer.EntryState) { Order.push_back(Index); }
    }
    const size_t Columns = std::max<size_t>(1, size_t(std::ceil(std::sqrt(double(Order.size())))));
    for(size_t At = 0; At < Order.size(); ++At)
    {
        InOutLayer.States[Order[At]].EditorX = float(240.0 * double(At % Columns));
        InOutLayer.States[Order[At]].EditorY = float(130.0 * double(At / Columns));
    }
    InOutLayer.AnyEditorX    = -250.0f;
    InOutLayer.AnyEditorY    = -130.0f;
    InOutLayer.bEditorPlaced = true;
}

void AnimStateGraph::OnSelectionChanged()
{
    if(bQuiet) { return; }
    const QList<QGraphicsItem*> Selected = Scene->selectedItems();
    for(QGraphicsItem* Each : Selected)
    {
        if(auto* State = dynamic_cast<StateItem*>(Each); State && State->Index >= 0) { emit StateSelected(State->Index); return; }
        if(auto* Arrow = dynamic_cast<TransitionItem*>(Each))                        { emit TransitionSelected(Arrow->Index); return; }
    }
    emit NothingSelected();
}

StateItem* AnimStateGraph::StateAt(const QPointF& InAt) const
{
    for(QGraphicsItem* Each : Scene->items(InAt))
    {
        if(auto* State = dynamic_cast<StateItem*>(Each)) { return State; }
    }
    return nullptr;
}

void AnimStateGraph::BeginLink(const int InFrom)
{
    StateItem* Source = InFrom < 0 ? Any : size_t(InFrom) < States.size() ? States[size_t(InFrom)] : nullptr;
    if(!Source || bReadOnly) { return; }
    LinkFrom = InFrom;
    LinkLine = Scene->addLine(QLineF(Source->pos(), View->CursorScenePos()), QPen(Picked, 1.6, Qt::DashLine));
    LinkLine->setZValue(2.0);
    View->setDragMode(QGraphicsView::NoDrag);
    View->setMouseTracking(true);
    Hint->setText(QStringLiteral("Click the state it goes to (Esc or right-click: cancel)."));
}

void AnimStateGraph::MoveLink(const QPointF& InAt)
{
    if(LinkLine) { LinkLine->setLine(QLineF(LinkLine->line().p1(), InAt)); }
}

void AnimStateGraph::EndLink(const QPointF& InAt)
{
    const int From = LinkFrom;
    StateItem* Target = StateAt(InAt);
    CancelLink();
    if(Target && Target->Index >= 0) { emit AddTransitionRequested(From, Target->Index); }
}

void AnimStateGraph::CancelLink()
{
    LinkFrom = NO_LINK;
    if(LinkLine)
    {
        Scene->removeItem(LinkLine);
        delete LinkLine;
        LinkLine = nullptr;
    }
    View->setDragMode(QGraphicsView::RubberBandDrag);
    Hint->setText(bReadOnly ? QStringLiteral("Read-only.")
                            : QStringLiteral("Right-click: add a state, make a transition, set the entry. Drag a state to move it; "
                                             "Delete removes what is selected. Dashed arrows wait for the clip's end."));
}

void AnimStateGraph::ContextMenu(const QPointF& InAt, const QPoint& InScreen)
{
    if(bReadOnly) { return; }
    QMenu Menu(this);
    QGraphicsItem* Under = nullptr;
    for(QGraphicsItem* Each : Scene->items(InAt))
    {
        if(dynamic_cast<StateItem*>(Each) || dynamic_cast<TransitionItem*>(Each)) { Under = Each; break; }
    }
    if(auto* State = dynamic_cast<StateItem*>(Under))
    {
        const int Index = State->Index;
        Menu.addAction(QStringLiteral("Make Transition"), this, [this, Index]() { BeginLink(Index); });
        if(Index >= 0)
        {
            QAction* Entry = Menu.addAction(QStringLiteral("Set as Entry"), this, [this, Index]() { emit EntryRequested(Index); });
            Entry->setEnabled(!State->bEntry);
            Menu.addSeparator();
            Menu.addAction(QStringLiteral("Delete State"), this, [this, Index]() { emit RemoveStateRequested(Index); });
        }
    }
    else if(auto* Arrow = dynamic_cast<TransitionItem*>(Under))
    {
        const int Index = Arrow->Index;
        Menu.addAction(QStringLiteral("Delete Transition"), this, [this, Index]() { emit RemoveTransitionRequested(Index); });
    }
    else
    {
        Menu.addAction(QStringLiteral("Add State"), this, [this, InAt]() { emit AddStateRequested(InAt); });
        Menu.addAction(QStringLiteral("Frame All"), this, &AnimStateGraph::FrameAll);
    }
    Menu.exec(InScreen);
}

void AnimStateGraph::DeleteSelected()
{
    if(bReadOnly) { return; }
    for(QGraphicsItem* Each : Scene->selectedItems())
    {
        if(auto* State = dynamic_cast<StateItem*>(Each); State && State->Index >= 0) { emit RemoveStateRequested(State->Index); return; }
        if(auto* Arrow = dynamic_cast<TransitionItem*>(Each))                        { emit RemoveTransitionRequested(Arrow->Index); return; }
    }
}

#endif // WITH_EDITOR
