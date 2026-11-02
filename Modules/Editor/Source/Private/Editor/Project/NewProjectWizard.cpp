#if WITH_EDITOR

#include "Private/Editor/Project/NewProjectWizard.h"

#include <functional>

#include <QCheckBox>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWizardPage>

#if defined(Q_OS_UNIX)
#include <signal.h>
#include <unistd.h>
#endif

#include "Public/Core/CoreLog.h"
#include "Private/Editor/Project/ProjectTemplate.h"
#include "Private/Editor/SourceBuild.h"

namespace
{
    /* Complete when IsReady says so. */
    class ReadyPage : public QWizardPage
    {
    public:
        std::function<bool()> IsReady;
        bool isComplete() const override { return IsReady && IsReady(); }
        void Changed() { emit completeChanged(); }
    };

    /* What each of UnpackExample.cmake's steps is called here. */
    QString StepTitle(const QString& InStep)
    {
        if(InStep == QLatin1String("unpack"))    { return QStringLiteral("Unpack and rename"); }
        if(InStep == QLatin1String("configure")) { return QStringLiteral("Configure (cmake --preset editor-relwithdebinfo)"); }
        if(InStep == QLatin1String("build"))     { return QStringLiteral("Build"); }
        if(InStep == QLatin1String("cook"))      { return QStringLiteral("Cook shaders (NimbleCook)"); }
        return InStep;
    }

    QString BinariesPlatform()
    {
#if defined(_WIN32)
        return QStringLiteral("Windows");
#elif defined(__APPLE__)
        return QStringLiteral("Mac");
#else
        return QStringLiteral("Linux");
#endif
    }
}

NewProjectWizard::NewProjectWizard(QWidget* InParent)
    : QWizard(InParent)
{
    setWindowTitle(QStringLiteral("New Project"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnLastPage);
    setOption(QWizard::NoCancelButtonOnLastPage);
    setPage(Page_Template, BuildTemplatePage());
    setPage(Page_Project, BuildProjectPage());
    setPage(Page_Creating, BuildCreatingPage());
    setPage(Page_Created, BuildCreatedPage());
    resize(680, 500);
}

NewProjectWizard::~NewProjectWizard()
{
    /* No OnRunFinished for widgets being destroyed. */
    if(Run) { Run->disconnect(this); }
    StopRun();
}

QWizardPage* NewProjectWizard::BuildTemplatePage()
{
    auto* Page = new ReadyPage();
    Page->setTitle(QStringLiteral("New Project"));
    Page->setSubTitle(QStringLiteral("Start from an empty game, or from a whole example game to take apart."));

    auto* Row = new QHBoxLayout(Page);
    Templates = new QListWidget(Page);
    Templates->setMaximumWidth(220);
    Row->addWidget(Templates);
    TemplateInfo = new QLabel(Page);
    TemplateInfo->setWordWrap(true);
    TemplateInfo->setTextFormat(Qt::RichText);
    TemplateInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    TemplateInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Row->addWidget(TemplateInfo, 1);

    auto* Blank = new QListWidgetItem(QStringLiteral("Blank Game"), Templates);
    if(EditorProject::GetGameTemplateDir().isEmpty())
    {
        Blank->setFlags(Blank->flags() & ~Qt::ItemIsEnabled);
    }
    QString Error;
    for(const EditorProject::ExampleInfo& Example : EditorProject::ListExamples(EditorProject::GetExamplesDir(), &Error))
    {
        auto* Item = new QListWidgetItem(QStringLiteral("Example: %1").arg(Example.Title), Templates);
        Item->setData(Qt::UserRole, Example.Name);
    }
    if(!Error.isEmpty())
    {
        CORE_LOG_WARN("[Project] " + Error.toStdString())
    }
    Templates->setCurrentRow(Blank->flags() & Qt::ItemIsEnabled ? 0 : qMin(1, Templates->count() - 1));

    Page->IsReady = [this]()
    {
        const QListWidgetItem* Item = Templates->currentItem();
        return Item && (Item->flags() & Qt::ItemIsEnabled);
    };
    connect(Templates, &QListWidget::currentRowChanged, this, [this, Page]() { RefreshTemplateInfo(); Page->Changed(); });
    RefreshTemplateInfo();
    return Page;
}

QWizardPage* NewProjectWizard::BuildProjectPage()
{
    auto* Page = new ReadyPage();
    Page->setTitle(QStringLiteral("New Project"));

    auto* Form = new QFormLayout(Page);

    NameEdit = new QLineEdit(Page);
    NameEdit->setPlaceholderText(QStringLiteral("MyGame"));
    NameEdit->setToolTip(QStringLiteral("Names the folder, the targets (<Name>, <Name>Game), the C++ classes and the start level"));
    Form->addRow(QStringLiteral("Name"), NameEdit);

    LocationEdit = new QLineEdit(QDir::toNativeSeparators(EditorProject::GetDefaultGamesDir()), Page);
    auto* Browse = new QPushButton(QStringLiteral("Browse..."), Page);
    auto* LocationRow = new QHBoxLayout();
    LocationRow->addWidget(LocationEdit, 1);
    LocationRow->addWidget(Browse);
    Form->addRow(QStringLiteral("Location"), LocationRow);

    BuildCheck = new QCheckBox(QStringLiteral("Build and cook it after unpacking (Editor - RelWithDebInfo)"), Page);
    BuildCheck->setChecked(true);
    BuildCheck->setToolTip(QStringLiteral("Configures and builds the project's own tree, editor included, then cooks its shaders. "
                                          "Takes a while; without it, the next page says how to do it later."));
    Form->addRow(QString(), BuildCheck);

    Status = new QLabel(Page);
    Status->setWordWrap(true);
    Status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Form->addRow(QString(), Status);

    connect(Browse, &QPushButton::clicked, this, [this]()
    {
        const QString Dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Project Location"), LocationEdit->text());
        if(!Dir.isEmpty()) { LocationEdit->setText(QDir::toNativeSeparators(Dir)); }
    });

    Page->IsReady = [this]()
    {
        const QString Location = LocationEdit->text().trimmed();
        const bool bSource = GetExampleIndex() >= 0 ? !EditorProject::GetNewProjectScript().isEmpty()
                                                    : !EditorProject::GetGameTemplateDir().isEmpty();
        return bSource && EditorProject::ValidateName(NameEdit->text()).isEmpty()
               && !Location.isEmpty() && !QFileInfo::exists(QDir(Location).filePath(NameEdit->text()));
    };
    const auto Changed = [this, Page]() { RefreshStatus(); Page->Changed(); };
    connect(NameEdit, &QLineEdit::textChanged, this, Changed);
    connect(LocationEdit, &QLineEdit::textChanged, this, Changed);
    return Page;
}

QWizardPage* NewProjectWizard::BuildCreatingPage()
{
    auto* Page = new ReadyPage();
    Page->setTitle(QStringLiteral("Creating the Project"));
    Page->setCommitPage(true);   // no going back to a half-made project
    Page->IsReady = [this]() { return bSucceeded; };

    auto* Layout = new QVBoxLayout(Page);
    StepList = new QListWidget(Page);
    StepList->setSelectionMode(QAbstractItemView::NoSelection);
    StepList->setFocusPolicy(Qt::NoFocus);
    StepList->setMaximumHeight(100);
    Layout->addWidget(StepList);

    Progress = new QProgressBar(Page);
    Layout->addWidget(Progress);

    Log = new QPlainTextEdit(Page);
    Log->setReadOnly(true);
    Log->setMaximumBlockCount(20000);
    Log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    Layout->addWidget(Log, 1);

    RetryButton = new QPushButton(QStringLiteral("Retry Step"), Page);
    RetryButton->setToolTip(QStringLiteral("Runs the failed or stopped step again, and the ones after it"));
    StopButton = new QPushButton(QStringLiteral("Stop"), Page);
    auto* Open = new QPushButton(QStringLiteral("Open Folder"), Page);
    auto* Row = new QHBoxLayout();
    Row->addStretch(1);
    Row->addWidget(RetryButton);
    Row->addWidget(StopButton);
    Row->addWidget(Open);
    Layout->addLayout(Row);

    connect(RetryButton, &QPushButton::clicked, this, [this]() { if(FailedStep >= 0) { StartRun(FailedStep); } });
    connect(StopButton, &QPushButton::clicked, this, [this]() { StopRun(); });
    connect(Open, &QPushButton::clicked, this, [this]()
    {
        const QString Dir = QFileInfo::exists(ProjectDir) ? ProjectDir : LocationEdit->text().trimmed();
        QDesktopServices::openUrl(QUrl::fromLocalFile(Dir));
    });
    return Page;
}

QWizardPage* NewProjectWizard::BuildCreatedPage()
{
    auto* Page = new QWizardPage();
    Page->setTitle(QStringLiteral("Project Created"));
    Page->setFinalPage(true);

    auto* Layout = new QVBoxLayout(Page);
    Summary = new QLabel(Page);
    Summary->setWordWrap(true);
    Summary->setTextFormat(Qt::RichText);
    Summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Layout->addWidget(Summary);

    Files = new QPlainTextEdit(Page);
    Files->setReadOnly(true);
    Files->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    Layout->addWidget(Files, 1);

    auto* Open = new QPushButton(QStringLiteral("Open Folder"), Page);
    connect(Open, &QPushButton::clicked, this, [this]()
    {
        QDesktopServices::openUrl(QUrl::fromLocalFile(ProjectDir));
    });
    auto* Row = new QHBoxLayout();
    Row->addStretch(1);
    Row->addWidget(Open);
    Layout->addLayout(Row);
    return Page;
}

int NewProjectWizard::GetExampleIndex() const
{
    const int Row = Templates ? Templates->currentRow() : 0;
    return Row >= 1 ? Row - 1 : -1;
}

void NewProjectWizard::RefreshTemplateInfo()
{
    const QListWidgetItem* Item = Templates->currentItem();
    if(!Item) { TemplateInfo->clear(); return; }
    if(GetExampleIndex() < 0)
    {
        TemplateInfo->setText(EditorProject::GetGameTemplateDir().isEmpty()
            ? QStringLiteral("<p>The project template (Engine/Templates/Game) was not found.</p>")
            : QStringLiteral("<p><b>Blank Game</b></p><p>A new game with its own module, level, game mode and game state, "
                             "and its Config, Scripts and Content folders -- as Games/Sandbox is laid out.</p>"));
        return;
    }

    const QString Name = Item->data(Qt::UserRole).toString();
    for(const EditorProject::ExampleInfo& Example : EditorProject::ListExamples(EditorProject::GetExamplesDir()))
    {
        if(Example.Name != Name) { continue; }
        const QLocale Locale;
        TemplateInfo->setText(QStringLiteral(
            "<p><b>%1</b></p><p>%2</p>"
            "<p>%3 files: %4 packed, %5 unpacked. Packed against engine %6.</p>"
            "<p>Unpacked into a folder of its own, renamed if you give it another name; then, if you like, "
            "configured, built and cooked.</p>")
            .arg(Example.Title.toHtmlEscaped(), Example.Description.toHtmlEscaped())
            .arg(Example.Files)
            .arg(Locale.formattedDataSize(Example.PackedBytes), Locale.formattedDataSize(Example.UnpackedBytes),
                 Example.EngineVersion.toHtmlEscaped()));
        return;
    }
}

int NewProjectWizard::nextId() const
{
    switch(currentId())
    {
        case Page_Template: return Page_Project;
        case Page_Project:  return GetExampleIndex() >= 0 ? Page_Creating : Page_Created;
        case Page_Creating: return Page_Created;
        default:            return -1;
    }
}

void NewProjectWizard::initializePage(const int InId)
{
    QWizard::initializePage(InId);
    setButtonText(QWizard::NextButton, InId == Page_Project ? QStringLiteral("Create") : QStringLiteral("Next >"));
    setButtonText(QWizard::CommitButton, QStringLiteral("Next >"));

    if(InId == Page_Project)
    {
        const bool bExample = GetExampleIndex() >= 0;
        page(Page_Project)->setSubTitle(bExample
            ? QStringLiteral("Where the example goes, and under what name: its own, or another (its module, classes "
                             "and settings are renamed to match).")
            : QStringLiteral("A new game with its own module, level, game mode and game state, "
                             "and its Config, Scripts and Content folders -- as Games/Sandbox is laid out."));
        BuildCheck->setVisible(bExample);

        /* An example's own name by default; a name typed by hand is kept. */
        const QString Default = bExample ? Templates->currentItem()->data(Qt::UserRole).toString() : QString();
        if(NameEdit->text() == DefaultName) { NameEdit->setText(Default); }
        DefaultName = Default;
        RefreshStatus();
    }
    else if(InId == Page_Creating)
    {
        Steps = EditorProject::GetExampleSteps(BuildCheck->isChecked());
        Log->clear();
        StartRun(0);
    }
}

void NewProjectWizard::RefreshStatus()
{
    const QString Name     = NameEdit->text();
    const QString Location = LocationEdit->text().trimmed();
    QString Problem;
    if(GetExampleIndex() >= 0)
    {
        if(EditorProject::GetNewProjectScript().isEmpty())
        {
            Problem = QStringLiteral("Engine/Binaries/NewProject.cmake, which unpacks examples, was not found");
        }
    }
    else if(EditorProject::GetGameTemplateDir().isEmpty())
    {
        Problem = QStringLiteral("The project template (Engine/Templates/Game) was not found");
    }
    if(Problem.isEmpty()) { Problem = EditorProject::ValidateName(Name); }
    if(Problem.isEmpty() && Location.isEmpty())
    {
        Problem = QStringLiteral("Choose a location");
    }
    const QString Target = QDir::toNativeSeparators(QDir::cleanPath(QDir(Location).filePath(Name)));
    if(Problem.isEmpty() && QFileInfo::exists(Target))
    {
        Problem = QStringLiteral("%1 already exists").arg(Target);
    }
    Status->setText(Problem.isEmpty() ? QStringLiteral("Creates %1").arg(Target) : Problem);
}

bool NewProjectWizard::validateCurrentPage()
{
    if(currentId() != Page_Project) { return true; }
    if(GetExampleIndex() >= 0) { return true; }   // the Creating page does it

    QString     Error;
    QStringList Written;
    if(!EditorProject::CreateProject(EditorProject::GetGameTemplateDir(), EditorProject::GetEngineDir(),
                                     LocationEdit->text().trimmed(), NameEdit->text(), ProjectDir, Error, &Written))
    {
        Status->setText(Error);
        CORE_LOG_ERROR("[Project] " + Error.toStdString())
        return false;
    }

    const QString Native = QDir::toNativeSeparators(ProjectDir);
    CORE_LOG("[Project] created " + Native.toStdString())
    Summary->setText(QStringLiteral(
        "<p><b>%1</b> is in <code>%2</code>.</p>"
        "<p>This editor edits the project it was built with. To open the new game, build its own tree "
        "from its folder, then run the editor in its <code>Binaries</code>:</p>"
        "<pre>cd \"%2\"\ncmake --preset editor-debug\ncmake --build --preset editor-debug</pre>"
        "<p>Or open the folder in an IDE as a CMake project. <code>Docs/GettingStarted.md</code> "
        "walks through the rest.</p>")
        .arg(NameEdit->text().toHtmlEscaped(), Native.toHtmlEscaped()));
    Files->setPlainText(Written.join(QLatin1Char('\n')));
    return true;
}

void NewProjectWizard::reject()
{
    StopRun();
    QWizard::reject();
}

void NewProjectWizard::StartRun(const int InFirstStep)
{
    if(Run) { return; }
    const QString Example = Templates->currentItem()->data(Qt::UserRole).toString();
    const QString Parent  = LocationEdit->text().trimmed();
    ProjectDir = QDir::cleanPath(QDir(Parent).filePath(NameEdit->text()));

    FirstStep   = InFirstStep;
    CurrentStep = InFirstStep;
    FailedStep  = -1;
    bStopped    = false;
    bSucceeded  = false;
    Pending.clear();
    RefreshSteps();
    page(Page_Creating)->setSubTitle(QStringLiteral("%1, from the %2 example, in %3")
        .arg(NameEdit->text(), Example, QDir::toNativeSeparators(Parent)));

    const QStringList Args = EditorProject::ExampleScriptArgs(EditorProject::GetNewProjectScript(), EditorProject::GetExamplesDir(),
                                                              Example, Parent, NameEdit->text(), Steps.mid(InFirstStep));
    Log->appendPlainText(QStringLiteral("> cmake %1").arg(Args.join(QLatin1Char(' '))));

    Run = new QProcess(this);
    Run->setProcessChannelMode(QProcess::MergedChannels);
    Run->setWorkingDirectory(EditorProject::GetEngineDir());
#if defined(Q_OS_UNIX)
    /* Its own process group, so Stop ends the compilers it starts too. */
    Run->setChildProcessModifier([]() { ::setpgid(0, 0); });
#endif
    connect(Run, &QProcess::readyReadStandardOutput, this, &NewProjectWizard::OnRunOutput);
    connect(Run, &QProcess::finished, this, [this](const int InExitCode, const QProcess::ExitStatus InStatus)
    {
        OnRunFinished(InExitCode, InStatus == QProcess::CrashExit);
    });
    connect(Run, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError InError)
    {
        if(InError == QProcess::FailedToStart) { OnRunFinished(-1, true); }
    });
    CORE_LOG("[Project] creating " + ProjectDir.toStdString() + " from the " + Example.toStdString() + " example")
    Run->start(SourceBuild::GetCMakeCommand(), Args);
}

void NewProjectWizard::StopRun()
{
    if(!Run) { return; }
    bStopped = true;
    const qint64 Pid = Run->processId();
#if defined(Q_OS_UNIX)
    /* SIGINT first: Ninja deletes what its interrupted commands half-wrote,
     * where a kill leaves empty objects that the next build links. */
    if(Pid > 0)
    {
        ::kill(-pid_t(Pid), SIGINT);
        if(Run->waitForFinished(10000)) { return; }
        ::kill(-pid_t(Pid), SIGKILL);
    }
#elif defined(Q_OS_WIN)
    if(Pid > 0) { QProcess::execute(QStringLiteral("taskkill"), { QStringLiteral("/T"), QStringLiteral("/F"), QStringLiteral("/PID"), QString::number(Pid) }); }
#endif
    Run->kill();
    Run->waitForFinished(5000);
}

void NewProjectWizard::OnRunOutput()
{
    Pending += Run->readAllStandardOutput();
    for(qsizetype End = Pending.indexOf('\n'); End >= 0; End = Pending.indexOf('\n'))
    {
        /* The compiler writes in the console code page, not UTF-8. */
        const QString Line = QString::fromLocal8Bit(Pending.left(End)).trimmed();
        Pending.remove(0, End + 1);
        Log->appendPlainText(Line);

        int Index = 0, Count = 0;
        QString What;
        if(EditorProject::ParseExampleStep(Line, Index, Count, What))
        {
            CurrentStep = FirstStep + Index - 1;
            RefreshSteps();
        }
    }
}

void NewProjectWizard::OnRunFinished(const int InExitCode, const bool bInCrashed)
{
    if(!Run) { return; }
    if(!Pending.isEmpty()) { Log->appendPlainText(QString::fromLocal8Bit(Pending).trimmed()); Pending.clear(); }
    Run->deleteLater();
    Run = nullptr;

    if(InExitCode == 0 && !bInCrashed && !bStopped)
    {
        CurrentStep = int(Steps.size());
        bSucceeded  = true;
        CORE_LOG("[Project] created " + ProjectDir.toStdString())
        RefreshSteps();
        const bool bBuilt = Steps.contains(QStringLiteral("build"));
        const QString Native = QDir::toNativeSeparators(ProjectDir);
        const QString Editor = QDir::toNativeSeparators(QStringLiteral("%1/Binaries/%2_RelWithDebInfo/NimbleEditor").arg(ProjectDir, BinariesPlatform()));
        ShowCreated(bBuilt
            ? QStringLiteral("<p><b>%1</b> is in <code>%2</code>, built and cooked.</p>"
                             "<p>This editor edits the project it was built with. Open the new game with its own:</p>"
                             "<pre>%3</pre><p><code>Docs/</code> in the project walks through the rest.</p>")
                  .arg(NameEdit->text().toHtmlEscaped(), Native.toHtmlEscaped(), Editor.toHtmlEscaped())
            : QStringLiteral("<p><b>%1</b> is in <code>%2</code>.</p>"
                             "<p>This editor edits the project it was built with. To open the new game, build its own tree "
                             "from its folder, then run the editor in its <code>Binaries</code>:</p>"
                             "<pre>cd \"%2\"\ncmake --preset editor-relwithdebinfo\ncmake --build --preset editor-relwithdebinfo</pre>"
                             "<p><code>Docs/</code> in the project walks through the rest.</p>")
                  .arg(NameEdit->text().toHtmlEscaped(), Native.toHtmlEscaped()));
        static_cast<ReadyPage*>(page(Page_Creating))->Changed();
        next();
        return;
    }

    FailedStep = qBound(FirstStep, CurrentStep, int(Steps.size()) - 1);
    CurrentStep = -1;
    /* A stopped unpack leaves its hidden staging folder; nothing else of it. */
    if(Steps.value(FailedStep) == QLatin1String("unpack"))
    {
        QDir(QDir(LocationEdit->text().trimmed()).filePath(QStringLiteral(".%1.creating").arg(NameEdit->text()))).removeRecursively();
    }
    const QString Why = bStopped ? QStringLiteral("stopped") : QStringLiteral("failed");
    Log->appendPlainText(QStringLiteral("\n%1 %2. %3").arg(StepTitle(Steps.value(FailedStep)), Why,
        FailedStep > 0 ? QStringLiteral("The project is kept; fix the cause and Retry Step.") : QStringLiteral("Nothing was left behind.")));
    CORE_LOG_ERROR("[Project] " + StepTitle(Steps.value(FailedStep)).toStdString() + " " + Why.toStdString() + " for " + ProjectDir.toStdString())
    RefreshSteps();
}

void NewProjectWizard::RefreshSteps()
{
    StepList->clear();
    for(int Index = 0; Index < Steps.size(); ++Index)
    {
        QString Mark = QStringLiteral("   ");
        if(Index == FailedStep)                                        { Mark = QString(QChar(0x2716)) + QLatin1Char(' '); }
        else if(Index < CurrentStep || (CurrentStep < 0 && Index < FailedStep)) { Mark = QString(QChar(0x2714)) + QLatin1Char(' '); }
        else if(Index == CurrentStep)                                  { Mark = QString(QChar(0x25B6)) + QLatin1Char(' '); }
        auto* Item = new QListWidgetItem(Mark + StepTitle(Steps[Index]), StepList);
        if(Index == FailedStep) { Item->setForeground(QColor(220, 80, 80)); }
    }
    const int Done = CurrentStep >= 0 ? CurrentStep : FailedStep;
    Progress->setRange(0, int(Steps.size()));
    Progress->setValue(qMax(0, Done));
    RetryButton->setEnabled(!Run && FailedStep >= 0);
    StopButton->setEnabled(Run != nullptr);
}

void NewProjectWizard::ShowCreated(const QString& InDetails)
{
    Summary->setText(InDetails);
    Files->setPlainText(Log->toPlainText());
}

#endif // WITH_EDITOR
