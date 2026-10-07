#if WITH_EDITOR

#include "Private/Editor/Content/ClipTimeline.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace
{
    constexpr qreal SIDE   = 10.0;   // room either side for the handles
    constexpr qreal TOP    = 22.0;   // the notifies' names above the strip
    constexpr qreal STRIP  = 30.0;
    constexpr qreal MARKER = 6.0;    // a marker's half width

    const QColor Back(30, 32, 37);
    const QColor Band(48, 52, 60);
    const QColor Outside(20, 21, 25, 200);
    const QColor Tick(86, 92, 104);
    const QColor Label(170, 174, 184);
    const QColor Range(224, 154, 62);
    const QColor Head(106, 168, 255);
    const QColor Notify(95, 207, 106);
    const QColor Picked(255, 255, 255);
}

ClipTimeline::ClipTimeline(QWidget* InParent) : QWidget(InParent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(QStringLiteral("Click or drag to scrub. Drag a notify to move it; double-click to add one. "
                              "Drag the orange In and Out handles to choose a range, then Crop to Range"));
}

void ClipTimeline::SetClip(const uint32_t InFrames, const float InRate)
{
    Frames = std::max<uint32_t>(InFrames, 1);
    Rate   = InRate > 0.0f ? InRate : 30.0f;
    First  = 0;
    Last   = Frames - 1;
    update();
}

void ClipTimeline::SetMarkers(std::vector<Marker> InMarkers)
{
    Markers = std::move(InMarkers);
    if(Selected >= int(Markers.size())) { Selected = -1; }
    update();
}

void ClipTimeline::SetPlayhead(const float InSeconds)
{
    if(std::fabs(InSeconds - Playhead) < 1e-5f) { return; }
    Playhead = InSeconds;
    update();
}

void ClipTimeline::SetSelected(const int InMarker)
{
    Selected = InMarker;
    update();
}

void ClipTimeline::SetReadOnly(const bool bInReadOnly)
{
    bReadOnly = bInReadOnly;
    update();
}

QRectF ClipTimeline::Strip() const
{
    return QRectF(SIDE, TOP, std::max(1.0, width() - 2.0 * SIDE), STRIP);
}

qreal ClipTimeline::XOf(const float InSeconds) const
{
    const QRectF S = Strip();
    return Duration() > 0.0f ? S.left() + S.width() * qreal(InSeconds / Duration()) : S.left();
}

float ClipTimeline::TimeAt(const qreal InX) const
{
    const QRectF S = Strip();
    return std::clamp(float((InX - S.left()) / S.width()), 0.0f, 1.0f) * Duration();
}

uint32_t ClipTimeline::FrameAt(const qreal InX) const
{
    return std::min(Frames - 1, uint32_t(std::lround(TimeAt(InX) * Rate)));
}

int ClipTimeline::MarkerAt(const QPointF& InAt) const
{
    /* The diamond and the name's row above it. */
    for(int Index = int(Markers.size()) - 1; Index >= 0; --Index)
    {
        const qreal X = XOf(Markers[size_t(Index)].Time);
        if(std::fabs(InAt.x() - X) <= MARKER + 2.0 && InAt.y() <= Strip().top() + 10.0) { return Index; }
    }
    return -1;
}

ClipTimeline::EDrag ClipTimeline::HandleAt(const QPointF& InAt) const
{
    if(InAt.y() < Strip().bottom() - 10.0) { return EDrag::None; }
    const qreal FirstX = XOf(float(First) / Rate), LastX = XOf(float(Last) / Rate);
    if(std::fabs(InAt.x() - FirstX) <= 6.0) { return EDrag::First; }
    if(std::fabs(InAt.x() - LastX) <= 6.0)  { return EDrag::Last; }
    return EDrag::None;
}

void ClipTimeline::paintEvent(QPaintEvent*)
{
    QPainter Painter(this);
    Painter.setRenderHint(QPainter::Antialiasing);
    Painter.fillRect(rect(), Back);
    const QRectF S = Strip();
    Painter.fillRect(S, Band);

    /* Frame ticks: every frame when there is room, else every fifth or tenth; seconds labelled. */
    const qreal PerFrame = Frames > 1 ? S.width() / qreal(Frames - 1) : S.width();
    const uint32_t Every = PerFrame >= 6.0 ? 1u : PerFrame >= 1.5 ? 5u : 10u;
    Painter.setPen(QPen(Tick, 1.0));
    for(uint32_t Frame = 0; Frame < Frames; Frame += Every)
    {
        const qreal X = S.left() + PerFrame * qreal(Frame);
        const bool bMajor = Frame % (Every * 5) == 0;
        Painter.drawLine(QPointF(X, S.bottom()), QPointF(X, S.bottom() - (bMajor ? 10.0 : 5.0)));
    }
    QFont Small = font();
    Small.setPointSizeF(Small.pointSizeF() * 0.8);
    Painter.setFont(Small);
    Painter.setPen(Label);
    Painter.drawText(QRectF(S.left(), S.bottom() + 2.0, S.width(), 16.0), Qt::AlignLeft, QStringLiteral("0"));
    Painter.drawText(QRectF(S.left(), S.bottom() + 2.0, S.width(), 16.0), Qt::AlignRight,
                     QStringLiteral("%1 s, %2 frames").arg(double(Duration()), 0, 'f', 2).arg(Frames));
    if(!IsWholeClip())
    {
        Painter.drawText(QRectF(S.left(), S.bottom() + 2.0, S.width(), 16.0), Qt::AlignHCenter,
                         QStringLiteral("range %1..%2 (%3 frames)").arg(First).arg(Last).arg(Last - First + 1));
    }

    /* Outside the range, shaded; its In and Out handles. */
    const qreal FirstX = XOf(float(First) / Rate), LastX = XOf(float(Last) / Rate);
    Painter.fillRect(QRectF(S.left(), S.top(), FirstX - S.left(), S.height()), Outside);
    Painter.fillRect(QRectF(LastX, S.top(), S.right() - LastX, S.height()), Outside);
    if(!bReadOnly)
    {
        Painter.setPen(QPen(Range, 2.0));
        for(const qreal X : { FirstX, LastX })
        {
            Painter.drawLine(QPointF(X, S.top()), QPointF(X, S.bottom()));
            QPainterPath Grip;
            const qreal Dir = X == FirstX ? 1.0 : -1.0;
            Grip.moveTo(X, S.bottom());
            Grip.lineTo(X + Dir * 7.0, S.bottom());
            Grip.lineTo(X, S.bottom() - 9.0);
            Grip.closeSubpath();
            Painter.fillPath(Grip, Range);
        }
    }

    /* Notifies: a diamond on the strip's top edge, the name above. */
    for(size_t Index = 0; Index < Markers.size(); ++Index)
    {
        const qreal X = XOf(Markers[Index].Time);
        const bool  bPicked = int(Index) == Selected;
        QPainterPath Diamond;
        Diamond.moveTo(X, S.top() - MARKER);
        Diamond.lineTo(X + MARKER, S.top());
        Diamond.lineTo(X, S.top() + MARKER);
        Diamond.lineTo(X - MARKER, S.top());
        Diamond.closeSubpath();
        Painter.fillPath(Diamond, bPicked ? Picked : Notify);
        Painter.setPen(QPen(bPicked ? Picked : Notify, 1.0, Qt::DotLine));
        Painter.drawLine(QPointF(X, S.top() + MARKER), QPointF(X, S.bottom()));
        Painter.setPen(bPicked ? Picked : Label);
        const QString Name = Painter.fontMetrics().elidedText(Markers[Index].Name, Qt::ElideRight, 90);
        Painter.drawText(QRectF(X - 60.0, 0.0, 120.0, S.top() - MARKER), Qt::AlignHCenter | Qt::AlignBottom, Name);
    }

    /* The playhead, over everything. */
    const qreal HeadX = XOf(std::clamp(Playhead, 0.0f, Duration()));
    Painter.setPen(QPen(Head, 2.0));
    Painter.drawLine(QPointF(HeadX, S.top() - 2.0), QPointF(HeadX, S.bottom() + 2.0));
}

void ClipTimeline::mousePressEvent(QMouseEvent* InEvent)
{
    if(InEvent->button() != Qt::LeftButton) { QWidget::mousePressEvent(InEvent); return; }
    const QPointF At = InEvent->position();
    if(!bReadOnly)
    {
        if(const int Marker = MarkerAt(At); Marker >= 0)
        {
            Drag = EDrag::Marker;
            DragMarker = Marker;
            Selected = Marker;
            emit MarkerPicked(Marker);
            update();
            return;
        }
        if(const EDrag Handle = HandleAt(At); Handle != EDrag::None)
        {
            Drag = Handle;
            return;
        }
    }
    Drag = EDrag::Scrub;
    emit Scrubbed(TimeAt(At.x()));
}

void ClipTimeline::mouseMoveEvent(QMouseEvent* InEvent)
{
    const QPointF At = InEvent->position();
    switch(Drag)
    {
        case EDrag::Scrub:
            emit Scrubbed(TimeAt(At.x()));
            break;
        case EDrag::Marker:
            if(DragMarker >= 0 && size_t(DragMarker) < Markers.size())
            {
                /* On a frame when near one: notifies usually sit on frames. */
                float Time = TimeAt(At.x());
                const float OnFrame = std::round(Time * Rate) / Rate;
                if(std::fabs(XOf(OnFrame) - XOf(Time)) < 4.0) { Time = OnFrame; }
                Markers[size_t(DragMarker)].Time = Time;
                emit MarkerMoved(DragMarker, Time);
                update();
            }
            break;
        case EDrag::First:
        case EDrag::Last:
        {
            const uint32_t Frame = FrameAt(At.x());
            if(Drag == EDrag::First) { First = std::min(Frame, Last); }
            else                     { Last  = std::max(Frame, First); }
            emit RangeChanged(First, Last);
            update();
            break;
        }
        case EDrag::None:
        {
            /* The cursor says what a press would do. */
            const bool bGrab = !bReadOnly && (MarkerAt(At) >= 0 || HandleAt(At) != EDrag::None);
            setCursor(bGrab ? Qt::SizeHorCursor : Qt::ArrowCursor);
            break;
        }
    }
}

void ClipTimeline::mouseReleaseEvent(QMouseEvent* InEvent)
{
    if(Drag == EDrag::Marker && DragMarker >= 0) { emit MarkerDropped(DragMarker); }
    Drag = EDrag::None;
    DragMarker = -1;
    QWidget::mouseReleaseEvent(InEvent);
}

void ClipTimeline::mouseDoubleClickEvent(QMouseEvent* InEvent)
{
    if(bReadOnly || InEvent->button() != Qt::LeftButton || MarkerAt(InEvent->position()) >= 0) { return; }
    emit AddRequested(TimeAt(InEvent->position().x()));
}

#endif // WITH_EDITOR
