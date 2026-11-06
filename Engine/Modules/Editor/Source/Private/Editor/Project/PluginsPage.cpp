#if WITH_EDITOR

#include "Private/Editor/Project/PluginsPage.h"

#include <algorithm>

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/Project/NewPluginDialog.h"
#include "Private/Editor/Project/PluginTemplate.h"
#include "Private/Editor/Project/ProjectTemplate.h"

namespace PluginCatalog = NimbleEngine::PluginCatalog;

namespace
{
    enum EColumn { Col_On = 0, Col_Name, Col_Version, Col_Category, Col_Location, Col_Status, Col_Count };

    QString Q(const std::string& InText) { return QString::fromStdString(InText); }
    std::string S(const QString& InText) { return InText.toStdString(); }
    QString Key(const QString& InName) { return InName.toLower(); }

    QString Native(const std::string& InPath) { return QDir::toNativeSeparators(Q(InPath)); }
}

PluginsPage::PluginsPage(const ConfigIniFile& InFile, const QString& InConfigPath, QWidget* InParent)
    : QWidget(InParent), ConfigPath(InConfigPath)
{
    for(const ConfigIniFile::Section& Each : InFile.GetSections(QStringLiteral("Plugins")))
    {
        if(Each.Name.compare(QStringLiteral("Plugins"), Qt::CaseInsensitive) != 0) { continue; }
        for(const QPair<QString, QString>& Line : Each.Keys) { FileSection.emplace_back(S(Line.first), S(Line.second)); }
    }

    /* The build's search folders; without a manifest (no plugin build yet),
     * the two every project has. */
    for(const std::string& Root : NimbleEngine::Plugins::GetSearchRoots()) { Roots << Q(Root); }
    if(Roots.isEmpty())
    {
        Roots << GetProjectPluginsDir();
        const QString Engine = EditorProject::GetEngineDir();
        if(!Engine.isEmpty()) { Roots << QDir(Engine).filePath(QStringLiteral("Plugins")); }
    }

    auto* Column = new QVBoxLayout(this);
    Column->addWidget(new QLabel(QStringLiteral("<b>Plugins</b>"), this));
    auto* Note = new QLabel(QStringLiteral(
        "Drop-in modules: code, content and config in a folder. A change applies at the game's next build, which this "
        "editor offers when you apply it, and loads when the editor restarts. "
        "<a href=\"guide\">How plugins work</a>"), this);
    Note->setWordWrap(true);
    connect(Note, &QLabel::linkActivated, this, []()
    {
        const QString Engine = EditorProject::GetEngineDir();
        QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(Engine).filePath(QStringLiteral("Docs/Guides/PluginGuide.md"))));
    });
    Column->addWidget(Note);

    Summary = new QLabel(this);
    Summary->setWordWrap(true);
    Summary->setTextFormat(Qt::RichText);
    Column->addWidget(Summary);

    auto* Buttons = new QHBoxLayout();
    auto* New = new QPushButton(QStringLiteral("New Plugin..."), this);
    New->setToolTip(QStringLiteral("Make a plugin from a starter (Engine/Templates/Plugin)"));
    connect(New, &QPushButton::clicked, this, [this]() { OpenNewPluginDialog(); });
    auto* Open = new QPushButton(QStringLiteral("Open Folder"), this);
    Open->setToolTip(QStringLiteral("The selected plugin's folder"));
    connect(Open, &QPushButton::clicked, this, [this]()
    {
        if(const PluginCatalog::PluginInfo* Info = FindPlugin(Selected))
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(Q(Info->Dir)));
        }
    });
    auto* Again = new QPushButton(QStringLiteral("Refresh"), this);
    Again->setToolTip(QStringLiteral("Read the search folders again"));
    connect(Again, &QPushButton::clicked, this, &PluginsPage::Refresh);
    Buttons->addWidget(New);
    Buttons->addWidget(Open);
    Buttons->addStretch(1);
    Buttons->addWidget(Again);
    Column->addLayout(Buttons);

    Table = new QTableWidget(0, Col_Count, this);
    Table->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Plugin"), QStringLiteral("Version"),
                                       QStringLiteral("Category"), QStringLiteral("Location"), QStringLiteral("Status") });
    Table->verticalHeader()->setVisible(false);
    Table->setSelectionBehavior(QAbstractItemView::SelectRows);
    Table->setSelectionMode(QAbstractItemView::SingleSelection);
    Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    Table->horizontalHeader()->setSectionResizeMode(Col_Status, QHeaderView::Stretch);
    Table->setMinimumHeight(170);
    connect(Table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* InItem)
    {
        if(!bFilling && InItem->column() == Col_On)
        {
            OnToggled(InItem->row(), InItem->checkState() == Qt::Checked);
        }
    });
    connect(Table, &QTableWidget::itemSelectionChanged, this, [this]()
    {
        const QList<QTableWidgetItem*> Items = Table->selectedItems();
        if(!bFilling && !Items.isEmpty())
        {
            Selected = Table->item(Items.front()->row(), Col_Name)->text();
            ShowDetails();
        }
    });
    Column->addWidget(Table, 1);

    Details = new QLabel(this);
    Details->setWordWrap(true);
    Details->setTextFormat(Qt::RichText);
    Details->setTextInteractionFlags(Qt::TextBrowserInteraction);
    Details->setOpenExternalLinks(true);
    Details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    Details->setMinimumHeight(150);
    Column->addWidget(Details);

    Refresh();
}

QString PluginsPage::GetProjectPluginsDir() const
{
    QDir Project = QFileInfo(ConfigPath).absoluteDir();   // <project>/Config
    Project.cdUp();
    return QDir::fromNativeSeparators(Project.filePath(QStringLiteral("Plugins")));
}

QStringList PluginsPage::GetTakenNames() const
{
    QStringList Taken = EditorPlugins::GetReservedNames(EditorProject::GetEngineDir());
    for(const PluginCatalog::PluginInfo& Info : Found)
    {
        Taken << Q(Info.Name);
        for(const PluginCatalog::ModuleInfo& Module : Info.Modules) { Taken << Q(Module.Name); }
    }
    if(const EngineCore::ConfigVar* Module = EngineCore::ConfigFindVar("Game", "Module"))
    {
        Taken << QString::fromUtf8(Module->GetString());
    }
    return Taken;
}

void PluginsPage::Refresh()
{
    std::vector<std::string> RootList;
    for(const QString& Root : Roots) { RootList.push_back(S(Root)); }
    Found = PluginCatalog::Find(RootList);
    std::stable_sort(Found.begin(), Found.end(), [](const PluginCatalog::PluginInfo& InA, const PluginCatalog::PluginInfo& InB)
    {
        return Q(InA.Name).compare(Q(InB.Name), Qt::CaseInsensitive) < 0;
    });
    Fill();
}

std::vector<std::pair<std::string, std::string>> PluginsPage::PendingSection() const
{
    std::vector<std::pair<std::string, std::string>> Section = FileSection;
    for(const auto& [Name, bOn] : Switches)
    {
        Section.emplace_back(S(Name), bOn ? "true" : "false");   // after the file's: the last line wins
    }
    return Section;
}

const PluginCatalog::PluginInfo* PluginsPage::FindPlugin(const QString& InName) const
{
    for(const PluginCatalog::PluginInfo& Info : Found)
    {
        if(Key(Q(Info.Name)) == Key(InName)) { return &Info; }
    }
    return nullptr;
}

bool PluginsPage::IsOn(const QString& InName) const
{
    const PluginCatalog::PluginInfo* Info = FindPlugin(InName);
    return Info && PluginCatalog::IsEnabled(*Info, PendingSection());
}

QString PluginsPage::LocationOf(const PluginCatalog::PluginInfo& InInfo) const
{
    const QString Root = QDir::fromNativeSeparators(Q(InInfo.Root));
    if(QDir(Root) == QDir(GetProjectPluginsDir())) { return QStringLiteral("Project"); }
    const QString Engine = EditorProject::GetEngineDir();
    if(!Engine.isEmpty() && QDir(Root) == QDir(QDir(Engine).filePath(QStringLiteral("Plugins")))) { return QStringLiteral("Engine"); }
    return QStringLiteral("Shared");
}

QString PluginsPage::StatusOf(const PluginCatalog::PluginInfo& InInfo, QString* OutTip) const
{
    const QString Name = Q(InInfo.Name);
    const bool bOn     = IsOn(Name);
    const bool bBuilt  = NimbleEngine::Plugins::IsBuilt(InInfo.Name);
    const bool bLoaded = NimbleEngine::Plugins::IsLoaded(InInfo.Name);
    QString Tip;
    QString Status;
    if(!InInfo.Error.empty())
    {
        Status = QStringLiteral("Error");
        Tip    = Q(InInfo.Error);
    }
    else if(bOn && bLoaded)
    {
        Status = QStringLiteral("Loaded");
    }
    else if(bOn && bBuilt)
    {
        Status = QStringLiteral("Built, not started");
        Tip    = Q(NimbleEngine::Plugins::GetLoadProblem(InInfo.Name));
    }
    else if(bOn)
    {
        Status = QStringLiteral("Enabled: build and restart to load");
    }
    else if(bBuilt)
    {
        Status = QStringLiteral("Disabled: build and restart to remove");
    }
    else
    {
        Status = QStringLiteral("Off");
    }
    if(bOn && InInfo.bHasShaders && !InInfo.bShadersCooked && InInfo.Error.empty())
    {
        Status += QStringLiteral(" (shaders not cooked)");
    }
    if(OutTip) { *OutTip = Tip; }
    return Status;
}

void PluginsPage::Fill()
{
    bFilling = true;
    Table->setRowCount(int(Found.size()));
    int SelectRow = -1;
    for(int Row = 0; Row < int(Found.size()); ++Row)
    {
        const PluginCatalog::PluginInfo& Info = Found[size_t(Row)];
        const QString Name = Q(Info.Name);

        auto* On = new QTableWidgetItem();
        On->setFlags(Info.Error.empty() ? Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::ItemIsSelectable);
        On->setCheckState(IsOn(Name) ? Qt::Checked : Qt::Unchecked);
        Table->setItem(Row, Col_On, On);

        const auto Cell = [&](const int InColumn, const QString& InText, const QString& InTip = QString())
        {
            auto* Item = new QTableWidgetItem(InText);
            Item->setToolTip(InTip.isEmpty() ? InText : InTip);
            Table->setItem(Row, InColumn, Item);
            return Item;
        };
        QTableWidgetItem* NameItem = Cell(Col_Name, Name, Q(Info.Description));
        if(Switches.count(Key(Name)))
        {
            QFont Font = NameItem->font();
            Font.setBold(true);   // switched, not applied yet
            NameItem->setFont(Font);
        }
        Cell(Col_Version, Q(Info.Version));
        Cell(Col_Category, Q(Info.Category));
        Cell(Col_Location, LocationOf(Info), Native(Info.Dir));
        QString Tip;
        const QString Status = StatusOf(Info, &Tip);
        QTableWidgetItem* StatusItem = Cell(Col_Status, Status, Tip);
        if(!Info.Error.empty() || Status.startsWith(QStringLiteral("Built, not")))
        {
            StatusItem->setForeground(QColor(QStringLiteral("#e06c5a")));
        }
        else if(Status == QStringLiteral("Loaded"))
        {
            StatusItem->setForeground(QColor(QStringLiteral("#7cc47f")));
        }
        else if(Status.contains(QStringLiteral("restart")) || Status.contains(QStringLiteral("cooked")))
        {
            StatusItem->setForeground(QColor(QStringLiteral("#e0b85a")));
        }
        if(Key(Name) == Key(Selected)) { SelectRow = Row; }
    }

    /* What a configure would say of it all, before one runs. */
    const PluginCatalog::Resolution Result = PluginCatalog::Resolve(Found, PendingSection(), PluginCatalog::GetEngineVersion());
    const int Enabled = int(std::count_if(Found.begin(), Found.end(), [this](const PluginCatalog::PluginInfo& InInfo)
    {
        return InInfo.Error.empty() && IsOn(Q(InInfo.Name));
    }));
    QString Text = QStringLiteral("%1 found in %2 folder%3, %4 enabled.")
                       .arg(Found.size()).arg(Roots.size()).arg(Roots.size() == 1 ? QString() : QStringLiteral("s")).arg(Enabled);
    if(!Switches.empty())
    {
        Text += QStringLiteral(" <b>%1 change%2 not applied</b> (bold).").arg(Switches.size()).arg(Switches.size() == 1 ? QString() : QStringLiteral("s"));
    }
    if(!Result.Error.empty())
    {
        Text += QStringLiteral("<br><span style=\"color:#e06c5a\"><b>The next build will stop:</b> %1</span>").arg(Q(Result.Error).toHtmlEscaped());
    }
    for(const std::string& Warning : Result.Warnings)
    {
        Text += QStringLiteral("<br><span style=\"color:#e0b85a\">%1</span>").arg(Q(Warning).toHtmlEscaped());
    }
    Summary->setText(Text);

    bFilling = false;
    if(SelectRow < 0 && !Found.empty()) { SelectRow = 0; }
    if(SelectRow >= 0)
    {
        Table->selectRow(SelectRow);
        Selected = Table->item(SelectRow, Col_Name)->text();
    }
    ShowDetails();
}

void PluginsPage::ShowDetails()
{
    const PluginCatalog::PluginInfo* Info = FindPlugin(Selected);
    if(!Info)
    {
        Details->setText(Found.empty() ? QStringLiteral("No plugins in %1. <b>New Plugin...</b> makes one.")
                                             .arg(Roots.join(QStringLiteral(", ")).toHtmlEscaped())
                                       : QString());
        return;
    }
    const auto Esc = [](const std::string& InText) { return Q(InText).toHtmlEscaped(); };
    QString Html = QStringLiteral("<b>%1</b> %2 &mdash; %3<br>").arg(Esc(Info->Name), Esc(Info->Version), Esc(Info->Description));
    Html += QStringLiteral("<a href=\"%1\">%2</a> (%3)").arg(QUrl::fromLocalFile(Q(Info->Dir)).toString(),
                                                          Native(Info->Dir).toHtmlEscaped(), LocationOf(*Info));
    if(!Info->Overrides.empty())
    {
        Html += QStringLiteral("<br>Hides the one in %1.").arg(Native(Info->Overrides).toHtmlEscaped());
    }
    if(!Info->Error.empty())
    {
        Html += QStringLiteral("<br><span style=\"color:#e06c5a\">%1</span>").arg(Esc(Info->Error));
        Details->setText(Html);
        return;
    }

    QStringList Modules;
    for(const PluginCatalog::ModuleInfo& Module : Info->Modules)
    {
        Modules << QStringLiteral("%1 (%2)").arg(Esc(Module.Name), Esc(Module.Type));
    }
    Html += QStringLiteral("<br><b>Modules:</b> %1").arg(Modules.isEmpty() ? QStringLiteral("none") : Modules.join(QStringLiteral(", ")));

    QStringList Deps;
    for(const PluginCatalog::Dependency& Dep : Info->Dependencies)
    {
        const PluginCatalog::PluginInfo* Needed = FindPlugin(Q(Dep.Name));
        QString Text = Esc(Dep.Name) + (Dep.MinVersion.empty() ? QString() : QStringLiteral(" &ge; ") + Esc(Dep.MinVersion));
        if(Dep.bOptional) { Text += QStringLiteral(" (optional)"); }
        const bool bMet = Needed && IsOn(Q(Dep.Name))
                          && (Dep.MinVersion.empty() || PluginCatalog::CompareVersions(Needed->Version, Dep.MinVersion) >= 0);
        if(!Dep.bOptional)
        {
            Text = QStringLiteral("<span style=\"color:%1\">%2 %3</span>")
                       .arg(bMet ? QStringLiteral("#7cc47f") : QStringLiteral("#e06c5a"), bMet ? QStringLiteral("&#10003;") : QStringLiteral("&#10007;"), Text);
        }
        Deps << Text;
    }
    Html += QStringLiteral("<br><b>Needs:</b> %1").arg(Deps.isEmpty() ? QStringLiteral("nothing") : Deps.join(QStringLiteral(", ")));

    QStringList Has;
    if(Info->bHasContent) { Has << QStringLiteral("content, read as <code>%1:/...</code>").arg(Esc(Info->Name)); }
    if(Info->bHasShaders)
    {
        Has << (Info->bShadersCooked ? QStringLiteral("shaders (cooked)")
                                     : QStringLiteral("shaders, <span style=\"color:#e0b85a\">not cooked: "
                                                      "<code>NimbleCook shaders -project &lt;game&gt;</code></span>"));
    }
    if(!Has.isEmpty()) { Html += QStringLiteral("<br><b>Has:</b> %1").arg(Has.join(QStringLiteral("; "))); }

    QString Tip;
    const QString Status = StatusOf(*Info, &Tip);
    Html += QStringLiteral("<br><b>Status:</b> %1").arg(Status.toHtmlEscaped());
    if(!Tip.isEmpty()) { Html += QStringLiteral(" &mdash; %1").arg(Tip.toHtmlEscaped()); }
    Details->setText(Html);
}

void PluginsPage::OnToggled(const int InRow, const bool bInOn)
{
    const PluginCatalog::PluginInfo& Info = Found[size_t(InRow)];
    const QString Name = Q(Info.Name);
    std::map<QString, bool> Next = Switches;
    const auto Set = [&Next, this](const QString& InName, const bool bOn)
    {
        /* A switch back to what the file says is no switch. */
        const PluginCatalog::PluginInfo* Each = FindPlugin(InName);
        if(Each && PluginCatalog::IsEnabled(*Each, FileSection) == bOn) { Next.erase(Key(InName)); }
        else                                                           { Next[Key(InName)] = bOn; }
    };
    Set(Name, bInOn);

    const auto WithNext = [this, &Next](const QString& InName)
    {
        std::map<QString, bool> Saved = Switches;
        Switches = Next;
        const bool bOn = IsOn(InName);
        Switches = Saved;
        return bOn;
    };

    if(bInOn)
    {
        /* What it needs, all the way down, not on yet. */
        QStringList Missing, Absent;
        QStringList Queue = { Name };
        while(!Queue.isEmpty())
        {
            const PluginCatalog::PluginInfo* Each = FindPlugin(Queue.takeFirst());
            if(!Each) { continue; }
            for(const PluginCatalog::Dependency& Dep : Each->Dependencies)
            {
                if(Dep.bOptional) { continue; }
                const QString DepName = Q(Dep.Name);
                if(!FindPlugin(DepName)) { if(!Absent.contains(DepName)) { Absent << DepName; } continue; }
                if(!WithNext(DepName) && !Missing.contains(DepName)) { Missing << DepName; Queue << DepName; }
            }
        }
        if(!Missing.isEmpty())
        {
            const auto Answer = QMessageBox::question(this, QStringLiteral("Enable %1").arg(Name),
                QStringLiteral("%1 needs %2, which %3 not enabled. Enable %4 too?")
                    .arg(Name, Missing.join(QStringLiteral(", ")), Missing.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                         Missing.size() == 1 ? QStringLiteral("it") : QStringLiteral("them")),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
            if(Answer == QMessageBox::Cancel) { Fill(); return; }
            if(Answer == QMessageBox::Yes) { for(const QString& Each : Missing) { Set(Each, true); } }
        }
        if(!Absent.isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("Enable %1").arg(Name),
                QStringLiteral("%1 needs %2, which is in none of the search folders. The next build will stop until it is "
                               "added to one of them:\n\n%3").arg(Name, Absent.join(QStringLiteral(", ")), Roots.join(QLatin1Char('\n'))));
        }
    }
    else
    {
        /* What needs it, all the way up, still on. */
        QStringList Dependents;
        QStringList Queue = { Name };
        while(!Queue.isEmpty())
        {
            const QString Gone = Queue.takeFirst();
            for(const PluginCatalog::PluginInfo& Each : Found)
            {
                const QString EachName = Q(Each.Name);
                if(!WithNext(EachName) || Dependents.contains(EachName)) { continue; }
                for(const PluginCatalog::Dependency& Dep : Each.Dependencies)
                {
                    if(!Dep.bOptional && Key(Q(Dep.Name)) == Key(Gone)) { Dependents << EachName; Queue << EachName; break; }
                }
            }
        }
        if(!Dependents.isEmpty())
        {
            const auto Answer = QMessageBox::question(this, QStringLiteral("Disable %1").arg(Name),
                QStringLiteral("%1 %2 %3. Disable %4 too?\n\nA level that uses a disabled plugin's components loads "
                               "without them (the log names each), and saving it in the editor drops them.")
                    .arg(Dependents.join(QStringLiteral(", ")), Dependents.size() == 1 ? QStringLiteral("needs") : QStringLiteral("need"),
                         Name, Dependents.size() == 1 ? QStringLiteral("it") : QStringLiteral("them")),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
            if(Answer == QMessageBox::Cancel) { Fill(); return; }
            if(Answer == QMessageBox::Yes) { for(const QString& Each : Dependents) { Set(Each, false); } }
        }
    }

    Switches = Next;
    Selected = Name;
    Fill();
}

QStringList PluginsPage::Apply(ConfigIniFile& InOutFile)
{
    QStringList Changed;
    std::string Live = "[Plugins]\n";
    for(const auto& [Lower, bOn] : Switches)
    {
        const PluginCatalog::PluginInfo* Info = FindPlugin(Lower);
        const QString Name = Info ? Q(Info->Name) : Lower;
        const QString Value = bOn ? QStringLiteral("true") : QStringLiteral("false");
        InOutFile.Set(QStringLiteral("Plugins"), Name, Value);
        FileSection.emplace_back(S(Name), S(Value));
        Changed << QStringLiteral("Plugins.%1 = %2").arg(Name, Value);
        Live += S(Name) + "=" + S(Value) + "\n";
    }
    if(!Switches.empty())
    {
        EngineCore::LoadConfigText(Live);   // the status bar's notice reads it
    }
    Switches.clear();
    Fill();
    return Changed;
}

void PluginsPage::OpenNewPluginDialog(const QString& InName, const QString& InStarter)
{
    QStringList Locations = Roots;
    if(!Locations.contains(GetProjectPluginsDir())) { Locations.prepend(GetProjectPluginsDir()); }
    NewPluginDialog Dialog(Locations, GetProjectPluginsDir(), GetTakenNames(), this);
    Dialog.Prefill(InName, InStarter);
    if(Dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    Refresh();
    const QString Name = Dialog.GetCreatedName();
    if(const PluginCatalog::PluginInfo* Info = FindPlugin(Name))
    {
        if(!PluginCatalog::IsEnabled(*Info, FileSection)) { Switches[Key(Name)] = true; }
    }
    Selected = Name;
    Fill();
}

#endif // WITH_EDITOR
