#ifndef NIMBLEEDITOR_EDITORMAINWINDOW_H
#define NIMBLEEDITOR_EDITORMAINWINDOW_H

#if WITH_EDITOR

#include <QMainWindow>
#include <QString>
#include <QTimer>

#include "Public/RenderCore/RenderTypes.h"

class DetailsPanel;
class EditorContext;
class OutlinerPanel;
class QLabel;
class ViewportPanel;
class ViewportWidget;

/*
 * The editor's frame: the viewport (with its navigation toolbar) in the
 * centre, the Outliner docked left and the Details panel docked right,
 * File/Edit/View menus and a status bar with render stats.
 *
 * The Renderer is started from here, once the window has been shown -- the
 * viewport's native window must exist before GDI builds a swapchain on it --
 * and shut down in closeEvent, while that window still exists to be released.
 */
class EditorMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    EditorMainWindow(EditorContext& InContext, RenderCore::ERenderBackend InBackend, QWidget* InParent = nullptr);
    ~EditorMainWindow() override;

    /* Opens InPath on startup instead of the demo scene; call before show(). */
    void SetInitialWorld(const QString& InPath) { InitialWorld = InPath; }

protected:
    void showEvent(QShowEvent* InEvent) override;
    void closeEvent(QCloseEvent* InEvent) override;

private:
    void BuildMenus();
    void StartRenderer();
    void UpdateTitle();
    void UpdateStats();

    void OpenWorld();
    bool SaveWorld();
    bool SaveWorldAs();

    EditorContext&             Context;
    RenderCore::ERenderBackend Backend;

    ViewportPanel*  ViewportArea = nullptr;
    ViewportWidget* Viewport = nullptr;   // ViewportArea's, cached
    OutlinerPanel*  Outliner = nullptr;
    DetailsPanel*   Details  = nullptr;
    QLabel*         StatsLabel = nullptr;

    QString CurrentPath;
    QString InitialWorld;
    bool    bRendererStarted = false;

    QTimer   StatsTimer;
    uint64_t LastFramesRendered = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORMAINWINDOW_H
