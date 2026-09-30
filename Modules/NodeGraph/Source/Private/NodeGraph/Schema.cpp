#include "Public/NodeGraph/Schema.h"

#include <algorithm>
#include <cctype>
#include <memory>

#include "Private/NodeGraph/GraphIndex.h"

namespace NodeGraph
{
    // === NodeDefinition =========================================================

    const PinDefinition* NodeDefinition::FindPin(const std::string& InName) const
    {
        for(const PinDefinition& Pin : Pins)
        {
            if(Pin.Name == InName) { return &Pin; }
        }
        return nullptr;
    }

    const PinDefinition* NodeDefinition::FindPin(const std::string& InName, const EPinDirection InDirection) const
    {
        for(const PinDefinition& Pin : Pins)
        {
            if(Pin.Name == InName && Pin.Direction == InDirection) { return &Pin; }
        }
        return nullptr;
    }

    const PropertyDefinition* NodeDefinition::FindProperty(const std::string& InName) const
    {
        for(const PropertyDefinition& Property : Properties)
        {
            if(Property.Name == InName) { return &Property; }
        }
        return nullptr;
    }

    // === Schema =================================================================

    Schema::Schema(std::string InName, std::string InFileTag, std::string InOutputExtension)
        : Name(std::move(InName))
        , FileTag(std::move(InFileTag))
        , OutputExtension(std::move(InOutputExtension))
    {
    }

    const NodeDefinition* Schema::FindNode(const std::string& InType) const
    {
        const auto Found = NodeIndex.find(InType);
        return Found == NodeIndex.end() ? nullptr : &Nodes[Found->second];
    }

    const PinTypeInfo* Schema::FindType(const std::string& InName) const
    {
        for(const PinTypeInfo& Type : Types)
        {
            if(Type.Name == InName) { return &Type; }
        }
        return nullptr;
    }

    bool Schema::CanConvert(const std::string& InFrom, const std::string& InTo) const
    {
        if(InFrom == WILDCARD_TYPE || InTo == WILDCARD_TYPE) { return false; }
        return InFrom == InTo || Conversions.count({ InFrom, InTo }) != 0;
    }

    std::string Schema::GetPinType(const Graph& InGraph, const Node& InNode, const PinDefinition& InPin) const
    {
        return ResolvePinType(InGraph, InNode, InPin);
    }

    std::string Schema::ResolvePinType(const Graph& InGraph, const Node& InNode, const PinDefinition& InPin) const
    {
        if(InPin.Type != WILDCARD_TYPE && InPin.Type != DERIVED_TYPE) { return InPin.Type; }

        /* A derived pin no override claimed has no better answer than the
         * node's own type. */
        const NodeDefinition* Def = FindNode(InNode.Type);
        return Def ? ResolveWildcard(InGraph, InNode, *Def) : std::string();
    }

    std::string Schema::DescribeNode(const Graph& /*InGraph*/, const Node& /*InNode*/) const
    {
        return std::string();
    }

    std::string Schema::GetWildcardType(const Graph& InGraph, const Node& InNode) const
    {
        const NodeDefinition* Def = InNode.IsPlaceholder() ? nullptr : FindNode(InNode.Type);
        return Def && Def->HasWildcard() ? ResolveWildcard(InGraph, InNode, *Def) : std::string();
    }

    std::string Schema::ResolveWildcard(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef) const
    {
        return ResolveWildcardDepth(InGraph, InNode, InDef, 0);
    }

    std::string Schema::ResolveWildcardDepth(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef,
                                             const int InDepth) const
    {
        if(InDef.WildcardTypes.empty()) { return std::string(); }

        const auto IsAllowed = [&](const std::string& InType)
        {
            return std::find(InDef.WildcardTypes.begin(), InDef.WildcardTypes.end(), InType) != InDef.WildcardTypes.end();
        };

        if(!InNode.WildcardType.empty() && IsAllowed(InNode.WildcardType))
        {
            return InNode.WildcardType;
        }

        /* Inferred from the far end of each link on a wildcard pin -- types
         * flow from sources, so the inputs' links decide, and the outputs'
         * only when no input does (a Constant feeding a vec3 input). When
         * links disagree, the type LATEST in WildcardTypes wins -- the widest,
         * as schemas order them (float before vec3) -- so the answer never
         * depends on the order links happen to be stored in. A chain of
         * wildcard nodes (Add into Multiply into Lerp) resolves through its
         * neighbours; the depth bound stops a cycle of them. */
        int Best = -1;
        for(int Pass = 0; Pass < 2 && Best < 0 && InDepth < 8; ++Pass)
        {
            const bool bInputs = Pass == 0;
            for(const Link& Each : InGraph.Links)
            {
                const bool bIn  = bInputs && Each.To.Node == InNode.Id;
                const bool bOut = !bInputs && Each.From.Node == InNode.Id;
                if(!bIn && !bOut) { continue; }

                const PinDefinition* Own = InDef.FindPin(bIn ? Each.To.Pin : Each.From.Pin,
                                                         bIn ? EPinDirection::Input : EPinDirection::Output);
                if(!Own || Own->Type != WILDCARD_TYPE) { continue; }

                const PinRef& Far = bIn ? Each.From : Each.To;
                const Node* FarNode = InGraph.FindNode(Far.Node);
                const NodeDefinition* FarDef = FarNode ? FindNode(FarNode->Type) : nullptr;
                const PinDefinition* FarPin = FarDef ? FarDef->FindPin(Far.Pin, bIn ? EPinDirection::Output : EPinDirection::Input) : nullptr;
                if(!FarPin || FarNode == &InNode) { continue; }

                const std::string FarType = FarPin->Type == WILDCARD_TYPE
                                          ? ResolveWildcardDepth(InGraph, *FarNode, *FarDef, InDepth + 1)
                                          : ResolvePinType(InGraph, *FarNode, *FarPin);   // fixed, or derived
                const auto Found = std::find(InDef.WildcardTypes.begin(), InDef.WildcardTypes.end(), FarType);
                if(Found != InDef.WildcardTypes.end())
                {
                    Best = std::max(Best, int(Found - InDef.WildcardTypes.begin()));
                }
            }
        }

        return InDef.WildcardTypes[size_t(std::max(Best, 0))];
    }

    void Schema::AddType(PinTypeInfo InType)
    {
        if(InType.DisplayName.empty()) { InType.DisplayName = InType.Name; }
        Types.push_back(std::move(InType));
    }

    void Schema::AddConversion(const std::string& InFrom, const std::string& InTo)
    {
        Conversions.insert({ InFrom, InTo });
    }

    NodeDefinition& Schema::AddNode(NodeDefinition InNode)
    {
        if(InNode.DisplayName.empty()) { InNode.DisplayName = InNode.Type; }

        const auto Existing = NodeIndex.find(InNode.Type);
        if(Existing != NodeIndex.end())
        {
            /* A later definition replaces an earlier one of the same name --
             * e.g. two components that sanitise to the same node type. */
            Nodes[Existing->second] = std::move(InNode);
            return Nodes[Existing->second];
        }

        NodeIndex.emplace(InNode.Type, Nodes.size());
        Nodes.push_back(std::move(InNode));
        return Nodes.back();
    }

    // === Registry ===============================================================

    const std::vector<const Schema*>& GetSchemas()
    {
        /* Built on first use, not at static initialisation: the script schema
         * reads GameCore's binding registry, which the other images fill in
         * during THEIR static initialisation. */
        static const std::vector<std::unique_ptr<Schema>> Owned = []
        {
            std::vector<std::unique_ptr<Schema>> Result;
            Result.push_back(CreateMaterialSchema());
            Result.push_back(CreateScriptSchema());
            return Result;
        }();

        static const std::vector<const Schema*> View = []
        {
            std::vector<const Schema*> Result;
            for(const std::unique_ptr<Schema>& Each : Owned) { Result.push_back(Each.get()); }
            return Result;
        }();
        return View;
    }

    const Schema* FindSchema(const std::string& InName)
    {
        for(const Schema* Each : GetSchemas())
        {
            if(Each->GetName() == InName) { return Each; }
        }
        return nullptr;
    }

    const Schema* FindSchemaByFileTag(const std::string& InTag)
    {
        std::string Lower = InTag;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](const unsigned char Ch) { return char(std::tolower(Ch)); });

        for(const Schema* Each : GetSchemas())
        {
            if(Each->GetFileTag() == Lower) { return Each; }
        }
        return nullptr;
    }

    bool IsIdentifier(const std::string& InText)
    {
        if(InText.empty() || (InText[0] >= '0' && InText[0] <= '9')) { return false; }
        for(const char Ch : InText)
        {
            const bool bOk = (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') || Ch == '_';
            if(!bOk) { return false; }
        }
        return true;
    }
}
