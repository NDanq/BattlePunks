#if WITH_EDITOR

#include "Private/Editor/ProjectSettingsDialog.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <iterator>
#include <map>
#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/NimbleEngine/WorldList.h"

#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"

#include "EditorProject.h"

namespace
{
    const char* const ProjectPageName = "Project";
    const char* const WorldsPageName  = "Worlds";

    /* The Worlds table's columns. */
    enum EWorldColumn { WC_Name = 0, WC_File, WC_Mode, WC_Script, WC_Cook, WC_Count };

    /* The startup script choice that is not a path. */
    const char* const NoScriptText      = "(none)";
    const char* const NoModeText        = "(none)";

    /* A level name a save holds (63 characters), and a section name. */
    constexpr int MaxWorldName = 63;

    /* The project's Lua files (runtime path, disk path): a world's startup
     * script choices. */
    QList<QPair<QString, QString>> EditorProjectScripts()
    {
        QList<QPair<QString, QString>> Out;
        const QString Root = EditorGraphs::GetProjectRoot();
        QDirIterator It(Root, { QStringLiteral("*.lua") }, QDir::Files, QDirIterator::Subdirectories);
        while(It.hasNext())
        {
            const QString File    = It.next();
            const QString Runtime = EditorContent::ToRuntimePath(File);
            bool bIgnored = Runtime.isEmpty() || Runtime.startsWith(QStringLiteral("Engine/")) || Runtime.startsWith(QStringLiteral("Cooked/"));
            for(const QString& Part : QDir(Root).relativeFilePath(File).split(QLatin1Char('/')))
            {
                bIgnored |= EditorGraphs::IsIgnoredFolder(Part);
            }
            if(!bIgnored)
            {
                Out.append({ Runtime, File });
            }
        }
        std::sort(Out.begin(), Out.end());
        return Out;
    }

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
        { "Game",        "StartLevel",    "Start world" },
    };

    /* Also on the Project page, under their own heading: build-time switches
     * (Engine/cmake/NimbleProject.cmake reads them as the game configures). */
    const ProjectKey ModuleKeys[] = {
        { "Modules", "NetCore",     "Networking (NetCore)" },
        { "Modules", "JoltPhysics", "Physics (JoltPhysics)" },
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
        { "Game",    "StartLevel",        "Start world",
          "The world the game starts in: a name from the Worlds page. Empty starts the first listed" },
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
        { "Modules", "NetCore",           "Networking (NetCore)",
          "Build the game with networking: replication, RPCs, its dedicated server (<Game>Server) and -connect. "
          "Off, Net components load but do not replicate. Takes effect at the game's next build" },
        { "Modules", "JoltPhysics",       "Physics (JoltPhysics)",
          "Build the game with rigid-body physics on Jolt: Rigid Body and Physics Constraint components simulate. "
          "Off, they load but do not move. Takes effect at the game's next build" },
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

    bool IsModuleKey(const QString& InSection, const QString& InKey)
    {
        for(const ProjectKey& P : ModuleKeys)
        {
            if(Same(InSection, P.Section) && Same(InKey, P.Key))
            {
                return true;
            }
        }
        return false;
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
        return IsModuleKey(InSection, InKey);
    }

    QString OnOff(const bool bInOn) { return bInOn ? QStringLiteral("on") : QStringLiteral("off"); }

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
    BuildWorldsPage();
    BuildSectionPages();
    Pages->setCurrentRow(0);

    Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(Buttons, &QDialogButtonBox::accepted, this, [this]()
    {
        /* Stays open if a module change was not confirmed. */
        if(Apply())
        {
            accept();
        }
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

    /* The optional engine modules: [Modules], read by CMake, so a build
     * applies them -- this editor has what it was built with. */
    auto* ModulesTitle = new QLabel(QStringLiteral("<br><b>Modules</b>"), Page);
    Form->addRow(ModulesTitle);
    auto* ModulesNote = new QLabel(QStringLiteral(
        "Optional engine modules the game builds with. A change applies at the game's next build, which "
        "re-configures from this file: close the editor, build, and start it again. This editor was built "
        "with networking %1 and physics %2.").arg(OnOff(WITH_NETCORE != 0), OnOff(WITH_JOLTPHYSICS != 0)), Page);
    ModulesNote->setWordWrap(true);
    Form->addRow(ModulesNote);
    for(const ProjectKey& P : ModuleKeys)
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

void ProjectSettingsDialog::ShowWorlds()
{
    for(int Index = 0; Index < Pages->count(); ++Index)
    {
        if(Pages->item(Index)->text() == QLatin1String(WorldsPageName))
        {
            Pages->setCurrentRow(Index);
            return;
        }
    }
}

void ProjectSettingsDialog::BuildWorldsPage()
{
    QFormLayout* Form = nullptr;
    QWidget*     Page = AddPage(QString::fromLatin1(WorldsPageName), Form);
    auto*        Column = static_cast<QVBoxLayout*>(Page->layout());

    auto* Note = new QLabel(QStringLiteral(
        "<p>The project's worlds. Each is a level, named here: what <b>Start world</b>, saves, the console's "
        "<tt>open</tt> and <tt>Game.OpenLevel</tt> name. It loads its world file, then runs its <b>game mode</b> "
        "(rules the game module registers) and its own <b>startup script</b>, if it names one. Play plays the open world if it is listed.</p>"
        "<p><b>Cook</b>: Build &gt; Build Project cooks and packages it; unticked, it stays out of the game.</p>"), Page);
    Note->setWordWrap(true);
    Column->insertWidget(1, Note);

    Worlds = new QTableWidget(0, WC_Count, Page);
    Worlds->setHorizontalHeaderLabels({ QStringLiteral("Name"), QStringLiteral("World file"), QStringLiteral("Game mode"),
                                        QStringLiteral("Startup script"), QStringLiteral("Cook") });
    Worlds->horizontalHeader()->setSectionResizeMode(WC_Name, QHeaderView::ResizeToContents);
    Worlds->horizontalHeader()->setSectionResizeMode(WC_File, QHeaderView::Stretch);
    Worlds->horizontalHeader()->setSectionResizeMode(WC_Mode, QHeaderView::ResizeToContents);
    Worlds->horizontalHeader()->setSectionResizeMode(WC_Script, QHeaderView::ResizeToContents);
    Worlds->horizontalHeader()->setSectionResizeMode(WC_Cook, QHeaderView::ResizeToContents);
    Worlds->verticalHeader()->setVisible(false);
    Worlds->setSelectionBehavior(QAbstractItemView::SelectRows);
    Worlds->setSelectionMode(QAbstractItemView::SingleSelection);
    Worlds->setMinimumHeight(220);
    Column->insertWidget(2, Worlds, 1);

    auto* Buttons = new QHBoxLayout();
    const auto AddButton = [this, Buttons](const QString& InText, const QString& InTip)
    {
        auto* Button = new QPushButton(InText, this);
        Button->setToolTip(InTip);
        Buttons->addWidget(Button);
        return Button;
    };
    QPushButton* Add      = AddButton(QStringLiteral("Add World..."), QStringLiteral("Add a .nmworld from the project's Content"));
    QPushButton* AddOpen  = AddButton(QStringLiteral("Add Open World"), QStringLiteral("Add the world open in the editor"));
    QPushButton* Browse   = AddButton(QStringLiteral("Change File..."), QStringLiteral("Point the selected world at another file"));
    QPushButton* Remove   = AddButton(QStringLiteral("Remove"), QStringLiteral("Take the selected world out of the project (its file stays)"));
    QPushButton* Up       = AddButton(QStringLiteral("Up"), QStringLiteral("Move the selected world up"));
    QPushButton* Down     = AddButton(QStringLiteral("Down"), QStringLiteral("Move the selected world down"));
    Buttons->addStretch(1);
    Column->insertLayout(3, Buttons);

    const auto PickWorld = [this]() -> QString
    {
        const QString Root = EditorGraphs::GetProjectRoot();
        const QString Start = QDir(Root).exists(QStringLiteral("Content/Levels")) ? QDir(Root).filePath(QStringLiteral("Content/Levels")) : Root;
        const QString Picked = QFileDialog::getOpenFileName(this, QStringLiteral("World"), Start, QStringLiteral("Worlds (*.nmworld)"));
        if(Picked.isEmpty())
        {
            return QString();
        }
        const QString Runtime = EditorContent::ToRuntimePath(Picked);
        if(Runtime.isEmpty() || Runtime.startsWith(QStringLiteral("Engine/")) || Runtime.contains(QStringLiteral("/Cooked/"))
           || Runtime.startsWith(QStringLiteral("Cooked/")))
        {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("%1\nis not one of the project's own worlds (under %2, not cooked).")
                                 .arg(QDir::toNativeSeparators(Picked), QDir::toNativeSeparators(Root)));
            return QString();
        }
        return Runtime;
    };

    connect(Add, &QPushButton::clicked, this, [this, PickWorld]()
    {
        const QString Runtime = PickWorld();
        if(!Runtime.isEmpty()) { AddWorld(Runtime); }
    });
    connect(AddOpen, &QPushButton::clicked, this, [this]()
    {
        if(OpenWorld.isEmpty())
        {
            QMessageBox::information(this, windowTitle(), QStringLiteral("No saved world is open in the editor."));
            return;
        }
        AddWorld(OpenWorld);
    });
    connect(Browse, &QPushButton::clicked, this, [this, PickWorld]()
    {
        const int Row = Worlds->currentRow();
        if(Row < 0) { return; }
        const QString Runtime = PickWorld();
        if(!Runtime.isEmpty()) { Worlds->item(Row, WC_File)->setText(Runtime); }
    });
    connect(Remove, &QPushButton::clicked, this, [this]()
    {
        const int Row = Worlds->currentRow();
        if(Row >= 0)
        {
            Worlds->removeRow(Row);
            RefreshStartLevelChoices();
        }
    });
    const auto Move = [this](const int InBy)
    {
        const int Row = Worlds->currentRow();
        const int To  = Row + InBy;
        if(Row < 0 || To < 0 || To >= Worlds->rowCount())
        {
            return;
        }
        /* Rebuilt from the sections: cell widgets do not move with takeItem. */
        QList<ConfigIniFile::Section> Sections = ReadWorlds();
        Sections.swapItemsAt(Row, To);
        Worlds->setRowCount(0);
        for(const ConfigIniFile::Section& Each : Sections) { AddWorldRow(Each); }
        Worlds->selectRow(To);
        RefreshStartLevelChoices();
    };
    connect(Up,   &QPushButton::clicked, this, [Move]() { Move(-1); });
    connect(Down, &QPushButton::clicked, this, [Move]() { Move(+1); });

    for(const ConfigIniFile::Section& Each : File.GetSections(QLatin1String(NimbleEngine::WORLD_SECTION_PREFIX)))
    {
        AddWorldRow(Each);
    }
    WorldsBaseline = ReadWorlds();
    connect(Worlds, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* InItem)
    {
        if(InItem && InItem->column() == WC_Name) { RefreshStartLevelChoices(); }
    });
    RefreshStartLevelChoices();
}

void ProjectSettingsDialog::AddWorldRow(const ConfigIniFile::Section& InSection)
{
    QString File, Mode, Script;
    bool    bCook = true;
    for(const QPair<QString, QString>& Key : InSection.Keys)
    {
        if(Same(Key.first, "File"))               { File = Key.second; }
        else if(Same(Key.first, "GameMode"))      { Mode = Key.second; }
        else if(Same(Key.first, "StartupScript")) { Script = Key.second; }
        else if(Same(Key.first, "Cook"))
        {
            const QString V = Key.second.trimmed().toLower();
            bCook = !(V == QStringLiteral("false") || V == QStringLiteral("0") || V == QStringLiteral("no") || V == QStringLiteral("off"));
        }
    }

    const QSignalBlocker Quiet(Worlds);
    const int Row = Worlds->rowCount();
    Worlds->insertRow(Row);

    const QString Name = InSection.Name.mid(int(std::strlen(NimbleEngine::WORLD_SECTION_PREFIX)));
    Worlds->setItem(Row, WC_Name, new QTableWidgetItem(Name));
    Worlds->setItem(Row, WC_File, new QTableWidgetItem(File));

    /* Modes the game module registers -- loaded for authoring, so known here. */
    auto* ModeChoice = new QComboBox(Worlds);
    ModeChoice->setEditable(true);
    ModeChoice->addItem(QString::fromLatin1(NoModeText));
    for(size_t Index = 0; Index < NimbleEngine::GetGameModeCount(); ++Index)
    {
        ModeChoice->addItem(QString::fromUtf8(NimbleEngine::GetGameModeName(Index)));
    }
    ModeChoice->setCurrentText(Mode.isEmpty() ? QString::fromLatin1(NoModeText) : Mode);
    ModeChoice->setToolTip(QStringLiteral("The rules: a game mode the game module registers (RegisterGameMode). "
                                          "Listed: those the loaded module has"));
    Worlds->setCellWidget(Row, WC_Mode, ModeChoice);

    auto* ScriptChoice = new QComboBox(Worlds);
    ScriptChoice->setEditable(true);
    ScriptChoice->addItem(QString::fromLatin1(NoScriptText));
    for(const QPair<QString, QString>& Lua : EditorProjectScripts())
    {
        ScriptChoice->addItem(Lua.first);
    }
    ScriptChoice->setCurrentText(Script.isEmpty() ? QString::fromLatin1(NoScriptText) : Script);
    ScriptChoice->setToolTip(QStringLiteral("The Lua file run once the world and its mode are loaded (bundle-relative, "
                                            "\"Scripts/Main.lua\"), or none"));
    Worlds->setCellWidget(Row, WC_Script, ScriptChoice);

    auto* Cook = new QTableWidgetItem();
    Cook->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    Cook->setCheckState(bCook ? Qt::Checked : Qt::Unchecked);
    Cook->setToolTip(QStringLiteral("Cooked and packaged by Build > Build Project"));
    Worlds->setItem(Row, WC_Cook, Cook);
}

void ProjectSettingsDialog::AddWorld(const QString& InRuntimePath)
{
    for(const ConfigIniFile::Section& Each : ReadWorlds())
    {
        for(const QPair<QString, QString>& Key : Each.Keys)
        {
            if(Same(Key.first, "File") && Key.second.compare(InRuntimePath, Qt::CaseInsensitive) == 0)
            {
                QMessageBox::information(this, windowTitle(), QStringLiteral("%1 is already listed.").arg(InRuntimePath));
                return;
            }
        }
    }

    /* Named for its file, as a section name allows; unique. */
    QString Base = QFileInfo(InRuntimePath).completeBaseName();
    Base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
    Base = Base.left(MaxWorldName - 3);
    if(Base.isEmpty()) { Base = QStringLiteral("World"); }
    QString Name = Base;
    const QList<ConfigIniFile::Section> Existing = ReadWorlds();
    const auto Taken = [&Existing](const QString& InName)
    {
        for(const ConfigIniFile::Section& Each : Existing)
        {
            if(Each.Name.compare(QLatin1String(NimbleEngine::WORLD_SECTION_PREFIX) + InName, Qt::CaseInsensitive) == 0) { return true; }
        }
        return false;
    };
    for(int Suffix = 2; Taken(Name); ++Suffix)
    {
        Name = Base + QString::number(Suffix);
    }

    ConfigIniFile::Section Section;
    Section.Name = QLatin1String(NimbleEngine::WORLD_SECTION_PREFIX) + Name;
    Section.Keys = { { QStringLiteral("File"), InRuntimePath }, { QStringLiteral("Cook"), QStringLiteral("true") } };
    AddWorldRow(Section);
    Worlds->selectRow(Worlds->rowCount() - 1);
    RefreshStartLevelChoices();
}

QList<ConfigIniFile::Section> ProjectSettingsDialog::ReadWorlds() const
{
    QList<ConfigIniFile::Section> Out;
    for(int Row = 0; Row < Worlds->rowCount(); ++Row)
    {
        ConfigIniFile::Section Section;
        Section.Name = QLatin1String(NimbleEngine::WORLD_SECTION_PREFIX) + Worlds->item(Row, WC_Name)->text().trimmed();
        Section.Keys.append({ QStringLiteral("File"), Worlds->item(Row, WC_File)->text().trimmed() });

        const QString Mode = static_cast<QComboBox*>(Worlds->cellWidget(Row, WC_Mode))->currentText().trimmed();
        if(!Mode.isEmpty() && Mode != QLatin1String(NoModeText))
        {
            Section.Keys.append({ QStringLiteral("GameMode"), Mode });
        }

        const QString Script = static_cast<QComboBox*>(Worlds->cellWidget(Row, WC_Script))->currentText().trimmed();
        if(!Script.isEmpty() && Script != QLatin1String(NoScriptText))
        {
            Section.Keys.append({ QStringLiteral("StartupScript"), Script });
        }

        Section.Keys.append({ QStringLiteral("Cook"), Worlds->item(Row, WC_Cook)->checkState() == Qt::Checked
                                                     ? QStringLiteral("true") : QStringLiteral("false") });
        Out.append(Section);
    }
    return Out;
}

QString ProjectSettingsDialog::ValidateWorlds() const
{
    static const QRegularExpression NameRule(QStringLiteral("^[A-Za-z0-9_]+$"));
    const QString Root = EditorGraphs::GetProjectRoot();
    QStringList Problems;
    QStringList Seen;
    for(int Row = 0; Row < Worlds->rowCount(); ++Row)
    {
        const QString Name = Worlds->item(Row, WC_Name)->text().trimmed();
        const QString File = Worlds->item(Row, WC_File)->text().trimmed();
        const QString Which = QStringLiteral("Row %1").arg(Row + 1);
        if(!NameRule.match(Name).hasMatch())
        {
            Problems << QStringLiteral("%1: the name '%2' must be letters, digits and _").arg(Which, Name);
        }
        else if(Name.size() > MaxWorldName)
        {
            Problems << QStringLiteral("%1: '%2' is longer than %3 characters (what a save holds)").arg(Which, Name).arg(MaxWorldName);
        }
        else if(Seen.contains(Name, Qt::CaseInsensitive))
        {
            Problems << QStringLiteral("%1: '%2' is listed twice").arg(Which, Name);
        }
        Seen << Name;

        if(File.isEmpty())
        {
            Problems << QStringLiteral("%1 (%2): no world file").arg(Which, Name);
        }
        else if(!QFileInfo::exists(QDir(Root).filePath(File)))
        {
            Problems << QStringLiteral("%1 (%2): %3 does not exist in the project").arg(Which, Name, File);
        }
    }
    return Problems.join(QLatin1Char('\n'));
}

void ProjectSettingsDialog::RefreshStartLevelChoices()
{
    if(!StartLevelChoice || !Worlds)
    {
        return;
    }
    const QString Current = StartLevelChoice->currentText();
    const QSignalBlocker Quiet(StartLevelChoice);
    StartLevelChoice->clear();
    for(int Row = 0; Row < Worlds->rowCount(); ++Row)
    {
        StartLevelChoice->addItem(Worlds->item(Row, WC_Name)->text().trimmed());
    }
    StartLevelChoice->setCurrentText(Current);
}

void ProjectSettingsDialog::BuildSectionPages()
{
    /* Section (as first spelled) -> keys, from the registry and the file. */
    std::map<QString, QPair<QString, QStringList>> Sections;   // lowercased -> (name, keys)
    const auto Note = [&Sections](const QString& InSection, const QString& InKey)
    {
        if(InSection.isEmpty() || Same(InSection, "Editor") || Same(InSection, "Build") || IsProjectKey(InSection, InKey)
           || InSection.startsWith(QLatin1String(NimbleEngine::WORLD_SECTION_PREFIX), Qt::CaseInsensitive))
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
    if(Same(InSection, "Game") && Same(InKey, "StartLevel"))
    {
        /* One of the Worlds page's names (RefreshStartLevelChoices); empty: the first. */
        auto* Combo = new QComboBox(this);
        Combo->setEditable(true);
        Combo->setInsertPolicy(QComboBox::NoInsert);
        StartLevelChoice = Combo;
        R.Show = [Combo](const QString& InText) { Combo->setCurrentText(InText); };
        R.Read = [Combo]() { return Combo->currentText().trimmed(); };
        Field = Combo;
    }
    else if(const ChoiceList* Choices = Var ? FindChoices(InSection, InKey) : nullptr)
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

bool ProjectSettingsDialog::ConfirmModuleChanges()
{
    QStringList Edits;
    for(const Row& R : Rows)
    {
        const QString Text = R.Read();
        if(Text == R.Baseline || !IsModuleKey(R.Section, R.Key))
        {
            continue;
        }
        const bool bOn = Text == QStringLiteral("true");
        Edits << QStringLiteral("%1: %2").arg(R.Label->text(), bOn ? QStringLiteral("compiled in") : QStringLiteral("compiled out"));
    }
    if(Edits.isEmpty())
    {
        return true;
    }

    QMessageBox Box(QMessageBox::Warning, windowTitle(),
                    QStringLiteral("Changing optional modules requires native recompilation."), QMessageBox::NoButton, this);
    Box.setInformativeText(
        QStringLiteral("%1\n\nThe engine and the game are rebuilt with %2: close the editor, build the project "
                       "(it re-configures from %3), and start the editor again. Until then, the editor and Play keep "
                       "the modules this build has.")
        .arg(Edits.join(QLatin1Char('\n')), Edits.size() == 1 ? QStringLiteral("the module compiled in or out")
                                                                : QStringLiteral("the modules compiled in or out"),
             QDir::toNativeSeparators(FilePath)));
    QPushButton* Save = Box.addButton(QStringLiteral("Save and Recompile Later"), QMessageBox::AcceptRole);
    QPushButton* Cancel = Box.addButton(QMessageBox::Cancel);
    Box.setDefaultButton(Cancel);
    Box.exec();
    if(Box.clickedButton() == Save)
    {
        return true;
    }

    /* Not confirmed: the switches go back to what the file says. */
    for(Row& R : Rows)
    {
        if(IsModuleKey(R.Section, R.Key) && R.Read() != R.Baseline)
        {
            R.Show(R.Baseline);
        }
    }
    return false;
}

bool ProjectSettingsDialog::Apply()
{
    /* The world list is saved whole or not at all. */
    const QList<ConfigIniFile::Section> WorldSections = ReadWorlds();
    const bool bWorldsChanged = WorldSections.size() != WorldsBaseline.size()
        || [&]()
           {
               for(int Index = 0; Index < WorldSections.size(); ++Index)
               {
                   if(WorldSections[Index].Name != WorldsBaseline[Index].Name || WorldSections[Index].Keys != WorldsBaseline[Index].Keys)
                   {
                       return true;
                   }
               }
               return false;
           }();
    if(bWorldsChanged)
    {
        const QString Problems = ValidateWorlds();
        if(!Problems.isEmpty())
        {
            ShowWorlds();
            QMessageBox::warning(this, windowTitle(), QStringLiteral("The world list cannot be saved:\n%1").arg(Problems));
            return false;
        }
    }

    /* A module switch is a native rebuild, not a setting: asked first. Other
     * changes apply either way. */
    const bool bModulesConfirmed = ConfirmModuleChanges();

    QStringList Changed;
    QStringList Rejected;
    QStringList ModulesChanged;
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
        if(IsModuleKey(R.Section, R.Key))
        {
            ModulesChanged << QStringLiteral("%1 = %2").arg(Path, Text);
        }
    }

    if(bWorldsChanged)
    {
        File.ReplaceSections(QLatin1String(NimbleEngine::WORLD_SECTION_PREFIX), WorldSections);
        WorldsBaseline = WorldSections;

        /* In this process too: the next Play makes its levels from these. */
        std::string Text;
        QStringList Names;
        for(const ConfigIniFile::Section& Each : WorldSections)
        {
            Text += "[" + ToStd(Each.Name) + "]\n";
            for(const QPair<QString, QString>& Key : Each.Keys)
            {
                Text += ToStd(Key.first) + "=" + ToStd(Key.second) + "\n";
            }
            Names << Each.Name.mid(int(std::strlen(NimbleEngine::WORLD_SECTION_PREFIX)));
        }
        EngineCore::ConfigRemoveEntries(NimbleEngine::WORLD_SECTION_PREFIX);
        EngineCore::LoadConfigText(Text);
        Changed << QStringLiteral("Worlds = %1").arg(Names.isEmpty() ? QStringLiteral("(none)") : Names.join(QStringLiteral(", ")));
    }

    if(!Rejected.isEmpty())
    {
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("Not valid values, so not saved:\n%1").arg(Rejected.join(QLatin1Char('\n'))));
    }
    if(Changed.isEmpty())
    {
        return bModulesConfirmed;
    }

    if(!File.Save(FilePath))
    {
        CORE_LOG_ERROR("[Editor] project settings: could not write " + ToStd(FilePath) + ": " + ToStd(File.GetError()))
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("The settings apply to this session, but could not be saved to\n%1\n\n%2")
                             .arg(QDir::toNativeSeparators(FilePath), File.GetError()));
        return false;
    }
    for(const QString& Line : Changed)
    {
        CORE_LOG("[Editor] project settings: " + ToStd(Line))
    }
    CORE_LOG("[Editor] project settings saved to " + ToStd(FilePath))

    if(!ModulesChanged.isEmpty())
    {
        CORE_LOG_WARN("[Editor] project settings: " + ToStd(ModulesChanged.join(QStringLiteral(", ")))
                      + " -- a native recompile applies it: close the editor and build the project")
    }

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
    return bModulesConfirmed;
}

#endif // WITH_EDITOR
