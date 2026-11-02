#if WITH_EDITOR

/*
 * NimbleLauncher: File > New Project without the editor (LauncherWindow.h).
 *
 *     NimbleLauncher [--no-warmup]
 *
 * --no-warmup leaves the engine warm-up stopped until its Start is pressed.
 */

#include <QApplication>
#include <QProcess>
#include <QStringList>

#include "EditorBuildTree.h"
#include "Private/Launcher/LauncherWindow.h"

namespace
{
    /* Started outside a developer prompt, MSVC's environment for this process
     * and so for every cmake it runs: the wizard's Configure and Build steps,
     * which run cmake directly, as well as the warm-up. */
    void ImportCompilerEnvironment()
    {
#ifdef _WIN32
        const QString VCVarsAll = QStringLiteral(NIMBLEEDITOR_VCVARSALL);
        if(VCVarsAll.isEmpty() || !qEnvironmentVariableIsEmpty("INCLUDE")) { return; }

        QProcess Shell;
        Shell.setProgram(QStringLiteral("cmd.exe"));
        Shell.setNativeArguments(QStringLiteral("/d /s /c \"\"%1\" %2 >nul 2>&1 && set\"")
            .arg(VCVarsAll, QStringLiteral(NIMBLEEDITOR_VCVARS_ARGS)));
        Shell.start();
        if(!Shell.waitForFinished(120000) || Shell.exitStatus() != QProcess::NormalExit || Shell.exitCode() != 0) { return; }

        const QStringList Lines = QString::fromLocal8Bit(Shell.readAllStandardOutput()).split(QLatin1Char('\n'));
        for(QString Line : Lines)
        {
            Line = Line.trimmed();
            const qsizetype Equals = Line.indexOf(QLatin1Char('='));
            if(Equals > 0)
            {
                qputenv(Line.left(Equals).toLocal8Bit().constData(), Line.mid(Equals + 1).toLocal8Bit());
            }
        }
#endif
    }
}

int main(int InArgc, char* InArgv[])
{
    QApplication App(InArgc, InArgv);
    QApplication::setApplicationName(QStringLiteral("NimbleLauncher"));
    QApplication::setOrganizationName(QStringLiteral("NimbleEngine"));

    ImportCompilerEnvironment();

    LauncherWindow Window;
    Window.show();
    if(!QApplication::arguments().contains(QStringLiteral("--no-warmup")))
    {
        Window.StartWarmup();
    }
    return QApplication::exec();
}

#endif // WITH_EDITOR
