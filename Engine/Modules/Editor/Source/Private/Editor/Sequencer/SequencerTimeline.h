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
 *   Ctrl+wheel                  zoom about the cursor; Shift+wheel pans; wheel scrolls rows
 *   right-click                 ContextMenuRequested
 * KEYS. Delete removes the selection.
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

    static constexpr int NAME_WIDTH   = 230;
    static constexpr int RULER_HEIGHT = 28;
    static constexpr int ROW_HEIGHT   = 22;

    explicit SequencerTimeline(SequencerDocument& InDocument, QWidget* InParent = nullptr);

    const std::vector<Row>& GetRows() const { return Rows; }
    /* -1 outside every row. */
    int    RowAt(int InY) const;
    QRect  RowRect(int InRow) const;
    double FrameAtX(double InX) const;
    double XOfFrame(double InFrame) const;

    double GetPixelsPerFrame() const { return PixelsPerFrame; }
    void   SetPixelsPerFrame(double InPixels);
    /* The whole sequence across the lanes. */
    void   FitAll();

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

protected:
    void paintEvent(QPaintEvent* InEvent) override;
    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* InEvent) override;
    void wheelEvent(QWheelEvent* InEvent) override;
    void keyPressEvent(QKeyEvent* InEvent) override;
    void contextMenuEvent(QContextMenuEvent* InEvent) override;

private:
    enum class EDrag : uint8_t { None, Scrub, Move, TrimStart, TrimEnd, Box };

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
    int    ScrollRow      = 0;     // the first row shown
    bool   bSnap          = true;

    EDrag   Drag = EDrag::None;
    QPoint  PressPos;
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
