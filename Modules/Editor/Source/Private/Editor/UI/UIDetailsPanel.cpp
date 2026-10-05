#if WITH_EDITOR

#include "Private/Editor/UI/UIDetailsPanel.h"

#include <memory>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/UI/UIDocument.h"
#include "Public/UI/UIFile.h"
#include "Public/UI/UIWidget.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/UI/UIDesignDocument.h"
#include "Private/Editor/UI/UIGraphLink.h"

namespace
{
    std::string Utf8(const QString& InText) { return InText.toUtf8().toStdString(); }

    const char* const INVALID_STYLE = "QLineEdit { border: 1px solid #d9534f; }";

    /* Whether InText is a value InDesc takes: its own form, or {Bind:...} if it binds. */
    bool Accepts(const UI::UIPropertyDesc& InDesc, const QString& InText)
    {
        bool bWhole = false;
        if((InDesc.Flags & UI::UPF_Bindable) && UI::ParseBinding(Utf8(InText), bWhole))
        {
            return bWhole || InDesc.Type == UI::EUIPropertyType::String;
        }
        UI::UIValue Ignored;
        return UI::ParsePropertyValue(InDesc, Utf8(InText), Ignored);
    }

    QColor ToQColor(const QString& InText)
    {
        UI::UIColor Color;
        if(!UI::ParseColor(Utf8(InText), Color)) return QColor(255, 255, 255);
        return QColor::fromRgbF(Color.R, Color.G, Color.B, Color.A);
    }
}

UIDetailsPanel::UIDetailsPanel(UIDesignDocument& InDesign, QWidget* InParent)
    : QScrollArea(InParent)
    , Design(InDesign)
{
    setWidgetResizable(true);
    connect(&Design, &UIDesignDocument::Changed, this, [this](bool) { ScheduleRebuild(); });
    connect(&Design, &UIDesignDocument::SelectionChanged, this, [this](int) { ScheduleRebuild(); });
    Rebuild();
}

QWidget* UIDetailsPanel::FindEditor(const QString& InProperty) const
{
    const auto Found = Editors.find(InProperty);
    return Found != Editors.end() ? Found->second : nullptr;
}

void UIDetailsPanel::ScheduleRebuild()
{
    if(bRebuildPending) return;
    bRebuildPending = true;
    QTimer::singleShot(0, this, [this]() { Rebuild(); });
}

const std::map<std::string, std::string>& UIDetailsPanel::GetDefaults(const UI::UIWidgetType& InType)
{
    auto Found = Defaults.find(InType.Name);
    if(Found != Defaults.end()) return Found->second;

    std::map<std::string, std::string>& Out = Defaults[InType.Name];
    if(InType.Construct)
    {
        /* One made and read: what a widget is before any property is set. */
        std::unique_ptr<std::max_align_t[]> Memory(new std::max_align_t[(InType.Size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1]);
        UI::UIWidget* Widget = InType.Construct(Memory.get());
        for(const UI::UIPropertyDesc& Each : InType.Properties)
        {
            if(Each.Get) Out[Each.Name] = UI::FormatPropertyValue(Each, Each.Get(*Widget));
        }
        Widget->~UIWidget();
    }
    return Out;
}

void UIDetailsPanel::Rebuild()
{
    bRebuildPending = false;
    const int Scroll = verticalScrollBar() ? verticalScrollBar()->value() : 0;

    Editors.clear();
    Content = new QWidget();
    Layout  = new QVBoxLayout(Content);
    Layout->setContentsMargins(6, 6, 6, 6);
    Layout->setSpacing(8);

    const int Selected = Design.GetSelected();
    if(Selected >= 0 && Selected < int(Design.GetAsset().Widgets.size())) { BuildWidget(Selected); }
    else                                                                  { BuildDocument(); }
    Layout->addStretch(1);

    setWidget(Content);   // deletes the previous one
    if(verticalScrollBar()) verticalScrollBar()->setValue(Scroll);
}

QWidget* UIDetailsPanel::MakeEditor(const UI::UIPropertyDesc& InDesc, const QString& InValue, const std::function<bool(const QString&)>& InWrite,
                                    const int InOwner)
{
    switch(InDesc.Type)
    {
        case UI::EUIPropertyType::Bool:
        {
            auto* Box = new QCheckBox();
            Box->setChecked(InValue == QStringLiteral("true"));
            connect(Box, &QCheckBox::toggled, this, [InWrite](const bool bOn) { InWrite(bOn ? QStringLiteral("true") : QStringLiteral("false")); });
            return Box;
        }
        case UI::EUIPropertyType::Enum:
        {
            auto* Box = new QComboBox();
            for(uint32_t At = 0; At < InDesc.Choices.Count; ++At) { Box->addItem(QString::fromLatin1(InDesc.Choices.Names[At])); }
            Box->setCurrentText(InValue);
            connect(Box, &QComboBox::activated, this, [Box, InWrite](int) { InWrite(Box->currentText()); });
            return Box;
        }
        default:
            break;
    }

    auto* Row  = new QWidget();
    auto* HBox = new QHBoxLayout(Row);
    HBox->setContentsMargins(0, 0, 0, 0);
    HBox->setSpacing(2);

    QLineEdit* Edit = nullptr;
    QComboBox* Picker = nullptr;
    if(InDesc.Type == UI::EUIPropertyType::Asset && InDesc.AssetKind == UI::EUIAssetKind::Texture)
    {
        /* The project's textures, or a path typed in. */
        Picker = new QComboBox();
        Picker->setEditable(true);
        Picker->addItem(QString());
        Picker->addItems(EditorContent::ListTexturePaths());
        Picker->setCurrentText(InValue);
        Edit = Picker->lineEdit();
        HBox->addWidget(Picker, 1);
    }
    else
    {
        Edit = new QLineEdit(InValue);
        HBox->addWidget(Edit, 1);
    }

    const UI::UIPropertyDesc Desc = InDesc;
    const auto Commit = [this, Edit, Desc, InWrite]()
    {
        const QString Text = Edit->text();
        if(!Accepts(Desc, Text))
        {
            Edit->setStyleSheet(QString::fromLatin1(INVALID_STYLE));
            emit StatusText(QStringLiteral("'%1' is not a %2 value").arg(Text, QString::fromStdString(Desc.Name)));
            return;
        }
        Edit->setStyleSheet(QString());
        InWrite(Text);
    };
    connect(Edit, &QLineEdit::editingFinished, this, Commit);
    if(Picker) connect(Picker, &QComboBox::activated, this, [Commit](int) { Commit(); });

    if(InDesc.Type == UI::EUIPropertyType::Color)
    {
        auto* Swatch = new QToolButton();
        Swatch->setFixedWidth(24);
        const QColor Color = ToQColor(InValue);
        Swatch->setStyleSheet(QStringLiteral("QToolButton { background: rgba(%1,%2,%3,%4); border: 1px solid #555; }")
                              .arg(Color.red()).arg(Color.green()).arg(Color.blue()).arg(Color.alpha()));
        connect(Swatch, &QToolButton::clicked, this, [this, Edit, Commit]()
        {
            const QColor Picked = QColorDialog::getColor(ToQColor(Edit->text()), this, QStringLiteral("Colour"), QColorDialog::ShowAlphaChannel);
            if(!Picked.isValid()) return;
            Edit->setText(QString::fromStdString(UI::FormatColor(UI::UIColor{ float(Picked.redF()), float(Picked.greenF()),
                                                                              float(Picked.blueF()), float(Picked.alphaF()) })));
            Commit();
        });
        HBox->addWidget(Swatch);
    }
    else if(InDesc.Type == UI::EUIPropertyType::Asset && InDesc.AssetKind != UI::EUIAssetKind::Texture)
    {
        auto* Browse = new QToolButton();
        Browse->setText(QStringLiteral("..."));
        const QString Filter = InDesc.AssetKind == UI::EUIAssetKind::Font   ? QStringLiteral("Fonts (*.ttf)")
                             : InDesc.AssetKind == UI::EUIAssetKind::Script ? QStringLiteral("Lua (*.lua)") : QStringLiteral("All files (*)");
        connect(Browse, &QToolButton::clicked, this, [this, Edit, Filter, Commit]()
        {
            const QString File = QFileDialog::getOpenFileName(this, QStringLiteral("Choose"), EditorGraphs::GetProjectRoot(), Filter);
            if(File.isEmpty()) return;
            const QString Runtime = EditorContent::ToRuntimePath(File);
            if(Runtime.isEmpty())
            {
                emit StatusText(QStringLiteral("%1 is outside the project and the engine's content").arg(File));
                return;
            }
            Edit->setText(Runtime);
            Commit();
        });
        HBox->addWidget(Browse);
    }
    else if(InDesc.Type == UI::EUIPropertyType::Action)
    {
        /* The native actions there are now, and Lua's and Event's forms. */
        auto* Pick = new QToolButton();
        Pick->setText(QStringLiteral("+"));
        Pick->setToolTip(QStringLiteral("Add an action"));
        Pick->setPopupMode(QToolButton::InstantPopup);
        auto* Menu = new QMenu(Pick);
        const auto Append = [Edit, Commit](const QString& InAction)
        {
            const QString Existing = Edit->text().trimmed();
            Edit->setText(Existing.isEmpty() ? InAction : Existing + QStringLiteral("; ") + InAction);
            Commit();
        };
        QMenu* Native = Menu->addMenu(QStringLiteral("Native"));
        for(const UI::UIActionInfo& Each : UI::GetUIActions())
        {
            QAction* Item = Native->addAction(QString::fromStdString(Each.Name));
            Item->setToolTip(QString::fromStdString(Each.Tooltip));
            const QString Name = QString::fromStdString(Each.Name);
            connect(Item, &QAction::triggered, this, [Append, Name]() { Append(QStringLiteral("Native:") + Name); });
        }
        Menu->addAction(QStringLiteral("Lua function..."), this, [Edit]()
        {
            Edit->setText(Edit->text().trimmed().isEmpty() ? QStringLiteral("Lua:") : Edit->text() + QStringLiteral("; Lua:"));
            Edit->setFocus();
        });
        Menu->addAction(QStringLiteral("Event..."), this, [Edit]()
        {
            Edit->setText(Edit->text().trimmed().isEmpty() ? QStringLiteral("Event:") : Edit->text() + QStringLiteral("; Event:"));
            Edit->setFocus();
        });
        Menu->addSeparator();
        const QString Property = QString::fromStdString(InDesc.Name);
        QAction* Handler = Menu->addAction(QStringLiteral("Graph handler"), this, [this, InOwner, Property]()
        {
            const QString Made = EditorUIGraph::AddHandler(Design, InOwner, Property, window());
            if(!Made.isEmpty()) emit StatusText(QStringLiteral("Graph:%1 -- its UI Event is open in the Graph Editor").arg(Made));
        });
        Handler->setToolTip(QStringLiteral("A UI Event in the document's UI graph (made if it has none), opened there"));
        Pick->setMenu(Menu);
        HBox->addWidget(Pick);

        /* Graph: actions open their handler; one the graph lacks is flagged. */
        const QStringList Named = EditorUIGraph::FindGraphActions(InValue);
        if(!Named.isEmpty())
        {
            auto* Go = new QToolButton();
            Go->setText(QStringLiteral("\u2192"));
            Go->setToolTip(QStringLiteral("Open %1 in the UI graph").arg(Named.front()));
            const QString First = Named.front();
            connect(Go, &QToolButton::clicked, this, [this, First]()
            {
                if(!EditorUIGraph::RevealHandler(Design, First, window()))
                {
                    emit StatusText(QStringLiteral("The UI graph has no UI Event named %1").arg(First));
                }
            });
            HBox->addWidget(Go);

            const QStringList Have = EditorUIGraph::GetHandlers(Design);
            QStringList Missing;
            for(const QString& Each : Named) { if(!Have.contains(Each)) Missing << Each; }
            if(!Missing.isEmpty())
            {
                Edit->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid #d9a030; }"));
                Edit->setToolTip(Design.GetAsset().Settings.Graph.empty()
                                 ? QStringLiteral("The document has no UI graph for Graph: to call")
                                 : QStringLiteral("The UI graph has no UI Event named %1").arg(Missing.join(QStringLiteral(", "))));
                Edit->setProperty("MissingHandler", true);
            }
        }
    }

    if(InDesc.Flags & UI::UPF_Bindable)
    {
        Edit->setPlaceholderText(QStringLiteral("value, or {Bind:Key}"));
    }
    return Row;
}

QWidget* UIDetailsPanel::MakeGraphRow(const QString& InValue, const std::function<bool(const QString&)>& InWrite)
{
    auto* Row  = new QWidget();
    auto* HBox = new QHBoxLayout(Row);
    HBox->setContentsMargins(0, 0, 0, 0);
    HBox->setSpacing(2);
    auto* Edit = new QLineEdit(InValue);
    Edit->setPlaceholderText(QStringLiteral("none -- Create makes one beside the document"));
    connect(Edit, &QLineEdit::editingFinished, this, [Edit, InWrite]() { InWrite(Edit->text().trimmed()); });
    HBox->addWidget(Edit, 1);

    auto* Button = new QToolButton();
    Button->setText(InValue.isEmpty() ? QStringLiteral("Create") : QStringLiteral("Open"));
    Button->setToolTip(InValue.isEmpty() ? QStringLiteral("Make <Document>.ui.nmgraph beside it and set it here")
                                         : QStringLiteral("Open the UI graph in the Graph Editor"));
    connect(Button, &QToolButton::clicked, this, [this]()
    {
        const QString Path = EditorUIGraph::EnsureGraph(Design, window(), false);
        if(Path.isEmpty())
        {
            emit StatusText(QStringLiteral("No UI graph: save the document first"));
            return;
        }
        EditorGraphs::OpenGraph(Path, window());
    });
    HBox->addWidget(Button);
    return Row;
}

void UIDetailsPanel::BuildWidget(const int InWidget)
{
    const UI::UIDocumentAsset& Asset = Design.GetAsset();
    const UI::UIWidgetDesc&    Desc  = Asset.Widgets[size_t(InWidget)];
    const UI::UIWidgetType*    Type  = UI::FindWidgetType(Desc.Type.c_str());
    if(!Type)
    {
        Layout->addWidget(new QLabel(QStringLiteral("'%1' is not a registered widget type (a game module's, not loaded?)")
                                     .arg(QString::fromStdString(Desc.Type))));
        return;
    }

    auto* Title = new QLabel(QStringLiteral("<b>%1</b>").arg(QString::fromStdString(Type->Name)));
    Layout->addWidget(Title);

    /* Which slot its parent reads: a Canvas's, a box's, or none (the root). */
    const UI::UIWidgetType* ParentType = Desc.Parent >= 0 ? UI::FindWidgetType(Asset.Widgets[size_t(Desc.Parent)].Type.c_str()) : nullptr;
    const UI::UIWidgetType* Canvas     = UI::FindWidgetType("Canvas");
    const UI::UIWidgetType* Grid       = UI::FindWidgetType("Grid");
    const bool bCanvasSlot = ParentType && Canvas && ParentType->IsA(*Canvas);
    const bool bGridSlot   = ParentType && Grid && ParentType->IsA(*Grid);
    const bool bBoxSlot    = ParentType && !bCanvasSlot && !bGridSlot;

    const std::vector<UI::UIPropertyText> Effective = Asset.GetEffectiveProperties(InWidget);
    const std::map<std::string, std::string>& TypeDefaults = GetDefaults(*Type);

    std::map<std::string, QFormLayout*> Groups;
    for(const UI::UIPropertyDesc& Each : Type->Properties)
    {
        if(Each.Flags & UI::UPF_EditorHidden) continue;
        const bool bSlot = Each.Flags & (UI::UPF_SlotCanvas | UI::UPF_SlotBox | UI::UPF_SlotGrid);
        if(bSlot && !((bCanvasSlot && (Each.Flags & UI::UPF_SlotCanvas)) || (bBoxSlot && (Each.Flags & UI::UPF_SlotBox))
                      || (bGridSlot && (Each.Flags & UI::UPF_SlotGrid)))) continue;
        if(Each.Name == "Class") continue;   // shown with the title, below

        QFormLayout*& Form = Groups[Each.Category];
        if(!Form)
        {
            auto* Box = new QGroupBox(QString::fromStdString(Each.Category == "Slot" ? (bCanvasSlot ? "Slot (Canvas)" : bGridSlot ? "Slot (Grid)" : "Slot") : Each.Category));
            Form = new QFormLayout(Box);
            Form->setLabelAlignment(Qt::AlignLeft);
            Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            Layout->addWidget(Box);
        }

        /* Its value and where it comes from. */
        const std::string* Own = Desc.Find(Each.Name);
        const UI::UIPropertyText* Styled = nullptr;
        for(const UI::UIPropertyText& Value : Effective) { if(Value.Name == Each.Name) Styled = &Value; }
        const auto Default = TypeDefaults.find(Each.Name);
        const QString Value = QString::fromStdString(Own ? *Own : Styled ? Styled->Value : Default != TypeDefaults.end() ? Default->second : std::string());

        const QString Name = QString::fromStdString(Each.Name);
        const QString Short = Name.contains(QLatin1Char('.')) ? Name.section(QLatin1Char('.'), 1) : Name;
        auto* Label = new QLabel(Own ? QStringLiteral("<b>%1</b>").arg(Short) : Styled ? QStringLiteral("<i>%1</i>").arg(Short) : Short);
        Label->setToolTip(QStringLiteral("%1%2%3").arg(Name, Each.Tooltip.empty() ? QString() : QStringLiteral(": ") + QString::fromStdString(Each.Tooltip),
                                                       Own ? QStringLiteral(" (set here)") : Styled ? QStringLiteral(" (from a style)") : QStringLiteral(" (its default)")));

        UIDesignDocument* DesignPtr = &Design;
        QWidget* Field = MakeEditor(Each, Value, [DesignPtr, InWidget, Name](const QString& InText)
        {
            DesignPtr->SetProperty(InWidget, Name, InText);
            return true;
        }, InWidget);
        Editors[Name] = Field;

        if(Own)
        {
            auto* Row  = new QWidget();
            auto* HBox = new QHBoxLayout(Row);
            HBox->setContentsMargins(0, 0, 0, 0);
            HBox->addWidget(Field, 1);
            auto* Reset = new QToolButton();
            Reset->setText(QStringLiteral("x"));
            Reset->setToolTip(QStringLiteral("Back to its style's or its type's value"));
            connect(Reset, &QToolButton::clicked, this, [DesignPtr, InWidget, Name]() { DesignPtr->ResetProperty(InWidget, Name); });
            HBox->addWidget(Reset);
            Form->addRow(Label, Row);
        }
        else
        {
            Form->addRow(Label, Field);
        }
    }

    /* Its styles: names from the document's <Styles>, space separated. */
    auto* StyleBox  = new QGroupBox(QStringLiteral("Styles"));
    auto* StyleForm = new QFormLayout(StyleBox);
    const std::string* Class = Desc.Find("Class");
    auto* ClassEdit = new QLineEdit(QString::fromStdString(Class ? *Class : std::string()));
    QStringList Known;
    for(const UI::UIStyle& Each : Asset.Styles) { Known << QString::fromStdString(Each.Name); }
    ClassEdit->setPlaceholderText(Known.isEmpty() ? QStringLiteral("no styles in this document") : Known.join(QLatin1Char(' ')));
    UIDesignDocument* DesignPtr = &Design;
    connect(ClassEdit, &QLineEdit::editingFinished, this, [DesignPtr, InWidget, ClassEdit]()
    {
        const QString Text = ClassEdit->text().trimmed();
        if(Text.isEmpty()) DesignPtr->ResetProperty(InWidget, QStringLiteral("Class"));
        else               DesignPtr->SetProperty(InWidget, QStringLiteral("Class"), Text);
    });
    Editors[QStringLiteral("Class")] = ClassEdit;
    StyleForm->addRow(QStringLiteral("Class"), ClassEdit);
    Layout->insertWidget(1, StyleBox);
}

void UIDetailsPanel::BuildDocument()
{
    Layout->addWidget(new QLabel(QStringLiteral("<b>Document</b> -- select a widget to edit it")));

    auto* Box  = new QGroupBox(QStringLiteral("Settings"));
    auto* Form = new QFormLayout(Box);
    Layout->addWidget(Box);

    UIDesignDocument* DesignPtr = &Design;
    const UI::UIDocumentSettings& Settings = Design.GetAsset().Settings;
    const UI::UIDocumentSettings  Defaults;
    const auto Value = [&Settings](const char* InName)
    {
        for(const UI::UIPropertyText& Each : Settings.ToText()) { if(Each.Name == InName) return QString::fromStdString(Each.Value); }
        return QString();
    };
    const auto Write = [DesignPtr](const QString& InName)
    {
        return [DesignPtr, InName](const QString& InText)
        {
            UI::UIDocumentSettings Probe = DesignPtr->GetAsset().Settings;
            if(!Probe.SetFromText(Utf8(InName), Utf8(InText))) return false;
            DesignPtr->Edit(QStringLiteral("Set %1").arg(InName), [InName, InText](UI::UIDocumentAsset& InAsset)
            {
                InAsset.Settings.SetFromText(Utf8(InName), Utf8(InText));
            }, QStringLiteral("doc:%1").arg(InName));
            return true;
        };
    };

    /* Each setting as a property of its kind, for MakeEditor. */
    struct Setting { const char* Name; UI::EUIPropertyType Type; UI::UIEnumNames Choices; UI::EUIAssetKind Kind; const char* Tip; };
    const Setting Rows[] = {
        { "ReferenceSize", UI::EUIPropertyType::Vec2,   {}, UI::EUIAssetKind::None,   "The resolution it is laid out at, before scaling" },
        { "ScaleMode",     UI::EUIPropertyType::Enum,   UI::GetEnumNames(UI::EUIScaleMode()), UI::EUIAssetKind::None, "How the reference size meets the screen" },
        { "InputMode",     UI::EUIPropertyType::Enum,   UI::GetEnumNames(UI::EUIInputMode()), UI::EUIAssetKind::None, "UIOnly: a menu, takes all input. GameAndUI: a HUD" },
        { "Layer",         UI::EUIPropertyType::Int,    {}, UI::EUIAssetKind::None,   "Higher draws over, and takes input first" },
        { "Script",        UI::EUIPropertyType::Asset,  {}, UI::EUIAssetKind::Script, "A .lua run once when it first opens: its handlers" },
        { "Graph",         UI::EUIPropertyType::Asset,  {}, UI::EUIAssetKind::None,   "A UI graph (.ui.nmgraph) run for each open copy: Graph: actions call its UI Events" },
        { "DefaultFont",   UI::EUIPropertyType::Asset,  {}, UI::EUIAssetKind::Font,   "Empty: the engine's" },
        { "OnOpen",        UI::EUIPropertyType::Action, {}, UI::EUIAssetKind::None,   "Run when it opens" },
        { "OnClose",       UI::EUIPropertyType::Action, {}, UI::EUIAssetKind::None,   "Run when it closes" },
        { "OnBack",        UI::EUIPropertyType::Action, {}, UI::EUIAssetKind::None,   "Run on Back (Escape, gamepad East) while it has input" },
    };
    for(const Setting& Each : Rows)
    {
        UI::UIPropertyDesc Desc;
        Desc.Name      = Each.Name;
        Desc.Type      = Each.Type;
        Desc.Choices   = Each.Choices;
        Desc.AssetKind = Each.Kind;
        auto* Label = new QLabel(QString::fromLatin1(Each.Name));
        Label->setToolTip(QString::fromLatin1(Each.Tip));
        QWidget* Field = std::string(Each.Name) == "Graph" ? MakeGraphRow(Value(Each.Name), Write(QString::fromLatin1(Each.Name)))
                                                           : MakeEditor(Desc, Value(Each.Name), Write(QString::fromLatin1(Each.Name)), -1);
        Editors[QString::fromLatin1(Each.Name)] = Field;
        Form->addRow(Label, Field);
    }

    /* Styles, as the XML has them: edited as XML, applied as a whole. */
    auto* StyleBox    = new QGroupBox(QStringLiteral("Styles"));
    auto* StyleLayout = new QVBoxLayout(StyleBox);
    auto* Text        = new QPlainTextEdit();
    UI::UIDocumentAsset Only;
    Only.Styles = Design.GetAsset().Styles;
    UI::UIWidgetDesc Root;
    Root.Type = "Canvas";
    Only.Widgets.push_back(Root);
    std::string Xml = UI::WriteUIXml(Only);
    const size_t Begin = Xml.find("<Styles>"), End = Xml.find("</Styles>");
    Text->setPlainText(Begin != std::string::npos && End != std::string::npos
                       ? QString::fromStdString(Xml.substr(Begin, End + 9 - Begin))
                       : QStringLiteral("<Styles>\n  <Style Name=\"MyStyle\" Type=\"Text\" FontSize=\"32\"/>\n</Styles>"));
    Text->setMinimumHeight(160);
    StyleLayout->addWidget(Text);
    auto* Apply = new QPushButton(QStringLiteral("Apply Styles"));
    StyleLayout->addWidget(Apply);
    connect(Apply, &QPushButton::clicked, this, [this, DesignPtr, Text]()
    {
        UI::UIDocumentAsset Parsed;
        std::string Error;
        const std::string Wrapped = "<UI>" + Utf8(Text->toPlainText()) + "<Canvas/></UI>";
        if(!UI::ParseUIXml(Wrapped, Parsed, &Error))
        {
            emit StatusText(QStringLiteral("Styles: %1").arg(QString::fromStdString(Error)));
            return;
        }
        DesignPtr->Edit(QStringLiteral("Edit Styles"), [Parsed](UI::UIDocumentAsset& InAsset) { InAsset.Styles = Parsed.Styles; });
    });
    Layout->addWidget(StyleBox);
}

#endif // WITH_EDITOR
