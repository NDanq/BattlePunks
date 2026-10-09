#ifndef NIMBLEEDITOR_SEQUENCERCURVEEDITOR_H
#define NIMBLEEDITOR_SEQUENCERCURVEEDITOR_H

#if WITH_EDITOR

#include <vector>

#include <QPointF>
#include <QWidget>

#include "Private/Editor/Sequencer/SequencerDocument.h"

/*
 * The curves of the current track: a Transform track's keyed channels (X
 * red, Y green, Z blue), or a Float or Fade track's one, value against
 * frame, fitted to the view.
 *
 *   click a key               select it (Ctrl adds); drag moves it in time and value
 *   drag a tangent handle     the key becomes User and leaves at that slope;
 *                             Alt breaks the two sides apart
 *   double-click near a curve add a key there
 *   right-click a key         its interpolation
 *   wheel                     zoom time about the cursor; Ctrl+wheel zooms value
 *   middle-drag               pan
 *   F                         fit the view
 */
class SequencerCurveEditor : public QWidget
{
    Q_OBJECT

public:
    explicit SequencerCurveEditor(SequencerDocument& InDocument, QWidget* InParent = nullptr);

    /* The channels shown: the current track's that have keys (a one-channel track's always). */
    std::vector<int> GetChannels() const;

    void FitView();

    QPointF ToScreen(double InFrame, double InValue) const;
    double  FrameAt(double InX) const;
    double  ValueAt(double InY) const;

    QSize sizeHint() const override { return QSize(800, 240); }

protected:
    void paintEvent(QPaintEvent* InEvent) override;
    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* InEvent) override;
    void wheelEvent(QWheelEvent* InEvent) override;
    void keyPressEvent(QKeyEvent* InEvent) override;
    void contextMenuEvent(QContextMenuEvent* InEvent) override;
    void resizeEvent(QResizeEvent* InEvent) override;

private:
    enum class EDrag : uint8_t { None, Key, In, Out, Pan };

    struct KeyHit
    {
        int    Channel = -1;
        int    Index   = -1;
        EDrag  Part    = EDrag::None;
    };

    KeyHit HitAt(const QPointF& InPos) const;
    QColor ChannelColor(int InChannel) const;
    /* Where a key's tangent handle sits on screen. */
    QPointF HandlePos(int InChannel, int InIndex, bool bInLeaving) const;

    SequencerDocument& Document;
    SequencerTrackRef  ShownTrack;

    double FrameMin = 0.0, FrameMax = 100.0;
    double ValueMin = -1.0, ValueMax = 1.0;

    EDrag   Drag = EDrag::None;
    KeyHit  Dragged;
    QPointF PressPos;
    double  PanFrame[2] = {}, PanValue[2] = {};
    bool    bMoved = false;
    bool    bBroken = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SEQUENCERCURVEEDITOR_H
