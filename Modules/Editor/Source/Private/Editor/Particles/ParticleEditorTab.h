#ifndef NIMBLEEDITOR_PARTICLEEDITORTAB_H
#define NIMBLEEDITOR_PARTICLEEDITORTAB_H

#if WITH_EDITOR

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>

#include "Public/GameCore/EntityID.h"

class EditorContext;
class EditorDocument;
class ParticleDocument;
class ParticleEmitterDetailsPanel;
class ParticleEmitterListPanel;
class QAction;
class QComboBox;
class QLabel;
class QTimer;
class ViewportPanel;

/*
 * One particle system in the Particle Editor (Editor README, "Particle
 * Editor"): its emitters and their settings, shown running in a preview of
 * its own.
 *
 *   Emitters   the system's emitters: add, duplicate, remove, reorder, switch off
 *   Details    the selected emitter's settings, and what it holds once running
 *   Viewport   the system on a floor under a sun and sky, in a World of its own
 *   Toolbar    Save; Pause, Restart, Speed; Move Emitter; Floor; Live in Level
 *
 * EVERY EDIT SHOWS AT ONCE. The preview's component names this tab's own
 * cache key ("transient://ParticleEditor/<n>"), and every change is stored
 * under it (RenderCore::ParticleSystemCache::Store); an emitter keeps its
 * particles through an edit unless its Max Particles changes. With Live in
 * Level (on by default) the change is also stored under the asset's own
 * path, so the level -- or a game playing -- shows it unsaved; closing
 * without saving puts the saved one back.
 *
 * The preview World is never saved and has no undo of its own: the emitter
 * can be dragged about in it, or sent round a circle (Move Emitter) to see
 * how its particles trail in world space.
 */
class ParticleEditorTab : public QMainWindow
{
    Q_OBJECT

public:
    explicit ParticleEditorTab(EditorContext& InContext, QWidget* InParent = nullptr);
    ~ParticleEditorTab() override;

    /* False with OutError if InPath is not a readable particle system. */
    bool Load(const QString& InPath, QString& OutError);

    /* Absolute. */
    const QString& GetPath() const { return Path; }

    /* "Smoke.nmparticle*". */
    QString GetTabTitle() const;

    bool IsDirty() const;

    /* Writes, stages, and reaches every component naming it. A message box on failure. */
    bool Save();

    /* Offers to save; on Discard, puts the saved system back where Live in Level changed it. False if cancelled. */
    bool ConfirmClose();

    ParticleDocument& GetDocument() { return *Particles; }

    /* The preview's playback. Speed is its Time Scale (the toolbar's nearest
     * choice shown); Moving is Move Emitter. */
    void SetPaused(bool bInPaused);
    void SetSpeed(float InSpeed);
    void SetMoving(bool bInMoving);
    void Restart();
    void FrameEffect();

    /* The preview's next frame as an image (EditorContext::RequestSceneCapture); 0 before it has drawn. */
    uint64_t RequestCapture();

signals:
    /* Saved, or edited since: the tab's title changed. */
    void TitleChanged();

    /* File > Close: the window asks ConfirmClose, then removes the tab. */
    void CloseRequested();

private:
    void BuildPreview();
    void BuildDocks();
    void BuildToolBar();
    void BuildActions();

    /* The emitter entity, made again if it is gone (deleted in the viewport, or Restart). */
    void EnsureEmitterEntity();

    /* Every change: the preview's copy, the level's (Live in Level), the stats. */
    void PushToRenderer();
    void UpdateStats();

    /* The emitter's component's playback settings. */
    void ApplyPlayback();

    /* Move Emitter's circle, a step a frame. */
    void Animate();

    /* "Content/Particles/Smoke.nmparticle", or empty outside the project. */
    QString GetRuntimePath() const;

    EditorContext&    Context;
    ParticleDocument* Particles = nullptr;
    EditorDocument*   Preview   = nullptr;
    QString           Path;
    QString           PreviewKey;
    int               PreviewNumber = 0;   // this tab's, in its key: unique among tabs

    ViewportPanel*               ViewportArea = nullptr;
    ParticleEmitterListPanel*    EmitterList  = nullptr;
    ParticleEmitterDetailsPanel* Details      = nullptr;

    GameCore::EntityID EmitterEntity;
    GameCore::EntityID FloorEntity;

    QAction*   SaveAction    = nullptr;
    QAction*   PauseAction   = nullptr;
    QAction*   MoveAction    = nullptr;
    QAction*   FloorAction   = nullptr;
    QAction*   LiveAction    = nullptr;
    QComboBox* SpeedBox      = nullptr;
    QLabel*    StatsLabel    = nullptr;

    QTimer*       AnimateTimer = nullptr;
    QElapsedTimer AnimateClock;
    double        AnimateAngle = 0.0;
    double        MoveRadius   = 150.0;
    float         Speed        = 1.0f;
    bool          bPaused      = false;
    bool          bLiveStored  = false;   // the asset's path holds an unsaved copy
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PARTICLEEDITORTAB_H
