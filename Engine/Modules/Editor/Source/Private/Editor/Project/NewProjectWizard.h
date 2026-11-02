#ifndef NIMBLEEDITOR_NEWPROJECTWIZARD_H
#define NIMBLEEDITOR_NEWPROJECTWIZARD_H

#if WITH_EDITOR

#include <QStringList>
#include <QWizard>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;

/*
 * File > New Project: a new game in Games/ (or anywhere), made from
 * Engine/Templates/Game by ProjectTemplate.h, or unpacked from an example
 * project in Engine/Examples.
 *
 *   Template  Blank Game, or one of the examples Examples.json lists
 *   Project   the name and the location, checked as they are typed; for an
 *             example, whether to build and cook it too
 *   Creating  an example only: Engine/Binaries/NewProject.cmake -DEXAMPLE=
 *             run step by step (unpack, configure, build, cook) with its log;
 *             a failed or cancelled step can be retried
 *   Created   what was written, and how to build and open it
 *
 * The editor edits the project it was built with, so the new game is opened
 * by building its own tree (its CMakePresets.json), not from here. Modal.
 *
 * NimbleLauncher (Source/Private/Launcher) runs this same wizard without the
 * editor; it needs only Qt Widgets, EngineCore, ProjectTemplate and
 * SourceBuild's cmake path.
 */
class NewProjectWizard : public QWizard
{
    Q_OBJECT

public:
    explicit NewProjectWizard(QWidget* InParent);
    ~NewProjectWizard() override;

signals:
    /* The Creating page started running InSteps (GetExampleSteps() names). */
    void RunStarted(const QStringList& InSteps);

private:
    enum EPage { Page_Template, Page_Project, Page_Creating, Page_Created };

    QWizardPage* BuildTemplatePage();
    QWizardPage* BuildProjectPage();
    QWizardPage* BuildCreatingPage();
    QWizardPage* BuildCreatedPage();

    int  nextId() const override;
    void initializePage(int InId) override;
    bool validateCurrentPage() override;
    void reject() override;

    void RefreshStatus();
    void RefreshTemplateInfo();

    /* The example chosen on the Template page; -1 for Blank Game. */
    int  GetExampleIndex() const;

    /* Runs the example's steps from InFirstStep (an index into Steps). */
    void StartRun(int InFirstStep);
    void StopRun();
    void OnRunOutput();
    void OnRunFinished(int InExitCode, bool bInCrashed);
    void RefreshSteps();
    void ShowCreated(const QString& InDetails);

    QListWidget*    Templates    = nullptr;
    QLabel*         TemplateInfo = nullptr;
    QLineEdit*      NameEdit     = nullptr;
    QLineEdit*      LocationEdit = nullptr;
    QCheckBox*      BuildCheck   = nullptr;
    QLabel*         Status       = nullptr;
    QListWidget*    StepList     = nullptr;
    QProgressBar*   Progress     = nullptr;
    QPlainTextEdit* Log          = nullptr;
    QPushButton*    RetryButton  = nullptr;
    QPushButton*    StopButton   = nullptr;
    QLabel*         Summary      = nullptr;
    QPlainTextEdit* Files        = nullptr;
    QString         ProjectDir;
    QString         DefaultName;   // what NameEdit was last filled with

    /* The run of the Creating page. */
    QProcess*   Run = nullptr;
    QByteArray  Pending;
    QStringList Steps;                 // GetExampleSteps(): this project's
    int         FirstStep   = 0;       // where this run started
    int         CurrentStep = -1;      // the one running; Steps.size() when all are done
    int         FailedStep  = -1;
    bool        bStopped    = false;
    bool        bSucceeded  = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_NEWPROJECTWIZARD_H
