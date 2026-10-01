#ifndef NIMBLEEDITOR_VIEWPORTWIDGET_H
#define NIMBLEEDITOR_VIEWPORTWIDGET_H

#if WITH_EDITOR

#include <QElapsedTimer>
#include <QPoint>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Editor/EditorCamera.h"
#include "Public/Editor/TransformGizmo.h"

class EditorContext;
namespace GameCore { struct TransformComponent; }

/*
 * The 3D viewport: a native child window the Renderer draws into. Qt never
 * paints here, so overlays are Renderer debug lines. The handle is winId()
 * (no GDIQtBridge; see Modules/Editor/CMakeLists.txt).
 *
 * CONTROLS
 *   Right drag          look around; with WASD / Q E held, fly (Shift = faster)
 *   Alt + Left drag     orbit the pivot (the selection, else the focus point)
 *   Alt + Right drag    dolly toward / away from the pivot
 *   Middle drag         pan (also Alt + Middle)
 *   Wheel               dolly toward the pivot
 *   Left click          select the primitive under the cursor (empty space
 *                       clears); on the axis gizmo, view from that axis
 *                       (Shift: from the opposite side)
 *   Left drag on a      move / rotate / scale the selection (Ctrl inverts
 *   transform handle    snapping; Esc or a right click cancels the drag)
 *   Q / W / E / R       transform handles off / move / rotate / scale
 *   X                   handles in world or local space
 *   F / Home            frame the selection / the whole scene
 */
class ViewportWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportWidget(EditorContext& InContext, QWidget* InParent = nullptr);

    /* Forces the native window into existence if it is not yet. */
    EngineCore::WindowHandle GetWindowHandle();

    /* PHYSICAL pixels -- what the Renderer's targets are sized from. */
    EngineCore::Size GetPixelSize() const;

    /* Starts the frame timer. Call once the Renderer is initialized. */
    void StartRendering();

    EditorCamera& GetCamera() { return Camera; }

    // === Navigation commands (toolbar, menus, keys) ==========================

    void FrameSelection();
    void FrameAll();
    void ApplyPreset(EditorCamera::EPreset InPreset);
    void SetSpeedScale(float InScale) { Camera.SpeedScale = InScale; }

    /* Front view of (0,0)-(1280,720), what Sandbox's camera shows. */
    void ApplySandboxView();

    /* Draw every entity's bounds, not just the selection's. */
    void SetShowAllBounds(bool bInShow) { bShowAllBounds = bInShow; }
    bool IsShowingAllBounds() const     { return bShowAllBounds; }

    // === Transform handles ==================================================

    void SetGizmoMode(TransformGizmo::EMode InMode);
    TransformGizmo::EMode GetGizmoMode() const { return Gizmo.GetMode(); }

    void SetGizmoSpace(TransformGizmo::ESpace InSpace);
    TransformGizmo::ESpace GetGizmoSpace() const { return Gizmo.GetSpace(); }

    /* Snap every drag to [Editor] SnapTranslate / SnapRotate / SnapScale;
     * holding Ctrl during a drag inverts this. */
    void SetGizmoSnap(bool bInSnap);
    bool IsGizmoSnapEnabled() const { return bGizmoSnap; }

    QPaintEngine* paintEngine() const override { return nullptr; }

signals:
    /* From the keys as well as the setters, so a toolbar can follow. */
    void GizmoModeChanged(TransformGizmo::EMode InMode);
    void GizmoSpaceChanged(TransformGizmo::ESpace InSpace);
    void GizmoSnapChanged(bool bInSnap);

    /* What the drag in progress has done ("Move X +120 ..."); empty when it ends. */
    void GizmoDragStatus(const QString& InText);

protected:
    void paintEvent(QPaintEvent* InEvent) override;
    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;
    void wheelEvent(QWheelEvent* InEvent) override;
    void keyPressEvent(QKeyEvent* InEvent) override;
    void keyReleaseEvent(QKeyEvent* InEvent) override;
    void focusOutEvent(QFocusEvent* InEvent) override;
    void dragEnterEvent(QDragEnterEvent* InEvent) override;
    void dragMoveEvent(QDragMoveEvent* InEvent) override;
    void dropEvent(QDropEvent* InEvent) override;

private:
    /* What a held mouse button is doing. One at a time. */
    enum class EDrag { None, Look, Orbit, Dolly, Pan, Gizmo };

    void OnFrame();

    /* WASD/QE flying while looking. */
    void ApplyFlyInput(float InDeltaSeconds);

    /* Selection bounds, all-bounds and the axis gizmo, as debug lines. */
    void QueueOverlay();
    void QueueAxisGizmo();

    /* The selection's centre if it has bounds, else the focus point. */
    EngineCore::Vector3 GetPivot() const;

    /* Re-derived each frame: the axis gizmo has a fixed screen spot and size. */
    void GetGizmoFrame(EngineCore::Vector3& OutOrigin, double& OutAxisLength) const;

    /* 0/1/2 for the X/Y/Z tip under InLogicalPos, -1 for none. */
    int HitGizmoAxis(const QPoint& InLogicalPos) const;

    /* The selection's Transform if handles should show, else null. */
    GameCore::TransformComponent* GetGizmoTarget() const;

    /* Writes a gizmo pose into InEntity's Transform and tells the panels. */
    void ApplyGizmoPose(GameCore::EntityID InEntity, const TransformGizmo::Pose& InPose);

    /* Ends a handle drag, keeping what it did or restoring where it began. */
    void FinishGizmoDrag(bool bInCancel);

    TransformGizmo::Snap GetGizmoSnap(Qt::KeyboardModifiers InModifiers) const;

    /* The nearest primitive quad under the pixel, or an unset id. */
    GameCore::EntityID Pick(const QPoint& InLogicalPos) const;

    /* A dragged .nmmesh in the project, as its runtime path; else empty. */
    QString DroppedMesh(const class QMimeData* InData) const;

    EngineCore::Size GetLogicalSize() const { return EngineCore::Size(width(), height()); }

    EditorContext& Context;
    EditorCamera   Camera;

    QTimer        FrameTimer;
    QElapsedTimer FrameClock;

    EDrag               Drag = EDrag::None;
    Qt::MouseButton     DragButton = Qt::NoButton;
    EngineCore::Vector3 DragPivot;
    QPoint              LastMousePos;
    QPoint              PressPos;
    QSet<int>           HeldKeys;

    bool bShowAllBounds = false;

    TransformGizmo          Gizmo;
    TransformGizmo::EHandle GizmoHover = TransformGizmo::EHandle::None;
    GameCore::EntityID      GizmoEntity;   // the one being dragged
    bool                    bGizmoSnap = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_VIEWPORTWIDGET_H
