#if WITH_EDITOR

#include "Private/Editor/Graph/GraphDocument.h"

#include <algorithm>

#include <QUndoCommand>
#include <QUndoStack>

#include "Public/NodeGraph/GraphFile.h"
#include "Private/Editor/Graph/GraphAssets.h"

using namespace NodeGraph;
using EditorGraphs::ToQ;
using EditorGraphs::ToStd;

namespace
{
    constexpr int HistoryLimit = 200;
}

/* One edit: the graph before and after it. */
class GraphSnapshotCommand final : public QUndoCommand
{
public:
    GraphSnapshotCommand(GraphDocument& InDocument, const QString& InLabel, Graph InBefore, Graph InAfter)
        : QUndoCommand(InLabel), Document(InDocument), Before(std::move(InBefore)), After(std::move(InAfter)) {}

    void undo() override { Document.Restore(Before); }
    void redo() override { Document.Restore(After); }

private:
    GraphDocument& Document;
    Graph          Before;
    Graph          After;
};

GraphDocument::GraphDocument(const QString& InPath, NodeGraph::Graph InGraph, QObject* InParent)
    : QObject(InParent)
    , Path(InPath)
    , Model(std::move(InGraph))
{
    SchemaPtr = FindSchema(Model.SchemaName);
    History = new QUndoStack(this);
    History->setUndoLimit(HistoryLimit);
    connect(History, &QUndoStack::cleanChanged, this, [this](const bool bInClean) { emit DirtyChanged(!bInClean); });
}

bool GraphDocument::IsDirty() const
{
    return !History->isClean();
}

void GraphDocument::Restore(const NodeGraph::Graph& InGraph)
{
    Model = InGraph;
    emit Changed();
}

void GraphDocument::Edit(const QString& InLabel, const std::function<void(NodeGraph::Graph&)>& InEdit)
{
    NodeGraph::Graph After = Model;
    InEdit(After);

    /* The text form is the one equality every part of a graph has. */
    if(WriteGraphText(After) == WriteGraphText(Model)) { return; }

    History->push(new GraphSnapshotCommand(*this, InLabel, Model, std::move(After)));   // push() runs redo()
}

// === Queries ====================================================================

const NodeDefinition* GraphDocument::FindDefinition(const NodeGuid& InNode) const
{
    const Node* Found = Model.FindNode(InNode);
    return Found && !Found->IsPlaceholder() ? SchemaPtr->FindNode(Found->Type) : nullptr;
}

const PinDefinition* GraphDocument::FindPin(const PinRef& InPin) const
{
    const NodeDefinition* Def = FindDefinition(InPin.Node);
    return Def ? Def->FindPin(InPin.Pin) : nullptr;
}

std::string GraphDocument::GetPinType(const PinRef& InPin) const
{
    const Node* Found = Model.FindNode(InPin.Node);
    const PinDefinition* Pin = FindPin(InPin);
    return Found && Pin ? SchemaPtr->GetPinType(Model, *Found, *Pin) : std::string();
}

bool GraphDocument::IsLinked(const PinRef& InPin) const
{
    return std::any_of(Model.Links.begin(), Model.Links.end(),
                       [&](const Link& Each) { return Each.From == InPin || Each.To == InPin; });
}

bool GraphDocument::CanConnect(const PinRef& InA, const PinRef& InB, QString* OutWhy) const
{
    LinkPlan Plan;
    return PlanLink(Model, InA, InB, Plan, OutWhy);
}

namespace
{
    /* Whether any link other than InIgnoring reaches one of InNode's wildcard pins. */
    bool HasOtherWildcardLinks(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef, const PinRef& InIgnoring)
    {
        for(const Link& Each : InGraph.Links)
        {
            for(const PinRef* End : { &Each.From, &Each.To })
            {
                if(End->Node != InNode.Id || *End == InIgnoring) { continue; }
                const PinDefinition* Pin = InDef.FindPin(End->Pin);
                if(Pin && Pin->Type == WILDCARD_TYPE) { return true; }
            }
        }
        return false;
    }
}

bool GraphDocument::PlanLink(const NodeGraph::Graph& InGraph, const PinRef& InA, const PinRef& InB,
                             LinkPlan& OutPlan, QString* OutWhy) const
{
    const auto Fail = [&](const QString& InWhy) { if(OutWhy) { *OutWhy = InWhy; } return false; };

    if(InA.Node == InB.Node) { return Fail(QStringLiteral("A node cannot link to itself")); }

    const Node* NodeA = InGraph.FindNode(InA.Node);
    const Node* NodeB = InGraph.FindNode(InB.Node);
    const NodeDefinition* DefA = NodeA && !NodeA->IsPlaceholder() ? SchemaPtr->FindNode(NodeA->Type) : nullptr;
    const NodeDefinition* DefB = NodeB && !NodeB->IsPlaceholder() ? SchemaPtr->FindNode(NodeB->Type) : nullptr;
    const PinDefinition* PinA = DefA ? DefA->FindPin(InA.Pin) : nullptr;
    const PinDefinition* PinB = DefB ? DefB->FindPin(InB.Pin) : nullptr;
    if(!PinA || !PinB) { return Fail(QStringLiteral("Unknown pin")); }
    if(PinA->Direction == PinB->Direction)
    {
        return Fail(PinA->IsInput() ? QStringLiteral("Both are inputs") : QStringLiteral("Both are outputs"));
    }
    if(PinA->Kind != PinB->Kind) { return Fail(QStringLiteral("An execution pin only links to an execution pin")); }

    const bool bAIsOutput = PinA->IsOutput();
    const Node& FromNode = bAIsOutput ? *NodeA : *NodeB;
    const Node& ToNode   = bAIsOutput ? *NodeB : *NodeA;
    const NodeDefinition& FromDef = bAIsOutput ? *DefA : *DefB;
    const NodeDefinition& ToDef   = bAIsOutput ? *DefB : *DefA;
    const PinDefinition& FromPin = bAIsOutput ? *PinA : *PinB;
    const PinDefinition& ToPin   = bAIsOutput ? *PinB : *PinA;

    OutPlan = LinkPlan();
    OutPlan.Made = Link{ PinRef{ FromNode.Id, FromPin.Name }, PinRef{ ToNode.Id, ToPin.Name } };
    if(FromPin.IsExec()) { return true; }

    /* A value must be computable in every stage the input leads to: a
     * material's pixel-only nodes cannot reach World Position Offset, even
     * through other nodes (Schema::CanLinkStages). */
    std::string StageWhy;
    if(!SchemaPtr->CanLinkStages(InGraph, FromNode, ToNode, ToPin.Name, &StageWhy)) { return Fail(ToQ(StageWhy)); }

    /* A wildcard end that nothing else pins down takes whatever type makes
     * this link exact, if it can; otherwise its current type stands. */
    const auto Candidates = [&](const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin,
                                const PinRef& InSelf, bool& bOutFree) -> std::vector<std::string>
    {
        bOutFree = false;
        /* Fixed, or derived from the node's settings: either way, not ours to choose. */
        if(InPin.Type != WILDCARD_TYPE) { return { SchemaPtr->GetPinType(InGraph, InNode, InPin) }; }
        if(InDef.bWildcardFromSchema || HasOtherWildcardLinks(InGraph, InNode, InDef, InSelf))
        {
            return { SchemaPtr->GetPinType(InGraph, InNode, InPin) };
        }
        bOutFree = true;
        return InDef.WildcardTypes;
    };

    bool bFromFree = false, bToFree = false;
    const std::vector<std::string> FromTypes = Candidates(FromNode, FromDef, FromPin, OutPlan.Made.From, bFromFree);
    const std::vector<std::string> ToTypes   = Candidates(ToNode, ToDef, ToPin, OutPlan.Made.To, bToFree);

    /* Exact first, then anything the compiler converts. */
    for(const bool bExact : { true, false })
    {
        for(const std::string& From : FromTypes)
        {
            for(const std::string& To : ToTypes)
            {
                if(bExact ? From != To : !SchemaPtr->CanConvert(From, To)) { continue; }
                if(bFromFree) { OutPlan.FromWildcard = From; }
                if(bToFree)   { OutPlan.ToWildcard = To; }
                return true;
            }
        }
    }

    return Fail(QStringLiteral("%1 does not convert to %2")
                .arg(ToQ(FromTypes.empty() ? std::string("?") : FromTypes.front()),
                     ToQ(ToTypes.empty() ? std::string("?") : ToTypes.front())));
}

void GraphDocument::ApplyLink(NodeGraph::Graph& InOutGraph, const LinkPlan& InPlan) const
{
    const PinDefinition* FromPin = nullptr;
    if(const Node* From = InOutGraph.FindNode(InPlan.Made.From.Node))
    {
        if(const NodeDefinition* Def = SchemaPtr->FindNode(From->Type)) { FromPin = Def->FindPin(InPlan.Made.From.Pin); }
    }
    const bool bExec = FromPin && FromPin->IsExec();

    /* One link into a data input; one link out of an exec output. */
    InOutGraph.Links.erase(std::remove_if(InOutGraph.Links.begin(), InOutGraph.Links.end(), [&](const Link& Each)
    {
        return bExec ? Each.From == InPlan.Made.From : Each.To == InPlan.Made.To;
    }), InOutGraph.Links.end());

    if(!InPlan.FromWildcard.empty()) { InOutGraph.FindNode(InPlan.Made.From.Node)->WildcardType = InPlan.FromWildcard; }
    if(!InPlan.ToWildcard.empty())   { InOutGraph.FindNode(InPlan.Made.To.Node)->WildcardType = InPlan.ToWildcard; }
    InOutGraph.Links.push_back(InPlan.Made);
}

// === Edits ======================================================================

NodeGuid GraphDocument::AddNode(const std::string& InType, const QPointF& InPos,
                                const std::map<std::string, std::string>& InProperties, const PinRef* InConnectFrom)
{
    const NodeDefinition* Def = SchemaPtr->FindNode(InType);
    if(!Def) { return NodeGuid(); }

    Node Added;
    Added.Id         = NodeGuid::Generate();
    Added.Type       = InType;
    Added.Version    = Def->Version;
    Added.X          = float(InPos.x());
    Added.Y          = float(InPos.y());
    Added.Properties = InProperties;

    const PinRef From = InConnectFrom ? *InConnectFrom : PinRef();
    Edit(QStringLiteral("Add %1").arg(ToQ(Def->DisplayName)), [&](NodeGraph::Graph& G)
    {
        G.Nodes.push_back(Added);
        if(!InConnectFrom) { return; }

        for(const PinDefinition& Pin : Def->Pins)
        {
            LinkPlan Plan;
            if(PlanLink(G, From, PinRef{ Added.Id, Pin.Name }, Plan, nullptr))
            {
                ApplyLink(G, Plan);
                break;
            }
        }
    });
    return Added.Id;
}

void GraphDocument::Connect(const PinRef& InA, const PinRef& InB, const Link* InReplacing)
{
    Edit(QStringLiteral("Link"), [&](NodeGraph::Graph& G)
    {
        if(InReplacing) { G.RemoveLink(*InReplacing); }
        LinkPlan Plan;
        if(PlanLink(G, InA, InB, Plan, nullptr)) { ApplyLink(G, Plan); }
    });
}

void GraphDocument::DisconnectPin(const PinRef& InPin)
{
    Edit(QStringLiteral("Break Links"), [&](NodeGraph::Graph& G) { G.RemoveLinksAt(InPin); });
}

void GraphDocument::RemoveLink(const Link& InLink)
{
    Edit(QStringLiteral("Break Link"), [&](NodeGraph::Graph& G) { G.RemoveLink(InLink); });
}

void GraphDocument::RemoveNodes(const std::vector<NodeGuid>& InNodes, const std::vector<int>& InComments)
{
    Edit(QStringLiteral("Delete"), [&](NodeGraph::Graph& G)
    {
        for(const NodeGuid& Each : InNodes) { G.RemoveNode(Each); }

        std::vector<int> Comments = InComments;
        std::sort(Comments.rbegin(), Comments.rend());
        for(const int Index : Comments)
        {
            if(Index >= 0 && Index < int(G.Comments.size())) { G.Comments.erase(G.Comments.begin() + Index); }
        }
    });
}

void GraphDocument::MoveItems(const std::map<NodeGuid, QPointF>& InNodes, const std::map<int, QPointF>& InComments)
{
    Edit(QStringLiteral("Move"), [&](NodeGraph::Graph& G)
    {
        for(const auto& Each : InNodes)
        {
            if(Node* Moved = G.FindNode(Each.first))
            {
                Moved->X = float(Each.second.x());
                Moved->Y = float(Each.second.y());
            }
        }
        for(const auto& Each : InComments)
        {
            if(Each.first >= 0 && Each.first < int(G.Comments.size()))
            {
                G.Comments[size_t(Each.first)].X = float(Each.second.x());
                G.Comments[size_t(Each.first)].Y = float(Each.second.y());
            }
        }
    });
}

void GraphDocument::SetConstant(const PinRef& InPin, const std::string& InValue)
{
    const PinDefinition* Pin = FindPin(InPin);
    Edit(QStringLiteral("Set %1").arg(ToQ(InPin.Pin)), [&](NodeGraph::Graph& G)
    {
        Node* Target = G.FindNode(InPin.Node);
        if(!Target) { return; }

        /* The pin's default is not stored, so a changed default reaches it. */
        if(Pin && InValue == Pin->Default) { Target->Constants.erase(InPin.Pin); }
        else                               { Target->Constants[InPin.Pin] = InValue; }
    });
}

void GraphDocument::SetProperty(const NodeGuid& InNode, const std::string& InName, const std::string& InValue)
{
    Edit(QStringLiteral("Set %1").arg(ToQ(InName)), [&](NodeGraph::Graph& G)
    {
        if(Node* Target = G.FindNode(InNode)) { Target->Properties[InName] = InValue; }
    });
}

void GraphDocument::SetWildcardType(const NodeGuid& InNode, const std::string& InType)
{
    Edit(QStringLiteral("Set Type"), [&](NodeGraph::Graph& G)
    {
        if(Node* Target = G.FindNode(InNode)) { Target->WildcardType = InType; }
    });
}

void GraphDocument::SetGraphProperty(const std::string& InName, const std::string& InValue)
{
    Edit(QStringLiteral("Set %1").arg(ToQ(InName)), [&](NodeGraph::Graph& G) { G.Properties[InName] = InValue; });
}

void GraphDocument::SetVariables(const std::vector<Variable>& InVariables)
{
    Edit(QStringLiteral("Edit Variables"), [&](NodeGraph::Graph& G)
    {
        /* A rename carries the getters and setters that used the old name. */
        for(size_t Index = 0; Index < G.Variables.size() && Index < InVariables.size(); ++Index)
        {
            const std::string& Old = G.Variables[Index].Name;
            const std::string& New = InVariables[Index].Name;
            if(Old == New) { continue; }
            for(Node& Each : G.Nodes)
            {
                const auto Found = Each.Properties.find("Variable");
                if(Found != Each.Properties.end() && Found->second == Old) { Found->second = New; }
            }
        }
        G.Variables = InVariables;
    });
}

void GraphDocument::AddComment(const Comment& InComment)
{
    Edit(QStringLiteral("Add Comment"), [&](NodeGraph::Graph& G) { G.Comments.push_back(InComment); });
}

void GraphDocument::SetCommentText(const int InIndex, const QString& InText)
{
    Edit(QStringLiteral("Edit Comment"), [&](NodeGraph::Graph& G)
    {
        if(InIndex >= 0 && InIndex < int(G.Comments.size())) { G.Comments[size_t(InIndex)].Text = ToStd(InText); }
    });
}

std::vector<NodeGuid> GraphDocument::Paste(const std::string& InFragment, const QPointF& InAt)
{
    std::vector<NodeGuid> Pasted;
    Edit(QStringLiteral("Paste"), [&](NodeGraph::Graph& G)
    {
        if(!PasteGraphFragment(InFragment, G, 0.0f, 0.0f, Pasted) || Pasted.empty()) { return; }

        float MinX = 0.0f, MinY = 0.0f;
        bool bFirst = true;
        for(const NodeGuid& Each : Pasted)
        {
            const Node* N = G.FindNode(Each);
            MinX = bFirst ? N->X : std::min(MinX, N->X);
            MinY = bFirst ? N->Y : std::min(MinY, N->Y);
            bFirst = false;
        }
        for(const NodeGuid& Each : Pasted)
        {
            Node* N = G.FindNode(Each);
            N->X += float(InAt.x()) - MinX;
            N->Y += float(InAt.y()) - MinY;
        }
    });
    return Pasted;
}

// === File =======================================================================

bool GraphDocument::Save(QString& OutError, QString& OutMessage,
                         QObject* InContext, std::function<void(bool, const QString&)> InStaged)
{
    std::string Error;
    if(!SaveGraphFile(ToStd(Path), Model, Error))
    {
        OutError = ToQ(Error);
        return false;
    }
    History->setClean();

    CompileResult Result;
    EditorGraphs::WriteOutput(Path, Model, Result, OutMessage, InContext, std::move(InStaged));
    emit Saved();
    return true;
}

#endif // WITH_EDITOR
