#if WITH_EDITOR

#include "Private/Editor/Build/ProjectBuildDialog.h"

#include <string>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "Public/Core/CoreLog.h"
#include "Public/NimbleEngine/WorldList.h"

#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/ProjectSettingsDialog.h"
#include "Private/Editor/SourceBuild.h"

namespace
{
    std::string ToStd(const QString& InText)
    {
        return std::string(InText.toUtf8().constData());
    }
}

ProjectBuildDialog::ProjectBuildDialog(QWidget* InParent, std::function<void()> InOpenProjectSettings)
    : QDialog(InParent)
    , OpenProjectSettings(std::move(InOpenProjectSettings))
{
    setWindowTitle(QStringLiteral("Build Project"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(820, 680);

    bool bSource = false;
    ProjectSettingsDialog::GetProjectConfigPath(&bSource);
    ProjectRoot = bSource ? EditorGraphs::GetProjectRoot() : QString();

    auto* Layout = new QVBoxLayout(this);

    // --- Options -------------------------------------------------------------
    auto* OptionsBox = new QGroupBox(QStringLiteral("Options"), this);
    auto* Form = new QFormLayout(OptionsBox);

    Configuration = new QComboBox(OptionsBox);
    for(int Index = 0; Index < EditorProjectBuild::BC_Count; ++Index)
    {
        Configuration->addItem(QString::fromLatin1(EditorProjectBuild::GetConfigurationName(EditorProjectBuild::EConfiguration(Index))));
    }
    Configuration->setToolTip(QStringLiteral("Shipping reads only cooked content and has no editor-only code; Debug and "
                                             "RelWithDebInfo cook what they load on first use as well"));
    Form->addRow(QStringLiteral("Configuration"), Configuration);

    CookContent = new QCheckBox(QStringLiteral("Cook content (the ticked worlds, prefabs, UI, node graphs) into Saved/Cooked/"), OptionsBox);
    CompileGame = new QCheckBox(QStringLiteral("Compile the game (the Client preset, in its own build tree)"), OptionsBox);
    OpenOutput  = new QCheckBox(QStringLiteral("Open the executable's folder when it is built"), OptionsBox);
    Form->addRow(QString(), CookContent);
    Form->addRow(QString(), CompileGame);
    Form->addRow(QString(), OpenOutput);

    RunUncooked = new QCheckBox(QStringLiteral("Run uncooked: the game reads the project's raw files (Debug, RelWithDebInfo)"), OptionsBox);
    RunUncooked->setToolTip(QStringLiteral("The built game reads config, scripts, Content, worlds, prefabs and UI straight from "
                                           "the project folder -- edits show at its next start, with no cook or build. -cooked "
                                           "on its command line runs it on the packed content. Not in Shipping"));
    Form->addRow(QString(), RunUncooked);

    BuildTests = new QCheckBox(QStringLiteral("Build test executables (Debug, RelWithDebInfo)"), OptionsBox);
    BuildTests->setToolTip(QStringLiteral("Also compile the engine's test programs (UITests, NimbleEngine*Tests, the smoke "
                                          "tests, ...) into the output folder beside the game, to run them against this "
                                          "build. Shipping never builds tests"));
    Form->addRow(QString(), BuildTests);
    connect(RunUncooked, &QCheckBox::toggled, this, [this](const bool bInOn) { bWantsUncooked = bInOn; });
    connect(BuildTests,  &QCheckBox::toggled, this, [this](const bool bInOn) { bWantsTests = bInOn; });

    Target = new QLabel(OptionsBox);
    Target->setWordWrap(true);
    Target->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Form->addRow(QStringLiteral("Output"), Target);

    auto* WorldsRow = new QHBoxLayout();
    WorldList = new QLabel(OptionsBox);
    WorldList->setWordWrap(true);
    WorldsRow->addWidget(WorldList, 1);
    auto* EditWorlds = new QPushButton(QStringLiteral("Edit Worlds..."), OptionsBox);
    EditWorlds->setToolTip(QStringLiteral("Project Settings > Worlds: which worlds the project has, and which are cooked"));
    WorldsRow->addWidget(EditWorlds, 0, Qt::AlignTop);
    Form->addRow(QStringLiteral("Worlds"), WorldsRow);
    Layout->addWidget(OptionsBox);

    connect(EditWorlds, &QPushButton::clicked, this, [this]() { if(OpenProjectSettings) { OpenProjectSettings(); } });
    connect(Configuration, &QComboBox::currentIndexChanged, this, [this](int)
    {
        RefreshTarget();
        RefreshDevelopmentOptions(!Configuration->isEnabled());
    });

    // --- Stats and output --------------------------------------------------------
    Stats = new QTableWidget(0, 6, this);
    Stats->setHorizontalHeaderLabels({ QStringLiteral("Content"), QStringLiteral("Cooked"), QStringLiteral("Failed"),
                                       QStringLiteral("Source size"), QStringLiteral("Cooked size"), QStringLiteral("Time") });
    Stats->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    Stats->verticalHeader()->setVisible(false);
    Stats->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Stats->setSelectionMode(QAbstractItemView::NoSelection);
    Stats->setMaximumHeight(190);
    Stats->setVisible(false);
    Layout->addWidget(Stats);

    Output = new QPlainTextEdit(this);
    Output->setReadOnly(true);
    Output->setLineWrapMode(QPlainTextEdit::NoWrap);
    Output->setMaximumBlockCount(5000);
    QFont Mono(QStringLiteral("monospace"));
    Mono.setStyleHint(QFont::Monospace);
    Output->setFont(Mono);
    Layout->addWidget(Output, 1);

    Status = new QLabel(this);
    Status->setWordWrap(true);
    Layout->addWidget(Status);

    // --- Buttons -------------------------------------------------------------------
    auto* Buttons = new QDialogButtonBox(this);
    BuildButton  = Buttons->addButton(QStringLiteral("Build"), QDialogButtonBox::ActionRole);
    CookButton   = Buttons->addButton(QStringLiteral("Cook Only"), QDialogButtonBox::ActionRole);
    CancelButton = Buttons->addButton(QStringLiteral("Cancel Build"), QDialogButtonBox::ActionRole);
    QPushButton* Close = Buttons->addButton(QDialogButtonBox::Close);
    BuildButton->setDefault(true);
    BuildButton->setToolTip(QStringLiteral("Cook and compile as ticked above"));
    CookButton->setToolTip(QStringLiteral("Cook the content, without compiling"));
    Layout->addWidget(Buttons);

    connect(BuildButton, &QPushButton::clicked, this, [this]() { Run(CookContent->isChecked(), CompileGame->isChecked()); });
    connect(CookButton, &QPushButton::clicked, this, &ProjectBuildDialog::RunCookOnly);
    connect(Close, &QPushButton::clicked, this, &QDialog::close);

    Builder = new SourceBuild(this);
    connect(Builder, &SourceBuild::OutputLine, this, &ProjectBuildDialog::Log);
    connect(Builder, &SourceBuild::Finished, this, [this](const bool bInSucceeded, const QString&) { OnStepFinished(bInSucceeded); });
    connect(CancelButton, &QPushButton::clicked, Builder, &SourceBuild::Cancel);

    ReadOptions();
    RefreshWorlds();
    RefreshTarget();
    SetRunning(false);

    if(ProjectRoot.isEmpty())
    {
        Status->setText(QStringLiteral("<b>The editor does not know the project's folder</b> (only the config staged beside "
                                       "it), so it cannot cook or build it. Configure the game with the editor "
                                       "(NIMBLE_PROJECT_CONFIG)."));
        BuildButton->setEnabled(false);
        CookButton->setEnabled(false);
    }
}

ProjectBuildDialog::~ProjectBuildDialog()
{
    /* SourceBuild kills a build still running, without Finished. */
}

void ProjectBuildDialog::ReadOptions()
{
    const EditorProjectBuild::BuildOptions Options = EditorProjectBuild::LoadOptions();
    Configuration->setCurrentIndex(int(Options.Configuration));
    CookContent->setChecked(Options.bCook);
    CompileGame->setChecked(Options.bCompile);
    OpenOutput->setChecked(Options.bOpenOutput);
    bWantsUncooked = Options.bRunUncooked;
    bWantsTests    = Options.bBuildTests;
    RefreshDevelopmentOptions(false);
}

void ProjectBuildDialog::RefreshDevelopmentOptions(const bool bInRunning)
{
    /* Shipping shows both unticked and disabled; going back to Debug or
     * RelWithDebInfo restores what was chosen. */
    const bool bDevelopment = EditorProjectBuild::IsDevelopment(EditorProjectBuild::EConfiguration(Configuration->currentIndex()));
    const QSignalBlocker BlockUncooked(RunUncooked);
    const QSignalBlocker BlockTests(BuildTests);
    RunUncooked->setChecked(bDevelopment && bWantsUncooked);
    BuildTests->setChecked(bDevelopment && bWantsTests);
    RunUncooked->setEnabled(bDevelopment && !bInRunning);
    BuildTests->setEnabled(bDevelopment && !bInRunning);
}

EditorProjectBuild::BuildOptions ProjectBuildDialog::GatherOptions() const
{
    EditorProjectBuild::BuildOptions Options;
    Options.Configuration = EditorProjectBuild::EConfiguration(Configuration->currentIndex());
    Options.bCook         = CookContent->isChecked();
    Options.bCompile      = CompileGame->isChecked();
    Options.bOpenOutput   = OpenOutput->isChecked();
    /* What was chosen, kept for Debug and RelWithDebInfo even while Shipping is picked. */
    Options.bRunUncooked  = bWantsUncooked;
    Options.bBuildTests   = bWantsTests;
    return Options;
}

void ProjectBuildDialog::RefreshWorlds()
{
    ConfigIniFile File;
    File.Load(ProjectSettingsDialog::GetProjectConfigPath());
    const QString Prefix = QLatin1String(NimbleEngine::WORLD_SECTION_PREFIX);
    QStringList In, Out;
    for(const ConfigIniFile::Section& Section : File.GetSections(Prefix))
    {
        bool bCook = true;
        for(const QPair<QString, QString>& Key : Section.Keys)
        {
            if(Key.first.compare(QStringLiteral("Cook"), Qt::CaseInsensitive) == 0)
            {
                const QString V = Key.second.trimmed().toLower();
                bCook = !(V == QStringLiteral("false") || V == QStringLiteral("0") || V == QStringLiteral("no") || V == QStringLiteral("off"));
            }
        }
        (bCook ? In : Out) << Section.Name.mid(Prefix.size()).toHtmlEscaped();
    }
    QString Text = In.isEmpty() ? QStringLiteral("<b>No world is cooked</b> -- the game would have nothing to load.")
                                : QStringLiteral("Cooked: <b>%1</b>").arg(In.join(QStringLiteral(", ")));
    if(!Out.isEmpty())
    {
        Text += QStringLiteral("<br>Left out: %1").arg(Out.join(QStringLiteral(", ")));
    }
    WorldList->setText(Text);
}

void ProjectBuildDialog::RefreshTarget()
{
    if(ProjectRoot.isEmpty())
    {
        Target->setText(QStringLiteral("(unknown project folder)"));
        return;
    }
    const auto Config = EditorProjectBuild::EConfiguration(Configuration->currentIndex());
    QString Error;
    const QString Found = EditorProjectBuild::FindClientPreset(ProjectRoot, Config, &Error);
    Target->setText(Found.isEmpty()
        ? QStringLiteral("<span style='color:#d9534f'>%1</span>").arg(Error.toHtmlEscaped())
        : QStringLiteral("%1<br><small>preset %2</small>")
          .arg(QDir::toNativeSeparators(EditorProjectBuild::GetOutputDir(ProjectRoot, Config)).toHtmlEscaped(), Found));
}

void ProjectBuildDialog::SetRunning(const bool bInRunning)
{
    const bool bKnown = !ProjectRoot.isEmpty();
    BuildButton->setEnabled(!bInRunning && bKnown);
    CookButton->setEnabled(!bInRunning && bKnown);
    CancelButton->setEnabled(bInRunning && (Step == EStep::Configuring || Step == EStep::Compiling));
    Configuration->setEnabled(!bInRunning);
    CookContent->setEnabled(!bInRunning);
    CompileGame->setEnabled(!bInRunning);
    OpenOutput->setEnabled(!bInRunning);
    RefreshDevelopmentOptions(bInRunning);
}

void ProjectBuildDialog::Log(const QString& InLine)
{
    if(RunLog.isOpen())
    {
        RunLog.write((EditorCook::LogStamp() + InLine + QLatin1Char('\n')).toUtf8());
        RunLog.flush();
    }
    Output->appendPlainText(InLine);
    Output->verticalScrollBar()->setValue(Output->verticalScrollBar()->maximum());
}

void ProjectBuildDialog::RunCookOnly()
{
    Run(true, false);
}

void ProjectBuildDialog::Run(const bool bInCook, const bool bInCompile)
{
    if(Step != EStep::Idle || ProjectRoot.isEmpty())
    {
        return;
    }
    if(!bInCook && !bInCompile)
    {
        Status->setText(QStringLiteral("Nothing to do: tick Cook content or Compile the game."));
        return;
    }

    QString Error;
    if(!EditorProjectBuild::SaveOptions(GatherOptions(), &Error))
    {
        CORE_LOG_WARN("[Build] could not save the build options: " + ToStd(Error))
    }

    Output->clear();
    Stats->setVisible(false);
    CookSummary.clear();
    RefreshWorlds();

    /* This run's log. */
    RunLog.close();
    const QString Kind = bInCompile ? QStringLiteral("Build") : QStringLiteral("Cook");
    RunLog.setFileName(EditorCook::NewLogPath(ProjectRoot, Kind));
    if(RunLog.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        RunLog.write((EditorCook::LogHeader(Kind) + QLatin1Char('\n')).toUtf8());
    }
    else
    {
        CORE_LOG_WARN("[Build] could not write the run log " + ToStd(RunLog.fileName()))
    }
    Log(QStringLiteral("== %1 %2, %3%4").arg(bInCook ? QStringLiteral("cook") : QString(),
                                             bInCompile ? QStringLiteral("compile") : QString(),
                                             QString::fromLatin1(EditorProjectBuild::GetConfigurationName(
                                                 EditorProjectBuild::EConfiguration(Configuration->currentIndex()))),
                                             QStringLiteral(" -- ") + QDir::toNativeSeparators(ProjectRoot)));

    if(bInCompile)
    {
        Preset = EditorProjectBuild::FindClientPreset(ProjectRoot, EditorProjectBuild::EConfiguration(Configuration->currentIndex()), &Error);
        if(Preset.isEmpty())
        {
            Finish(false, Error);
            return;
        }
    }

    if(bInCook)
    {
        if(!Cook())
        {
            return;
        }
    }
    if(bInCompile)
    {
        StartCompile();
    }
    else
    {
        Finish(true, CookSummary);
    }
}

bool ProjectBuildDialog::Cook()
{
    Step = EStep::Cooking;
    SetRunning(true);
    Status->setText(QStringLiteral("Cooking..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    /* A Shipping build is packaged, its shader caches naming nothing: cook those too. */
    const bool bShipping = EditorProjectBuild::EConfiguration(Configuration->currentIndex()) == EditorProjectBuild::BC_Shipping;
    const EditorCook::CookStats Result = EditorCook::CookProject(ProjectRoot, [this](const QString& InText)
    {
        Log(InText);
        Status->setText(InText + QStringLiteral("..."));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }, bShipping);

    QApplication::restoreOverrideCursor();
    for(const QString& Line : EditorCook::DescribeStats(Result))
    {
        Log(Line);
    }
    for(const QString& Failure : Result.Failures)
    {
        Log(QStringLiteral("FAILED: ") + Failure);
    }
    ShowStats(Result);

    int Cooked = 0;
    for(const EditorCook::TypeStats& Type : Result.Types)
    {
        Cooked += Type.Cooked;
    }
    CookSummary = QStringLiteral("Cooked %1 file(s) in %2 s into %3.")
                  .arg(Cooked).arg(Result.Seconds, 0, 'f', 1)
                  .arg(QDir::toNativeSeparators(EditorCook::GetCookedRoot(ProjectRoot)));

    if(!Result.Succeeded())
    {
        Finish(false, QStringLiteral("The cook had %1 failure(s); nothing was compiled. The output above and the Console "
                                     "say what failed.").arg(Result.GetFailedCount()));
        return false;
    }
    if(Result.Worlds.isEmpty())
    {
        Finish(false, QStringLiteral("No world was cooked: tick at least one in Project Settings > Worlds."));
        return false;
    }
    return true;
}

void ProjectBuildDialog::ShowStats(const EditorCook::CookStats& InStats)
{
    Stats->setRowCount(0);
    const auto AddRow = [this](const QString& InName, const int InCooked, const int InFailed, const QString& InIn,
                               const QString& InOut, const double InSeconds, const bool bInBold)
    {
        const int Row = Stats->rowCount();
        Stats->insertRow(Row);
        const QStringList Cells = { InName, QString::number(InCooked), QString::number(InFailed), InIn, InOut,
                                    QStringLiteral("%1 s").arg(InSeconds, 0, 'f', 2) };
        for(int Column = 0; Column < Cells.size(); ++Column)
        {
            auto* Item = new QTableWidgetItem(Cells[Column]);
            if(Column > 0) { Item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter); }
            if(bInBold) { QFont Font = Item->font(); Font.setBold(true); Item->setFont(Font); }
            if(Column == 2 && InFailed > 0) { Item->setForeground(QColor(0xd9, 0x53, 0x4f)); }
            Stats->setItem(Row, Column, Item);
        }
    };

    int    Cooked = 0, Failed = 0;
    qint64 In = 0, Out = 0;
    for(int Type = 0; Type < EditorCook::AT_Count; ++Type)
    {
        const EditorCook::TypeStats& T = InStats.Types[Type];
        const bool bSized = EditorCook::IsSized(EditorCook::EAssetType(Type));
        AddRow(QString::fromLatin1(EditorCook::GetTypeName(EditorCook::EAssetType(Type))), T.Cooked, T.Failed,
               bSized ? EditorCook::FormatBytes(T.BytesIn) : QStringLiteral("-"),
               bSized ? EditorCook::FormatBytes(T.BytesOut) : QStringLiteral("-"), T.Seconds, false);
        Cooked += T.Cooked;
        Failed += T.Failed;
        In     += T.BytesIn;
        Out    += T.BytesOut;
    }
    AddRow(QStringLiteral("Total"), Cooked, Failed, EditorCook::FormatBytes(In), EditorCook::FormatBytes(Out), InStats.Seconds, true);
    Stats->setVisible(true);
}

void ProjectBuildDialog::StartCompile()
{
    Step = EStep::Configuring;
    SetRunning(true);
    Status->setText(QStringLiteral("Configuring %1...").arg(Preset));
    Log(QStringLiteral("== cmake --preset %1").arg(Preset));
    const EditorProjectBuild::BuildOptions Options = GatherOptions();
    if(EditorProjectBuild::IsDevelopment(Options.Configuration) && Options.bBuildTests)
    {
        Log(QStringLiteral("   with the engine's test executables (NIMBLE_BUILD_TESTS), into the output folder"));
    }
    if(!Builder->StartCommand(EditorProjectBuild::ConfigureArgs(Preset, Options), ProjectRoot, Preset))
    {
        Finish(false, QStringLiteral("cmake could not be started; see the Console."));
    }
}

void ProjectBuildDialog::OnStepFinished(const bool bInSucceeded)
{
    if(Step == EStep::Configuring)
    {
        if(!bInSucceeded)
        {
            Finish(false, QStringLiteral("Configuring %1 failed; the output above says why.").arg(Preset));
            return;
        }
        Step = EStep::Compiling;
        SetRunning(true);
        Status->setText(QStringLiteral("Compiling %1...").arg(Preset));
        Log(QStringLiteral("== cmake --build --preset %1").arg(Preset));
        if(!Builder->StartCommand(EditorProjectBuild::BuildArgs(Preset), ProjectRoot, Preset))
        {
            Finish(false, QStringLiteral("cmake could not be started; see the Console."));
        }
        return;
    }
    if(Step != EStep::Compiling)
    {
        return;
    }
    if(!bInSucceeded)
    {
        Finish(false, QStringLiteral("Compiling %1 failed or was cancelled; the output above says why.").arg(Preset));
        return;
    }

    const QString OutputDir = EditorProjectBuild::GetOutputDir(ProjectRoot, EditorProjectBuild::EConfiguration(Configuration->currentIndex()));
    QString Summary = (CookSummary.isEmpty() ? QString() : CookSummary + QLatin1Char(' '))
                    + QStringLiteral("Built %1 into %2.").arg(Preset, QDir::toNativeSeparators(OutputDir));
    if(OpenOutput->isChecked())
    {
        if(QFileInfo(OutputDir).isDir())
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(OutputDir));
        }
        else
        {
            Summary += QStringLiteral(" (That folder is not there to open: the project may put Binaries elsewhere.)");
        }
    }
    Finish(true, Summary);
}

void ProjectBuildDialog::Finish(const bool bInSucceeded, const QString& InSummary)
{
    Step = EStep::Idle;
    SetRunning(false);
    Status->setText(bInSucceeded ? QStringLiteral("<b>Done.</b> %1").arg(InSummary.toHtmlEscaped())
                                 : QStringLiteral("<b style='color:#d9534f'>Stopped.</b> %1").arg(InSummary.toHtmlEscaped()));
    Log(bInSucceeded ? QStringLiteral("== done: ") + InSummary : QStringLiteral("== stopped: ") + InSummary);
    if(bInSucceeded) { CORE_LOG("[Build] " + ToStd(InSummary)) }
    else             { CORE_LOG_ERROR("[Build] " + ToStd(InSummary)) }

    if(RunLog.isOpen())
    {
        RunLog.close();
        Status->setText(Status->text() + QStringLiteral("<br><small>Log: %1</small>")
                        .arg(QDir::toNativeSeparators(RunLog.fileName()).toHtmlEscaped()));
        CORE_LOG("[Build] log: " + ToStd(RunLog.fileName()))
    }
}

#endif // WITH_EDITOR
