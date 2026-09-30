#ifndef NODEGRAPH_GRAPH_H
#define NODEGRAPH_GRAPH_H

#include <map>
#include <string>
#include <vector>

#include "Public/GameCore/EntityGuid.h"

/*
 * A node graph as data: what a .nmgraph file holds and what the editor edits.
 * Nothing here knows what a node DOES -- a node is a type name plus values,
 * and its pins are looked up in the Schema that the graph names (Schema.h).
 * That is what lets a graph whose node type has since disappeared still load,
 * keep its links and save unchanged (Node::PlaceholderXml).
 */
namespace NodeGraph
{
    /* A node's persistent identity: the entity GUID type, reused -- 128 random
     * bits, written as 32 hex digits. Links name nodes by it, never by index,
     * so a merge that adds nodes on both sides does not renumber anything. */
    using NodeGuid = GameCore::EntityGuid;

    std::string GuidToString(const NodeGuid& InGuid);
    bool        GuidFromString(const std::string& InText, NodeGuid& OutGuid);

    /* One pin of one node. Pins are named, not numbered, so adding a pin to a
     * node type leaves every existing link pointing where it did. */
    struct PinRef
    {
        NodeGuid    Node;
        std::string Pin;

        friend bool operator==(const PinRef& L, const PinRef& R) { return L.Node == R.Node && L.Pin == R.Pin; }
        friend bool operator!=(const PinRef& L, const PinRef& R) { return !(L == R); }
        friend bool operator<(const PinRef& L, const PinRef& R)  { return L.Node != R.Node ? L.Node < R.Node : L.Pin < R.Pin; }
    };

    /* Always output -> input, whichever way the user dragged it. */
    struct Link
    {
        PinRef From;
        PinRef To;

        friend bool operator==(const Link& L, const Link& R) { return L.From == R.From && L.To == R.To; }
        friend bool operator<(const Link& L, const Link& R)  { return L.From != R.From ? L.From < R.From : L.To < R.To; }
    };

    struct Node
    {
        NodeGuid    Id;
        std::string Type;          // "Math.Add", "Component.Transform.SetPosition"
        unsigned    Version = 1;   // the NodeDefinition's version when this was saved
        float       X = 0.0f;
        float       Y = 0.0f;

        /* Literal values of UNLINKED input pins, as text in the pin type's
         * literal form ("0.5", "1 0 0", "Hello"). Only values that differ from
         * the pin's default are kept, so a default change reaches old graphs. */
        std::map<std::string, std::string> Constants;

        /* Per-node settings that are not pins: a parameter's name, the event a
         * handler listens to, the variable a getter reads. */
        std::map<std::string, std::string> Properties;

        /* The concrete type a wildcard ("*") node was resolved to -- "vec3",
         * "number". Empty lets the compiler infer it from the links. */
        std::string WildcardType;

        /* Non-empty: this node's type is unknown to its schema. The element
         * as read, written back verbatim on save so nothing is lost; the node
         * shows as an error and fails compilation. */
        std::string PlaceholderXml;

        bool IsPlaceholder() const { return !PlaceholderXml.empty(); }

        const std::string* FindConstant(const std::string& InPin) const;
        const std::string* FindProperty(const std::string& InName) const;
        std::string        GetProperty(const std::string& InName, const std::string& InDefault = std::string()) const;
    };

    /* A graph-wide value a script graph reads and writes (self.<Name>). */
    struct Variable
    {
        std::string Name;
        std::string Type;      // one of the schema's variable types
        std::string Default;   // literal, as Node::Constants
    };

    /* A labelled box behind a group of nodes. Editor-only; compiles to nothing. */
    struct Comment
    {
        float       X = 0.0f, Y = 0.0f, Width = 0.0f, Height = 0.0f;
        std::string Text;
    };

    class Graph
    {
    public:
        std::string SchemaName;   // "Material", "Script"
        std::string Name;         // the file's base name; names generated identifiers

        /* Graph-wide settings declared by the schema (a script's TickPhase). */
        std::map<std::string, std::string> Properties;

        std::vector<Variable> Variables;
        std::vector<Node>     Nodes;
        std::vector<Link>     Links;
        std::vector<Comment>  Comments;

        Node*       FindNode(const NodeGuid& InId);
        const Node* FindNode(const NodeGuid& InId) const;

        const Variable* FindVariable(const std::string& InName) const;

        std::string GetProperty(const std::string& InName, const std::string& InDefault = std::string()) const;

        /* The link feeding an input pin, or nullptr. A data input has at most
         * one; for an exec input this is the first of possibly several. */
        const Link* FindLinkTo(const PinRef& InInput) const;

        /* Every link leaving an output pin, in stored order. */
        std::vector<const Link*> FindLinksFrom(const PinRef& InOutput) const;

        bool HasLink(const Link& InLink) const;

        /* Removes the node and every link touching it. False if absent. */
        bool RemoveNode(const NodeGuid& InId);

        /* Removes every link touching the pin, whichever end. */
        void RemoveLinksAt(const PinRef& InPin);

        bool RemoveLink(const Link& InLink);
    };
}

#endif //NODEGRAPH_GRAPH_H
