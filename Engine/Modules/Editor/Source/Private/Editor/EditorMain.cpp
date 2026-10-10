/* Without WITH_EDITOR every file compiles empty and the link fails with a
 * baffling "no main"; fail clearly here instead. */
#ifndef WITH_EDITOR
    #error "WITH_EDITOR is not defined -- configure through Engine/CMakeLists.txt"
#elif !WITH_EDITOR
    #error "NimbleEditor is built only for an Editor target (NIMBLE_TARGET=Editor)"
#endif

#if WITH_EDITOR

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreJob.h"
#include "Public/Core/CoreLog.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorLog.h"
#include "Public/Editor/EditorMainWindow.h"
#include "Private/Editor/Build/ContentCook.h"
#include "Private/Editor/Content/AssetImport.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/EditorTheme.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/SettingsDialog.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/GameSession.h"

/*
 * NimbleEditor's entry point.
 *
 *   NimbleEditor [--rhi N] [--play] [world.nmworld]
 *
 * Backend: [Editor] RHI from the staged DefaultEngine.ini, then the user's
 * EditorSettings.ini, then --rhi. Numbered as [Display] RHI (0=Null,
 * 1=OpenGL, 2=Vulkan, 3=Metal, 4=D3D12), but separate from the game's key.
 */
NIMBLE_CONFIG_INT(GEditorRHI, "Editor", "RHI", int(RenderCore::ERenderBackend::Vulkan), nullptr);

int main(int argc, char* argv[])
{
#if defined(__linux__)
    /*
     * GDI on Linux presents into X11 windows only: GDIQtWindow passes winId()
     * straight through as an X11 Window id. Under Qt's wayland platform winId()
     * is not one, and the Vulkan surface comes back with no present support.
     * So pin Qt to xcb (XWayland on a Wayland desktop) before QApplication
     * picks a platform -- unless the user chose one explicitly.
     */
    if(!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif

    QApplication App(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Nimble Editor"));
    QApplication::setOrganizationName(QStringLiteral("NimbleEngine"));

    /* The application icon, every window's unless it sets its own. One PNG per
     * size, so the title bar and taskbar never scale the 512 down themselves. */
    QIcon AppIcon;
    for(const int Size : {16, 32, 48, 64, 128, 256, 512})
    {
        AppIcon.addFile(QStringLiteral(":/app/NimbleEditor-%1.png").arg(Size), QSize(Size, Size));
    }
    QApplication::setWindowIcon(AppIcon);

    /* First, so the Console panel's history starts with everything logged
     * from here on -- config load and renderer startup included. */
    EditorLog::Install();

    EditorContent::SetImportEngineContentRoot(EditorContent::GetEngineContentRoot());

    QCommandLineParser Parser;
    Parser.setApplicationDescription(QStringLiteral("NimbleEngine editor"));
    Parser.addHelpOption();
    const QCommandLineOption RhiOption(QStringLiteral("rhi"),
                                       QStringLiteral("Render backend: 0=Null 1=OpenGL 2=Vulkan 3=Metal 4=D3D12."),
                                       QStringLiteral("N"));
    Parser.addOption(RhiOption);
    const QCommandLineOption ProbeOption(QStringLiteral("sdl-input-probe"),
                                         QStringLiteral("Route viewport input through SDL and report it (Play-in-Editor spike)."));
    Parser.addOption(ProbeOption);
    const QCommandLineOption PlayOption(QStringLiteral("play"),
                                        QStringLiteral("Start Play-in-Editor once the viewport is up."));
    Parser.addOption(PlayOption);
    const QCommandLineOption CommandOption(QStringLiteral("command"),
                                           QStringLiteral("Run a console command once the viewport is up. Repeatable."),
                                           QStringLiteral("line"));
    Parser.addOption(CommandOption);
    Parser.addPositionalArgument(QStringLiteral("world"), QStringLiteral("A .nmworld file to open."), QStringLiteral("[world]"));
    Parser.process(App);

    /* Resolve the positional path BEFORE changing directory, so a path
     * relative to where the editor was launched still means that. */
    QString InitialWorld;
    if(!Parser.positionalArguments().isEmpty())
    {
        InitialWorld = QFileInfo(Parser.positionalArguments().front()).absoluteFilePath();
    }

    /* Asset paths are relative to the exe's folder, as for the game hosts. */
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    /* This session's log: <project>/Saved/Logs/Editor-<time>.log (CoreLog.h). */
    if(EngineCore::OpenRunLog(EditorCook::GetLogsDir(EditorGraphs::GetProjectRoot()).toUtf8().toStdString(), "Editor"))
    {
        CORE_LOG("[Editor] logging to " + EngineCore::GetRunLogPath())
    }

    if(QFileInfo::exists(QStringLiteral("Config/DefaultEngine.ini")))
    {
        EngineCore::LoadConfigFile("Config/DefaultEngine.ini");
    }
    /* The game's own settings ([Input_*]), over the engine's. */
    if(QFileInfo::exists(QStringLiteral("Config/DefaultGame.ini")))
    {
        EngineCore::LoadConfigFile("Config/DefaultGame.ini");
    }
    /* The user's own choices, over the project's (Edit > Editor Settings). */
    SettingsDialog::LoadUserSettings();
    /* [Modules] asking for what this build was configured without, or the
     * reverse: the project's next build applies it (Project Settings). */
    NimbleEngine::WarnModuleConfigMismatch();
    EditorGraphs::InstallShaderPDBDir();   // before the renderer loads a shader
    EditorTheme::Install();

    int Rhi = GEditorRHI.GetInt();
    if(Parser.isSet(RhiOption))
    {
        bool bOk = false;
        const int Requested = Parser.value(RhiOption).toInt(&bOk);
        if(bOk && Requested >= 0 && Requested <= int(RenderCore::ERenderBackend::D3D12))
        {
            Rhi = Requested;
        }
        else
        {
            CORE_LOG_WARN("[Editor] ignoring --rhi " + Parser.value(RhiOption).toStdString())
        }
    }

    /* The job pool (CoreJob.h): physical cores less one, alive before the
     * window loads any module (the game's, at edit or Play) and until the last
     * has unloaded -- module teardown drops a module's queued jobs through it.
     * Animation, texture compression and parallel draw recording
     * (MULTITHREADED_RENDERING.md) find it by GetJobSystem(). */
    EngineCore::JobSystem Jobs;
    EngineCore::SetJobSystem(&Jobs);
    /* Before any Play session or preview can make a sound; no device just
     * leaves the editor silent. */
    NimbleEngine::StartAudio();

    int Result = 0;
    {
        EditorContext    Context;
        EditorMainWindow Window(Context, RenderCore::ERenderBackend(Rhi));
        Window.SetInitialWorld(InitialWorld);
        Window.SetStartSDLInputProbe(Parser.isSet(ProbeOption));
        Window.SetStartPlay(Parser.isSet(PlayOption));
        Window.SetStartupCommands(Parser.values(CommandOption));
        Window.show();

        Result = App.exec();
    }
    Jobs.WaitForIdle();
    EngineCore::SetJobSystem(nullptr);

    /* The window and every Play session are gone, and their voices with them. */
    NimbleEngine::StopAudio();

    EditorLog::Uninstall();
    EngineCore::CloseRunLog();
    return Result;
}

#endif // WITH_EDITOR
