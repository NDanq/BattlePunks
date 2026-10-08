#ifndef NIMBLEEDITOR_CLIPTIMELINE_H
#define NIMBLEEDITOR_CLIPTIMELINE_H

#if WITH_EDITOR

#include <cstdint>
#include <vector>

#include <QString>
#include <QWidget>

/*
 * A clip's timeline in the mesh viewer (ANIMATION.md, "In the editor"): its
 * frames, the playhead, an In/Out range to crop it to, and its notifies as
 * markers. Click or drag on the strip to scrub; drag a marker to move its
 * notify, an In/Out handle to set the range; double-click to add a notify.
 * It shows what it is given and says what was done; the window edits the
 * clip.
 */
class ClipTimeline final : public QWidget
{
    Q_OBJECT

public:
    struct Marker
    {
        QString Name;
        float   Time = 0.0f;
    };

    explicit ClipTimeline(QWidget* InParent = nullptr);

    /* The clip: InFrames frames at InRate. The range is reset to all of it. */
    void SetClip(uint32_t InFrames, float InRate);
    void SetMarkers(std::vector<Marker> InMarkers);
    void SetPlayhead(float InSeconds);
    void SetSelected(int InMarker);     // -1: none
    void SetReadOnly(bool bInReadOnly);

    uint32_t GetRangeFirst() const { return First; }
    uint32_t GetRangeLast() const  { return Last; }
    bool     IsWholeClip() const   { return First == 0 && Last + 1 >= Frames; }

    QSize sizeHint() const override        { return QSize(480, 74); }
    QSize minimumSizeHint() const override { return QSize(160, 74); }

signals:
    void Scrubbed(float InSeconds);
    void MarkerPicked(int InMarker);
    void MarkerMoved(int InMarker, float InSeconds);     // while dragging
    void MarkerDropped(int InMarker);                    // the drag is over
    void AddRequested(float InSeconds);
    void RangeChanged(uint32_t InFirst, uint32_t InLast);

protected:
    void paintEvent(QPaintEvent* InEvent) override;
    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* InEvent) override;

private:
    enum class EDrag { None, Scrub, Marker, First, Last };

    float  Duration() const { return Frames > 1 && Rate > 0.0f ? float(Frames - 1) / Rate : 0.0f; }
    QRectF Strip() const;
    qreal  XOf(float InSeconds) const;
    float  TimeAt(qreal InX) const;                      // clamped to the clip
    uint32_t FrameAt(qreal InX) const;
    int    MarkerAt(const QPointF& InAt) const;          // -1: none
    EDrag  HandleAt(const QPointF& InAt) const;          // First, Last or None

    uint32_t            Frames = 1;
    float               Rate   = 30.0f;
    uint32_t            First  = 0, Last = 0;
    float               Playhead = 0.0f;
    std::vector<Marker> Markers;
    int                 Selected  = -1;
    bool                bReadOnly = false;

    EDrag Drag = EDrag::None;
    int   DragMarker = -1;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_CLIPTIMELINE_H
