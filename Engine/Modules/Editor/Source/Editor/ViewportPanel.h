#ifndef NIMBLEEDITOR_VIEWPORTPANEL_H
#define NIMBLEEDITOR_VIEWPORTPANEL_H

#include <QWidget>

class EditorContext;
class QAction;
class QLabel;
class QSlider;
class QToolBar;
class ViewportWidget;

/*
 * The viewport plus its navigation toolbar: view presets (Perspective and the
 * six axis views), Frame Selected / Frame All, the all-bounds toggle and the
 * camera speed. A toolbar ABOVE the native render surface rather than drawn
 * over it -- nothing Qt paints can sit on top of a window the GPU presents to.
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

private:
    void SetSpeedStep(int InStep);

    ViewportWidget* Viewport = nullptr;
    QToolBar*       Toolbar  = nullptr;
    QSlider*        Speed    = nullptr;
    QLabel*         SpeedLabel = nullptr;

    QAction* FrameSelectionAction = nullptr;
    QAction* FrameAllAction       = nullptr;
    QAction* ShowAllBoundsAction  = nullptr;
};

#endif //NIMBLEEDITOR_VIEWPORTPANEL_H
