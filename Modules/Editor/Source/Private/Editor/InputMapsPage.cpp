#if WITH_EDITOR

#include "Private/Editor/InputMapsPage.h"

#include <map>
#include <string>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "Public/Core/CoreConfig.h"
#include "Public/NimbleEngine/Input.h"
#include "Private/Editor/ConfigIniFile.h"
#include "Private/Editor/QtInputNames.h"

namespace
{
    enum EColumn { IC_Name, IC_Kind, IC_Keys, IC_Note, IC_COUNT };

    const QString InputPrefix = QStringLiteral("Input_");

    /* "Space, -Gamepad.LeftX*0.5" -> { "space", "gamepad.leftx" }: the keys,
     * without sign or scale, for comparing. */
    QStringList KeysOf(const QString& InBindings)
    {
        QStringList Keys;
        for(QString Item : InBindings.split(QLatin1Char(','), Qt::SkipEmptyParts))
        {
            Item = Item.trimmed();
            if(Item.startsWith(QLatin1Char('-'))) { Item = Item.mid(1); }
            const int Star = Item.indexOf(QLatin1Char('*'));
            if(Star >= 0) { Item = Item.left(Star); }
            Item = Item.trimmed().toLower();
            if(!Item.isEmpty()) { Keys << Item; }
        }
        return Keys;
    }

    /* Waits for one key or mouse button and names it as the engine does. */
    class KeyCaptureDialog : public QDialog
    {
    public:
        explicit KeyCaptureDialog(QWidget* InParent) : QDialog(InParent)
        {
            setWindowTitle(QStringLiteral("Press a Key"));
            auto* Layout = new QVBoxLayout(this);
            auto* Prompt = new QLabel(QStringLiteral("Press a key or click a mouse button.\n"
                                                     "Gamepad buttons and sticks: pick them from the list."), this);
            Prompt->setAlignment(Qt::AlignCenter);
            Prompt->setMinimumSize(320, 90);
            Layout->addWidget(Prompt);
            auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
            Buttons->setFocusPolicy(Qt::NoFocus);
            Buttons->button(QDialogButtonBox::Cancel)->setFocusPolicy(Qt::NoFocus);
            connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
            Layout->addWidget(Buttons);
            setFocusPolicy(Qt::StrongFocus);
        }

        QString Captured;

    protected:
        void keyPressEvent(QKeyEvent* InEvent) override
        {
            if(const char* Name = QtKeyToInputName(InEvent->key()))
            {
                Captured = QString::fromLatin1(Name);
                accept();
                return;
            }
            QDialog::keyPressEvent(InEvent);
        }

        void mousePressEvent(QMouseEvent* InEvent) override
        {
            /* A click on Cancel is Cancel's; anywhere else is the button pressed. */
            if(const char* Name = QtButtonToInputName(InEvent->button()))
            {
                Captured = QString::fromLatin1(Name);
                accept();
                return;
            }
            QDialog::mousePressEvent(InEvent);
        }
    };
}

InputMapsPage::InputMapsPage(const ConfigIniFile& InFile, QWidget* InParent)
    : QWidget(InParent)
{
    auto* Layout = new QVBoxLayout(this);
    auto* Title  = new QLabel(QStringLiteral("<b>Input</b>"), this);
    Layout->addWidget(Title);
    auto* About = new QLabel(QStringLiteral("The game's actions and axes. Keys are comma-separated; on an axis, '-' before a "
                                            "key pushes it the other way and *0.5 scales it. Saved as [Input_&lt;Map&gt;] in "
                                            "the project's DefaultEngine.ini."), this);
    About->setWordWrap(true);
    Layout->addWidget(About);

    Tree = new QTreeWidget(this);
    Tree->setColumnCount(IC_COUNT);
    Tree->setHeaderLabels({ QStringLiteral("Name"), QStringLiteral("Kind"), QStringLiteral("Keys"), QStringLiteral("Note") });
    Tree->header()->setSectionResizeMode(IC_Name, QHeaderView::ResizeToContents);
    Tree->header()->setSectionResizeMode(IC_Kind, QHeaderView::ResizeToContents);
    Tree->header()->setSectionResizeMode(IC_Keys, QHeaderView::Stretch);
    Tree->header()->setSectionResizeMode(IC_Note, QHeaderView::ResizeToContents);
    Tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    Layout->addWidget(Tree, 1);

    auto* Row = new QHBoxLayout();
    KeyChoice = new QComboBox(this);
    KeyChoice->setEditable(false);
    for(size_t Index = 0; Index < NimbleEngine::GetInputKeyCount(); ++Index)
    {
        KeyChoice->addItem(QString::fromLatin1(NimbleEngine::GetInputKeyName(NimbleEngine::GetInputKeyByIndex(Index))));
    }
    KeyChoice->setToolTip(QStringLiteral("Every key, mouse button and axis, and gamepad button and axis"));
    AddKey  = new QPushButton(QStringLiteral("Add Key"), this);
    AddKey->setToolTip(QStringLiteral("Add the key chosen to the selected entry"));
    Capture = new QPushButton(QStringLiteral("Press a Key..."), this);
    Capture->setToolTip(QStringLiteral("Add the next key or mouse button pressed to the selected entry"));
    Reset   = new QPushButton(QStringLiteral("Reset"), this);
    Reset->setToolTip(QStringLiteral("Put the game's own keys back in the selected entry; Apply writes them"));
    Row->addWidget(KeyChoice, 1);
    Row->addWidget(AddKey);
    Row->addWidget(Capture);
    Row->addWidget(Reset);
    Layout->addLayout(Row);

    Note = new QLabel(this);
    Note->setWordWrap(true);
    Layout->addWidget(Note);

    connect(Tree, &QTreeWidget::itemSelectionChanged, this, &InputMapsPage::OnSelectionChanged);
    connect(Tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int InColumn)
    {
        if(InColumn == IC_Keys) { RefreshAll(); }
    });
    connect(AddKey, &QPushButton::clicked, this, [this]() { AppendKey(KeyChoice->currentText()); });
    connect(Capture, &QPushButton::clicked, this, &InputMapsPage::CaptureKey);
    connect(Reset, &QPushButton::clicked, this, &InputMapsPage::ResetSelected);

    Populate(InFile);
    OnSelectionChanged();
}

void InputMapsPage::Populate(const ConfigIniFile& InFile)
{
    /* Map -> its entries in order: the registry's, then the file's others. */
    std::map<QString, std::vector<Entry>> ByMap;
    std::vector<QString> Order;
    const auto Ensure = [&](const QString& InMap) -> std::vector<Entry>&
    {
        if(ByMap.find(InMap) == ByMap.end()) { Order.push_back(InMap); }
        return ByMap[InMap];
    };

    for(size_t MapIndex = 0; MapIndex < NimbleEngine::GetInputMapCount(); ++MapIndex)
    {
        const char* MapName = NimbleEngine::GetInputMapName(MapIndex);
        std::vector<Entry>& List = Ensure(QString::fromUtf8(MapName));
        for(size_t Index = 0; Index < NimbleEngine::GetInputMapEntryCount(MapName); ++Index)
        {
            NimbleEngine::InputMapEntry Current;
            if(!NimbleEngine::GetInputMapEntry(MapName, Index, Current)) { continue; }
            Entry Added;
            Added.Map   = QString::fromUtf8(MapName);
            Added.Name  = QString::fromUtf8(Current.Name);
            Added.bAxis = Current.bAxis;
            const std::string Section = "Input_" + std::string(MapName);
            const EngineCore::ConfigVar* Var = EngineCore::ConfigFindVar(Section.c_str(), Current.Name);
            Added.Default = Var ? QString::fromStdString(Var->DefaultToString()) : QString::fromUtf8(Current.Bindings);
            const QString FromFile = InFile.Get(InputPrefix + Added.Map, Added.Name, &Added.bInFile);
            Added.Baseline = Added.bInFile ? FromFile : Added.Default;
            List.push_back(Added);
        }
    }

    /* A map only the file knows: no module that defines it is loaded. */
    for(const ConfigIniFile::Section& Section : InFile.GetSections(InputPrefix))
    {
        const QString MapName = Section.Name.mid(InputPrefix.size());
        std::vector<Entry>& List = Ensure(MapName);
        for(const QPair<QString, QString>& Key : Section.Keys)
        {
            bool bKnown = false;
            for(const Entry& Each : List) { bKnown = bKnown || Each.Name.compare(Key.first, Qt::CaseInsensitive) == 0; }
            if(bKnown) { continue; }
            Entry Added;
            Added.Map      = MapName;
            Added.Name     = Key.first;
            Added.Baseline = Key.second;
            Added.bInFile  = true;
            List.push_back(Added);
        }
    }

    const QSignalBlocker Block(Tree);
    for(const QString& MapName : Order)
    {
        auto* Top = new QTreeWidgetItem(Tree);
        Top->setText(IC_Name, MapName);
        Top->setFirstColumnSpanned(false);
        QFont Bold = Top->font(IC_Name);
        Bold.setBold(true);
        Top->setFont(IC_Name, Bold);
        Top->setFlags(Top->flags() & ~Qt::ItemIsEditable);
        if(ByMap[MapName].empty() || ByMap[MapName].front().Default.isEmpty())
        {
            Top->setToolTip(IC_Name, QStringLiteral("Only in the project's .ini: no loaded game module defines this map"));
        }
        for(Entry& Each : ByMap[MapName])
        {
            Each.Item = new QTreeWidgetItem(Top);
            Each.Item->setText(IC_Name, Each.Name);
            Each.Item->setText(IC_Kind, Each.bAxis ? QStringLiteral("Axis") : QStringLiteral("Action"));
            Each.Item->setText(IC_Keys, Each.Baseline);
            Each.Item->setFlags(Each.Item->flags() | Qt::ItemIsEditable);
            if(!Each.Default.isEmpty()) { Each.Item->setToolTip(IC_Keys, QStringLiteral("The game's own: %1").arg(Each.Default)); }
            Entries.push_back(Each);
        }
        Top->setExpanded(true);
    }
    if(Order.empty())
    {
        Note->setText(QStringLiteral("No input maps: the game module is not loaded, and the .ini has no [Input_*] section."));
    }
    RefreshAll();
}

InputMapsPage::Entry* InputMapsPage::Selected()
{
    const QList<QTreeWidgetItem*> Items = Tree->selectedItems();
    if(Items.isEmpty()) { return nullptr; }
    for(Entry& Each : Entries)
    {
        if(Each.Item == Items.front()) { return &Each; }
    }
    return nullptr;
}

void InputMapsPage::OnSelectionChanged()
{
    const Entry* Current = Selected();
    AddKey->setEnabled(Current != nullptr);
    Capture->setEnabled(Current != nullptr);
    Reset->setEnabled(Current != nullptr && !Current->Default.isEmpty());
    if(Current)
    {
        Note->setText(Current->Item->text(IC_Note).isEmpty()
                      ? QStringLiteral("%1.%2 -- double-click the keys to type them").arg(Current->Map, Current->Name)
                      : Current->Item->text(IC_Note));
    }
}

void InputMapsPage::AppendKey(const QString& InKey)
{
    Entry* Current = Selected();
    if(!Current || InKey.isEmpty()) { return; }
    const QString Keys = Current->Item->text(IC_Keys).trimmed();
    if(KeysOf(Keys).contains(InKey.toLower())) { return; }
    Current->Item->setText(IC_Keys, Keys.isEmpty() ? InKey : Keys + QStringLiteral(", ") + InKey);
    OnSelectionChanged();
}

void InputMapsPage::CaptureKey()
{
    if(!Selected()) { return; }
    KeyCaptureDialog Dialog(this);
    if(Dialog.exec() == QDialog::Accepted) { AppendKey(Dialog.Captured); }
}

void InputMapsPage::ResetSelected()
{
    Entry* Current = Selected();
    if(!Current || Current->Default.isEmpty()) { return; }
    Current->Item->setText(IC_Keys, Current->Default);
    OnSelectionChanged();
}

void InputMapsPage::RefreshAll()
{
    const QSignalBlocker Block(Tree);
    for(Entry& Each : Entries)
    {
        /* Bold: the project's .ini sets it. Italic: changed, not yet applied. */
        QFont Font = Each.Item->font(IC_Keys);
        Font.setBold(Each.bInFile);
        Font.setItalic(Each.Item->text(IC_Keys) != Each.Baseline);
        Each.Item->setFont(IC_Keys, Font);

        /* Another entry of the same map on one of its keys. */
        QStringList Clashes;
        const QStringList Mine = KeysOf(Each.Item->text(IC_Keys));
        for(const Entry& Other : Entries)
        {
            if(&Other == &Each || Other.Map != Each.Map) { continue; }
            for(const QString& Key : KeysOf(Other.Item->text(IC_Keys)))
            {
                if(Mine.contains(Key)) { Clashes << QStringLiteral("%1 (%2)").arg(Other.Name, Key); }
            }
        }
        Each.Item->setText(IC_Note, Clashes.isEmpty() ? QString() : QStringLiteral("also on ") + Clashes.join(QStringLiteral(", ")));
        Each.Item->setForeground(IC_Note, QBrush(QColor(220, 140, 40)));
    }
}

QStringList InputMapsPage::Apply(ConfigIniFile& InOutFile)
{
    QStringList Changed;
    for(Entry& Each : Entries)
    {
        const QString Keys = Each.Item->text(IC_Keys).trimmed();
        if(Keys == Each.Baseline) { continue; }
        const QString Section = InputPrefix + Each.Map;
        InOutFile.Set(Section, Each.Name, Keys);
        NimbleEngine::SetInputBinding(Each.Map.toUtf8().constData(), Each.Name.toUtf8().constData(), Keys.toUtf8().constData());
        Each.Baseline = Keys;
        Each.bInFile  = true;
        Changed << QStringLiteral("%1.%2 = %3").arg(Section, Each.Name, Keys);
    }
    RefreshAll();
    return Changed;
}

#endif // WITH_EDITOR
