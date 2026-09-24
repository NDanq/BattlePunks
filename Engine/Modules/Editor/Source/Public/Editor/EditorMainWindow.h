#ifndef NIMBLEEDITOR_EDITORMAINWINDOW_H
#define NIMBLEEDITOR_EDITORMAINWINDOW_H

#if WITH_EDITOR

#include <QList>
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include "Public/RenderCore/RenderTypes.h"
#include "Public/Editor/EditorConsole.h"

class ConsolePanel;
class DetailsPanel;
class EditorContext;
class OutlinerPanel;
class QAction;
class QLabel;
class SDLInputProbe;
class ViewportPanel;
class ViewportWidget;

/*
 * The editor's frame: the viewport (with its navigation toolbar) in the
 * centre, the Outliner docked left, the Details panel docked right and the
 * Console docked below, File/Edit/View/Play/Help menus and a status bar with render
 * stats. Owns the EditorConsole and registers the scene commands on it.
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

    /* Starts the SDL input probe as soon as the viewport is up (the
     * --sdl-input-probe switch); see SDLInputProbe.h. Call before show(). */
    void SetStartSDLInputProbe(bool bInStart) { bStartSDLInputProbe = bInStart; }

    /* Starts Play-in-Editor as soon as the renderer is up (the --play
     * switch). Call before show(). */
    void SetStartPlay(bool bInStart) { bStartPlay = bInStart; }

protected:
    void showEvent(QShowEvent* InEvent) override;
    void closeEvent(QCloseEvent* InEvent) override;

private:
    void BuildMenus();
    void BuildPlayControls();
    void BuildHelpMenu();
    void RegisterConsoleCommands();
    void OnPlayStateChanged(bool bInPlaying);
    void StartPlay();
    void StartRenderer();
    void UpdateTitle();
    void UpdateStats();
    void SetSDLInputProbe(bool bInOn);

    void OpenWorld();
    bool SaveWorld();
    bool SaveWorldAs();

    EditorContext&             Context;
    RenderCore::ERenderBackend Backend;

    ViewportPanel*  ViewportArea = nullptr;
    ViewportWidget* Viewport = nullptr;   // ViewportArea's, cached
    OutlinerPanel*  Outliner = nullptr;
    DetailsPanel*   Details  = nullptr;
    ConsolePanel*   ConsoleArea = nullptr;
    EditorConsole   Console;
    QLabel*         StatsLabel = nullptr;
    QLabel*         PlayLabel  = nullptr;

    QAction*        PlayAction  = nullptr;
    QAction*        StopAction  = nullptr;
    QAction*        EjectAction = nullptr;

    /* Replace the edited World, so unavailable while playing. */
    QList<QAction*> SceneActions;

    QString CurrentPath;
    QString InitialWorld;
    bool    bRendererStarted = false;

    SDLInputProbe* InputProbe = nullptr;
    QAction*       InputProbeAction = nullptr;
    bool           bStartSDLInputProbe = false;
    bool           bStartPlay = false;

    QTimer   StatsTimer;
    uint64_t LastFramesRendered = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORMAINWINDOW_H
