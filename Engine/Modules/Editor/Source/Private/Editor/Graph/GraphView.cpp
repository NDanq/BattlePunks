#if WITH_EDITOR

#include "Private/Editor/Graph/GraphView.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>

namespace
{
    constexpr qreal ZoomStep = 1.15;
    constexpr qreal ZoomMin  = 0.15;
    constexpr qreal ZoomMax  = 2.5;
}

GraphView::GraphView(QGraphicsScene* InScene, QWidget* InParent)
    : QGraphicsView(InScene, InParent)
{
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setDragMode(QGraphicsView::RubberBandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);   // bezier links repaint cleanly
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);
    setContextMenuPolicy(Qt::DefaultContextMenu);
}

void GraphView::Frame(const QRectF& InRect)
{
    if(InRect.isEmpty()) { return; }
    fitInView(InRect.adjusted(-60.0, -60.0, 60.0, 60.0), Qt::KeepAspectRatio);
    if(transform().m11() > 1.0)
    {
        const QPointF Centre = InRect.center();
        setTransform(QTransform());
        centerOn(Centre);
    }
}

QPointF GraphView::CursorScenePos() const
{
    const QPoint Local = mapFromGlobal(QCursor::pos());
    return rect().contains(Local) ? mapToScene(Local) : mapToScene(rect().center());
}

void GraphView::wheelEvent(QWheelEvent* InEvent)
{
    const qreal Steps  = InEvent->angleDelta().y() / 120.0;
    const qreal Target = std::clamp(transform().m11() * std::pow(ZoomStep, Steps), ZoomMin, ZoomMax);
    const qreal Factor = Target / transform().m11();
    scale(Factor, Factor);
    InEvent->accept();
}

void GraphView::mousePressEvent(QMouseEvent* InEvent)
{
    if(InEvent->button() == Qt::MiddleButton)
    {
        bPanning = true;
        PanLast = InEvent->pos();
        setCursor(Qt::ClosedHandCursor);
        InEvent->accept();
        return;
    }
    QGraphicsView::mousePressEvent(InEvent);
}

void GraphView::mouseMoveEvent(QMouseEvent* InEvent)
{
    if(bPanning)
    {
        const QPoint Delta = InEvent->pos() - PanLast;
        PanLast = InEvent->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - Delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - Delta.y());
        InEvent->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(InEvent);
}

void GraphView::mouseReleaseEvent(QMouseEvent* InEvent)
{
    if(bPanning && InEvent->button() == Qt::MiddleButton)
    {
        bPanning = false;
        unsetCursor();
        InEvent->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(InEvent);
}

#endif // WITH_EDITOR
