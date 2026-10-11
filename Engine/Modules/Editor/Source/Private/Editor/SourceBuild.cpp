#if WITH_EDITOR

#include "Private/Editor/SourceBuild.h"

#include <string>

#include <QDir>
#include <QRegularExpression>
#include <QStringList>

#include "Public/Core/CoreLog.h"

#include "EditorBuildTree.h"

/* NIMBLE_BUILD_CONFIG comes from the Editor's CMakeLists.txt as a bare token,
 * as it does for AboutDialog.cpp. */
#define NIMBLE_SOURCEBUILD_STRINGIFY2(X) #X
#define NIMBLE_SOURCEBUILD_STRINGIFY(X)  NIMBLE_SOURCEBUILD_STRINGIFY2(X)

namespace
{
    std::string ToStd(const QString& InText)
    {
        return std::string(InText.toUtf8().constData());
    }

    QString Quoted(const QString& InText)
    {
        return QLatin1Char('"') + InText + QLatin1Char('"');
    }
}

SourceBuild::SourceBuild(QObject* InParent)
    : QObject(InParent)
{
}

SourceBuild::~SourceBuild()
{
    if(Process)
    {
        /* No Finished for a build the editor is closing on. */
        Process->disconnect(this);
        Process->kill();
        Process->waitForFinished(3000);
        delete Process;
        Process = nullptr;
    }
}

QString SourceBuild::GetCMakeCommand()
{
    return QStringLiteral(NIMBLEEDITOR_CMAKE_COMMAND);
}

bool SourceBuild::Start(const QStringList& InTargets)
{
    const QString BuildDir = QStringLiteral(NIMBLEEDITOR_BUILD_DIR);
    if(!QDir(BuildDir).exists())
    {
        CORE_LOG_ERROR("[Build] the build tree this editor came from is gone: " + ToStd(BuildDir))
        return false;
    }

    /* --config is what a multi-config generator needs and a single-config one
     * ignores. */
    if(InTargets.isEmpty()) { return false; }
    QStringList Args = {
        QStringLiteral("--build"),  BuildDir,
        QStringLiteral("--config"), QStringLiteral(NIMBLE_SOURCEBUILD_STRINGIFY(NIMBLE_BUILD_CONFIG)),
        QStringLiteral("--target"),
    };

    Args << InTargets;   // cmake 3.15+: several targets after one --target

    /* The first target names the build: the game module. */
    return StartCommand(Args, BuildDir, InTargets.first());
}

bool SourceBuild::StartCommand(const QStringList& InArgs, const QString& InWorkingDir, const QString& InLabel)
{
    if(Process)
    {
        CORE_LOG_WARN("[Build] already building " + ToStd(Target))
        return false;
    }

    const QString CMake = GetCMakeCommand();
    const QStringList& Args = InArgs;

    Process = new QProcess(this);
    Process->setProcessChannelMode(QProcess::MergedChannels);
    Process->setWorkingDirectory(InWorkingDir);
    connect(Process, &QProcess::readyReadStandardOutput, this, &SourceBuild::OnOutput);
    connect(Process, &QProcess::finished, this, &SourceBuild::OnFinished);
    connect(Process, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError InError)
    {
        /* A process that never started sends no finished. */
        if(InError == QProcess::FailedToStart)
        {
            CORE_LOG_ERROR("[Build] could not run " + ToStd(Process->program()) + ": " + ToStd(Process->errorString()))
            OnFinished(-1, QProcess::CrashExit);
        }
    });

    Target     = InLabel;
    bCancelled = false;
    Pending.clear();

    const QString VCVarsAll = QStringLiteral(NIMBLEEDITOR_VCVARSALL);
    QString       Shown     = Quoted(CMake) + QLatin1Char(' ') + Args.join(QLatin1Char(' '));

#ifdef _WIN32
    /* Outside a developer prompt cl.exe finds no headers: run vcvarsall
     * (silenced) in the same cmd line. /s strips only the outer quotes. */
    if(!VCVarsAll.isEmpty() && qEnvironmentVariableIsEmpty("INCLUDE"))
    {
        QStringList Quoting;
        for(const QString& Arg : Args)
        {
            Quoting << Quoted(Arg);
        }
        const QString Line = Quoted(VCVarsAll) + QLatin1Char(' ') + QStringLiteral(NIMBLEEDITOR_VCVARS_ARGS)
                           + QStringLiteral(" >nul 2>&1 && ") + Quoted(CMake) + QLatin1Char(' ') + Quoting.join(QLatin1Char(' '));
        Process->setProgram(QStringLiteral("cmd.exe"));
        Process->setNativeArguments(QStringLiteral("/d /s /c \"") + Line + QLatin1Char('"'));
        Shown = QStringLiteral("(vcvarsall %1) ").arg(QStringLiteral(NIMBLEEDITOR_VCVARS_ARGS)) + Shown;
    }
    else
#endif
    {
        Process->setProgram(CMake);
        Process->setArguments(Args);
    }

    CORE_LOG("[Build] " + ToStd(Shown))
    Clock.start();
    emit Started(Target);
    Process->start();
    return Process != nullptr;   // a failed start has already finished
}

void SourceBuild::Cancel()
{
    if(Process)
    {
        bCancelled = true;
        Process->kill();
    }
}

void SourceBuild::OnOutput()
{
    Pending += Process->readAllStandardOutput();
    for(qsizetype End = Pending.indexOf('\n'); End >= 0; End = Pending.indexOf('\n'))
    {
        /* The compiler writes in the console code page, not UTF-8. */
        LogLine(QString::fromLocal8Bit(Pending.left(End)).trimmed());
        Pending.remove(0, End + 1);
    }
}

bool SourceBuild::IsErrorLine(const QString& InLine)
{
    /* MSVC "x.cpp(12): error C2065", "LINK : fatal error LNK1104", clang and
     * gcc "x.cpp:12:3: error:", and Ninja's own "FAILED:". */
    static const QRegularExpression Error(QStringLiteral(R"(:\s*(fatal\s+)?error\b)"));
    return InLine.startsWith(QStringLiteral("FAILED:")) || Error.match(InLine).hasMatch();
}

void SourceBuild::LogLine(const QString& InLine)
{
    if(InLine.isEmpty())
    {
        return;
    }

    static const QRegularExpression Warning(QStringLiteral(R"(:\s*warning\b)"));

    emit OutputLine(InLine);

    const std::string Line = "[Build] " + ToStd(InLine);
    if(IsErrorLine(InLine))
    {
        CORE_LOG_ERROR(Line)
    }
    else if(Warning.match(InLine).hasMatch())
    {
        CORE_LOG_WARN(Line)
    }
    else
    {
        CORE_LOG(Line)
    }
}

void SourceBuild::OnFinished(const int InExitCode, const QProcess::ExitStatus InStatus)
{
    if(!Process)
    {
        return;
    }

    if(!Pending.isEmpty())
    {
        LogLine(QString::fromLocal8Bit(Pending).trimmed());
        Pending.clear();
    }

    const bool    bSucceeded = !bCancelled && InStatus == QProcess::NormalExit && InExitCode == 0;
    const QString Seconds    = QString::number(double(Clock.elapsed()) / 1000.0, 'f', 1);
    if(bSucceeded)
    {
        CORE_LOG("[Build] " + ToStd(Target) + " built in " + ToStd(Seconds) + " s")
    }
    else if(bCancelled)
    {
        CORE_LOG_WARN("[Build] " + ToStd(Target) + " cancelled")
    }
    else
    {
        CORE_LOG_ERROR("[Build] " + ToStd(Target) + " failed after " + ToStd(Seconds) + " s")
    }

    /* Later: this runs inside one of the process's own signals. */
    Process->deleteLater();
    Process = nullptr;
    emit Finished(bSucceeded, Target);
}

#endif // WITH_EDITOR
