#if WITH_EDITOR

#include "Private/Editor/Project/NewProjectWizard.h"

#include <functional>

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWizardPage>

#include "Public/Core/CoreLog.h"
#include "Private/Editor/Project/ProjectTemplate.h"

namespace
{
    /* Complete only when the name and location would make a project. */
    class ProjectPage : public QWizardPage
    {
    public:
        std::function<bool()> IsReady;
        bool isComplete() const override { return IsReady && IsReady(); }
        void Changed() { emit completeChanged(); }
    };
}

NewProjectWizard::NewProjectWizard(QWidget* InParent)
    : QWizard(InParent)
{
    setWindowTitle(QStringLiteral("New Project"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnLastPage);
    setOption(QWizard::NoCancelButtonOnLastPage);
    setButtonText(QWizard::NextButton, QStringLiteral("Create"));
    setPage(Page_Project, BuildProjectPage());
    setPage(Page_Created, BuildCreatedPage());
    resize(620, 440);
}

QWizardPage* NewProjectWizard::BuildProjectPage()
{
    auto* Page = new ProjectPage();
    Page->setTitle(QStringLiteral("New Project"));
    Page->setSubTitle(QStringLiteral("A new game with its own module, level, game mode and game state, "
                                     "and its Config, Scripts and Content folders -- as Games/Sandbox is laid out."));

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
        return !EditorProject::GetGameTemplateDir().isEmpty() && EditorProject::ValidateName(NameEdit->text()).isEmpty()
               && !Location.isEmpty() && !QFileInfo::exists(QDir(Location).filePath(NameEdit->text()));
    };
    const auto Changed = [this, Page]() { RefreshStatus(); Page->Changed(); };
    connect(NameEdit, &QLineEdit::textChanged, this, Changed);
    connect(LocationEdit, &QLineEdit::textChanged, this, Changed);
    RefreshStatus();
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

void NewProjectWizard::RefreshStatus()
{
    const QString Name     = NameEdit->text();
    const QString Location = LocationEdit->text().trimmed();
    QString Problem = EditorProject::GetGameTemplateDir().isEmpty()
                    ? QStringLiteral("The project template (Engine/Templates/Game) was not found")
                    : EditorProject::ValidateName(Name);
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

#endif // WITH_EDITOR
