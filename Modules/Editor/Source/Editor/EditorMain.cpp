#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorMainWindow.h"

/*
 * NimbleEditor's entry point.
 *
 *   NimbleEditor [--rhi N] [world.nmworld]
 *
 * The backend comes from [Editor] RHI in Config/DefaultEngine.ini when that
 * file is beside the executable (a game's staged config usually is), then
 * from --rhi. Same numbering as [Display] RHI: 0=Null, 1=OpenGL, 2=Vulkan,
 * 3=Metal, 4=D3D12. A separate key from the game's, so a game configured for
 * one backend does not force the editor onto it.
 */
NIMBLE_CONFIG_INT(GEditorRHI, "Editor", "RHI", int(RenderCore::ERenderBackend::Vulkan), nullptr);

namespace
{
    /*
     * CoreLog into Qt's message system, so output reaches the debugger (a
     * WIN32 exe has no console for the default stderr sink). Fatal stays
     * fatal: qFatal aborts, as the default handler would.
     */
    void EditorLogHandler(const EngineCore::LogVerbosity InVerbosity, const char* InMessage)
    {
        const char* Message = InMessage ? InMessage : "";
        switch(InVerbosity)
        {
            case EngineCore::LogVerbosity::Log:     qInfo("%s", Message);     break;
            case EngineCore::LogVerbosity::Warning: qWarning("%s", Message);  break;
            case EngineCore::LogVerbosity::Error:   qCritical("%s", Message); break;
            case EngineCore::LogVerbosity::Fatal:   qFatal("%s", Message);    break;
        }
    }
}

int main(int argc, char* argv[])
{
    QApplication App(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Nimble Editor"));
    QApplication::setOrganizationName(QStringLiteral("NimbleEngine"));

    EngineCore::SetLogHandler(&EditorLogHandler);

    QCommandLineParser Parser;
    Parser.setApplicationDescription(QStringLiteral("NimbleEngine editor"));
    Parser.addHelpOption();
    const QCommandLineOption RhiOption(QStringLiteral("rhi"),
                                       QStringLiteral("Render backend: 0=Null 1=OpenGL 2=Vulkan 3=Metal 4=D3D12."),
                                       QStringLiteral("N"));
    Parser.addOption(RhiOption);
    Parser.addPositionalArgument(QStringLiteral("world"), QStringLiteral("A .nmworld file to open."), QStringLiteral("[world]"));
    Parser.process(App);

    /* Resolve the positional path BEFORE changing directory, so a path
     * relative to where the editor was launched still means that. */
    QString InitialWorld;
    if(!Parser.positionalArguments().isEmpty())
    {
        InitialWorld = QFileInfo(Parser.positionalArguments().front()).absoluteFilePath();
    }

    /* The cooked shaders and Config/ are staged beside the executable, and
     * every asset path is bundle-relative -- the working directory the game
     * hosts also run from. */
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    if(QFileInfo::exists(QStringLiteral("Config/DefaultEngine.ini")))
    {
        EngineCore::LoadConfigFile("Config/DefaultEngine.ini");
    }

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
        Window.show();

        Result = App.exec();
    }

    EngineCore::SetLogHandler(nullptr);
    return Result;
}
