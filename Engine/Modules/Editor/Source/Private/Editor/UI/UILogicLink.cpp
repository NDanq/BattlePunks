#if WITH_EDITOR

#include "Private/Editor/UI/UILogicLink.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>

#include "Public/NodeGraph/GraphFile.h"
#include "Public/NodeGraph/Schema.h"
#include "Public/UI/UIDocument.h"
#include "Public/UI/UIProperty.h"
#include "Private/Editor/Content/ContentPaths.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphEditorTab.h"
#include "Private/Editor/Graph/GraphEditorWindow.h"
#include "Private/Editor/UI/UIDesignDocument.h"

namespace
{
    const char* const HANDLER_TYPE = "UI.Event.Handler";

    std::string Utf8(const QString& InText) { return InText.toUtf8().toStdString(); }
    QString     ToQ(const std::string& InText) { return QString::fromUtf8(InText.c_str()); }

    /* The document's runtime path ("Content/UI/Menu.nmui"); its file name if outside the project. */
    QString RuntimePathOf(const QString& InFile)
    {
        const QString Runtime = EditorContent::ToRuntimePath(InFile);
        return Runtime.isEmpty() ? QFileInfo(InFile).fileName() : Runtime;
    }

    /* The open tab on InGraphPath, if any. */
    GraphEditorTab* FindOpenTab(const QString& InGraphPath)
    {
        GraphEditorWindow* Window = GraphEditorWindow::Find();
        if(!Window) return nullptr;
        const QString Wanted = QFileInfo(InGraphPath).canonicalFilePath();
        for(GraphEditorTab* Each : Window->GetTabs())
        {
            if(QFileInfo(Each->GetDocument().GetPath()).canonicalFilePath() == Wanted) return Each;
        }
        return nullptr;
    }

    const NodeGraph::Node* FindHandlerNode(const NodeGraph::Graph& InGraph, const std::string& InHandler)
    {
        for(const NodeGraph::Node& Each : InGraph.Nodes)
        {
            if(Each.Type == HANDLER_TYPE && Each.GetProperty("Handler", "Handler") == InHandler) return &Each;
        }
        return nullptr;
    }

    /* Where a new handler goes: under the graph's lowest node. */
    QPointF FreeSpot(const NodeGraph::Graph& InGraph)
    {
        if(InGraph.Nodes.empty()) return QPointF(-300.0, -120.0);
        float Left = InGraph.Nodes.front().X, Bottom = InGraph.Nodes.front().Y;
        for(const NodeGraph::Node& Each : InGraph.Nodes)
        {
            Left   = std::min(Left, Each.X);
            Bottom = std::max(Bottom, Each.Y);
        }
        return QPointF(Left, Bottom + 180.0);
    }

    /* InProperty's action text now: the widget's own, or the document setting's. */
    QString ActionText(const UIDesignDocument& InDesign, const int InWidget, const QString& InProperty)
    {
        const UI::UIDocumentAsset& Asset = InDesign.GetAsset();
        if(InWidget >= 0)
        {
            const std::string* Own = Asset.Widgets[size_t(InWidget)].Find(Utf8(InProperty));
            return Own ? ToQ(*Own) : QString();
        }
        for(const UI::UIPropertyText& Each : Asset.Settings.ToText())
        {
            if(ToQ(Each.Name) == InProperty) return ToQ(Each.Value);
        }
        return QString();
    }
}

namespace EditorUILogic
{
    QString MakeHandlerName(const QString& InWidget, const QString& InProperty)
    {
        QString Base = InWidget.isEmpty() ? QStringLiteral("Document") : InWidget;
        Base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
        if(Base.isEmpty() || Base.front().isDigit()) Base.prepend(QLatin1Char('_'));
        return Base + QLatin1Char('_') + InProperty;
    }

    QStringList FindGraphActions(const QString& InActions)
    {
        QStringList Out;
        const QRegularExpression Pattern(QStringLiteral("Graph\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*)"));
        for(QRegularExpressionMatchIterator It = Pattern.globalMatch(InActions); It.hasNext();) { Out << It.next().captured(1); }
        return Out;
    }

    QString GetGraphPath(const UIDesignDocument& InDesign)
    {
        const QString Logic = ToQ(InDesign.GetAsset().Settings.Logic);
        if(Logic.isEmpty()) return QString();
        if(QFileInfo(Logic).isAbsolute()) return Logic;
        /* A runtime path is the project's; failing that, beside the document. */
        const QString InProject = QDir(EditorGraphs::GetProjectRoot()).filePath(Logic);
        if(QFileInfo::exists(InProject)) return InProject;
        return QFileInfo(InDesign.GetPath()).dir().filePath(QFileInfo(Logic).fileName());
    }

    QString EnsureGraph(UIDesignDocument& InDesign, QWidget* InParent, const bool bInAsk)
    {
        const QString Existing = GetGraphPath(InDesign);
        if(!Existing.isEmpty() && QFileInfo::exists(Existing)) return Existing;

        const NodeGraph::Schema* Schema = NodeGraph::FindSchema("UI");
        if(!Schema || InDesign.GetPath().isEmpty()) return QString();

        const QFileInfo Doc(InDesign.GetPath());
        const QString Path = Doc.dir().filePath(Doc.completeBaseName() + QStringLiteral(".ui.nmgraph"));
        if(bInAsk && QMessageBox::question(InParent, QStringLiteral("Logic Graph"),
                QStringLiteral("%1 has no logic graph. Make %2 beside it?").arg(Doc.fileName(), QFileInfo(Path).fileName()))
                != QMessageBox::Yes)
        {
            return QString();
        }
        if(!QFileInfo::exists(Path))
        {
            QString Error;
            if(!EditorGraphs::CreateGraphFile(*Schema, Path, Error, { { "Document", Utf8(RuntimePathOf(InDesign.GetPath())) } }))
            {
                QMessageBox::warning(InParent, QStringLiteral("Logic Graph"), QStringLiteral("Could not write\n%1\n\n%2").arg(Path, Error));
                return QString();
            }
        }
        const std::string Logic = Utf8(RuntimePathOf(Path));
        InDesign.Edit(QStringLiteral("Set Logic"), [Logic](UI::UIDocumentAsset& InAsset) { InAsset.Settings.Logic = Logic; });
        return Path;
    }

    QString AddHandler(UIDesignDocument& InDesign, const int InWidget, const QString& InProperty, QWidget* InParent)
    {
        const UI::UIDocumentAsset& Asset = InDesign.GetAsset();
        if(InWidget >= int(Asset.Widgets.size())) return QString();
        const QString Path = EnsureGraph(InDesign, InParent, false);
        if(Path.isEmpty()) return QString();

        const UI::UIWidgetDesc* Widget = InWidget >= 0 ? &Asset.Widgets[size_t(InWidget)] : nullptr;
        const QString WidgetName = Widget ? ToQ(Widget->GetName()) : QString();
        const QString Handler    = MakeHandlerName(WidgetName, InProperty);

        GraphEditorTab* Tab = EditorGraphs::OpenGraph(Path, InParent);
        if(!Tab) return QString();
        GraphDocument& Graph = Tab->GetDocument();

        NodeGraph::NodeGuid Node;
        if(const NodeGraph::Node* Found = FindHandlerNode(Graph.GetGraph(), Utf8(Handler)))
        {
            Node = Found->Id;
        }
        else
        {
            const bool bWasClean = !Graph.IsDirty();
            const bool bBool = Widget && Widget->Type == "CheckBox" && InProperty == QStringLiteral("OnChanged");
            Node = Graph.AddNode(HANDLER_TYPE, FreeSpot(Graph.GetGraph()),
                                 { { "Handler", Utf8(Handler) }, { "Widget", Utf8(WidgetName) }, { "Event", Utf8(InProperty) },
                                   { "ValueType", bBool ? "Bool" : "Number" } });
            if(bWasClean)
            {
                /* Saved, so the handler exists when the document next runs. */
                QString Error, Message;
                Graph.Save(Error, Message);
            }
        }
        Tab->RevealNode(Node);

        /* The action, unless it is there already. */
        const QString Text = ActionText(InDesign, InWidget, InProperty).trimmed();
        if(!FindGraphActions(Text).contains(Handler))
        {
            const QString Action  = QStringLiteral("Graph:") + Handler;
            const QString NewText = Text.isEmpty() ? Action : Text + QStringLiteral("; ") + Action;
            if(InWidget >= 0)
            {
                InDesign.SetProperty(InWidget, InProperty, NewText);
            }
            else
            {
                const std::string Name = Utf8(InProperty), Value = Utf8(NewText);
                InDesign.Edit(QStringLiteral("Set %1").arg(InProperty),
                              [Name, Value](UI::UIDocumentAsset& InAsset) { InAsset.Settings.SetFromText(Name, Value); });
            }
        }
        return Handler;
    }

    bool RevealHandler(const UIDesignDocument& InDesign, const QString& InHandler, QWidget* InParent)
    {
        const QString Path = GetGraphPath(InDesign);
        if(Path.isEmpty() || !QFileInfo::exists(Path)) return false;
        GraphEditorTab* Tab = EditorGraphs::OpenGraph(Path, InParent);
        if(!Tab) return false;
        const NodeGraph::Node* Found = FindHandlerNode(Tab->GetDocument().GetGraph(), Utf8(InHandler));
        if(Found) Tab->RevealNode(Found->Id);
        return Found != nullptr;
    }

    QStringList GetHandlers(const UIDesignDocument& InDesign)
    {
        const QString Path = GetGraphPath(InDesign);
        if(Path.isEmpty()) return {};

        NodeGraph::Graph Loaded;
        const NodeGraph::Graph* Graph = nullptr;
        if(GraphEditorTab* Tab = FindOpenTab(Path))
        {
            Graph = &Tab->GetDocument().GetGraph();
        }
        else
        {
            std::string Error;
            if(!QFileInfo::exists(Path) || !NodeGraph::LoadGraphFile(Utf8(Path), Loaded, Error)) return {};
            Graph = &Loaded;
        }
        QStringList Out;
        for(const NodeGraph::Node& Each : Graph->Nodes)
        {
            if(Each.Type == HANDLER_TYPE) Out << ToQ(Each.GetProperty("Handler", "Handler"));
        }
        return Out;
    }
}

#endif // WITH_EDITOR
