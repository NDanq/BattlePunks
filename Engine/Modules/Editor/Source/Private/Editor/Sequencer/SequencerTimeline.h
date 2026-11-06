#ifndef NIMBLEEDITOR_SEQUENCERTIMELINE_H
#define NIMBLEEDITOR_SEQUENCERTIMELINE_H

#if WITH_EDITOR

#include <vector>

#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

#include "Private/Editor/Sequencer/SequencerDocument.h"

/*
 * The Sequencer's track list and timeline, one painted widget so names and
 * lanes always line up: a column of row names on the left, the ruler along
 * the top, and a lane per row with its keys, sections and events.
 *
 * ROWS. A binding, then its tracks; a Transform track's keyed channels
 * under it. Then the sequence's own tracks under "Sequence". Bindings and
 * Transform tracks collapse (the arrow).
 *
 * MOUSE.
 *   ruler                       scrub (a marker: drag it)
 *   key / section / event       select (Ctrl adds), drag to move; Alt-drag copies
 *   a section's edge            trim it
 *   empty lane                  box select (Ctrl adds)
 *   double-click a lane         add a key, event or section there
 *   Ctrl+wheel, wheel on ruler  zoom about the cursor; Shift+wheel pans; wheel scrolls rows
 *   middle-drag                 pan (time and rows)
 *   right-click                 ContextMenuRequested
 * KEYS. Delete removes the selection; = and - (or Ctrl with them) zoom about
 * the playhead; F fits the whole sequence.
 */
class SequencerTimeline : public QWidget
{
    Q_OBJECT

public:
    struct Row
    {
        enum class EKind : uint8_t { Binding, Track, Channel, Header };
        EKind             Kind = EKind::Track;
        int               Binding = -1;
        SequencerTrackRef Track;
        int               Channel = -1;   // Channel rows
        int               Depth   = 0;
        QString           Label;
        QString           CollapseKey;    // "" if it does not collapse
    };

    static constexpr int NAME_WIDTH   = 290;
    static constexpr int RULER_HEIGHT = 40;

    /* Rows by kind: a binding's header, a track (a section track's taller,
     * for its labels), a Transform channel, the sequence's header. */
    static constexpr int BINDING_HEIGHT = 34;
    static constexpr int TRACK_HEIGHT   = 30;
    static constexpr int SECTION_HEIGHT = 38;
    static constexpr int CHANNEL_HEIGHT = 26;

    explicit SequencerTimeline(SequencerDocument& InDocument, QWidget* InParent = nullptr);

    const std::vector<Row>& GetRows() const { return Rows; }
    int    RowHeight(int InRow) const;
    /* -1 outside every row. */
    int    RowAt(int InY) const;
    QRect  RowRect(int InRow) const;
    double FrameAtX(double InX) const;
    double XOfFrame(double InFrame) const;

    double GetPixelsPerFrame() const { return PixelsPerFrame; }
    void   SetPixelsPerFrame(double InPixels);

    static constexpr double MIN_PIXELS_PER_FRAME = 0.05;
    static constexpr double MAX_PIXELS_PER_FRAME = 80.0;

    /* Zoom by InFactor (>1 in) keeping the frame at lane x InPivotX where it
     * is; a negative pivot keeps the playhead (or the lanes' middle, if the
     * playhead is out of view). */
    void   ZoomBy(double InFactor, double InPivotX = -1.0);
    /* The frame at the lanes' left edge, and how many frames the lanes show. */
    double GetScrollFrame() const { return ScrollFrame; }
    void   SetScrollFrame(double InFrame);
    double GetVisibleFrames() const;
    /* The whole sequence across the lanes. */
    void   FitAll();
    /* Rows scrolled so InTrack's row shows (no change if it already does). */
    void   ScrollToTrack(const SequencerTrackRef& InTrack);

    /* Scrubbing lands on whole frames. */
    void SetSnap(bool bInSnap) { bSnap = bInSnap; }
    bool IsSnapping() const    { return bSnap; }

    /* What is under a lane point; Index -1 for nothing. */
    SequencerItem HitItem(const QPoint& InPos, bool* bOutEdgeStart = nullptr, bool* bOutEdgeEnd = nullptr) const;

    QSize sizeHint() const override { return QSize(900, 320); }

signals:
    /* A right-click: InRow -1 on the ruler. */
    void ContextMenuRequested(int InRow, double InFrame, const QPoint& InGlobalPos);
    /* A name double-clicked: the tab offers a rename. */
    void RowActivated(int InRow);
    /* The zoom or the horizontal scroll changed. */
    void ViewChanged();

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
    enum class EDrag : uint8_t { None, Scrub, Move, TrimStart, TrimEnd, Box, Pan };

    void RebuildRows();
    void PaintRuler(class QPainter& InPainter, const QRect& InRect);
    void PaintRow(class QPainter& InPainter, int InRow, const QRect& InRect);
    QColor TrackColor(Sequencer::ESequenceTrackType InType) const;

    /* The frames InItem sits on, in the live asset. */
    int  ItemFrame(const SequencerItem& InItem) const;
    /* Moves every dragged item to its start frame plus InDelta (live). */
    void ApplyMove(int InDelta);
    void ApplyTrim(int InDelta);
    /* After the drag's step: the selection found again by where it went. */
    void ReselectMoved(int InDelta);
    void ScrubTo(double InX);
    std::vector<SequencerItem> ItemsInRect(const QRect& InRect) const;

    SequencerDocument& Document;
    std::vector<Row>   Rows;

    double PixelsPerFrame = 6.0;
    double ScrollFrame    = 0.0;   // the frame at the lanes' left edge
    bool   bFitPending    = false; // FitAll asked before the lanes had a width: done on the first real size
    int    ScrollRow      = 0;     // the first row shown
    bool   bSnap          = true;

    EDrag   Drag = EDrag::None;
    QPoint  PressPos;
    double  PanStartFrame = 0.0;
    int     PanStartRow   = 0;
    QPoint  DragPos;
    bool    bDragMoved = false;
    int     LastDelta  = 0;
    std::vector<SequencerItem> DragItems;
    std::vector<int>           DragStarts;   // each dragged item's frame at the press
    Sequencer::SequenceSection TrimStartSection;
    int                        TrimResultStart = 0;   // where a trimmed section's start went
    std::vector<SequencerItem> BoxBase;      // the selection a Ctrl box adds to
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SEQUENCERTIMELINE_H
