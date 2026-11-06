#ifndef NIMBLEEDITOR_EDITORMAINWINDOW_H
#define NIMBLEEDITOR_EDITORMAINWINDOW_H

#if WITH_EDITOR

#include <QByteArray>
#include <QList>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "Public/RenderCore/RenderTypes.h"
#include "Public/Editor/EditorConsole.h"
#include "Public/Editor/EditorNotifications.h"

class ConsolePanel;
class ContentBrowserPanel;
class DetailsPanel;
class EditorContext;
class EditorDocument;
class OutlinerPanel;
class NotificationCenter;
class NotificationToasts;
class ProjectBuildDialog;
class QAction;
class QMenu;
class QProcess;
class QLabel;
class QToolButton;
class SDLInputProbe;
class SourceBuild;
class EditorCollabSession;
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

    /* Plugins (Project Settings > Plugins): Compile Source, which builds the
     * enabled plugins with the game, and when it succeeds a notification
     * offering RestartEditor -- plugins load only at startup. */
    void RebuildForPlugins();
    /* Asks to save the level, then starts this editor again on it and closes. */
    void RestartEditor();
    /* Project Settings at Plugins; bInNewPlugin opens New Plugin over it. */
    void OpenPluginSettings(bool bInNewPlugin = false, const QString& InName = QString(), const QString& InStarter = QString());
    /* The status bar's "Plugins: rebuild" notice, shown while [Plugins] and
     * this build disagree (NimbleEngine::Plugins::GetConfigMismatches). */
    void UpdatePluginNotice();

protected:
    void showEvent(QShowEvent* InEvent) override;
    void closeEvent(QCloseEvent* InEvent) override;

private:
    void BuildMenus();
    void BuildPlayControls();
    void BuildBuildMenu();
    void BuildHelpMenu();
    void RegisterConsoleCommands();

    /* Saves capture InRequest (EditorContext::RequestSceneCapture) to InPath
     * once a frame answers it; the Console says how it went. bInWholeWindow:
     * the whole window instead, the frame laid where the viewport is (a
     * widget grab cannot see what the GPU drew there). */
    void SaveCaptureWhenReady(uint64_t InRequest, const QString& InPath, bool bInWholeWindow = false);
    void OnPlayStateChanged(bool bInPlaying);
    void StartPlay();
    void CompileSource();
    /* Build > Cook Content: Build Project's dialog, cooking at once. */
    void CookContent();
    /* Build > Build Project: cook and compile the game (ProjectBuildDialog). */
    ProjectBuildDialog* OpenBuildProject();
    /* InPage: a page to open on ("Play"); empty keeps the first. */
    void OpenSettings(const QString& InPage = QString());
    /* bInWorlds: at the Worlds page. */
    void OpenProjectSettings(bool bInWorlds = false);
    void OnCompileFinished(bool bInSucceeded);
    /* Build > Compile Source's outcome, replacing its progress toast; errors offer Show Log. */
    void PostCompileResult(EditorNotifications::ESeverity InSeverity, const QString& InTitle, const QString& InBody = QString());
    /* The Console dock, shown and in front: a notification's "Show Log". */
    void ShowConsole();
    /* The project's DefaultEngine.ini and DefaultGame.ini read again (a pull
     * changed them), the user's settings over them. */
    void ReloadProjectConfig();
    /* EditorContentEvents: files a pull changed, staged again. */
    void OnContentChanged(const QStringList& InFiles);
    void StartRenderer();
    void UpdateTitle();
    void UpdateHistoryActions();
    void UpdateStats();
    void UpdateDetailLabel();

    /* Whether the world's bake still matches its static entities and lights
     * (LightBake::Scene::Hash): the status bar says when it needs rebaking.
     * Run a moment after edits settle (BakeCheckTimer). */
    void CheckBake();
    void SetSDLInputProbe(bool bInOn);

    void OpenWorld();
    void OpenWorldFile(const QString& InPath);
    /* OpenWorldFile once MaybeSaveLevel lets it: the Content Browser and Open Recent. */
    void RequestOpenWorld(const QString& InPath);
    bool SaveWorld();
    bool SaveWorldAs();

    /* Unsaved level edits: Save, Discard or Cancel. False to stay put (Cancel,
     * or a save that failed); true at once when the level is clean. */
    bool MaybeSaveLevel(const QString& InTitle);

    /* File > Open Recent: this project's worlds, newest first (EditorLayout.ini). */
    void AddRecentWorld(const QString& InPath);
    void FillRecentMenu();

    /* The docks' arrangement and the window's size, kept in EditorLayout.ini
     * between runs; View > Reset Layout puts back the one built here. */
    void RestoreLayout();
    void SaveLayout() const;
    void ResetLayout();

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

    /* This editor in a collaboration session (EditorCollabSession.h). */
    EditorCollabSession* CollabSession = nullptr;
    QLabel*         DetailLabel = nullptr;   // the world's detail tier, and where it comes from
    QLabel*         StatsLabel = nullptr;
    QLabel*         PlayLabel  = nullptr;
    QToolButton*    PluginNotice = nullptr;
    bool            bOfferPluginRestart = false;
    bool            bRestartOnClose     = false;   // RestartEditor: start again once the close goes through
    QLabel*         BakeLabel  = nullptr;   // "Lighting needs to be rebuilt"

    /* EditorNotifications on screen (NotificationWidgets.h). */
    NotificationCenter* Notifications = nullptr;
    NotificationToasts* Toasts        = nullptr;

    QAction*        UndoAction  = nullptr;
    QAction*        RedoAction  = nullptr;

    QAction*        PlayAction  = nullptr;
    QAction*        StopAction  = nullptr;
    QAction*        EjectAction = nullptr;
    QAction*        ClientWindowAction = nullptr;

    /* Play > Net > Launch Local Server: a NimbleEditorServer on this machine, ended with the editor. */
    QProcess*       LocalSessionServer = nullptr;
    void            ToggleLocalSessionServer();

    SourceBuild*    Compiler            = nullptr;
    QAction*        CompileAction       = nullptr;
    QAction*        CancelCompileAction = nullptr;
    QString         FirstCompileError;   // the build output's first error line, for its notification

    /* Replace the edited World, so unavailable while playing. */
    QList<QAction*> SceneActions;

    QString CurrentPath;
    QString InitialWorld;
    bool    bRendererStarted = false;

    QMenu*     RecentMenu = nullptr;
    QByteArray DefaultLayout;          // saveState() as built, before the user's was restored
    bool       bDiscardLevelOnClose = false;   // "quit discard": no prompt, for scripted runs

    /* The game module was rebuilt while a run kept the old one: script nodes
     * are reread when it stops. */
    bool    bDeclarationsStale = false;

    SDLInputProbe* InputProbe = nullptr;
    QAction*       InputProbeAction = nullptr;
    bool           bStartSDLInputProbe = false;
    bool           bStartPlay = false;
    QStringList    StartupCommands;

    QTimer   StatsTimer;
    QTimer   BakeCheckTimer;
    uint64_t LastFramesRendered = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORMAINWINDOW_H
