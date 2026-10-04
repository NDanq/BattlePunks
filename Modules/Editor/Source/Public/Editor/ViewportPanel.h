#ifndef NIMBLEEDITOR_VIEWPORTPANEL_H
#define NIMBLEEDITOR_VIEWPORTPANEL_H

#if WITH_EDITOR

#include <QWidget>

class EditorContext;
class EditorDocument;
class QAction;
class QComboBox;
class QToolBar;
class QToolButton;
class ViewportWidget;

/*
 * The viewport and its toolbar (handle mode, space, snap, view preset,
 * frame, Show, camera speed). The toolbar sits above the native surface,
 * since Qt cannot paint over it. Combos never take focus, so viewport keys
 * keep working. Frame actions are shared with the menus.
 *
 * Show: View Mode -- Lit, Unlit, or Render Target, which asks which of the
 * render targets the last frame drew to show (EditorContext::SetViewMode) --
 * and Editor Gizmos: every entity's bounds, every light's reach.
 */
class ViewportPanel : public QWidget
{
    Q_OBJECT

public:
    ViewportPanel(EditorContext& InContext, EditorDocument& InDocument, QWidget* InParent = nullptr);

    ViewportWidget* GetViewport() const { return Viewport; }

    QAction* GetFrameSelectionAction() const { return FrameSelectionAction; }
    QAction* GetFrameAllAction()       const { return FrameAllAction; }

    /* The Show menu's checks and text, after the view mode changed elsewhere (the console). */
    void RefreshViewMode() { UpdateViewModeUI(); }

    /* For the main window to put Play/Stop/Eject in front of the navigation. */
    QToolBar* GetToolbar() const { return Toolbar; }

private:
    void SetSpeedStep(int InStep);

    /* Show > View Mode > Render Target: asks which; false if cancelled or there is none. */
    bool PickRenderTarget();

    /* The Show button's text and the mode actions, as the context has them. */
    void UpdateViewModeUI();

    EditorContext&  Context;

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

    QToolButton* ShowButton        = nullptr;
    QAction*     LitModeAction     = nullptr;
    QAction*     UnlitModeAction   = nullptr;
    QAction*     TargetModeAction  = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_VIEWPORTPANEL_H
