#if WITH_EDITOR

#include "Private/Editor/Project/NewPluginDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include "Public/Core/CoreLog.h"
#include "Public/NimbleEngine/PluginCatalog.h"
#include "Private/Editor/Project/PluginTemplate.h"

namespace
{
    EditorPlugins::NewPluginOptions Options(const QString& InName, const QString& InStarter, const QString& InCategory,
                                            const QString& InDescription, const bool bInTest, const bool bInContent, const bool bInEditor)
    {
        EditorPlugins::NewPluginOptions Result;
        Result.Name          = InName;
        Result.Starter       = InStarter;
        Result.Category      = InCategory;
        Result.Description   = InDescription;
        Result.EngineVersion = QString::fromUtf8(NimbleEngine::PluginCatalog::GetEngineVersion());
        Result.bTest         = bInTest;
        Result.bContent      = bInContent;
        Result.bEditor       = bInEditor;
        return Result;
    }
}

NewPluginDialog::NewPluginDialog(const QStringList& InLocations, const QString& InProjectDir, const QStringList& InTaken, QWidget* InParent)
    : QDialog(InParent), TemplateDir(EditorPlugins::GetPluginTemplateDir()), Taken(InTaken)
{
    setWindowTitle(QStringLiteral("New Plugin"));
    resize(700, 720);

    auto* Column = new QVBoxLayout(this);
    auto* Form = new QFormLayout();
    Form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    Column->addLayout(Form);

    Name = new QLineEdit(this);
    Name->setPlaceholderText(QStringLiteral("Weather"));
    Name->setToolTip(QStringLiteral("Names the folder, the descriptor, the module, its classes, its script library and the "
                                    "Name:/ content prefix -- all from this one name"));
    Form->addRow(QStringLiteral("Name"), Name);
    NameCheck = new QLabel(this);
    NameCheck->setWordWrap(true);
    NameCheck->setMinimumWidth(480);
    Form->addRow(QString(), NameCheck);

    Location = new QComboBox(this);
    for(const QString& Each : InLocations)
    {
        const QString Label = QDir(Each) == QDir(InProjectDir) ? QStringLiteral("This project") :
                              Each.endsWith(QStringLiteral("/Engine/Plugins")) ? QStringLiteral("The engine (every project)")
                                                                               : QStringLiteral("Shared folder");
        Location->addItem(QStringLiteral("%1 -- %2").arg(Label, QDir::toNativeSeparators(Each)), Each);
    }
    Location->setToolTip(QStringLiteral("Where it is made: one of the folders the build looks for plugins in"));
    Form->addRow(QStringLiteral("Location"), Location);

    Starter = new QComboBox(this);
    for(const QString& Each : EditorPlugins::GetStarters(TemplateDir))
    {
        Starter->addItem(Each == QStringLiteral("RenderPass") ? QStringLiteral("Render Pass") : Each, Each);
    }
    Starter->setCurrentIndex(qMax(0, Starter->findData(QStringLiteral("Component"))));
    Form->addRow(QStringLiteral("Starter"), Starter);
    StarterNote = new QLabel(this);
    StarterNote->setWordWrap(true);
    StarterNote->setMinimumWidth(480);
    Form->addRow(QString(), StarterNote);

    Category = new QComboBox(this);
    Category->setEditable(true);
    Category->addItems({ QStringLiteral("Gameplay"), QStringLiteral("Rendering"), QStringLiteral("Audio"), QStringLiteral("AI"),
                         QStringLiteral("Physics"), QStringLiteral("UI"), QStringLiteral("Tools") });
    Category->setToolTip(QStringLiteral("The descriptor's Category, and the Add Component group its components appear under"));
    Form->addRow(QStringLiteral("Category"), Category);

    Description = new QLineEdit(this);
    Description->setPlaceholderText(QStringLiteral("One line: what it does"));
    Form->addRow(QStringLiteral("Description"), Description);

    Test = new QCheckBox(QStringLiteral("A test (Tests/, run by ctest -L plugin)"), this);
    Test->setChecked(true);
    Content = new QCheckBox(QStringLiteral("A Content folder (read as Name:/...)"), this);
    Editor = new QCheckBox(QStringLiteral("An Editor-only module (built only in Editor targets)"), this);
    auto* OptionsColumn = new QVBoxLayout();
    OptionsColumn->addWidget(Test);
    OptionsColumn->addWidget(Content);
    OptionsColumn->addWidget(Editor);
    Form->addRow(QStringLiteral("Add"), OptionsColumn);

    Column->addWidget(new QLabel(QStringLiteral("It will write:"), this));
    Preview = new QPlainTextEdit(this);
    Preview->setReadOnly(true);
    Preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    Column->addWidget(Preview, 1);

    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    CreateButton = Buttons->addButton(QStringLiteral("Create"), QDialogButtonBox::AcceptRole);
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(CreateButton, &QPushButton::clicked, this, &NewPluginDialog::Create);
    Column->addWidget(Buttons);

    connect(Name, &QLineEdit::textChanged, this, &NewPluginDialog::Update);
    connect(Location, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &NewPluginDialog::Update);
    connect(Starter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &NewPluginDialog::Update);
    connect(Category, &QComboBox::currentTextChanged, this, &NewPluginDialog::Update);
    connect(Description, &QLineEdit::textChanged, this, &NewPluginDialog::Update);
    for(QCheckBox* Each : { Test, Content, Editor }) { connect(Each, &QCheckBox::toggled, this, &NewPluginDialog::Update); }

    if(TemplateDir.isEmpty())
    {
        NameCheck->setText(QStringLiteral("<span style=\"color:#e06c5a\">Engine/Templates/Plugin is missing: this editor "
                                          "cannot find the engine it was built from.</span>"));
    }
    Update();
}

void NewPluginDialog::Prefill(const QString& InName, const QString& InStarter)
{
    if(!InStarter.isEmpty())
    {
        const int Index = Starter->findData(InStarter);
        if(Index >= 0) { Starter->setCurrentIndex(Index); }
    }
    if(!InName.isEmpty()) { Name->setText(InName); }
}

void NewPluginDialog::Update()
{
    const QString Chosen = Starter->currentData().toString();
    StarterNote->setText(QStringLiteral("<i>%1</i>").arg(EditorPlugins::DescribeStarter(Chosen).toHtmlEscaped()));

    QString Why = QString::fromStdString(NimbleEngine::PluginCatalog::ValidateName(Name->text().toStdString(),
                                                                                  [this]()
                                                                                  {
                                                                                      std::vector<std::string> List;
                                                                                      for(const QString& Each : Taken) { List.push_back(Each.toStdString()); }
                                                                                      return List;
                                                                                  }()));
    const QString Folder = QDir(Location->currentData().toString()).filePath(Name->text());
    if(Why.isEmpty() && QFileInfo::exists(Folder))
    {
        Why = QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(Folder));
    }
    if(Why.isEmpty()) { Why = EditorPlugins::ValidateCategory(Category->currentText()); }
    if(TemplateDir.isEmpty()) { Why = QStringLiteral("No template to make it from."); }

    if(!Why.isEmpty())
    {
        NameCheck->setText(QStringLiteral("<span style=\"color:#e06c5a\">%1</span>").arg(Why.toHtmlEscaped()));
        Preview->setPlainText(QString());
        CreateButton->setEnabled(false);
        return;
    }
    const QString N = Name->text();
    NameCheck->setText(QStringLiteral("<span style=\"color:#7cc47f\">&#10003;</span> folder <b>%1/</b> &middot; module <b>%1</b> "
                                      "&middot; exports <b>%2_API</b> &middot; content <b>%1:/...</b>")
                           .arg(N.toHtmlEscaped(), N.toUpper().toHtmlEscaped()));

    /* What it will write: the real thing, made where it does no harm. */
    QTemporaryDir Scratch;
    QString Dir, Error;
    QStringList Files;
    const bool bMade = Scratch.isValid() && EditorPlugins::CreatePlugin(TemplateDir, Scratch.path(),
        Options(N, Chosen, Category->currentText(), Description->text(), Test->isChecked(), Content->isChecked(), Editor->isChecked()),
        {}, Dir, Error, &Files);
    Files.sort();
    Preview->setPlainText(bMade ? QDir::toNativeSeparators(Folder) + QStringLiteral("/\n  ") + Files.join(QStringLiteral("\n  "))
                                : Error);
    CreateButton->setEnabled(bMade);
}

void NewPluginDialog::Create()
{
    const QString N = Name->text();
    QString Dir, Error;
    QStringList Files;
    if(!EditorPlugins::CreatePlugin(TemplateDir, Location->currentData().toString(),
                                    Options(N, Starter->currentData().toString(), Category->currentText(), Description->text(),
                                            Test->isChecked(), Content->isChecked(), Editor->isChecked()),
                                    Taken, Dir, Error, &Files))
    {
        QMessageBox::warning(this, windowTitle(), Error);
        return;
    }
    CORE_LOG("[Editor] new plugin " + N.toStdString() + " (" + Starter->currentData().toString().toStdString() + ") in "
             + Dir.toStdString() + ": " + std::to_string(Files.size()) + " files")
    CreatedName = N;
    CreatedDir  = Dir;
    accept();
}

#endif // WITH_EDITOR
