#include "Public/NodeGraph/Schema.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>

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

    const NodeDefinition* Schema::FindDefinition(const Graph& /*InGraph*/, const Node& InNode) const
    {
        return InNode.IsPlaceholder() ? nullptr : FindNode(InNode.Type);
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

    namespace
    {
        uint32_t RequiredStages(const Schema& InSchema, const Graph& InGraph, const Node& InNode, const std::string& InPin,
                                std::set<NodeGuid>& InOutVisited)
        {
            const NodeDefinition* Def = InSchema.FindDefinition(InGraph, InNode);
            if(!Def) { return 0; }
            if(Def->Role == ENodeRole::Output)
            {
                const PinDefinition* Pin = Def->FindPin(InPin, EPinDirection::Input);
                return Pin ? Pin->Stage : 0;
            }
            if(!InOutVisited.insert(InNode.Id).second) { return 0; }   // a cycle; reported elsewhere

            uint32_t Stages = 0;
            for(const Link& Each : InGraph.Links)
            {
                if(Each.From.Node != InNode.Id) { continue; }
                if(const Node* Consumer = InGraph.FindNode(Each.To.Node))
                {
                    Stages |= RequiredStages(InSchema, InGraph, *Consumer, Each.To.Pin, InOutVisited);
                }
            }
            return Stages;
        }

        uint32_t AvailableStages(const Schema& InSchema, const Graph& InGraph, const Node& InNode, std::set<NodeGuid>& InOutVisited)
        {
            const NodeDefinition* Def = InSchema.FindDefinition(InGraph, InNode);
            uint32_t Stages = Def ? Def->StageMask : ~0u;
            if(!InOutVisited.insert(InNode.Id).second) { return Stages; }

            for(const Link& Each : InGraph.Links)
            {
                if(Each.To.Node != InNode.Id) { continue; }
                if(const Node* Source = InGraph.FindNode(Each.From.Node))
                {
                    Stages &= AvailableStages(InSchema, InGraph, *Source, InOutVisited);
                }
            }
            return Stages;
        }
    }

    uint32_t Schema::GetRequiredStages(const Graph& InGraph, const Node& InNode, const std::string& InPin) const
    {
        std::set<NodeGuid> Visited;
        return RequiredStages(*this, InGraph, InNode, InPin, Visited);
    }

    uint32_t Schema::GetAvailableStages(const Graph& InGraph, const Node& InNode) const
    {
        std::set<NodeGuid> Visited;
        return AvailableStages(*this, InGraph, InNode, Visited);
    }

    bool Schema::CanLinkStages(const Graph& InGraph, const Node& InFromNode, const Node& InToNode, const std::string& InToPin,
                               std::string* OutWhy) const
    {
        const uint32_t Required  = GetRequiredStages(InGraph, InToNode, InToPin);
        const uint32_t Available = GetAvailableStages(InGraph, InFromNode);
        const uint32_t Missing   = Required & ~Available;
        if(Missing == 0) { return true; }

        if(OutWhy)
        {
            uint32_t Bit = 1;
            while(!(Missing & Bit)) { Bit <<= 1; }
            const NodeDefinition* Def = FindDefinition(InGraph, InFromNode);
            const std::string Stage = GetStageName(Bit);
            *OutWhy = (Def ? Def->DisplayName : InFromNode.Type) + " (or something feeding it) can't be computed in the "
                    + (Stage.empty() ? std::string("required") : Stage) + " stage, which this input leads to";
        }
        return false;
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
        const NodeDefinition* Def = FindDefinition(InGraph, InNode);
        return Def ? ResolveWildcard(InGraph, InNode, *Def) : std::string();
    }

    std::string Schema::DescribeNode(const Graph& /*InGraph*/, const Node& /*InNode*/) const
    {
        return std::string();
    }

    std::string Schema::GetWildcardType(const Graph& InGraph, const Node& InNode) const
    {
        const NodeDefinition* Def = FindDefinition(InGraph, InNode);
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

        /* From the far end of each wildcard link: inputs decide, outputs only
         * if no input does. On disagreement the latest (widest) WildcardType
         * wins, independent of link order. Wildcard chains resolve through
         * neighbours; the depth bound stops cycles. */
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
                const NodeDefinition* FarDef = FarNode ? FindDefinition(InGraph, *FarNode) : nullptr;
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

    namespace
    {
        /* Retired schemas are kept to the end of the process: an open graph
         * and its node items point into them (RefreshSchemas). A schema is a
         * few hundred node definitions, and a refresh is a person's action. */
        struct SchemaSet
        {
            std::vector<std::unique_ptr<Schema>> Live;
            std::vector<std::unique_ptr<Schema>> Retired;
            std::vector<const Schema*>           View;

            void Build()
            {
                for(std::unique_ptr<Schema>& Each : Live) { Retired.push_back(std::move(Each)); }
                Live.clear();
                Live.push_back(CreateMaterialSchema());
                Live.push_back(CreateScriptSchema());
                Live.push_back(CreateUIScriptSchema());
        Live.push_back(CreatePostProcessSchema());

                View.clear();
                for(const std::unique_ptr<Schema>& Each : Live) { View.push_back(Each.get()); }
            }
        };

        /* On first use: the script schema reads bindings other images register
         * during their static init. */
        SchemaSet& GetSchemaSet()
        {
            static SchemaSet Set;
            if(Set.Live.empty()) { Set.Build(); }
            return Set;
        }
    }

    const std::vector<const Schema*>& GetSchemas()
    {
        return GetSchemaSet().View;
    }

    void RefreshSchemas()
    {
        GetSchemaSet().Build();
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
