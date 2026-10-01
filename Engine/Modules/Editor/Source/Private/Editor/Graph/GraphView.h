#ifndef NIMBLEEDITOR_GRAPHVIEW_H
#define NIMBLEEDITOR_GRAPHVIEW_H

#if WITH_EDITOR

#include <QGraphicsView>
#include <QPoint>

/*
 * The canvas's camera: wheel to zoom about the cursor, middle-drag to pan,
 * left-drag on empty canvas to box-select.
 */
class GraphView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit GraphView(QGraphicsScene* InScene, QWidget* InParent = nullptr);

    /* Fits InRect (scene coordinates) in view, never zooming in past 1:1. */
    void Frame(const QRectF& InRect);

    /* Under the cursor, else the view centre: where new nodes appear. */
    QPointF CursorScenePos() const;

protected:
    void wheelEvent(QWheelEvent* InEvent) override;
    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;

private:
    bool   bPanning = false;
    QPoint PanLast;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHVIEW_H
