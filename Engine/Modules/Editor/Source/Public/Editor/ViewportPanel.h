#ifndef NIMBLEEDITOR_VIEWPORTPANEL_H
#define NIMBLEEDITOR_VIEWPORTPANEL_H

#if WITH_EDITOR

#include <QWidget>

class EditorContext;
class EditorDocument;
class QAction;
class QComboBox;
class QToolBar;
class ViewportWidget;

/*
 * The viewport and its toolbar (handle mode, space, view preset, camera
 * speed, snap, frame, all-bounds). The toolbar sits above the native surface,
 * since Qt cannot paint over it. Combos never take focus, so viewport keys
 * keep working. Frame and bounds actions are shared with the menus.
 */
class ViewportPanel : public QWidget
{
    Q_OBJECT

public:
    ViewportPanel(EditorContext& InContext, EditorDocument& InDocument, QWidget* InParent = nullptr);

    ViewportWidget* GetViewport() const { return Viewport; }

    QAction* GetFrameSelectionAction() const { return FrameSelectionAction; }
    QAction* GetFrameAllAction()       const { return FrameAllAction; }
    QAction* GetShowAllBoundsAction()  const { return ShowAllBoundsAction; }
    QAction* GetShowLightGizmosAction() const { return ShowLightGizmosAction; }

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
    QAction* ShowLightGizmosAction = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_VIEWPORTPANEL_H
