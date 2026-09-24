#ifndef NIMBLEEDITOR_VIEWPORTWIDGET_H
#define NIMBLEEDITOR_VIEWPORTWIDGET_H

#include <QElapsedTimer>
#include <QPoint>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/EntityID.h"
#include "Editor/EditorCamera.h"

class EditorContext;

/*
 * The 3D viewport: a native child window the Renderer draws into, exactly as
 * it draws into the game's SDL window. Qt never paints here -- paintEngine()
 * is null and WA_PaintOnScreen hands the surface to the GPU backend.
 *
 * The native handle is winId() cast to a WindowHandle -- the HWND on Windows,
 * the same thing GDIQt::ToWindowHandle produces, without linking GDIQtBridge
 * (see Modules/Editor/CMakeLists.txt for why the editor must not).
 *
 * CONTROLS
 *   Right drag          look around; with WASD / Q E held, fly (Shift = faster)
 *   Middle drag         pan
 *   Wheel               dolly along the view direction
 *   Left click          select the primitive under the cursor (empty space clears)
 *   F                   frame the selection
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

    /* Moves the camera to look at InEntity's position from a fixed distance. */
    void FrameEntity(GameCore::EntityID InEntity);

    QPaintEngine* paintEngine() const override { return nullptr; }

protected:
    void paintEvent(QPaintEvent* InEvent) override;
    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;
    void wheelEvent(QWheelEvent* InEvent) override;
    void keyPressEvent(QKeyEvent* InEvent) override;
    void keyReleaseEvent(QKeyEvent* InEvent) override;
    void focusOutEvent(QFocusEvent* InEvent) override;

private:
    void OnFrame();

    /* WASD/QE flying while the right button is held. */
    void ApplyFlyInput(float InDeltaSeconds);

    /* The nearest primitive quad under the pixel, or an unset id. */
    GameCore::EntityID Pick(const QPoint& InLogicalPos) const;

    EditorContext& Context;
    EditorCamera   Camera;

    QTimer        FrameTimer;
    QElapsedTimer FrameClock;

    QPoint     LastMousePos;
    QPoint     PressPos;
    bool       bLooking = false;
    bool       bPanning = false;
    QSet<int>  HeldKeys;
};

#endif //NIMBLEEDITOR_VIEWPORTWIDGET_H
