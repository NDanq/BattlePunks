#ifndef NIMBLEEDITOR_SEQUENCERDOCUMENT_H
#define NIMBLEEDITOR_SEQUENCERDOCUMENT_H

#if WITH_EDITOR

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QString>

#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Sequencer/SequenceAsset.h"

class QUndoStack;

/* A track by where it lives: Binding -1 for the sequence's own tracks. */
struct SequencerTrackRef
{
    int Binding = -1;
    int Track   = -1;

    bool IsValid() const { return Track >= 0; }
    friend bool operator==(const SequencerTrackRef& L, const SequencerTrackRef& R) { return L.Binding == R.Binding && L.Track == R.Track; }
    friend bool operator!=(const SequencerTrackRef& L, const SequencerTrackRef& R) { return !(L == R); }
};

/* One selected thing on the timeline. */
struct SequencerItem
{
    enum class EKind : uint8_t { Key, Section, Event, Marker };

    EKind             Kind = EKind::Key;
    SequencerTrackRef Track;          // not for a Marker
    int               Channel = 0;    // Key: the channel (a Transform track's 0..8)
    int               Index   = -1;   // the key, section, event or marker

    friend bool operator==(const SequencerItem& L, const SequencerItem& R)
    {
        return L.Kind == R.Kind && L.Track == R.Track && L.Channel == R.Channel && L.Index == R.Index;
    }
};

/*
 * One sequence open in the Sequencer: the asset, what is selected, the
 * playhead, and an undo history of its own. Every edit is a before/after
 * snapshot of the whole asset -- a cutscene's worth of plain data -- as the
 * Particle Editor's are.
 *
 *   Edit(Text, Change)   a change made here, one undo step
 *   BeginDrag/EndDrag    a drag: GetLiveAsset changed freely in between,
 *                        shown (LiveChanged), recorded as one step at the end
 *
 * Signals: Changed after any change to the asset (undo included),
 * LiveChanged during a drag, SelectionChanged, PlayheadChanged, RowsChanged
 * when the rows the timeline shows changed (bindings, tracks, collapsing).
 */
class SequencerDocument : public QObject
{
    Q_OBJECT

public:
    explicit SequencerDocument(QObject* InParent = nullptr);
    ~SequencerDocument() override;

    /* From a file on disk (either form); false with OutError, leaving the document as it was. */
    bool Load(const QString& InFilePath, QString& OutError);
    /* A new, unsaved sequence (tests; Create writes one first). */
    void Reset(const Sequencer::SequenceAsset& InAsset);

    /* To a file on disk, as XML; marks the history clean. */
    bool Save(const QString& InFilePath, QString& OutError);

    const Sequencer::SequenceAsset& GetAsset() const { return Asset; }
    /* A copy for a player or preview; the same pointer until the next change. */
    std::shared_ptr<const Sequencer::SequenceAsset> GetSnapshot();

    /* What a drag edits, between BeginDrag and EndDrag. */
    Sequencer::SequenceAsset& GetLiveAsset() { return Asset; }

    const Sequencer::SequenceTrack* FindTrack(const SequencerTrackRef& InRef) const;
    Sequencer::SequenceTrack*       FindLiveTrack(const SequencerTrackRef& InRef);

    // --- Editing -----------------------------------------------------------------

    void Edit(const QString& InText, const std::function<void(Sequencer::SequenceAsset&)>& InChange);

    void BeginDrag();
    /* Shows the live asset; nothing recorded. */
    void PreviewDrag();
    /* Records the drag as one step (nothing if it changed nothing); bInCancel puts it back. */
    void EndDrag(const QString& InText, bool bInCancel = false);
    bool IsDragging() const { return bDragging; }

    // --- Common edits, each one undo step ---------------------------------------------

    /* A binding to an entity of the level, by its guid and name. Its index. */
    int  AddPossessBinding(const QString& InName, const GameCore::EntityGuid& InGuid);
    int  AddSpawnBinding(const QString& InName, const QString& InPrefab);
    void RemoveBinding(int InBinding);

    /* InType on InBinding (-1: the sequence's own). Its ref. */
    SequencerTrackRef AddTrack(int InBinding, Sequencer::ESequenceTrackType InType, const QString& InComponent = QString(),
                               const QString& InField = QString());
    void RemoveTrack(const SequencerTrackRef& InTrack);

    /* A key at InFrame on InChannel: replaced if one is there. */
    void SetKey(const SequencerTrackRef& InTrack, int InChannel, int InFrame, float InValue, const QString& InText = QStringLiteral("Set Key"));
    /* A section [InStart, InEnd) naming InAsset; selected. */
    void AddSection(const SequencerTrackRef& InTrack, int InStart, int InEnd, const QString& InAsset);
    void AddEvent(const SequencerTrackRef& InTrack, int InFrame, const QString& InName);
    void AddMarker(int InFrame, const QString& InName);

    /* Everything selected, gone. */
    void DeleteSelection();

    /*
     * Auto-key: InBinding's Transform track (made if it has none) keyed at
     * InFrame for each channel InAfter moved from InBefore (the entity as
     * the sequence showed it). A channel keyed for the first time away from
     * frame 0 also gets InBefore's value at 0. bInAll keys InAfter's
     * position and rotation whatever moved (and scale where it is keyed or
     * not 1): the K key. False if nothing was keyed.
     */
    bool KeyTransform(int InBinding, const GameCore::TransformPose& InBefore, const GameCore::TransformPose& InAfter, int InFrame,
                      bool bInAll = false);

    /* The value InTrack's InChannel gives at InFrame; InDefault with no keys. */
    float EvaluateChannel(const SequencerTrackRef& InTrack, int InChannel, double InFrame, float InDefault) const;

    // --- Selection and playhead ------------------------------------------------

    const std::vector<SequencerItem>& GetSelection() const { return Selection; }
    void SetSelection(const std::vector<SequencerItem>& InItems);
    void ClearSelection() { SetSelection({}); }
    bool IsSelected(const SequencerItem& InItem) const;

    /* The row picked in the track list (for adding keys, the details). */
    const SequencerTrackRef& GetCurrentTrack() const { return CurrentTrack; }
    int  GetCurrentBinding() const { return CurrentBinding; }
    void SetCurrent(int InBinding, const SequencerTrackRef& InTrack);

    double GetPlayhead() const { return Playhead; }
    void   SetPlayhead(double InFrame);

    /* Rows collapsed in the timeline: a binding by id, a Transform track by ref. */
    bool IsCollapsed(const QString& InKey) const;
    void SetCollapsed(const QString& InKey, bool bInCollapsed);

    QUndoStack& GetHistory() { return *History; }
    bool IsDirty() const;

signals:
    void Changed();
    void LiveChanged();
    void SelectionChanged();
    void PlayheadChanged(double InFrame);
    void RowsChanged();

private:
    friend class SequencerSnapshotCommand;

    /* Undo and redo: the asset as it was. */
    void Restore(const Sequencer::SequenceAsset& InAsset);
    void Record(const QString& InText, Sequencer::SequenceAsset InBefore);
    /* Drops selected items and the current track that no longer exist. */
    void PruneSelection();
    QString UniqueBindingId(const QString& InBase) const;

    Sequencer::SequenceAsset   Asset;
    Sequencer::SequenceAsset   DragBefore;
    bool                       bDragging = false;
    std::shared_ptr<const Sequencer::SequenceAsset> Snapshot;

    std::vector<SequencerItem> Selection;
    SequencerTrackRef          CurrentTrack;
    int                        CurrentBinding = -1;
    double                     Playhead = 0.0;
    std::vector<QString>       Collapsed;
    QUndoStack*                History = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SEQUENCERDOCUMENT_H
