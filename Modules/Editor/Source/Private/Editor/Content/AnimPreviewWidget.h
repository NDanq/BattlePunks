#ifndef NIMBLEEDITOR_ANIMPREVIEWWIDGET_H
#define NIMBLEEDITOR_ANIMPREVIEWWIDGET_H

#if WITH_EDITOR

#include <memory>

#include <QElapsedTimer>
#include <QWidget>

#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/MeshAsset.h"

class MeshPreviewView;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;

/*
 * A skinned mesh, animated: the renderer's preview of it (MeshPreviewView)
 * posed by an AnimInstance this widget owns, under a transport -- play and
 * pause, a scrubber, speed, looping and the skeleton drawn over it
 * (ANIMATION.md). The mesh viewer and the anim state editor show one.
 *
 * It plays in the editor alone: nothing in a level changes.
 */
class AnimPreviewWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit AnimPreviewWidget(QWidget* InParent = nullptr);

    MeshPreviewView* GetView() const { return View; }

    /* The skinned mesh it poses, by runtime path and data. False (and the
     * plain mesh shown) if it is not skinned or its skeleton will not load. */
    bool SetMesh(const QString& InRuntimePath, const RenderCore::MeshData& InMesh);
    bool HasSkeleton() const { return Instance.GetSkeleton() != nullptr; }
    const RenderCore::SkeletonData* GetSkeleton() const { return Instance.GetSkeleton().get(); }

    /* What it plays: a clip (null: the reference pose), or an anim state. Starts playing. */
    void PlayClip(std::shared_ptr<const RenderCore::AnimClipData> InClip);
    void PlayState(std::shared_ptr<const RenderCore::AnimStateData> InState);

    /* For parameters and the state each layer is in; Repose after a change made through it. */
    RenderCore::AnimInstance& GetInstance() { return Instance; }
    void Repose();

    void  SetBonesShown(bool bInShown);
    void  SetPlaying(bool bInPlaying);
    bool  IsPlaying() const { return bPlaying; }
    void  Seek(float InSeconds);

signals:
    /* After each step or seek: the time, and an anim state's states, may have moved. */
    void Advanced();
    void NotifyFired(const QString& InName);

private:
    void Step();
    void PushPose();
    void UpdateTransport();

    RenderCore::AnimInstance Instance;
    const RenderCore::MeshData* Mesh = nullptr;   // the caller's, while it shows it
    RenderCore::MeshData        MeshCopy;

    MeshPreviewView* View       = nullptr;
    QToolButton*     PlayButton = nullptr;
    QSlider*         Scrubber   = nullptr;
    QLabel*          TimeLabel  = nullptr;
    QDoubleSpinBox*  Speed      = nullptr;
    QCheckBox*       Loop       = nullptr;
    QCheckBox*       Bones      = nullptr;
    QTimer*          Clock      = nullptr;
    QElapsedTimer    Elapsed;
    bool             bPlaying   = false;
    bool             bScrubbing = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_ANIMPREVIEWWIDGET_H
