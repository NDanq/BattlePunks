#include "Public/NodeGraph/Graph.h"

#include <algorithm>

namespace NodeGraph
{
    std::string GuidToString(const NodeGuid& InGuid)
    {
        char Text[NodeGuid::STRING_BUFFER];
        InGuid.ToString(Text);
        return Text;
    }

    bool GuidFromString(const std::string& InText, NodeGuid& OutGuid)
    {
        return NodeGuid::Parse(InText.c_str(), OutGuid);
    }

    // === Node ===================================================================

    const std::string* Node::FindConstant(const std::string& InPin) const
    {
        const auto Found = Constants.find(InPin);
        return Found == Constants.end() ? nullptr : &Found->second;
    }

    const std::string* Node::FindProperty(const std::string& InName) const
    {
        const auto Found = Properties.find(InName);
        return Found == Properties.end() ? nullptr : &Found->second;
    }

    std::string Node::GetProperty(const std::string& InName, const std::string& InDefault) const
    {
        const std::string* Value = FindProperty(InName);
        return Value ? *Value : InDefault;
    }

    // === Graph ==================================================================

    Node* Graph::FindNode(const NodeGuid& InId)
    {
        for(Node& Each : Nodes)
        {
            if(Each.Id == InId) { return &Each; }
        }
        return nullptr;
    }

    const Node* Graph::FindNode(const NodeGuid& InId) const
    {
        return const_cast<Graph*>(this)->FindNode(InId);
    }

    const Variable* Graph::FindVariable(const std::string& InName) const
    {
        for(const Variable& Each : Variables)
        {
            if(Each.Name == InName) { return &Each; }
        }
        return nullptr;
    }

    const GeneratedCode* Graph::FindGenCode(const std::string& InExtension) const
    {
        for(const GeneratedCode& Each : GenCode)
        {
            if(Each.Extension == InExtension) { return &Each; }
        }
        return nullptr;
    }

    std::string Graph::GetProperty(const std::string& InName, const std::string& InDefault) const
    {
        const auto Found = Properties.find(InName);
        return Found == Properties.end() ? InDefault : Found->second;
    }

    const Link* Graph::FindLinkTo(const PinRef& InInput) const
    {
        for(const Link& Each : Links)
        {
            if(Each.To == InInput) { return &Each; }
        }
        return nullptr;
    }

    std::vector<const Link*> Graph::FindLinksFrom(const PinRef& InOutput) const
    {
        std::vector<const Link*> Result;
        for(const Link& Each : Links)
        {
            if(Each.From == InOutput) { Result.push_back(&Each); }
        }
        return Result;
    }

    bool Graph::HasLink(const Link& InLink) const
    {
        return std::find(Links.begin(), Links.end(), InLink) != Links.end();
    }

    bool Graph::RemoveNode(const NodeGuid& InId)
    {
        const auto Found = std::find_if(Nodes.begin(), Nodes.end(), [&](const Node& Each) { return Each.Id == InId; });
        if(Found == Nodes.end()) { return false; }
        Nodes.erase(Found);

        Links.erase(std::remove_if(Links.begin(), Links.end(), [&](const Link& Each)
        {
            return Each.From.Node == InId || Each.To.Node == InId;
        }), Links.end());
        return true;
    }

    void Graph::RemoveLinksAt(const PinRef& InPin)
    {
        Links.erase(std::remove_if(Links.begin(), Links.end(), [&](const Link& Each)
        {
            return Each.From == InPin || Each.To == InPin;
        }), Links.end());
    }

    bool Graph::RemoveLink(const Link& InLink)
    {
        const auto Found = std::find(Links.begin(), Links.end(), InLink);
        if(Found == Links.end()) { return false; }
        Links.erase(Found);
        return true;
    }
}
