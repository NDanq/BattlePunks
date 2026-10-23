#if WITH_EDITOR

#include "Private/Editor/Graph/GraphDetailsPanel.h"
#include "Private/Editor/EditorIcons.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "Public/UI/UIDocument.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphScene.h"

using namespace NodeGraph;
using EditorGraphs::ToQ;
using EditorGraphs::ToStd;

namespace
{
    QLabel* Heading(const QString& InText)
    {
        auto* Label = new QLabel(InText);
        QFont Bold = Label->font();
        Bold.setBold(true);
        Label->setFont(Bold);
        return Label;
    }

    QLabel* Note(const QString& InText)
    {
        auto* Label = new QLabel(InText);
        Label->setWordWrap(true);
        Label->setForegroundRole(QPalette::PlaceholderText);
        return Label;
    }

    /* A line edit that commits once, when editing finishes with a change. */
    QLineEdit* CommitEdit(const QString& InValue, std::function<void(const QString&)> InCommit)
    {
        auto* Edit = new QLineEdit(InValue);
        QObject::connect(Edit, &QLineEdit::editingFinished, Edit, [Edit, InValue, InCommit]()
        {
            if(Edit->text() != InValue) { InCommit(Edit->text()); }
        });
        return Edit;
    }

    /*
     * A Texture setting: the texture's runtime path ("Content/Textures/Brick.nmtexture"),
     * typed, or dropped from the Content Browser (or Explorer, if in the project).
     * Commits once per change, like CommitEdit.
     */
    class TexturePathEdit final : public QLineEdit
    {
    public:
        TexturePathEdit(const QString& InValue, std::function<void(const QString&)> InCommit)
            : QLineEdit(InValue), Commit(std::move(InCommit)), Committed(InValue)
        {
            setAcceptDrops(true);
            setPlaceholderText(QStringLiteral("Drop a .nmtexture (or an .nmrt2d / .nmrt3d) here"));
            connect(this, &QLineEdit::editingFinished, this, [this]() { Set(text().trimmed()); });
        }

        /* Commit last: the panel may rebuild, and delete this, in it. */
        void Set(const QString& InValue)
        {
            if(InValue == Committed) { return; }
            Committed = InValue;
            setText(InValue);
            Commit(InValue);
        }

        /* What the setting stores for a file: empty if not a texture in the
         * project -- a .nmtexture, or a render target (.nmrt2d, .nmrt3d). */
        static QString ValueFor(const QString& InFile)
        {
            if(InFile.endsWith(QStringLiteral(".nmtexture"), Qt::CaseInsensitive)) { return EditorContent::ToRuntimePath(InFile); }
            return EditorContent::RenderTargetPathFor(InFile);
        }

    protected:
        void dragEnterEvent(QDragEnterEvent* InEvent) override { Accept(InEvent); }
        void dragMoveEvent(QDragMoveEvent* InEvent) override   { Accept(InEvent); }
        void dropEvent(QDropEvent* InEvent) override
        {
            const QString Value = ValueFor(EditorContent::SingleDroppedFile(InEvent->mimeData()));
            if(Value.isEmpty())
            {
                InEvent->ignore();
                return;
            }
            InEvent->acceptProposedAction();
            Set(Value);
        }

    private:
        void Accept(QDropEvent* InEvent)
        {
            if(!ValueFor(EditorContent::SingleDroppedFile(InEvent->mimeData())).isEmpty()) { InEvent->acceptProposedAction(); }
            else { InEvent->ignore(); }
        }

        std::function<void(const QString&)> Commit;
        QString                             Committed;
    };

    /* The literal's numbers as a colour, and back, for the picker. Script
     * colours are 0-255 integers; material vectors 0-1 floats. */
    QColor LiteralToColor(const std::string& InValue, const bool bInBytes)
    {
        QStringList Parts = ToQ(InValue).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        while(Parts.size() < 4) { Parts << (Parts.isEmpty() ? QStringLiteral("0") : Parts.back()); }
        const auto Channel = [&](const int InIndex) { return bInBytes ? Parts[InIndex].toDouble() / 255.0 : Parts[InIndex].toDouble(); };
        return QColor::fromRgbF(float(std::clamp(Channel(0), 0.0, 1.0)), float(std::clamp(Channel(1), 0.0, 1.0)),
                                float(std::clamp(Channel(2), 0.0, 1.0)), float(std::clamp(Channel(3), 0.0, 1.0)));
    }

    std::string ColorToLiteral(const QColor& InColor, const bool bInBytes, const int InChannels)
    {
        const double Values[4] = { InColor.redF(), InColor.greenF(), InColor.blueF(), InColor.alphaF() };
        QStringList Parts;
        for(int Index = 0; Index < InChannels; ++Index)
        {
            Parts << (bInBytes ? QString::number(int(std::lround(Values[Index] * 255.0)))
                               : QString::number(std::round(Values[Index] * 1000.0) / 1000.0));
        }
        return ToStd(Parts.join(QLatin1Char(' ')));
    }
}

GraphDetailsPanel::GraphDetailsPanel(GraphDocument& InDocument, GraphScene& InScene, QWidget* InParent)
    : QScrollArea(InParent)
    , Document(InDocument)
    , Scene(InScene)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setMinimumWidth(260);

    connect(&Scene, &QGraphicsScene::selectionChanged, this, &GraphDetailsPanel::Refresh);
    connect(&Document, &GraphDocument::Changed, this, &GraphDetailsPanel::Refresh, Qt::QueuedConnection);
    Refresh();
}

void GraphDetailsPanel::Refresh()
{
    const std::vector<NodeGuid> Selected = Scene.GetSelectedNodes();
    const Graph& G = Document.GetGraph();

    /* What would be shown, as text: unchanged means nothing to rebuild. */
    std::string Next;
    if(Selected.size() == 1)
    {
        Next = "node:" + WriteGraphFragment(G, Selected);
        for(const Variable& Each : G.Variables) { Next += Each.Name + ":" + Each.Type + ";"; }
        for(const GraphFunction& Each : G.Functions)
        {
            Next += Each.Name + "(";
            for(const FunctionPin& Pin : Each.Inputs)  { Next += Pin.Name + ":" + Pin.Type + ","; }
            Next += ")->(";
            for(const FunctionPin& Pin : Each.Outputs) { Next += Pin.Name + ":" + Pin.Type + ","; }
            Next += ");";
        }
        for(const Link& Each : G.Links)
        {
            if(Each.To.Node == Selected.front() || Each.From.Node == Selected.front()) { Next += Each.From.Pin + ">" + Each.To.Pin + ";"; }
        }
    }
    else if(Selected.empty())
    {
        Graph Settings;
        Settings.SchemaName = G.SchemaName;
        Settings.Properties = G.Properties;
        Settings.Variables  = G.Variables;
        Settings.Functions  = G.Functions;
        Next = "graph:" + WriteGraphText(Settings);
    }
    else
    {
        Next = "many:" + std::to_string(Selected.size());
    }
    if(Next == Shown && widget()) { return; }
    Shown = Next;

    auto* Page = new QWidget();
    auto* Form = new QFormLayout(Page);
    Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    if(Selected.size() == 1)
    {
        if(const Node* Found = G.FindNode(Selected.front())) { BuildNode(Form, *Found); }
    }
    else if(Selected.empty())
    {
        BuildGraph(Form);
    }
    else
    {
        Form->addRow(Note(QStringLiteral("%1 nodes selected.").arg(Selected.size())));
    }

    setWidget(Page);   // deletes the previous page
}

void GraphDetailsPanel::BuildNode(QFormLayout* InForm, const Node& InNode)
{
    const NodeDefinition* Def = Document.FindDefinition(InNode.Id);
    const NodeGuid Id = InNode.Id;

    if(!Def)
    {
        InForm->addRow(Heading(ToQ(InNode.Type)));
        InForm->addRow(Note(QStringLiteral("This node's type is not known to the %1 schema. It is kept as it was, "
                                           "links and all, but the graph will not compile until it is deleted or "
                                           "the type returns.").arg(ToQ(Document.GetSchema().GetName()))));
        return;
    }

    InForm->addRow(Heading(ToQ(Def->DisplayName)));
    InForm->addRow(Note(ToQ(Def->Type) + (Def->Tooltip.empty() ? QString() : QStringLiteral("\n") + ToQ(Def->Tooltip))));

    // --- Type --------------------------------------------------------------------
    if(Def->HasWildcard() && !Def->bWildcardFromSchema)
    {
        auto* Combo = new QComboBox();
        const std::string Resolved = Document.GetSchema().GetWildcardType(Document.GetGraph(), InNode);
        Combo->addItem(QStringLiteral("Automatic (%1)").arg(ToQ(Resolved)), QString());
        for(const std::string& Type : Def->WildcardTypes) { Combo->addItem(ToQ(Type), ToQ(Type)); }
        Combo->setCurrentIndex(std::max(0, Combo->findData(ToQ(InNode.WildcardType))));
        connect(Combo, QOverload<int>::of(&QComboBox::activated), this, [this, Combo, Id](int)
        {
            Document.SetWildcardType(Id, ToStd(Combo->currentData().toString()));
        });
        InForm->addRow(QStringLiteral("Type"), Combo);
    }

    // --- Properties ------------------------------------------------------------
    for(const PropertyDefinition& Property : Def->Properties)
    {
        const std::string Value = InNode.GetProperty(Property.Name, Property.Default);
        const std::string Name  = Property.Name;
        QWidget* Editor = nullptr;

        if(Property.Kind == EPropertyKind::Bool)
        {
            auto* Check = new QCheckBox();
            Check->setChecked(Value == "true");
            connect(Check, &QCheckBox::toggled, this, [this, Id, Name](const bool bInOn)
            {
                Document.SetProperty(Id, Name, bInOn ? "true" : "false");
            });
            Editor = Check;
        }
        else if(Property.Kind == EPropertyKind::Choice || Property.Kind == EPropertyKind::Variable
                || Property.Kind == EPropertyKind::Function)
        {
            auto* Combo = new QComboBox();
            if(Property.Kind == EPropertyKind::Choice)
            {
                for(const std::string& Choice : Property.Choices) { Combo->addItem(ToQ(Choice)); }
            }
            else if(Property.Kind == EPropertyKind::Variable)
            {
                for(const Variable& Each : Document.GetGraph().Variables) { Combo->addItem(ToQ(Each.Name)); }
                if(Combo->findText(ToQ(Value)) < 0) { Combo->addItem(ToQ(Value)); }   // a missing variable stays visible
            }
            else
            {
                for(const GraphFunction& Each : Document.GetGraph().Functions) { Combo->addItem(ToQ(Each.Name)); }
                if(Combo->findText(ToQ(Value)) < 0) { Combo->addItem(ToQ(Value)); }   // and a missing function
            }
            Combo->setCurrentText(ToQ(Value));
            connect(Combo, QOverload<int>::of(&QComboBox::activated), this, [this, Combo, Id, Name](int)
            {
                Document.SetProperty(Id, Name, ToStd(Combo->currentText()));
            });
            Editor = Combo;
        }
        else if(Property.Kind == EPropertyKind::WidgetName)
        {
            /* The names in the UI document the graph is the logic of; anything may be typed. */
            auto* Combo = new QComboBox();
            Combo->setEditable(true);
            const std::string DocumentPath = Document.GetGraph().GetProperty("Document");
            if(std::shared_ptr<const UI::UIDocumentAsset> Asset = DocumentPath.empty() ? nullptr : UI::UIAssetCache::Find(DocumentPath))
            {
                for(const UI::UIWidgetDesc& Each : Asset->Widgets)
                {
                    if(!Each.GetName().empty()) { Combo->addItem(ToQ(Each.GetName())); }
                }
            }
            Combo->setCurrentText(ToQ(Value));
            const auto Commit = [this, Combo, Id, Name]() { Document.SetProperty(Id, Name, ToStd(Combo->currentText())); };
            connect(Combo, QOverload<int>::of(&QComboBox::activated), this, Commit);
            connect(Combo->lineEdit(), &QLineEdit::editingFinished, this, Commit);
            Editor = Combo;
        }
        else if(Property.Kind == EPropertyKind::Texture)
        {
            auto* Row    = new QWidget();
            auto* Layout = new QHBoxLayout(Row);
            Layout->setContentsMargins(0, 0, 0, 0);
            auto* Edit = new TexturePathEdit(ToQ(Value), [this, Id, Name](const QString& InText) { Document.SetProperty(Id, Name, ToStd(InText)); });
            Layout->addWidget(Edit, 1);

            auto* Pick = new QToolButton();
            Pick->setIcon(EditorIcons::Get(EditorIcons::EIcon::Browse));
            Pick->setToolTip(QStringLiteral("Choose a texture in the project"));
            Layout->addWidget(Pick);
            connect(Pick, &QToolButton::clicked, this, [this, Edit]()
            {
                const QString Chosen = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a Texture"), EditorGraphs::GetProjectRoot(),
                                                                    QStringLiteral("Textures (*.nmtexture);;Render Targets (*.nmrt2d *.nmrt3d)"));
                const QString Value  = Chosen.isEmpty() ? QString() : TexturePathEdit::ValueFor(Chosen);
                if(!Value.isEmpty()) { Edit->Set(Value); }
            });
            Layout->addWidget(EditorContent::MakeRevealButton([Edit]() { return Edit->text(); }, Row));
            Editor = Row;
        }
        else
        {
            Editor = CommitEdit(ToQ(Value), [this, Id, Name](const QString& InText) { Document.SetProperty(Id, Name, ToStd(InText)); });
        }
        Editor->setToolTip(ToQ(Property.Tooltip));
        InForm->addRow(ToQ(Property.Name), Editor);
    }

    // --- Literal inputs ----------------------------------------------------------
    bool bHeading = false;
    for(const PinDefinition& Pin : Def->Pins)
    {
        if(!Pin.IsInput() || Pin.IsExec()) { continue; }
        const PinRef Ref{ Id, Pin.Name };
        if(Document.IsLinked(Ref)) { continue; }
        if(!Pin.Implicit.empty() && !InNode.FindConstant(Pin.Name)) { continue; }   // no literal: the schema supplies it

        const std::string Type = Document.GetPinType(Ref);
        const PinTypeInfo* Info = Document.GetSchema().FindType(Type);
        if(!Info || !Info->bHasLiteral) { continue; }

        if(!bHeading)
        {
            InForm->addRow(Heading(QStringLiteral("Inputs")));
            bHeading = true;
        }
        const std::string* Given = InNode.FindConstant(Pin.Name);
        const std::string Value = Given ? *Given : (!Pin.Default.empty() ? Pin.Default : Info->DefaultLiteral);
        QWidget* Editor = MakeLiteralEditor(Ref, Type, Value);
        Editor->setToolTip(QStringLiteral("%1 (%2)%3").arg(ToQ(Pin.Name), ToQ(Info->DisplayName),
                                                          Pin.Tooltip.empty() ? QString() : QStringLiteral("\n") + ToQ(Pin.Tooltip)));
        InForm->addRow(ToQ(Pin.Name), Editor);
    }
}

QWidget* GraphDetailsPanel::MakeLiteralEditor(const PinRef& InPin, const std::string& InType, const std::string& InValue)
{
    if(InType == "bool")
    {
        auto* Check = new QCheckBox();
        Check->setChecked(InValue == "true" || InValue == "1");
        connect(Check, &QCheckBox::toggled, this, [this, InPin](const bool bInOn)
        {
            Document.SetConstant(InPin, bInOn ? "true" : "false");
        });
        return Check;
    }

    QLineEdit* Edit = CommitEdit(ToQ(InValue), [this, InPin](const QString& InText) { Document.SetConstant(InPin, ToStd(InText.trimmed())); });

    /* A colour-like value gets a picker beside its text. */
    const bool bScriptColor = InType == "Color";
    const bool bMaterialColor = Document.GetSchema().GetName() == "Material" && (InType == "vec3" || InType == "vec4");
    if(!bScriptColor && !bMaterialColor) { return Edit; }

    auto* Row = new QWidget();
    auto* Layout = new QHBoxLayout(Row);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->addWidget(Edit, 1);

    auto* Pick = new QToolButton();
    Pick->setText(QStringLiteral("..."));
    Pick->setToolTip(QStringLiteral("Pick a colour"));
    const int Channels = InType == "vec3" ? 3 : 4;
    connect(Pick, &QToolButton::clicked, this, [this, InPin, InValue, bScriptColor, Channels]()
    {
        const QColor Picked = QColorDialog::getColor(LiteralToColor(InValue, bScriptColor), this, QStringLiteral("Pick a Colour"),
                                                     Channels == 4 ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions());
        if(Picked.isValid()) { Document.SetConstant(InPin, ColorToLiteral(Picked, bScriptColor, Channels)); }
    });
    Layout->addWidget(Pick);
    return Row;
}

void GraphDetailsPanel::BuildGraph(QFormLayout* InForm)
{
    const Schema& S = Document.GetSchema();
    const Graph&  G = Document.GetGraph();

    InForm->addRow(Heading(QStringLiteral("%1 Graph").arg(ToQ(S.GetName()))));
    InForm->addRow(Note(QStringLiteral("Generates %1. Right-click the canvas or press Tab to add a node; drag between pins "
                                       "to link them, Alt+click a pin to break its links.").arg(ToQ(S.GetOutputExtension()))));

    for(const PropertyDefinition& Property : S.GetGraphProperties())
    {
        const std::string Name = Property.Name;
        if(Property.Kind != EPropertyKind::Choice)
        {
            QWidget* Edit = CommitEdit(ToQ(G.GetProperty(Name, Property.Default)),
                                       [this, Name](const QString& InText) { Document.SetGraphProperty(Name, ToStd(InText)); });
            Edit->setToolTip(ToQ(Property.Tooltip));
            InForm->addRow(ToQ(Name), Edit);
            continue;
        }
        auto* Combo = new QComboBox();
        for(const std::string& Choice : Property.Choices) { Combo->addItem(ToQ(Choice)); }
        Combo->setCurrentText(ToQ(G.GetProperty(Name, Property.Default)));
        Combo->setToolTip(ToQ(Property.Tooltip));
        connect(Combo, QOverload<int>::of(&QComboBox::activated), this, [this, Combo, Name](int)
        {
            Document.SetGraphProperty(Name, ToStd(Combo->currentText()));
        });
        InForm->addRow(ToQ(Name), Combo);
    }

    // --- Variables -----------------------------------------------------------------
    const std::vector<std::string>& Types = S.GetVariableTypes();
    if(Types.empty()) { return; }

    InForm->addRow(Heading(QStringLiteral("Variables")));
    if(G.Variables.empty())
    {
        InForm->addRow(Note(QStringLiteral("None yet. A variable keeps its value between events, per entity (self.Name).")));
    }

    for(size_t Index = 0; Index < G.Variables.size(); ++Index)
    {
        const Variable& Var = G.Variables[Index];
        auto* Row = new QWidget();
        auto* Layout = new QHBoxLayout(Row);
        Layout->setContentsMargins(0, 0, 0, 0);

        QLineEdit* Name = CommitEdit(ToQ(Var.Name), [this, Index](const QString& InText)
        {
            std::vector<Variable> Next = Document.GetGraph().Variables;
            Next[Index].Name = ToStd(InText.trimmed());
            Document.SetVariables(Next);
        });
        Name->setPlaceholderText(QStringLiteral("Name"));
        Layout->addWidget(Name, 2);

        auto* Type = new QComboBox();
        for(const std::string& Each : Types) { Type->addItem(ToQ(Each)); }
        Type->setCurrentText(ToQ(Var.Type));
        connect(Type, QOverload<int>::of(&QComboBox::activated), this, [this, Type, Index](int)
        {
            std::vector<Variable> Next = Document.GetGraph().Variables;
            Next[Index].Type = ToStd(Type->currentText());
            Next[Index].Default.clear();   // the old literal rarely fits the new type
            Document.SetVariables(Next);
        });
        Layout->addWidget(Type, 1);

        QLineEdit* Default = CommitEdit(ToQ(Var.Default), [this, Index](const QString& InText)
        {
            std::vector<Variable> Next = Document.GetGraph().Variables;
            Next[Index].Default = ToStd(InText.trimmed());
            Document.SetVariables(Next);
        });
        Default->setPlaceholderText(QStringLiteral("Default"));
        Layout->addWidget(Default, 1);

        auto* Remove = new QToolButton();
        Remove->setIcon(EditorIcons::Get(EditorIcons::EIcon::Close));
        Remove->setToolTip(QStringLiteral("Remove this variable"));
        connect(Remove, &QToolButton::clicked, this, [this, Index]()
        {
            std::vector<Variable> Next = Document.GetGraph().Variables;
            Next.erase(Next.begin() + std::ptrdiff_t(Index));
            Document.SetVariables(Next);
        });
        Layout->addWidget(Remove);

        InForm->addRow(Row);
    }

    auto* Add = new QPushButton(QStringLiteral("Add Variable"));
    connect(Add, &QPushButton::clicked, this, [this, Types]()
    {
        std::vector<Variable> Next = Document.GetGraph().Variables;
        std::string Name = "NewVariable";
        for(int Suffix = 2; std::any_of(Next.begin(), Next.end(), [&](const Variable& V) { return V.Name == Name; }); ++Suffix)
        {
            Name = "NewVariable" + std::to_string(Suffix);
        }
        Next.push_back({ Name, Types.front(), std::string() });
        Document.SetVariables(Next);
    });
    InForm->addRow(Add);

    BuildFunctions(InForm);
}

void GraphDetailsPanel::BuildFunctions(QFormLayout* InForm)
{
    const Schema& S = Document.GetSchema();
    const Graph&  G = Document.GetGraph();
    if(!S.FindNode("Function.Call")) { return; }   // a schema without graph functions

    /* What an input or output may be: any variable type, or an Entity. */
    std::vector<std::string> Types = S.GetVariableTypes();
    Types.push_back("Entity");

    InForm->addRow(Heading(QStringLiteral("Functions")));
    if(G.Functions.empty())
    {
        InForm->addRow(Note(QStringLiteral("None yet. A function is a chain you call from anywhere in this graph: its "
                                           "Entry node starts it, Return from it gives its outputs.")));
    }

    /* Every edit: a copy of the list, changed, as one step. */
    const auto Change = [this](const std::function<void(std::vector<GraphFunction>&)>& InChange)
    {
        std::vector<GraphFunction> Next = Document.GetGraph().Functions;
        InChange(Next);
        Document.SetFunctions(Next);
    };

    const auto UniqueName = [](const std::string& InBase, const auto& InTaken)
    {
        std::string Name = InBase;
        for(int Suffix = 2; std::any_of(InTaken.begin(), InTaken.end(), [&](const auto& Each) { return Each.Name == Name; }); ++Suffix)
        {
            Name = InBase + std::to_string(Suffix);
        }
        return Name;
    };

    for(size_t Index = 0; Index < G.Functions.size(); ++Index)
    {
        const GraphFunction& Function = G.Functions[Index];

        auto* Header = new QWidget();
        auto* HeaderLayout = new QHBoxLayout(Header);
        HeaderLayout->setContentsMargins(0, 8, 0, 0);

        QLineEdit* Name = CommitEdit(ToQ(Function.Name), [Change, Index](const QString& InText)
        {
            Change([&](std::vector<GraphFunction>& Next) { Next[Index].Name = ToStd(InText.trimmed()); });
        });
        Name->setPlaceholderText(QStringLiteral("Function name"));
        QFont Bold = Name->font();
        Bold.setBold(true);
        Name->setFont(Bold);
        HeaderLayout->addWidget(Name, 1);

        auto* Remove = new QToolButton();
        Remove->setIcon(EditorIcons::Get(EditorIcons::EIcon::Close));
        Remove->setToolTip(QStringLiteral("Remove this function (its nodes stay, and no longer compile)"));
        connect(Remove, &QToolButton::clicked, this, [Change, Index]()
        {
            Change([&](std::vector<GraphFunction>& Next) { Next.erase(Next.begin() + std::ptrdiff_t(Index)); });
        });
        HeaderLayout->addWidget(Remove);
        InForm->addRow(Header);

        /* One list: the function's inputs (bOutputs false) or its outputs. */
        for(const bool bOutputs : { false, true })
        {
            const std::vector<FunctionPin>& Pins = bOutputs ? Function.Outputs : Function.Inputs;
            const auto ListOf = [bOutputs, Index](std::vector<GraphFunction>& InFunctions) -> std::vector<FunctionPin>&
            {
                return bOutputs ? InFunctions[Index].Outputs : InFunctions[Index].Inputs;
            };

            for(size_t PinIndex = 0; PinIndex < Pins.size(); ++PinIndex)
            {
                const FunctionPin& Pin = Pins[PinIndex];
                auto* Row = new QWidget();
                auto* Layout = new QHBoxLayout(Row);
                Layout->setContentsMargins(0, 0, 0, 0);

                QLineEdit* PinName = CommitEdit(ToQ(Pin.Name), [Change, ListOf, PinIndex](const QString& InText)
                {
                    Change([&](std::vector<GraphFunction>& Next) { ListOf(Next)[PinIndex].Name = ToStd(InText.trimmed()); });
                });
                PinName->setPlaceholderText(QStringLiteral("Name"));
                Layout->addWidget(PinName, 2);

                auto* Type = new QComboBox();
                for(const std::string& Each : Types) { Type->addItem(ToQ(Each)); }
                if(Type->findText(ToQ(Pin.Type)) < 0) { Type->addItem(ToQ(Pin.Type)); }   // an unknown type stays visible
                Type->setCurrentText(ToQ(Pin.Type));
                connect(Type, QOverload<int>::of(&QComboBox::activated), this, [Change, ListOf, PinIndex, Type](int)
                {
                    Change([&](std::vector<GraphFunction>& Next) { ListOf(Next)[PinIndex].Type = ToStd(Type->currentText()); });
                });
                Layout->addWidget(Type, 1);

                auto* RemovePin = new QToolButton();
                RemovePin->setIcon(EditorIcons::Get(EditorIcons::EIcon::Close));
                RemovePin->setToolTip(bOutputs ? QStringLiteral("Remove this output") : QStringLiteral("Remove this input"));
                connect(RemovePin, &QToolButton::clicked, this, [Change, ListOf, PinIndex]()
                {
                    Change([&](std::vector<GraphFunction>& Next) { ListOf(Next).erase(ListOf(Next).begin() + std::ptrdiff_t(PinIndex)); });
                });
                Layout->addWidget(RemovePin);

                InForm->addRow(PinIndex == 0 ? (bOutputs ? QStringLiteral("Outputs") : QStringLiteral("Inputs")) : QString(), Row);
            }
        }

        auto* Buttons = new QWidget();
        auto* ButtonLayout = new QHBoxLayout(Buttons);
        ButtonLayout->setContentsMargins(0, 0, 0, 0);
        for(const bool bOutputs : { false, true })
        {
            auto* AddPin = new QPushButton(bOutputs ? QStringLiteral("Add Output") : QStringLiteral("Add Input"));
            connect(AddPin, &QPushButton::clicked, this, [Change, UniqueName, Types, Index, bOutputs]()
            {
                Change([&](std::vector<GraphFunction>& Next)
                {
                    /* Unique across both lists: a Call node has every one as a pin. */
                    std::vector<FunctionPin> Taken = Next[Index].Inputs;
                    Taken.insert(Taken.end(), Next[Index].Outputs.begin(), Next[Index].Outputs.end());
                    const std::string Name = UniqueName(bOutputs ? "Result" : "Value", Taken);
                    (bOutputs ? Next[Index].Outputs : Next[Index].Inputs).push_back({ Name, Types.front() });
                });
            });
            ButtonLayout->addWidget(AddPin);
        }
        InForm->addRow(Buttons);
    }

    auto* Add = new QPushButton(QStringLiteral("Add Function"));
    connect(Add, &QPushButton::clicked, this, [Change, UniqueName]()
    {
        Change([&](std::vector<GraphFunction>& Next)
        {
            GraphFunction Added;
            Added.Name = UniqueName("NewFunction", Next);
            Next.push_back(std::move(Added));
        });
    });
    InForm->addRow(Add);
}

#endif // WITH_EDITOR
