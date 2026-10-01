#ifndef NIMBLEEDITOR_SOURCEBUILD_H
#define NIMBLEEDITOR_SOURCEBUILD_H

#if WITH_EDITOR

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>

/*
 * Build > Compile Source: background `cmake --build` of one target on the
 * editor's own build tree, output logged as "[Build] ...".
 *
 * Only the game module is useful: it is shadow-copied, while the engine DLLs
 * and editor are locked by this process.
 */
class SourceBuild : public QObject
{
    Q_OBJECT

public:
    explicit SourceBuild(QObject* InParent = nullptr);
    ~SourceBuild() override;   // kills a build still running

    /* False, logged, if already running or unable to start. */
    bool Start(const QString& InTarget);

    /* Kills the running build; Finished follows with bInSucceeded false. */
    void Cancel();

    bool IsRunning() const { return Process != nullptr; }

    /* The target being built, or the last one. */
    const QString& GetTarget() const { return Target; }

signals:
    void Started(const QString& InTarget);
    void Finished(bool bInSucceeded, const QString& InTarget);

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
