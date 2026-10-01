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

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorLog.h"
#include "Public/Editor/EditorMainWindow.h"
#include "Private/Editor/EditorTheme.h"
#include "Private/Editor/SettingsDialog.h"

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
    QApplication App(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Nimble Editor"));
    QApplication::setOrganizationName(QStringLiteral("NimbleEngine"));

    /* First, so the Console panel's history starts with everything logged
     * from here on -- config load and renderer startup included. */
    EditorLog::Install();

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

    if(QFileInfo::exists(QStringLiteral("Config/DefaultEngine.ini")))
    {
        EngineCore::LoadConfigFile("Config/DefaultEngine.ini");
    }
    /* The user's own choices, over the project's (Edit > Editor Settings). */
    SettingsDialog::LoadUserSettings();
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

    int Result = 0;
    {
        EditorContext    Context;
        EditorMainWindow Window(Context, RenderCore::ERenderBackend(Rhi));
        Window.SetInitialWorld(InitialWorld);
        Window.SetStartSDLInputProbe(Parser.isSet(ProbeOption));
        Window.SetStartPlay(Parser.isSet(PlayOption));
        Window.show();

        Result = App.exec();
    }

    EditorLog::Uninstall();
    return Result;
}

#endif // WITH_EDITOR
