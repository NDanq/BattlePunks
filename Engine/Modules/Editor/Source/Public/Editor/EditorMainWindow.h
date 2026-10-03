#ifndef NIMBLEEDITOR_EDITORMAINWINDOW_H
#define NIMBLEEDITOR_EDITORMAINWINDOW_H

#if WITH_EDITOR

#include <QList>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "Public/RenderCore/RenderTypes.h"
#include "Public/Editor/EditorConsole.h"

class ConsolePanel;
class ContentBrowserPanel;
class DetailsPanel;
class EditorContext;
class EditorDocument;
class OutlinerPanel;
class QAction;
class QLabel;
class SDLInputProbe;
class SourceBuild;
class ViewportPanel;
class ViewportWidget;

/*
 * The editor's frame: viewport, docks (Outliner, Details, Console), menus and
 * a render-stats status bar. Owns the EditorConsole's scene commands.
 *
 * Starts the Renderer once shown (the native window must exist) and stops it
 * in closeEvent (while it still does).
 */
class EditorMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    EditorMainWindow(EditorContext& InContext, RenderCore::ERenderBackend InBackend, QWidget* InParent = nullptr);
    ~EditorMainWindow() override;

    /* Opens InPath on startup instead of the demo scene; call before show(). */
    void SetInitialWorld(const QString& InPath) { InitialWorld = InPath; }

    /* --sdl-input-probe (SDLInputProbe.h). Call before show(). */
    void SetStartSDLInputProbe(bool bInStart) { bStartSDLInputProbe = bInStart; }

    /* --play. Call before show(). */
    void SetStartPlay(bool bInStart) { bStartPlay = bInStart; }

    /* Console commands to run once the renderer is up (after --play). */
    void SetStartupCommands(const QStringList& InCommands) { StartupCommands = InCommands; }

protected:
    void showEvent(QShowEvent* InEvent) override;
    void closeEvent(QCloseEvent* InEvent) override;

private:
    void BuildMenus();
    void BuildPlayControls();
    void BuildBuildMenu();
    void BuildHelpMenu();
    void RegisterConsoleCommands();
    void OnPlayStateChanged(bool bInPlaying);
    void StartPlay();
    void CompileSource();
    void CookContent();
    void OpenSettings();
    void OpenProjectSettings();
    void OnCompileFinished(bool bInSucceeded);
    void StartRenderer();
    void UpdateTitle();
    void UpdateHistoryActions();
    void UpdateStats();
    void SetSDLInputProbe(bool bInOn);

    void OpenWorld();
    void OpenWorldFile(const QString& InPath);
    bool SaveWorld();
    bool SaveWorldAs();

    EditorContext&             Context;
    EditorDocument&            Level;     // the context's level document
    RenderCore::ERenderBackend Backend;

    ViewportPanel*  ViewportArea = nullptr;
    ViewportWidget* Viewport = nullptr;   // ViewportArea's, cached
    OutlinerPanel*  Outliner = nullptr;
    DetailsPanel*   Details  = nullptr;
    ConsolePanel*   ConsoleArea = nullptr;
    ContentBrowserPanel* ContentArea = nullptr;
    EditorConsole   Console;
    QLabel*         StatsLabel = nullptr;
    QLabel*         PlayLabel  = nullptr;

    QAction*        UndoAction  = nullptr;
    QAction*        RedoAction  = nullptr;

    QAction*        PlayAction  = nullptr;
    QAction*        StopAction  = nullptr;
    QAction*        EjectAction = nullptr;

    SourceBuild*    Compiler            = nullptr;
    QAction*        CompileAction       = nullptr;
    QAction*        CancelCompileAction = nullptr;

    /* Replace the edited World, so unavailable while playing. */
    QList<QAction*> SceneActions;

    QString CurrentPath;
    QString InitialWorld;
    bool    bRendererStarted = false;

    /* The game module was rebuilt while a run kept the old one: script nodes
     * are reread when it stops. */
    bool    bDeclarationsStale = false;

    SDLInputProbe* InputProbe = nullptr;
    QAction*       InputProbeAction = nullptr;
    bool           bStartSDLInputProbe = false;
    bool           bStartPlay = false;
    QStringList    StartupCommands;

    QTimer   StatsTimer;
    uint64_t LastFramesRendered = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORMAINWINDOW_H
