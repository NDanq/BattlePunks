#if WITH_EDITOR

#include "Private/Editor/PlayProcesses.h"

#include <string>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"

#include "EditorProject.h"

namespace
{
    std::string ToStd(const QString& InText) { return std::string(InText.toUtf8().constData()); }
}

namespace GameExecutables
{
    QString GetModule()
    {
        const EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar("Game", "Module");
        return Var ? QString::fromUtf8(Var->GetString()) : QString();
    }

    QStringList FindTargets(const QString& InModule, const QString& InKind)
    {
        QStringList Found;
        const QStringList Entries = QStringLiteral(NIMBLEEDITOR_GAME_EXECUTABLES).split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for(const QString& Entry : Entries)
        {
            const QStringList Parts = Entry.split(QLatin1Char(':'));
            if(Parts.size() == 3 && Parts[0] == InKind && Parts[1] == InModule) { Found << Parts[2]; }
        }
        return Found;
    }

    QString GetPath(const QString& InTarget)
    {
#ifdef _WIN32
        const QString Name = InTarget + QStringLiteral(".exe");
#else
        const QString Name = InTarget;
#endif
        return QDir(QCoreApplication::applicationDirPath()).filePath(Name);
    }
}

PlayProcesses::PlayProcesses(QObject* InParent)
    : QObject(InParent)
{
}

PlayProcesses::~PlayProcesses()
{
    StopAll();
}

bool PlayProcesses::StartServer(const uint16_t InPort)
{
    const QStringList Servers = GameExecutables::FindTargets(GameExecutables::GetModule(), QStringLiteral("server"));
    if(Servers.isEmpty())
    {
        CORE_LOG_ERROR("[PIE] the game has no dedicated server target (nimble_add_server_launcher), or NetCore is off")
        return false;
    }
    return Launch(Servers.first(), { QStringLiteral("-port=%1").arg(InPort) }, QStringLiteral("[Server]"));
}

bool PlayProcesses::StartClient(const QString& InAddress, const uint16_t InPort, const QString& InExtraArgs)
{
    const QStringList Clients = GameExecutables::FindTargets(GameExecutables::GetModule(), QStringLiteral("client"));
    if(Clients.isEmpty())
    {
        CORE_LOG_ERROR("[PIE] the game has no launcher target (nimble_add_game_launcher) to run as a client")
        return false;
    }
    const int Number = ClientCount + 1;
    QStringList Args = {
        QStringLiteral("-connect=%1:%2").arg(InAddress).arg(InPort),
        /* Smaller than the game's own, so the windows sit beside the editor. */
        QStringLiteral("-Display.ResolutionX=960"),
        QStringLiteral("-Display.ResolutionY=540"),
    };
    Args << InExtraArgs.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if(!Launch(Clients.first(), Args, QStringLiteral("[Client %1]").arg(Number)))
    {
        return false;
    }
    ClientCount = Number;
    return true;
}

bool PlayProcesses::Launch(const QString& InTarget, const QStringList& InArgs, const QString& InTag)
{
    const QString Path = GameExecutables::GetPath(InTarget);
    if(!QFileInfo::exists(Path))
    {
        CORE_LOG_ERROR("[PIE] " + ToStd(InTarget) + " is not built -- Build > Compile Source builds it (" + ToStd(Path) + ")")
        return false;
    }

    auto* Entry    = new Running();
    Entry->Tag     = InTag;
    Entry->Process = new QProcess(this);
    Entry->Process->setProcessChannelMode(QProcess::MergedChannels);
    Entry->Process->setWorkingDirectory(QFileInfo(Path).absolutePath());

    /* Its log, line by line, into the editor's Console. */
    connect(Entry->Process, &QProcess::readyReadStandardOutput, this, [Entry]()
    {
        Entry->Pending += Entry->Process->readAllStandardOutput();
        for(qsizetype End = Entry->Pending.indexOf('\n'); End >= 0; End = Entry->Pending.indexOf('\n'))
        {
            const QString Line = QString::fromLocal8Bit(Entry->Pending.left(End)).trimmed();
            Entry->Pending.remove(0, End + 1);
            if(Line.isEmpty()) { continue; }
            const std::string Text = ToStd(Entry->Tag + QLatin1Char(' ') + Line);
            if(Line.startsWith(QStringLiteral("[ERROR]")))     { CORE_LOG_ERROR(Text) }
            else if(Line.startsWith(QStringLiteral("[WARN]"))) { CORE_LOG_WARN(Text) }
            else                                              { CORE_LOG(Text) }
        }
    });
    connect(Entry->Process, &QProcess::finished, this, [Entry](const int InExitCode, const QProcess::ExitStatus)
    {
        CORE_LOG(ToStd(Entry->Tag) + " exited (" + std::to_string(InExitCode) + ")")
    });

    Entry->Process->start(Path, InArgs);
    if(!Entry->Process->waitForStarted(5000))
    {
        CORE_LOG_ERROR("[PIE] could not start " + ToStd(Path) + ": " + ToStd(Entry->Process->errorString()))
        delete Entry->Process;
        delete Entry;
        return false;
    }

    CORE_LOG("[PIE] started " + ToStd(InTag) + " " + ToStd(InTarget) + " " + ToStd(InArgs.join(QLatin1Char(' '))))
    Processes.push_back(Entry);
    return true;
}

void PlayProcesses::StopAll()
{
    for(Running* Entry : Processes)
    {
        Entry->Process->disconnect(this);
        if(Entry->Process->state() != QProcess::NotRunning)
        {
            /* No console to Ctrl+C: the run is over, so end it. */
            Entry->Process->kill();
            Entry->Process->waitForFinished(3000);
        }
        delete Entry->Process;
        delete Entry;
    }
    if(!Processes.empty()) { CORE_LOG("[PIE] stopped the game's other processes") }
    Processes.clear();
    ClientCount = 0;
}

#endif // WITH_EDITOR
