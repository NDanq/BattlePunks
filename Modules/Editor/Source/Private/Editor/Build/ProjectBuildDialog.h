#ifndef NIMBLEEDITOR_PROJECTBUILDDIALOG_H
#define NIMBLEEDITOR_PROJECTBUILDDIALOG_H

#if WITH_EDITOR

#include <functional>

#include <QDialog>
#include <QFile>
#include <QString>

#include "Private/Editor/Build/ContentCook.h"
#include "Private/Editor/Build/ProjectBuild.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class SourceBuild;

/*
 * Build > Build Project: the project as a game, in one place.
 *
 *   Options   configuration, cook, compile, open the folder after -- kept in
 *             [Build] of Config/DefaultEngine.ini; which worlds are cooked is
 *             Project Settings > Worlds' (shown here, edited there)
 *   Cook      ContentCook.h: the included worlds and the project's content,
 *             into <project>/Cooked; its stats shown when it ends
 *   Compile   the game's Client preset (ProjectBuild.h), configured and built
 *             in its own tree; then its folder opens
 *
 * Every run writes its whole output -- the cook's lines and stats, cmake's --
 * to <project>/Saved/Logs/Cook-<time>.log (cook only) or Build-<time>.log.
 *
 * The cook reads the saved files: unsaved edits are not in it. Closing the
 * dialog cancels a compile in progress.
 */
class ProjectBuildDialog : public QDialog
{
    Q_OBJECT

public:
    /* InOpenProjectSettings shows Project Settings at its Worlds page. */
    ProjectBuildDialog(QWidget* InParent, std::function<void()> InOpenProjectSettings);
    ~ProjectBuildDialog() override;

    /* Cooks only, at once (Build > Cook Content). */
    void RunCookOnly();

private:
    enum class EStep { Idle, Cooking, Configuring, Compiling };

    void ReadOptions();
    EditorProjectBuild::BuildOptions GatherOptions() const;
    void RefreshWorlds();
    void RefreshTarget();
    void SetRunning(bool bInRunning);
    void Log(const QString& InLine);

    /* Cook if asked, then compile if asked. */
    void Run(bool bInCook, bool bInCompile);
    /* False if the cook failed. */
    bool Cook();
    void ShowStats(const EditorCook::CookStats& InStats);
    void StartCompile();
    void OnStepFinished(bool bInSucceeded);
    void Finish(bool bInSucceeded, const QString& InSummary);

    std::function<void()> OpenProjectSettings;

    QString         ProjectRoot;
    QComboBox*      Configuration = nullptr;
    QCheckBox*      CookContent   = nullptr;
    QCheckBox*      CompileGame   = nullptr;
    QCheckBox*      OpenOutput    = nullptr;
    QCheckBox*      RunUncooked   = nullptr;
    QLabel*         Target        = nullptr;
    QLabel*         WorldList     = nullptr;
    QTableWidget*   Stats         = nullptr;
    QPlainTextEdit* Output        = nullptr;
    QLabel*         Status        = nullptr;
    QPushButton*    BuildButton   = nullptr;
    QPushButton*    CookButton    = nullptr;
    QPushButton*    CancelButton  = nullptr;

    SourceBuild* Builder = nullptr;
    EStep        Step    = EStep::Idle;
    QString      Preset;
    QString      CookSummary;
    QFile        RunLog;   // this run's Saved/Logs file
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PROJECTBUILDDIALOG_H
