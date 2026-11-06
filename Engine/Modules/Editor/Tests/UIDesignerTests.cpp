/*
 * NimbleEditorUIDesignerTests -- the UI Designer's document and panels,
 * offscreen: edits and their undo, a drag or retyped value as one step,
 * saving and reading back, moves the hierarchy refuses, and the details
 * panel writing what is typed into it (and refusing what is wrong). The
 * canvas needs a renderer and is not made here; `ui screenshot` drives it.
 */

#include <cstdio>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QUndoStack>

#include "Public/UI/UIFile.h"
#include "Private/Editor/UI/UIDesignDocument.h"
#include "Private/Editor/UI/UIDetailsPanel.h"
#include "Private/Editor/UI/UIHierarchyPanel.h"
#include "Private/Editor/UI/UIGraphLink.h"
#include "Private/Editor/UI/UIPreviewRuntime.h"
#include "Public/Core/CoreConfig.h"
#include "Public/UI/UISystem.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphEditorTab.h"
#include "Private/Editor/Graph/GraphEditorWindow.h"
#include "Public/NodeGraph/GraphFile.h"

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

namespace
{
    void PumpFor(const int InMs)
    {
        QElapsedTimer Clock;
        Clock.start();
        while(Clock.elapsed() < InMs) { QApplication::processEvents(QEventLoop::AllEvents, 10); }
    }

    const char* const MENU = R"XML(<?xml version="1.0" encoding="utf-8"?>
<UI Format="1" ReferenceSize="1920,1080" ScaleMode="FitHeight" InputMode="UIOnly">
  <Styles>
    <Style Name="Big" Type="Text" FontSize="48"/>
  </Styles>
  <Canvas Name="Root">
    <VerticalBox Name="Buttons" Position="100,100" AutoSize="true">
      <Button Name="Play" OnClick="Native:Game.Quit"><Text Name="PlayLabel" Text="Play"/></Button>
      <Button Name="Quit"/>
    </VerticalBox>
    <Text Name="Title" Class="Big" Text="Hello"/>
  </Canvas>
</UI>
)XML";

    std::string Get(const UIDesignDocument& InDesign, const char* InWidget, const char* InProperty)
    {
        const int Index = InDesign.GetAsset().FindWidget(InWidget);
        const std::string* Value = Index >= 0 ? InDesign.GetAsset().Widgets[size_t(Index)].Find(InProperty) : nullptr;
        return Value ? *Value : std::string("<unset>");
    }

    void TestDocument(const QString& InDir)
    {
        std::printf("[document]\n");
        const QString Path = InDir + QStringLiteral("/Menu.nmui");
        QFile File(Path);
        File.open(QIODevice::WriteOnly);
        File.write(MENU);
        File.close();

        UIDesignDocument Design;
        int Changes = 0, Structural = 0;
        QObject::connect(&Design, &UIDesignDocument::Changed, [&](const bool bStructure) { ++Changes; if(bStructure) ++Structural; });
        Expect(Design.Load(Path) && Design.GetAsset().Widgets.size() == 6 && !Design.IsDirty(), "a document loads, clean");

        const int Play = Design.GetAsset().FindWidget("Play");
        Design.SetProperty(Play, QStringLiteral("Opacity"), QStringLiteral("0.5"));
        Design.SetProperty(Play, QStringLiteral("Opacity"), QStringLiteral("0.75"));
        Expect(Get(Design, "Play", "Opacity") == "0.75" && Design.GetUndoStack()->count() == 1, "typing a value twice is one undo step");
        Expect(Design.IsDirty(), "and the document is dirty");
        Design.SetProperty(Play, QStringLiteral("Opacity"), QStringLiteral("0.75"));
        Expect(Design.GetUndoStack()->count() == 1, "setting what is there records nothing");

        Design.GetUndoStack()->undo();
        Expect(Get(Design, "Play", "Opacity") == "<unset>" && !Design.IsDirty(), "undo puts it back, clean again");
        Design.GetUndoStack()->redo();
        Expect(Get(Design, "Play", "Opacity") == "0.75", "redo does it again");

        /* Structure: add, duplicate, delete, move. */
        Changes = Structural = 0;
        Design.Select(Design.GetAsset().FindWidget("Buttons"));
        const int Added = Design.AddWidget(QStringLiteral("Button"), Design.GetSelected(), -1);
        Expect(Added > 0 && Design.GetSelected() == Added && Structural == 1, "a widget is added to the selection and selected");
        Expect(Design.GetAsset().Widgets[size_t(Added)].GetName() == "Button", "named for its type");
        Expect(Design.AddWidget(QStringLiteral("Text"), Design.GetAsset().FindWidget("Play"), -1) < 0, "a full Button refuses a second child");
        Expect(Design.AddWidget(QStringLiteral("BoxWidget"), 0, -1) < 0, "an abstract type cannot be added");

        Design.Select(Design.GetAsset().FindWidget("Play"));
        Design.DuplicateSelected();
        Expect(Design.GetAsset().FindWidget("PlayCopy") == Design.GetSelected() && Design.GetAsset().FindWidget("PlayLabelCopy") > 0,
               "duplicating copies the subtree under new names, beside it");
        Expect(Design.GetAsset().Validate(), "and the document stays valid");

        const int Title = Design.GetAsset().FindWidget("Title");
        Expect(Design.MoveWidget(Title, Design.GetAsset().FindWidget("Buttons"), -1), "a widget moves into a box");
        Expect(Design.GetAsset().Widgets[size_t(Design.GetAsset().FindWidget("Title"))].Parent == Design.GetAsset().FindWidget("Buttons"),
               "under its new parent");
        Expect(!Design.MoveWidget(Design.GetAsset().FindWidget("Buttons"), Design.GetAsset().FindWidget("Title"), -1),
               "a box cannot move into its own child");

        const QString Copied = (Design.Select(Design.GetAsset().FindWidget("Quit")), Design.CopySelected());
        Design.Select(0);
        Expect(Design.Paste(Copied) && Design.GetAsset().FindWidget("QuitCopy") > 0, "a copy pastes into the selection");

        Design.Select(Design.GetAsset().FindWidget("QuitCopy"));
        Design.DeleteSelected();
        Expect(Design.GetAsset().FindWidget("QuitCopy") < 0 && Design.GetSelected() == 0, "delete takes it and selects its parent");

        /* Undo all the way: the file as loaded. */
        while(Design.GetUndoStack()->canUndo()) { Design.GetUndoStack()->undo(); }
        UI::UIDocumentAsset Original;
        UI::ParseUIXml(MENU, Original);
        Expect(UI::WriteUIXml(Design.GetAsset()) == UI::WriteUIXml(Original), "undoing everything gives back the file");

        /* Save and read back. */
        Design.GetUndoStack()->redo();
        Expect(Design.Save() && !Design.IsDirty(), "saving makes it clean");
        UIDesignDocument Again;
        Expect(Again.Load(Path) && Get(Again, "Play", "Opacity") == "0.75", "what was saved reads back");
        Expect(UI::WriteUIXml(Again.GetAsset()) == UI::WriteUIXml(Design.GetAsset()), "exactly");
    }

    void TestDetails(const QString& InDir)
    {
        std::printf("[details]\n");
        const QString Path = InDir + QStringLiteral("/Details.nmui");
        QFile File(Path);
        File.open(QIODevice::WriteOnly);
        File.write(MENU);
        File.close();

        UIDesignDocument Design;
        Design.Load(Path);
        UIDetailsPanel   Details(Design);
        UIHierarchyPanel Hierarchy(Design);
        Details.resize(400, 900);
        Details.show();

        Design.Select(Design.GetAsset().FindWidget("Title"));
        PumpFor(50);
        QWidget* TextField = Details.FindEditor(QStringLiteral("Text"));
        QLineEdit* Text = TextField ? TextField->findChild<QLineEdit*>() : nullptr;
        Expect(Text && Text->text() == QStringLiteral("Hello"), "the Text field shows the widget's text");
        QWidget* SizeField = Details.FindEditor(QStringLiteral("FontSize"));
        QLineEdit* Size = SizeField ? SizeField->findChild<QLineEdit*>() : nullptr;
        Expect(Size && Size->text() == QStringLiteral("48"), "a styled value shows its style's");
        Expect(Details.FindEditor(QStringLiteral("Anchors")) != nullptr && Details.FindEditor(QStringLiteral("HAlign")) == nullptr,
               "a Canvas child shows the Canvas slot, not a box's");

        if(Text)
        {
            Text->setText(QStringLiteral("Score: {Bind:Player0.Score}"));
            emit Text->editingFinished();
            PumpFor(30);
            Expect(Get(Design, "Title", "Text") == "Score: {Bind:Player0.Score}", "a binding is written to a bindable String");
        }
        if((Size = Details.FindEditor(QStringLiteral("FontSize")) ? Details.FindEditor(QStringLiteral("FontSize"))->findChild<QLineEdit*>() : nullptr))
        {
            const int Steps = Design.GetUndoStack()->count();
            Size->setText(QStringLiteral("big"));
            emit Size->editingFinished();
            PumpFor(30);
            Expect(Get(Design, "Title", "FontSize") == "<unset>" && Design.GetUndoStack()->count() == Steps, "a bad number is refused, nothing written");
            Size->setText(QStringLiteral("64"));
            emit Size->editingFinished();
            PumpFor(30);
            Expect(Get(Design, "Title", "FontSize") == "64", "a good one is written");
        }

        QWidget* WrapField = Details.FindEditor(QStringLiteral("Wrap"));
        QCheckBox* Wrap = qobject_cast<QCheckBox*>(WrapField);
        Expect(Wrap != nullptr, "a bool is a check box");
        if(Wrap)
        {
            Wrap->setChecked(true);
            PumpFor(30);
            Expect(Get(Design, "Title", "Wrap") == "true", "ticking it writes true");
        }
        QComboBox* Justify = qobject_cast<QComboBox*>(Details.FindEditor(QStringLiteral("Justify")));
        Expect(Justify && Justify->count() == 3, "an enum is a combo of its names");

        /* A box child: the box slot, not the Canvas's. */
        Design.Select(Design.GetAsset().FindWidget("Quit"));
        PumpFor(30);
        Expect(Details.FindEditor(QStringLiteral("HAlign")) != nullptr && Details.FindEditor(QStringLiteral("Anchors")) == nullptr,
               "a box child shows the box slot");

        /* Nothing selected: the document's settings. */
        Design.Select(-1);
        PumpFor(30);
        QComboBox* Mode = qobject_cast<QComboBox*>(Details.FindEditor(QStringLiteral("InputMode")));
        Expect(Mode && Mode->currentText() == QStringLiteral("UIOnly"), "with nothing selected, the document's settings");
        if(Mode)
        {
            Mode->setCurrentText(QStringLiteral("GameAndUI"));
            emit Mode->activated(Mode->currentIndex());
            PumpFor(30);
            Expect(Design.GetAsset().Settings.InputMode == UI::EUIInputMode::GameAndUI, "and they are written");
        }

        Expect(Hierarchy.topLevelItemCount() == 1 && Hierarchy.topLevelItem(0)->childCount() == 2, "the hierarchy is the document's tree");

        /* A Grid's child: the Grid slot (Row, Column, spans, alignment), no Fill or Anchors. */
        Design.Select(0);
        const int Grid = Design.AddWidget(QStringLiteral("Grid"), 0, -1);
        const int Cell = Design.AddWidget(QStringLiteral("Button"), Grid, -1);
        Design.Select(Cell);
        PumpFor(30);
        Expect(Details.FindEditor(QStringLiteral("Row")) && Details.FindEditor(QStringLiteral("ColumnSpan")) && Details.FindEditor(QStringLiteral("HAlign"))
               && !Details.FindEditor(QStringLiteral("Fill")) && !Details.FindEditor(QStringLiteral("Anchors")),
               "a Grid child shows the Grid slot");
    }

    void TestUIGraph(const QString& InDir)
    {
        std::printf("[ui graph]\n");
        const QString Path = InDir + QStringLiteral("/Logic.nmui");
        QFile File(Path);
        File.open(QIODevice::WriteOnly);
        File.write(R"XML(<?xml version="1.0" encoding="utf-8"?>
<UI Format="1" ReferenceSize="1920,1080" InputMode="UIOnly">
  <Canvas Name="Root">
    <Button Name="Play" OnClick="Native:Game.Quit"/>
    <CheckBox Name="Shadows"/>
  </Canvas>
</UI>
)XML");
        File.close();

        UIDesignDocument Design;
        Expect(Design.Load(Path), "a document to give logic");
        Expect(EditorUIGraph::MakeHandlerName(QStringLiteral("Play"), QStringLiteral("OnClick")) == QStringLiteral("Play_OnClick")
               && EditorUIGraph::MakeHandlerName(QString(), QStringLiteral("OnBack")) == QStringLiteral("Document_OnBack")
               && EditorUIGraph::MakeHandlerName(QStringLiteral("2 Up"), QStringLiteral("OnClick")) == QStringLiteral("_2_Up_OnClick"),
               "handler names are identifiers from the widget and its event");
        Expect(EditorUIGraph::FindGraphActions(QStringLiteral("Native:UI.Close; Graph:A_B ;Graph: C")) == QStringList({ QStringLiteral("A_B"), QStringLiteral("C") }),
               "Graph: actions are found in action text");

        const int Play = Design.GetAsset().FindWidget("Play");
        const QString Handler = EditorUIGraph::AddHandler(Design, Play, QStringLiteral("OnClick"), nullptr);
        PumpFor(50);
        const QString GraphPath = InDir + QStringLiteral("/Logic.ui.nmgraph");
        Expect(Handler == QStringLiteral("Play_OnClick") && QFileInfo::exists(GraphPath), "Graph handler makes the UI graph beside it");
        Expect(Get(Design, "Play", "OnClick") == "Native:Game.Quit; Graph:Play_OnClick", "and adds Graph:<Handler> to the action");
        Expect(QFileInfo(QString::fromStdString(Design.GetAsset().Settings.Graph)).fileName() == QStringLiteral("Logic.ui.nmgraph"),
               "the document's Graph names it");

        NodeGraph::Graph Saved;
        std::string Error;
        Expect(NodeGraph::LoadGraphFile(GraphPath.toStdString(), Saved, Error), "the graph was saved");
        int Handlers = 0;
        for(const NodeGraph::Node& Each : Saved.Nodes)
        {
            if(Each.Type == "UI.Event.Handler" && Each.GetProperty("Handler") == "Play_OnClick" && Each.GetProperty("Widget") == "Play") ++Handlers;
        }
        Expect(Handlers == 1 && Saved.GetProperty("Document").find("Logic.nmui") != std::string::npos,
               "with the UI Event, and the document it is the logic of");

        Expect(EditorUIGraph::AddHandler(Design, Play, QStringLiteral("OnClick"), nullptr) == Handler, "asking again finds it");
        PumpFor(50);
        Expect(EditorUIGraph::GetHandlers(Design).count(QStringLiteral("Play_OnClick")) == 1
               && Get(Design, "Play", "OnClick") == "Native:Game.Quit; Graph:Play_OnClick", "no second node, no second action");

        EditorUIGraph::AddHandler(Design, Design.GetAsset().FindWidget("Shadows"), QStringLiteral("OnChanged"), nullptr);
        EditorUIGraph::AddHandler(Design, -1, QStringLiteral("OnBack"), nullptr);
        PumpFor(50);
        Expect(Design.GetAsset().Settings.OnBack == "Graph:Document_OnBack", "a document event gets a handler too");
        bool bBool = false;
        if(GraphEditorWindow* Window = GraphEditorWindow::Find())
        {
            for(GraphEditorTab* Tab : Window->GetTabs())
            {
                for(const NodeGraph::Node& Each : Tab->GetDocument().GetGraph().Nodes)
                {
                    if(Each.GetProperty("Handler") == "Shadows_OnChanged") bBool = Each.GetProperty("ValueType") == "Bool";
                }
            }
        }
        Expect(bBool, "a check box's OnChanged handler takes a bool");
        Expect(EditorUIGraph::RevealHandler(Design, QStringLiteral("Play_OnClick"), nullptr)
               && !EditorUIGraph::RevealHandler(Design, QStringLiteral("Nope"), nullptr), "a handler is revealed by name");

        Design.GetUndoStack()->undo();   // the OnBack action
        Expect(Design.GetAsset().Settings.OnBack.empty(), "undo takes an added action back");

        /* A field naming a handler the graph lacks is flagged. */
        Design.SetProperty(Play, QStringLiteral("OnClick"), QStringLiteral("Graph:Gone"));
        UIDetailsPanel Details(Design);
        Design.Select(Play);
        PumpFor(50);
        QWidget* Field = Details.FindEditor(QStringLiteral("OnClick"));
        QLineEdit* Edit = Field ? Field->findChild<QLineEdit*>() : nullptr;
        Expect(Edit && Edit->property("MissingHandler").toBool(), "an action naming a missing handler is flagged");
        Design.SetProperty(Play, QStringLiteral("OnClick"), QStringLiteral("Graph:Play_OnClick"));
        PumpFor(50);
        Field = Details.FindEditor(QStringLiteral("OnClick"));
        Edit  = Field ? Field->findChild<QLineEdit*>() : nullptr;
        Expect(Edit && !Edit->property("MissingHandler").toBool(), "one it has is not");

        /* Renames and deletes: the graph follows (UIEditorTab asks; here, directly). */
        QStringList Renamed, Removed;
        QObject::connect(&Design, &UIDesignDocument::WidgetRenamed, [&](const QString& InOld, const QString& InNew) { Renamed << InOld << InNew; });
        QObject::connect(&Design, &UIDesignDocument::WidgetsRemoved, [&](const QStringList& InNames) { Removed << InNames; });
        Design.SetProperty(Play, QStringLiteral("Name"), QStringLiteral("Start"));
        Expect(Renamed == QStringList({ QStringLiteral("Play"), QStringLiteral("Start") }), "a rename is announced");
        Expect(EditorUIGraph::HandlersForWidget(Design, QStringLiteral("Play")) == QStringList({ QStringLiteral("Play_OnClick") }),
               "the handlers made for the old name are found");
        /* A handler of its own name, for the same widget: it stays put through a rename. */
        EditorUIGraph::RevealHandler(Design, QStringLiteral("Play_OnClick"), nullptr);   // the graph is open
        for(GraphEditorTab* Each : GraphEditorWindow::Find()->GetTabs())
        {
            Each->GetDocument().AddNode("UI.Event.Handler", QPointF(0, 900), { { "Handler", "Mine" }, { "Widget", "Play" }, { "Event", "OnHovered" } });
        }
        const QStringList Done = EditorUIGraph::RenameWidgetHandlers(Design, QStringLiteral("Play"), QStringLiteral("Start"), true);
        PumpFor(50);
        const QStringList Now = EditorUIGraph::GetHandlers(Design);
        Expect(Done == QStringList({ QStringLiteral("Start_OnClick") }) && Now.contains(QStringLiteral("Start_OnClick"))
               && !Now.contains(QStringLiteral("Play_OnClick")) && Now.contains(QStringLiteral("Mine")),
               "a handler named for the widget follows it; one with its own name stays");
        Expect(Get(Design, "Start", "OnClick") == "Graph:Start_OnClick", "and the action calling it is rewritten");
        Expect(EditorUIGraph::HandlersForWidget(Design, QStringLiteral("Start")).size() == 2, "every handler now notes the new name");

        Renamed.clear();
        Design.GetUndoStack()->undo();
        Expect(Renamed.isEmpty(), "an undo announces nothing");
        Design.GetUndoStack()->redo();

        Design.Select(Design.GetAsset().FindWidget("Start"));
        Design.DeleteSelected();
        Expect(Removed == QStringList({ QStringLiteral("Start") }), "a delete names what went");
        Expect(EditorUIGraph::HandlersForWidget(Design, QStringLiteral("Start")).size() == 2, "and its handlers are found to report");

        if(GraphEditorWindow* Window = GraphEditorWindow::Find())
        {
            for(GraphEditorTab* Tab : Window->GetTabs()) { Tab->GetDocument().GetHistory().setClean(); }
            Window->close();
        }
    }
    void TestPreviewRuntime(const QString& InDir)
    {
        std::printf("[preview runtime]\n");
        const QString GraphPath = InDir + QStringLiteral("/Preview.ui.nmgraph");
        const auto WriteGraph = [&GraphPath](const char* InLua)
        {
            QFile File(GraphPath);
            File.open(QIODevice::WriteOnly | QIODevice::Truncate);
            File.write(QByteArray("<NodeGraph Version=\"1\" Schema=\"UI\"><gen_code><Output Extension=\".lua\"><![CDATA[") + InLua
                       + "]]></Output></gen_code></NodeGraph>\n");
        };
        WriteGraph("return { OnOpen = function(self) UI.SetNumber('Opened', 1) end,\n"
                   "  Go_OnClick = function(self) Config.SetBool('Shadows.Enabled', not Config.GetBool('Shadows.Enabled'))\n"
                   "    Game.SetPaused(true); UI.SetString('Doc', self.Document) end }");
        UI::UIDocumentAsset Parsed;
        const std::string Xml = "<UI Graph=\"" + GraphPath.toStdString() + "\"><Canvas><Button Name=\"Go\" OnClick=\"Graph:Go_OnClick\"/></Canvas></UI>";
        Expect(UI::ParseUIXml(Xml, Parsed), "a document with a UI graph");
        auto Asset = std::make_shared<const UI::UIDocumentAsset>(Parsed);

        EngineCore::ConfigVar* Shadows = EngineCore::ConfigFindVarByPath("Shadows.Enabled");
        Expect(Shadows != nullptr, "a config var for the graph to change");
        if(!Shadows) return;
        const bool bShadows = Shadows->GetBool();

        UI::UISystem System;
        System.SetExecuteActions(false);   // as the designer's
        UIPreviewRuntime Runtime;
        Expect(Runtime.Start(System), "the preview's runtime starts");
        UI::UIDocumentHandle Doc = System.OpenAsset(Asset, "Content/UI/Preview.nmui", false, false);
        System.RunQueuedActions();
        Expect(System.GetValue("Opened") && System.GetValue("Opened")->AsNumber() == 1.0, "its On Document Open runs");

        UI::UIActionList Click;
        Click.Parse("Graph:Go_OnClick");
        System.QueueActions(Click, System.Find(Doc), System.Find(Doc)->FindWidget("Go"));
        System.RunQueuedActions();
        Expect(Shadows->GetBool() != bShadows, "a handler runs, and may change config vars");
        Expect(System.GetValue("Doc") && System.GetValue("Doc")->AsString() == "Content/UI/Preview.nmui", "self.Document is the document's path");
        Expect(Runtime.GetErrorCount() == 0, "Game.SetPaused in the preview is quiet, not a script error");

        WriteGraph("return { OnOpen = function(self) UI.SetNumber('Opened', 2) end }");
        Runtime.ForgetGraph(GraphPath.toStdString());
        System.CloseAll(true);
        System.OpenAsset(Asset, "Content/UI/Preview.nmui", false, false);
        System.RunQueuedActions();
        Expect(System.GetValue("Opened")->AsNumber() == 2.0, "a saved graph is read again");

        Runtime.Stop(System);
        Expect(Shadows->GetBool() == bShadows, "stopping puts the config vars back");
        Expect(System.GetDocumentCount() == 0 && !Runtime.IsRunning(), "and closes the preview's documents");
    }
}

int main(int argc, char** argv)
{
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication App(argc, argv);

    std::printf("NimbleEditorUIDesignerTests\n");
    QTemporaryDir Dir;
    TestDocument(Dir.path());
    TestDetails(Dir.path());
    TestUIGraph(Dir.path());
    TestPreviewRuntime(Dir.path());

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
