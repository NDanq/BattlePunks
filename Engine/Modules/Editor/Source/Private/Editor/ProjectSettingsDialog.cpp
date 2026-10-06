#if WITH_EDITOR

#include "Private/Editor/ProjectSettingsDialog.h"

#include <algorithm>
#include <climits>
#include <iterator>
#include <map>
#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"

#include "EditorProject.h"

namespace
{
    const char* const ProjectPageName = "Project";

    /* On the Project page, so not repeated on their section's. */
    struct ProjectKey
    {
        const char* Section;
        const char* Key;
        const char* Label;
    };
    const ProjectKey ProjectKeys[] = {
        { "Application", "Title",         "Title" },
        { "Game",        "Module",        "Game module" },
        { "Game",        "StartLevel",    "Start level" },
        { "Scripting",   "StartupScript", "Startup script" },
    };

    /* Ints that are really a choice. The same numbering as DefaultEngine.ini's
     * comments; a value the list lacks still shows, as Other. */
    struct IntChoice
    {
        const char* Text;
        int         Value;
    };
    struct ChoiceList
    {
        const char*      Section;
        const char*      Key;
        const IntChoice* Choices;
        int              Count;
    };
    const IntChoice RhiChoices[] = {
        { "Null (draws nothing)", 0 }, { "OpenGL (does not start yet)", 1 }, { "Vulkan", 2 },
        { "Metal (Apple only)", 3 },   { "D3D12", 4 },
    };
    const IntChoice ShadowResolutionChoices[] = { { "1024", 1024 }, { "2048", 2048 }, { "4096", 4096 } };
    const IntChoice ShadowFilterChoices[]     = { { "Hard", 0 }, { "PCF 3x3", 1 }, { "PCF 5x5", 2 } };
    const IntChoice ShadowLightChoices[]      = { { "None", 0 }, { "1", 1 }, { "2", 2 }, { "3", 3 }, { "4", 4 } };
    const ChoiceList ChoiceLists[] = {
        { "Display", "RHI",               RhiChoices,              int(std::size(RhiChoices)) },
        { "Shadows", "Resolution",        ShadowResolutionChoices, int(std::size(ShadowResolutionChoices)) },
        { "Shadows", "Filter",            ShadowFilterChoices,     int(std::size(ShadowFilterChoices)) },
        { "Shadows", "MaxShadowedLights", ShadowLightChoices,      int(std::size(ShadowLightChoices)) },
        { "Shadows", "MaxInsetShadows",   ShadowLightChoices,      int(std::size(ShadowLightChoices)) },
        { "Shadows", "InsetFilter",       ShadowFilterChoices,     int(std::size(ShadowFilterChoices)) },
    };

    /* A readable label and what a key does, where its name alone says too
     * little. Keys without one show as named. */
    struct KeyLabel
    {
        const char* Section;
        const char* Key;
        const char* Label;
        const char* Tip;
    };
    const KeyLabel KeyLabels[] = {
        { "Shadows", "Enabled",           "Draw shadows",
          "Off: every light draws unshadowed, whatever its Cast Shadows says" },
        { "Shadows", "Resolution",        "Shadow map size",
          "Each shadow atlas's side. The sun's cascades share one; point and spot lights share the other "
          "(a spot takes a sixth of its width, a point light six of those)" },
        { "Shadows", "Filter",            "Filter",
          "How soft a shadow's edge is: one depth test per pixel, or the average of 9 or 25" },
        { "Shadows", "MaxShadowedLights", "Shadowed point and spot lights",
          "How many point and spot lights may cast at once, nearest the camera first" },
        { "Shadows", "MaxInsetShadows",   "Inset shadows",
          "How many Render components with Cast Inset Shadow get a sun shadow map of their own, largest on screen first. "
          "They share the point and spot lights' atlas: each takes a tile the lights leave" },
        { "Shadows", "InsetFilter",       "Inset shadow filter",
          "How soft an inset shadow's edge is. Their maps are already sharp, so a smaller filter than Filter's "
          "usually looks the same and costs less" },
        { "Shadows", "DistanceScale",     "Sun shadow distance scale",
          "Multiplies every Directional Light's Shadow Distance: lower is sharper and shorter" },
    };

    bool Same(const QString& InA, const char* InB);

    const KeyLabel* FindKeyLabel(const QString& InSection, const QString& InKey)
    {
        for(const KeyLabel& Each : KeyLabels)
        {
            if(Same(InSection, Each.Section) && Same(InKey, Each.Key))
            {
                return &Each;
            }
        }
        return nullptr;
    }

    bool Same(const QString& InA, const char* InB)
    {
        return InA.compare(QLatin1String(InB), Qt::CaseInsensitive) == 0;
    }

    bool IsProjectKey(const QString& InSection, const QString& InKey)
    {
        for(const ProjectKey& P : ProjectKeys)
        {
            if(Same(InSection, P.Section) && Same(InKey, P.Key))
            {
                return true;
            }
        }
        return false;
    }

    const ChoiceList* FindChoices(const QString& InSection, const QString& InKey)
    {
        for(const ChoiceList& C : ChoiceLists)
        {
            if(Same(InSection, C.Section) && Same(InKey, C.Key))
            {
                return &C;
            }
        }
        return nullptr;
    }

    EngineCore::ConfigVar* FindVar(const QString& InSection, const QString& InKey)
    {
        return EngineCore::ConfigFindVar(InSection.toUtf8().constData(), InKey.toUtf8().constData());
    }

    QString StagedConfigPath()
    {
        /* main() made the executable's directory current; the game stages
         * Config/ there. */
        return QDir::current().absoluteFilePath(QStringLiteral("Config/DefaultEngine.ini"));
    }

    std::string ToStd(const QString& InText)
    {
        return std::string(InText.toUtf8().constData());
    }
}

QString ProjectSettingsDialog::GetProjectConfigPath(bool* OutbIsSource)
{
    const QString Source = QStringLiteral(NIMBLEEDITOR_PROJECT_CONFIG);
    const bool    bSource = !Source.isEmpty() && QFileInfo::exists(Source);
    if(OutbIsSource)
    {
        *OutbIsSource = bSource;
    }
    return bSource ? QFileInfo(Source).absoluteFilePath() : StagedConfigPath();
}

ProjectSettingsDialog::ProjectSettingsDialog(QWidget* InParent)
    : QDialog(InParent)
{
    setWindowTitle(QStringLiteral("Project Settings"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(760, 560);

    FilePath = GetProjectConfigPath(&bFileIsSource);
    if(!File.Load(FilePath))
    {
        CORE_LOG_ERROR("[Editor] project settings: could not read " + ToStd(FilePath) + ": " + ToStd(File.GetError()))
    }

    auto* Layout = new QVBoxLayout(this);
    auto* Body   = new QHBoxLayout();
    Layout->addLayout(Body, 1);

    Pages = new QListWidget(this);
    Pages->setFixedWidth(170);
    Body->addWidget(Pages);

    Stack = new QStackedWidget(this);
    Body->addWidget(Stack, 1);
    connect(Pages, &QListWidget::currentRowChanged, Stack, &QStackedWidget::setCurrentIndex);

    BuildProjectPage();
    BuildSectionPages();
    Pages->setCurrentRow(0);

    Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(Buttons, &QDialogButtonBox::accepted, this, [this]()
    {
        Apply();
        accept();
    });
    connect(Buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &ProjectSettingsDialog::Apply);
    Layout->addWidget(Buttons);
}

QWidget* ProjectSettingsDialog::AddPage(const QString& InName, QFormLayout*& OutForm)
{
    auto* Scroll = new QScrollArea(Stack);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);

    auto* Page = new QWidget(Scroll);
    auto* Column = new QVBoxLayout(Page);
    auto* Title = new QLabel(QStringLiteral("<b>%1</b>").arg(InName.toHtmlEscaped()), Page);
    Column->addWidget(Title);

    OutForm = new QFormLayout();
    OutForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    Column->addLayout(OutForm);
    Column->addStretch(1);

    Scroll->setWidget(Page);
    Stack->addWidget(Scroll);
    Pages->addItem(InName);
    return Page;
}

void ProjectSettingsDialog::BuildProjectPage()
{
    QFormLayout* Form = nullptr;
    QWidget*     Page = AddPage(QString::fromLatin1(ProjectPageName), Form);

    for(const ProjectKey& P : ProjectKeys)
    {
        AddRow(Form, QString::fromLatin1(P.Section), QString::fromLatin1(P.Key), QString::fromLatin1(P.Label));
    }

    /* Where things are, below the settings. */
    auto* Where = new QLabel(Page);
    Where->setWordWrap(true);
    Where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QString Text = QStringLiteral("<p>Saved to <b>%1</b></p>").arg(QDir::toNativeSeparators(FilePath).toHtmlEscaped());
    if(!bFileIsSource)
    {
        Text += QStringLiteral("<p><b>This is the copy staged beside the editor.</b> The editor does not know the "
                               "project's own config file, so the next build of the game will overwrite these "
                               "changes. Configure the game with the editor (Games/&lt;Game&gt;/CMakeLists.txt "
                               "sets NIMBLE_PROJECT_CONFIG on NimbleEditor) to edit its master copy.</p>");
    }
    Text += QStringLiteral("<p>Every other page is one config section. Bold settings are set in the file; the "
                           "rest use their compiled default. A game module's own settings have their proper "
                           "editors only while it is loaded (during Play); otherwise they show as text. "
                           "Most settings are read at startup or at Play, so take effect then.</p>"
                           "<p>[Editor] is not here: those are per user, under Edit &gt; Editor Settings.</p>");
    Where->setText(Text);
    static_cast<QVBoxLayout*>(Page->layout())->insertWidget(2, Where);
}

void ProjectSettingsDialog::BuildSectionPages()
{
    /* Section (as first spelled) -> keys, from the registry and the file. */
    std::map<QString, QPair<QString, QStringList>> Sections;   // lowercased -> (name, keys)
    const auto Note = [&Sections](const QString& InSection, const QString& InKey)
    {
        if(InSection.isEmpty() || Same(InSection, "Editor") || IsProjectKey(InSection, InKey))
        {
            return;
        }
        QPair<QString, QStringList>& Entry = Sections[InSection.toLower()];
        if(Entry.first.isEmpty())
        {
            Entry.first = InSection;
        }
        if(!Entry.second.contains(InKey, Qt::CaseInsensitive))
        {
            Entry.second.append(InKey);
        }
    };

    for(const EngineCore::ConfigVar* Var = EngineCore::ConfigGetFirst(); Var; Var = Var->GetNext())
    {
        Note(QString::fromUtf8(Var->GetSection()), QString::fromUtf8(Var->GetName()));
    }
    for(const QPair<QString, QString>& Key : File.GetKeys())
    {
        Note(Key.first, Key.second);
    }

    for(auto& [Lower, Entry] : Sections)
    {
        /* Labelled keys first, in KeyLabels' order; the rest by name. */
        QStringList Keys = Entry.second;
        const QString Section = Entry.first;
        const auto Rank = [&Section](const QString& InKey)
        {
            const KeyLabel* Label = FindKeyLabel(Section, InKey);
            return Label ? int(Label - KeyLabels) : int(std::size(KeyLabels));
        };
        std::sort(Keys.begin(), Keys.end(), [&Rank](const QString& InA, const QString& InB)
        {
            const int RankA = Rank(InA), RankB = Rank(InB);
            return RankA != RankB ? RankA < RankB : InA.compare(InB, Qt::CaseInsensitive) < 0;
        });

        QFormLayout* Form = nullptr;
        AddPage(Entry.first, Form);
        for(const QString& Key : Keys)
        {
            const KeyLabel* Label = FindKeyLabel(Entry.first, Key);
            AddRow(Form, Entry.first, Key, Label ? QString::fromLatin1(Label->Label) : Key);
        }
    }
}

void ProjectSettingsDialog::AddRow(QFormLayout* InForm, const QString& InSection, const QString& InKey,
                                   const QString& InLabel)
{
    Row R;
    R.Section = InSection;
    R.Key     = InKey;

    EngineCore::ConfigVar* Var = FindVar(InSection, InKey);
    const QString FileValue = File.Get(InSection, InKey, &R.bInFile);
    const QString Default   = Var ? QString::fromStdString(Var->DefaultToString()) : QString();

    QWidget* Field = nullptr;
    if(const ChoiceList* Choices = Var ? FindChoices(InSection, InKey) : nullptr)
    {
        auto* Combo = new QComboBox(this);
        for(int Index = 0; Index < Choices->Count; ++Index)
        {
            Combo->addItem(QString::fromLatin1(Choices->Choices[Index].Text), Choices->Choices[Index].Value);
        }
        R.Show = [Combo](const QString& InText)
        {
            const int Value = InText.toInt();
            int Index = Combo->findData(Value);
            if(Index < 0)
            {
                Combo->addItem(QStringLiteral("Other (%1)").arg(Value), Value);
                Index = Combo->count() - 1;
            }
            Combo->setCurrentIndex(Index);
        };
        R.Read = [Combo]() { return QString::number(Combo->currentData().toInt()); };
        Field = Combo;
    }
    else if(Var && Var->GetType() == EngineCore::EConfigType::CT_Bool)
    {
        auto* Check = new QCheckBox(this);
        R.Show = [Check](const QString& InText)
        {
            const QString T = InText.trimmed().toLower();
            Check->setChecked(T == QStringLiteral("true") || T == QStringLiteral("1") || T == QStringLiteral("yes")
                              || T == QStringLiteral("on"));
        };
        R.Read = [Check]() { return Check->isChecked() ? QStringLiteral("true") : QStringLiteral("false"); };
        Field = Check;
    }
    else if(Var && Var->GetType() == EngineCore::EConfigType::CT_Int)
    {
        auto* Spin = new QSpinBox(this);
        Spin->setRange(INT_MIN, INT_MAX);
        R.Show = [Spin](const QString& InText) { Spin->setValue(InText.toInt()); };
        R.Read = [Spin]() { return QString::number(Spin->value()); };
        Field = Spin;
    }
    else if(Var && Var->GetType() == EngineCore::EConfigType::CT_Float)
    {
        auto* Spin = new QDoubleSpinBox(this);
        Spin->setRange(-1.0e9, 1.0e9);
        Spin->setDecimals(4);
        Spin->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
        R.Show = [Spin](const QString& InText) { Spin->setValue(InText.toDouble()); };
        R.Read = [Spin]() { return QString::number(Spin->value(), 'g', 7); };
        Field = Spin;
    }
    else
    {
        /* Strings, and keys nothing loaded declares. */
        auto* Edit = new QLineEdit(this);
        if(Var)
        {
            Edit->setMaxLength(int(EngineCore::CONFIG_STRING_MAX) - 1);
        }
        R.Show = [Edit](const QString& InText) { Edit->setText(InText); };
        R.Read = [Edit]() { return Edit->text(); };
        Field = Edit;
    }

    /* What the project has now, read back through the widget, so a value it
     * spells differently ("1" for true) only counts as changed if edited. */
    R.Show(R.bInFile ? FileValue : Default);
    R.Baseline = R.Read();

    auto* Holder = new QWidget(this);
    auto* Line   = new QHBoxLayout(Holder);
    Line->setContentsMargins(0, 0, 0, 0);
    Line->addWidget(Field, 1);
    if(Var)
    {
        auto* Reset = new QToolButton(Holder);
        Reset->setText(QStringLiteral("↺"));
        Reset->setAutoRaise(true);
        Reset->setToolTip(QStringLiteral("Put the compiled default (%1) in the field; Apply writes it").arg(Default));
        const std::function<void(const QString&)> Show = R.Show;
        connect(Reset, &QToolButton::clicked, this, [Show, Default]() { Show(Default); });
        Line->addWidget(Reset);
    }
    else
    {
        Line->addSpacing(28);
    }

    QString Tip = InSection + QLatin1Char('.') + InKey;
    if(const KeyLabel* Label = FindKeyLabel(InSection, InKey))
    {
        Tip = QString::fromLatin1(Label->Tip) + QStringLiteral("\n\n") + Tip;
    }
    if(Var)
    {
        Tip += QStringLiteral("\nDefault: %1").arg(Default.isEmpty() ? QStringLiteral("(empty)") : Default);
    }
    else
    {
        Tip += QStringLiteral("\nNothing loaded declares this key -- a game module's setting, typed only while "
                              "the module is loaded (during Play). Edited as text.");
    }
    Field->setToolTip(Tip);

    R.Label = new QLabel(InLabel, this);
    R.Label->setToolTip(Tip);
    if(!Var)
    {
        QFont Italic = R.Label->font();
        Italic.setItalic(true);
        R.Label->setFont(Italic);
    }
    UpdateLabel(R);

    InForm->addRow(R.Label, Holder);
    Rows.push_back(std::move(R));
}

void ProjectSettingsDialog::UpdateLabel(const Row& InRow)
{
    QFont Font = InRow.Label->font();
    Font.setBold(InRow.bInFile);
    InRow.Label->setFont(Font);
}

void ProjectSettingsDialog::Apply()
{
    QStringList Changed;
    QStringList Rejected;
    for(Row& R : Rows)
    {
        const QString Text = R.Read();
        if(Text == R.Baseline)
        {
            continue;
        }
        const QString Path = R.Section + QLatin1Char('.') + R.Key;
        if(EngineCore::ConfigVar* Var = FindVar(R.Section, R.Key))
        {
            /* The same parse the file gets at load: refuse what Core would. */
            if(!Var->SetFromString(ToStd(Text)))
            {
                Rejected << QStringLiteral("%1 = %2").arg(Path, Text);
                continue;
            }
        }
        File.Set(R.Section, R.Key, Text);
        R.Baseline = Text;
        R.bInFile  = true;
        UpdateLabel(R);
        Changed << QStringLiteral("%1 = %2").arg(Path, Text);
    }

    if(!Rejected.isEmpty())
    {
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("Not valid values, so not saved:\n%1").arg(Rejected.join(QLatin1Char('\n'))));
    }
    if(Changed.isEmpty())
    {
        return;
    }

    if(!File.Save(FilePath))
    {
        CORE_LOG_ERROR("[Editor] project settings: could not write " + ToStd(FilePath) + ": " + ToStd(File.GetError()))
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("The settings apply to this session, but could not be saved to\n%1\n\n%2")
                             .arg(QDir::toNativeSeparators(FilePath), File.GetError()));
        return;
    }
    for(const QString& Line : Changed)
    {
        CORE_LOG("[Editor] project settings: " + ToStd(Line))
    }
    CORE_LOG("[Editor] project settings saved to " + ToStd(FilePath))

    /* The staged copy too, so the next Play and the next launch see the
     * change before any build copies it across. */
    const QString Staged = StagedConfigPath();
    if(bFileIsSource && QFileInfo(Staged).absoluteFilePath() != QFileInfo(FilePath).absoluteFilePath())
    {
        QDir().mkpath(QFileInfo(Staged).absolutePath());
        if(!File.Save(Staged))
        {
            CORE_LOG_WARN("[Editor] project settings: could not refresh the staged copy " + ToStd(Staged)
                          + "; the next build will")
        }
    }
}

#endif // WITH_EDITOR
