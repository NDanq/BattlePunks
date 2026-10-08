#ifndef NIMBLEEDITOR_SOURCEBUILD_H
#define NIMBLEEDITOR_SOURCEBUILD_H

#if WITH_EDITOR

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

/*
 * Build > Compile Source: background `cmake --build` of the game's targets on the
 * editor's own build tree, output logged as "[Build] ...".
 *
 * The game module (shadow-copied, so rebuilt while loaded), its launcher and
 * its dedicated server -- never the engine DLLs and editor,
 * which this process holds locked.
 *
 * StartCommand runs any cmake command line the same way -- Build Project's
 * `cmake --preset` and `cmake --build --preset` in the game's own folder.
 */
class SourceBuild : public QObject
{
    Q_OBJECT

public:
    explicit SourceBuild(QObject* InParent = nullptr);
    ~SourceBuild() override;   // kills a build still running

    /* False, logged, if already running or unable to start. */
    /* InTargets built together, in one cmake --build; the first names the
     * build (GetTarget): the game module. */
    bool Start(const QStringList& InTargets);

    /* `cmake <InArgs>` in InWorkingDir, named InLabel (GetTarget). With MSVC
     * outside a developer prompt, vcvarsall first, as Start. False, logged,
     * if already running or unable to start. */
    bool StartCommand(const QStringList& InArgs, const QString& InWorkingDir, const QString& InLabel);

    /* The cmake this editor was configured with. */
    static QString GetCMakeCommand();

    /* Kills the running build; Finished follows with bInSucceeded false. */
    void Cancel();

    bool IsRunning() const { return Process != nullptr; }

    /* The target being built, or the last one. */
    const QString& GetTarget() const { return Target; }

signals:
    void Started(const QString& InTarget);
    void Finished(bool bInSucceeded, const QString& InTarget);
    /* Each line of the tool's output, as logged. */
    void OutputLine(const QString& InLine);

private:
    void OnOutput();
    void OnFinished(int InExitCode, QProcess::ExitStatus InStatus);
    void LogLine(const QString& InLine);

    QProcess*     Process = nullptr;
    QByteArray    Pending;   // output past the last newline
    QString       Target;
    QElapsedTimer Clock;
    bool          bCancelled = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SOURCEBUILD_H
