#ifndef NIMBLEEDITOR_SOURCEBUILD_H
#define NIMBLEEDITOR_SOURCEBUILD_H

#if WITH_EDITOR

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>

/*
 * Build > Compile Source: `cmake --build` on the tree this editor was built
 * from (EditorBuildTree.h.in), for one target, in the background.
 *
 * Every line the build prints goes to the log -- so to the Console -- as
 * "[Build] ...", errors and warnings at their own level. Finished reports how
 * it went; the editor reloads the game from there.
 *
 * Only the game module is worth building from inside the editor. The engine's
 * DLLs and the editor itself are loaded by this process and cannot be
 * relinked while it runs; the game module can, because the module manager
 * loads a shadow copy (PlaySession.h). A build of the game that also has to
 * relink an engine DLL fails at that link, and says so.
 *
 * Private to the editor: EditorMainWindow owns it.
 */
class SourceBuild : public QObject
{
    Q_OBJECT

public:
    explicit SourceBuild(QObject* InParent = nullptr);
    ~SourceBuild() override;   // kills a build still running

    /* Starts building InTarget. False, with the reason logged, if a build is
     * already running or the build could not be started. */
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
