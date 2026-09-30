#ifndef NODEGRAPH_GRAPHINDEX_H
#define NODEGRAPH_GRAPHINDEX_H

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/Schema.h"

/*
 * What the schema compilers share: lookups over one graph that the model's
 * linear searches would make quadratic, and the text helpers every literal
 * goes through. Private to the module.
 */
namespace NodeGraph
{
    /* The schema factories, called once by GetSchemas(). */
    std::unique_ptr<Schema> CreateMaterialSchema();
    std::unique_ptr<Schema> CreateScriptSchema();

    class GraphIndex
    {
    public:
        GraphIndex(const Graph& InGraph, const Schema& InSchema);

        const Graph&  GetGraph() const  { return G; }
        const Schema& GetSchema() const { return S; }

        const Node*           FindNode(const NodeGuid& InId) const;
        const NodeDefinition* FindDefinition(const Node& InNode) const;   // nullptr for a placeholder

        /* The link into an input pin, or nullptr. */
        const Link* LinkInto(const NodeGuid& InNode, const std::string& InPin) const;

        /* Every link out of an output pin, in graph order. */
        const std::vector<const Link*>& LinksOutOf(const NodeGuid& InNode, const std::string& InPin) const;

        /* The concrete type of a pin -- Schema::ResolvePinType, cached. */
        std::string PinType(const Node& InNode, const PinDefinition& InPin) const;

        /* The nodes, in GUID order -- the order every compiler walks roots in,
         * so output does not depend on the order nodes were created. */
        const std::vector<const Node*>& SortedNodes() const { return Sorted; }

    private:
        const Graph&  G;
        const Schema& S;
        std::unordered_map<NodeGuid, const Node*, GameCore::EntityGuidHash> Nodes;
        std::map<PinRef, const Link*>              Into;
        std::map<PinRef, std::vector<const Link*>> OutOf;
        std::vector<const Node*>                   Sorted;
        mutable std::map<std::pair<const Node*, std::string>, std::string> TypeCache;
    };

    // === Literals ===============================================================

    /* std::to_chars' shortest round-trip form -- the same text for the same
     * value every time, and locale-independent. */
    std::string FormatNumber(double InValue);
    std::string FormatNumber(float InValue);

    bool ParseNumber(const std::string& InText, double& OutValue);

    /*
     * Whitespace-separated numbers into exactly InCount values. One number
     * fills every lane ("1" as a vec3 is 1 1 1); an empty string is all zeros.
     * False if the text is neither.
     */
    bool ParseNumbers(const std::string& InText, size_t InCount, std::vector<double>& OutValues);

    /* A-Z, a-z, 0-9 and '_' kept, everything else '_'; a leading digit gets a
     * '_' in front. "Glow Ring" -> "Glow_Ring". */
    std::string SanitizeIdentifier(const std::string& InText);

    std::string ShortGuid(const NodeGuid& InGuid);   // the first 8 hex digits, for messages
}

#endif //NODEGRAPH_GRAPHINDEX_H
