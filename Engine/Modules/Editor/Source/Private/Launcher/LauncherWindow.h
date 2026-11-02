#ifndef NIMBLELAUNCHER_LAUNCHERWINDOW_H
#define NIMBLELAUNCHER_LAUNCHERWINDOW_H

#if WITH_EDITOR

#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class SourceBuild;

/*
 * NimbleLauncher's window: File > New Project without the editor. "New
 * Project..." opens the editor's own NewProjectWizard, so a Blank Game or an
 * example (Demo Lab, Worlds Lab) is made exactly as File > New Project makes it.
 *
 * Engine warm-up: at start, the engine modules (WITH_EDITOR, RelWithDebInfo,
 * no editor, no tests) are configured and built in a tree of the launcher's
 * own, Engine/Intermediate/LauncherWarmup, unless that tree is already up to
 * date (Ninja finds nothing to do). Configuring it also prebuilds the
 * third-party libraries into Engine/Intermediate/ThirdParty, which every
 * game's tree on this toolchain reuses; the engine objects themselves are not
 * reused yet -- a game's tree compiles the engine again (no engine SDK).
 * Paused while the wizard configures or builds a project, so the two do not
 * fight over the cores, and resumed when the wizard closes.
 */
class LauncherWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LauncherWindow(QWidget* InParent = nullptr);
    ~LauncherWindow() override;

    /* Starts (or resumes) the warm-up: configure if the tree has none, then build. */
    void StartWarmup();

private:
    void OpenNewProject();
    void StopWarmup(const QString& InWhy);
    void OnWarmupLine(const QString& InLine);
    void OnWarmupFinished(bool bInSucceeded, const QString& InStep);
    void SetWarmupStatus(const QString& InText);
    QString GetWarmupDir() const;

    SourceBuild*    Warmup        = nullptr;
    QLabel*         WarmupStatus  = nullptr;
    QProgressBar*   WarmupBar     = nullptr;
    QPushButton*    WarmupButton  = nullptr;
    QPushButton*    LogButton     = nullptr;
    QPlainTextEdit* WarmupLog     = nullptr;
    QString         StopReason;          // why the running step was cancelled
    bool            bResumeWarmup = false;   // restart once the paused step reports
};

#endif // WITH_EDITOR

#endif //NIMBLELAUNCHER_LAUNCHERWINDOW_H
