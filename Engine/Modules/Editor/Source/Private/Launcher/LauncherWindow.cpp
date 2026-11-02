#if WITH_EDITOR

#include "Private/Launcher/LauncherWindow.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>

#include "Private/Editor/Project/NewProjectWizard.h"
#include "Private/Editor/Project/ProjectTemplate.h"
#include "Private/Editor/SourceBuild.h"

namespace
{
    const QString STEP_CONFIGURE = QStringLiteral("configure");
    const QString STEP_BUILD     = QStringLiteral("build");
}

LauncherWindow::LauncherWindow(QWidget* InParent)
    : QWidget(InParent)
{
    setWindowTitle(QStringLiteral("NimbleEngine Launcher"));
    resize(720, 460);

    const QString Engine = EditorProject::GetEngineDir();
    auto* Layout = new QVBoxLayout(this);

    auto* Heading = new QLabel(this);
    Heading->setTextFormat(Qt::RichText);
    Heading->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Heading->setText(QStringLiteral("<h2>NimbleEngine</h2><p>Engine: <code>%1</code></p>")
        .arg(Engine.isEmpty() ? QStringLiteral("not found") : QDir::toNativeSeparators(Engine).toHtmlEscaped()));
    Layout->addWidget(Heading);

    auto* About = new QLabel(this);
    About->setWordWrap(true);
    About->setTextFormat(Qt::RichText);
    About->setText(QStringLiteral(
        "<p><b>New Project</b> makes a Blank Game, or unpacks an example (Demo Lab, Worlds Lab), as the "
        "editor's File &gt; New Project does -- without building the editor first.</p>"
        "<p>An example's <i>Build and cook</i> option still compiles the engine and the game's own editor in the "
        "game's tree (<code>editor-relwithdebinfo</code>): the long part, about 15-20 minutes the first time. "
        "Leave it off to only unpack; the last page says how to build later.</p>"));
    Layout->addWidget(About);

    auto* NewProject = new QPushButton(QStringLiteral("New Project..."), this);
    NewProject->setDefault(true);
    NewProject->setEnabled(!Engine.isEmpty());
    auto* OpenGames = new QPushButton(QStringLiteral("Open Games Folder"), this);
    auto* Row = new QHBoxLayout();
    Row->addWidget(NewProject);
    Row->addWidget(OpenGames);
    Row->addStretch(1);
    Layout->addLayout(Row);

    auto* Group = new QGroupBox(QStringLiteral("Engine warm-up"), this);
    auto* GroupLayout = new QVBoxLayout(Group);
    WarmupStatus = new QLabel(Group);
    WarmupStatus->setWordWrap(true);
    WarmupStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    WarmupStatus->setToolTip(QStringLiteral(
        "The engine modules, built in the launcher's own tree. Configuring it also prebuilds the third-party "
        "libraries (Engine/Intermediate/ThirdParty), which every game's tree on this toolchain reuses."));
    GroupLayout->addWidget(WarmupStatus);
    WarmupBar = new QProgressBar(Group);
    WarmupBar->setRange(0, 0);
    WarmupBar->setVisible(false);
    GroupLayout->addWidget(WarmupBar);
    WarmupButton = new QPushButton(QStringLiteral("Stop"), Group);
    LogButton    = new QPushButton(QStringLiteral("Show Log"), Group);
    LogButton->setCheckable(true);
    auto* GroupRow = new QHBoxLayout();
    GroupRow->addStretch(1);
    GroupRow->addWidget(WarmupButton);
    GroupRow->addWidget(LogButton);
    GroupLayout->addLayout(GroupRow);
    WarmupLog = new QPlainTextEdit(Group);
    WarmupLog->setReadOnly(true);
    WarmupLog->setMaximumBlockCount(5000);
    WarmupLog->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    WarmupLog->setVisible(false);
    GroupLayout->addWidget(WarmupLog, 1);
    Layout->addWidget(Group);
    Layout->addStretch(1);   // the log, when shown, takes it instead

    Warmup = new SourceBuild(this);
    connect(Warmup, &SourceBuild::OutputLine, this, &LauncherWindow::OnWarmupLine);
    connect(Warmup, &SourceBuild::Finished, this, &LauncherWindow::OnWarmupFinished);

    connect(NewProject, &QPushButton::clicked, this, &LauncherWindow::OpenNewProject);
    connect(OpenGames, &QPushButton::clicked, this, []()
    {
        const QString Games = EditorProject::GetDefaultGamesDir();
        QDir().mkpath(Games);
        QDesktopServices::openUrl(QUrl::fromLocalFile(Games));
    });
    connect(WarmupButton, &QPushButton::clicked, this, [this]()
    {
        if(Warmup->IsRunning()) { StopWarmup(QStringLiteral("Stopped. The next start of the launcher carries on.")); }
        else                    { StartWarmup(); }
    });
    connect(LogButton, &QPushButton::toggled, this, [this, Layout, Group](const bool bInShown)
    {
        WarmupLog->setVisible(bInShown);
        Layout->setStretchFactor(Group, bInShown ? 1 : 0);
        Layout->setStretch(Layout->count() - 1, bInShown ? 0 : 1);
        LogButton->setText(bInShown ? QStringLiteral("Hide Log") : QStringLiteral("Show Log"));
    });

    SetWarmupStatus(QStringLiteral("Not started."));
    WarmupButton->setText(QStringLiteral("Start"));
    WarmupButton->setEnabled(!Engine.isEmpty());
}

LauncherWindow::~LauncherWindow()
{
    /* No OnWarmupFinished for widgets being destroyed. */
    if(Warmup->IsRunning())
    {
        Warmup->disconnect(this);
        Warmup->Cancel();
    }
}

QString LauncherWindow::GetWarmupDir() const
{
    return QDir(EditorProject::GetEngineDir()).filePath(QStringLiteral("Intermediate/LauncherWarmup"));
}

void LauncherWindow::StartWarmup()
{
    const QString Engine = EditorProject::GetEngineDir();
    if(Warmup->IsRunning() || Engine.isEmpty()) { return; }
    bResumeWarmup = false;
    StopReason.clear();

    const QString Dir = GetWarmupDir();
    QString Step;
    QStringList Args;
    if(!QFileInfo::exists(QDir(Dir).filePath(QStringLiteral("CMakeCache.txt")))
       || !QFileInfo::exists(QDir(Dir).filePath(QStringLiteral("build.ninja"))))
    {
        /* As a game's editor-relwithdebinfo preset configures the engine -- Ninja,
         * the same compiler -- so the third-party prebuild it makes is the one
         * the game's tree finds. No editor, no tests, no Sandbox. */
        Step = STEP_CONFIGURE;
        Args = { QStringLiteral("-S"), Engine, QStringLiteral("-B"), Dir, QStringLiteral("-G"), QStringLiteral("Ninja"),
                 QStringLiteral("-DCMAKE_BUILD_TYPE=RelWithDebInfo"),
                 QStringLiteral("-DNIMBLE_TARGET=Editor"),
                 QStringLiteral("-DNIMBLE_BUILD_EDITOR=OFF"),
                 QStringLiteral("-DNIMBLE_BUILD_TESTS=OFF"),
                 QStringLiteral("-DNIMBLE_BUILD_SANDBOX=OFF"),
                 QStringLiteral("-DNIMBLE_BINARIES_DIR=%1/Binaries").arg(Dir) };
        const QString Ninja = QStringLiteral(NIMBLELAUNCHER_NINJA);
        if(!Ninja.isEmpty()) { Args << QStringLiteral("-DCMAKE_MAKE_PROGRAM=%1").arg(Ninja); }
    }
    else
    {
        Step = STEP_BUILD;
        Args = { QStringLiteral("--build"), Dir };
    }

    WarmupLog->appendPlainText(QStringLiteral("> cmake %1").arg(Args.join(QLatin1Char(' '))));
    if(!Warmup->StartCommand(Args, Engine, Step))
    {
        SetWarmupStatus(QStringLiteral("Could not run %1.").arg(SourceBuild::GetCMakeCommand()));
        return;
    }
    WarmupBar->setRange(0, 0);
    WarmupBar->setVisible(true);
    WarmupButton->setText(QStringLiteral("Stop"));
    SetWarmupStatus(Step == STEP_CONFIGURE
        ? QStringLiteral("Configuring the engine (and prebuilding the third-party libraries, once per toolchain)...")
        : QStringLiteral("Building the engine modules..."));
}

void LauncherWindow::StopWarmup(const QString& InWhy)
{
    if(!Warmup->IsRunning()) { return; }
    StopReason = InWhy;
    Warmup->Cancel();
}

void LauncherWindow::OpenNewProject()
{
    NewProjectWizard Wizard(this);
    bool bPaused = false;
    connect(&Wizard, &NewProjectWizard::RunStarted, this, [this, &bPaused](const QStringList& InSteps)
    {
        if(Warmup->IsRunning() && (InSteps.contains(QStringLiteral("configure")) || InSteps.contains(QStringLiteral("build"))))
        {
            bPaused = true;
            StopWarmup(QStringLiteral("Paused while New Project builds the game; it resumes when New Project closes."));
        }
    });
    Wizard.exec();

    if(bPaused)
    {
        /* The cancelled step may not have reported yet; it resumes when it does. */
        bResumeWarmup = true;
        if(!Warmup->IsRunning()) { StartWarmup(); }
    }
}

void LauncherWindow::OnWarmupLine(const QString& InLine)
{
    WarmupLog->appendPlainText(InLine);

    /* Ninja's "[12/345] Building CXX object ...". */
    static const QRegularExpression Progress(QStringLiteral(R"(^\[(\d+)/(\d+)\])"));
    const QRegularExpressionMatch Match = Progress.match(InLine);
    if(Match.hasMatch())
    {
        const int Done  = Match.captured(1).toInt();
        const int Total = Match.captured(2).toInt();
        WarmupBar->setRange(0, Total);
        WarmupBar->setValue(Done);
        SetWarmupStatus(QStringLiteral("Building the engine modules: %1 of %2...").arg(Done).arg(Total));
    }
}

void LauncherWindow::OnWarmupFinished(const bool bInSucceeded, const QString& InStep)
{
    if(Warmup->WasCancelled())
    {
        WarmupBar->setVisible(false);
        WarmupButton->setText(QStringLiteral("Resume"));
        SetWarmupStatus(StopReason.isEmpty() ? QStringLiteral("Stopped.") : StopReason);
        if(bResumeWarmup) { StartWarmup(); }
        return;
    }
    if(!bInSucceeded)
    {
        WarmupBar->setVisible(false);
        WarmupButton->setText(QStringLiteral("Retry"));
        SetWarmupStatus(QStringLiteral("The engine %1 failed; see the log. New Project does not need it.").arg(InStep));
        LogButton->setChecked(true);
        return;
    }
    if(InStep == STEP_CONFIGURE)
    {
        StartWarmup();   // finds the tree configured: builds
        return;
    }

    WarmupBar->setVisible(false);
    WarmupButton->setText(QStringLiteral("Check Again"));
    SetWarmupStatus(QStringLiteral("The engine modules are built and up to date (%1).")
        .arg(QDir::toNativeSeparators(GetWarmupDir())));
}

void LauncherWindow::SetWarmupStatus(const QString& InText)
{
    WarmupStatus->setText(InText);
}

#endif // WITH_EDITOR
