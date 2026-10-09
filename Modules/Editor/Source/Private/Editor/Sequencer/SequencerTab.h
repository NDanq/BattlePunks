#ifndef NIMBLEEDITOR_SEQUENCERTAB_H
#define NIMBLEEDITOR_SEQUENCERTAB_H

#if WITH_EDITOR

#include <memory>
#include <set>
#include <vector>

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>

#include "Public/Editor/EditorCamera.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Sequencer/SequenceInstance.h"

class EditorContext;
class QAction;
class QLabel;
class QTimer;
class SequencerCurveEditor;
class SequencerDetails;
class SequencerDocument;
class SequencerTimeline;

/*
 * One sequence open in the Sequencer: the timeline and track list, the
 * curve editor under them, the details beside, and a toolbar.
 *
 * THE PREVIEW. While open (and the editor is not playing) the sequence is
 * shown on the level being edited, at the playhead: a SequenceInstance
 * that makes nothing in the level -- no sounds, spawns or camera entity.
 * Everything it changed goes back when the tab closes, Play starts, or the
 * level is saved (and comes back after the save), so the level's file
 * never holds a preview.
 *
 * AUTO-KEY. On, moving a bound entity (the gizmo, the details) keys what
 * moved at the playhead instead of leaving it moved. K keys the current
 * binding's whole pose.
 *
 * CAMERA CUTS. On, the level viewport looks through the shot at the
 * playhead. PILOT, on too, makes flying the viewport move the shot's
 * camera (keyed when it settles, with auto-key).
 */
class SequencerTab : public QMainWindow
{
    Q_OBJECT

public:
    explicit SequencerTab(EditorContext& InContext, QWidget* InParent = nullptr);
    ~SequencerTab() override;

    bool Load(const QString& InPath, QString& OutError);

    const QString& GetPath() const { return Path; }
    QString        GetRuntimePath() const;
    QString        GetTabTitle() const;
    bool           IsDirty() const;
    bool           Save();
    /* Asks to save if dirty; puts the level back. False if cancelled. */
    bool           ConfirmClose();

    SequencerDocument&           GetDocument()   { return *Document; }
    SequencerTimeline&           GetTimeline()   { return *Timeline; }
    Sequencer::SequenceInstance* GetPreview()    { return Preview.get(); }

    // --- Commands (the toolbar's; public for tests) ---------------------------------

    void SetPlaying(bool bInPlaying);
    bool IsPlaying() const { return bPlaying; }
    void StepFrames(int InFrames);

    /* A binding to the level's selected entity (its index; -1 if none is selected). */
    int  AddSelectedEntity();
    /* The current binding's entity's pose, keyed whole at the playhead. */
    void KeyCurrentBinding();

    void SetAutoKey(bool bInOn);
    void SetViewLocked(bool bInOn);
    void SetPilot(bool bInOn);

    /* The preview made (again) for the document now, at the playhead. */
    void RefreshPreview();
    /* Everything the preview changed put back; no preview until the next refresh. */
    void ReleasePreview();
    /* Keys what auto-key has seen move since the last flush. */
    void FlushAutoKey();

signals:
    void TitleChanged();
    void CloseRequested();

private:
    void BuildToolBar();
    void ShowContextMenu(int InRow, double InFrame, const QPoint& InGlobalPos);
    void FillAddTrackMenu(class QMenu& InMenu, int InBinding);
    void AddTrackPrompted(int InBinding, Sequencer::ESequenceTrackType InType);
    void UpdateTimecode();
    /* The zoom slider and time scrollbar, from the timeline's view. */
    void SyncViewControls();
    void OnPlayTick();
    void OnBeforeFrame();
    void OnPropertyEdited(GameCore::EntityID InEntity, uint16_t InType);
    void BindToSelected(int InBinding);
    /* The binding InEntity plays in the preview; -1 if none. */
    int  BindingOf(GameCore::EntityID InEntity);
    /* What the preview showed for each binding, as the "before" of an auto-key. */
    void RememberShownPoses();

    EditorContext&     Context;
    QString            Path;
    SequencerDocument* Document = nullptr;
    SequencerTimeline* Timeline = nullptr;
    SequencerCurveEditor* Curves = nullptr;
    SequencerDetails*  Details  = nullptr;
    QLabel*            Timecode = nullptr;
    class QSlider*     ZoomSlider = nullptr;
    class QScrollBar*  TimeScroll = nullptr;
    bool               bSyncingView = false;

    QAction* PlayAction   = nullptr;
    QAction* LoopAction   = nullptr;
    QAction* AutoKeyAction = nullptr;
    QAction* LockAction   = nullptr;
    QAction* PilotAction  = nullptr;
    QAction* UndoAction   = nullptr;
    QAction* RedoAction   = nullptr;

    std::unique_ptr<Sequencer::SequenceInstance>   Preview;
    std::shared_ptr<const Sequencer::SequenceAsset> PreviewAsset;
    std::vector<GameCore::TransformPose>           Shown;     // by binding: as the preview left it
    std::vector<bool>                              bShown;

    bool          bPlaying = false;
    QTimer*       PlayTimer = nullptr;
    QElapsedTimer PlayClock;

    bool          bAutoKey = false;
    bool          bGizmoDragging = false;
    std::set<int> PendingKeys;          // bindings auto-key saw move
    QTimer*       AutoKeyTimer = nullptr;

    bool         bViewLocked = false;
    bool         bPilot      = false;
    bool         bCameraSaved = false;
    EditorCamera SavedCamera;          // the viewport's own, given back when unlocked
    EditorCamera LastSetCamera;        // what the shot set last frame: a difference is the user flying
    bool         bLastSetValid = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SEQUENCERTAB_H
