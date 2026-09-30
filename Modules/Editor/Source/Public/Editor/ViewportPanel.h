#ifndef NIMBLEEDITOR_VIEWPORTPANEL_H
#define NIMBLEEDITOR_VIEWPORTPANEL_H

#if WITH_EDITOR

#include <QWidget>

class EditorContext;
class QAction;
class QComboBox;
class QToolBar;
class ViewportWidget;

/*
 * The viewport plus its toolbar, kept compact: combo boxes for the transform
 * handle mode (Select / Move / Rotate / Scale), World / Local space, the view
 * preset (Perspective and the six axis views) and camera speed; a Snap
 * toggle; a Frame split button (Selected, or All from its menu); and the
 * all-bounds toggle. A toolbar ABOVE the native render surface rather than
 * drawn over it -- nothing Qt paints can sit on top of a window the GPU
 * presents to.
 *
 * The combo boxes never take keyboard focus, so Q W E R, X, F and flying keep
 * going to the viewport after one is used.
 *
 * The frame and bounds actions are exposed so the main window's menus share
 * them (one QAction, one checked state, one shortcut).
 */
class ViewportPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportPanel(EditorContext& InContext, QWidget* InParent = nullptr);

    ViewportWidget* GetViewport() const { return Viewport; }

    QAction* GetFrameSelectionAction() const { return FrameSelectionAction; }
    QAction* GetFrameAllAction()       const { return FrameAllAction; }
    QAction* GetShowAllBoundsAction()  const { return ShowAllBoundsAction; }

    /* For the main window to put Play/Stop/Eject in front of the navigation. */
    QToolBar* GetToolbar() const { return Toolbar; }

private:
    void SetSpeedStep(int InStep);

    ViewportWidget* Viewport = nullptr;
    QToolBar*       Toolbar  = nullptr;

    QComboBox* ModeCombo  = nullptr;   // indexed by TransformGizmo::EMode
    QComboBox* SpaceCombo = nullptr;   // indexed by TransformGizmo::ESpace
    QComboBox* ViewCombo  = nullptr;
    QComboBox* SpeedCombo = nullptr;   // index = speed step
    QAction*   GizmoSnapAction = nullptr;

    QAction* FrameSelectionAction = nullptr;
    QAction* FrameAllAction       = nullptr;
    QAction* ShowAllBoundsAction  = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_VIEWPORTPANEL_H
