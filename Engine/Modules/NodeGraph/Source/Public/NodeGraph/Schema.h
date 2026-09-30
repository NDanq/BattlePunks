#ifndef NODEGRAPH_SCHEMA_H
#define NODEGRAPH_SCHEMA_H

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Public/NodeGraph/Graph.h"

/*
 * What a graph's nodes mean. A Schema is one kind of graph -- Material,
 * Script -- and owns its pin types, which types connect (and by what
 * conversion), its node definitions, and the compiler that turns a graph
 * into source text. Graph.h holds only names; this is where they resolve.
 *
 * The schemas are built once, on first use (GetSchemas()). The script schema
 * reads GameCore's component script bindings at that moment, so every
 * component a loaded image registered shows up as nodes with no per-component
 * code here.
 */
namespace NodeGraph
{
    struct CompileResult;

    enum class EPinDirection : uint8_t { Input, Output };
    enum class EPinKind      : uint8_t { Data, Exec };

    /* A pin type that each node resolves to one of NodeDefinition::WildcardTypes
     * -- Math.Add is float, vec2, vec3 or vec4 in a material, as its links say. */
    constexpr const char* WILDCARD_TYPE = "*";

    struct PinTypeInfo
    {
        std::string Name;          // "vec3", "number", "Component:Transform"
        std::string DisplayName;
        uint32_t    Color = 0x9A9A9A;   // 0xRRGGBB, the pin and link colour
        std::string DefaultLiteral;     // an unlinked input's value when neither node nor pin gives one
        bool        bHasLiteral = true; // false: an input of this type must be linked (a component)
    };

    struct PinDefinition
    {
        std::string   Name;
        EPinDirection Direction = EPinDirection::Input;
        EPinKind      Kind      = EPinKind::Data;
        std::string   Type;      // a PinTypeInfo name, or WILDCARD_TYPE; "" for exec
        std::string   Default;   // literal; empty = the type's DefaultLiteral
        std::string   Tooltip;

        /* A per-output expression, for schemas that build values from
         * templates -- "{A} + {B}". Its meaning is the schema's. */
        std::string   Expression;

        bool IsInput()  const { return Direction == EPinDirection::Input; }
        bool IsOutput() const { return Direction == EPinDirection::Output; }
        bool IsExec()   const { return Kind == EPinKind::Exec; }
    };

    enum class EPropertyKind : uint8_t
    {
        String,       // free text
        Identifier,   // letters, digits, '_', not starting with a digit
        Choice,       // one of Choices
        Variable,     // the name of one of the graph's Variables
    };

    struct PropertyDefinition
    {
        std::string              Name;
        EPropertyKind            Kind = EPropertyKind::String;
        std::string              Default;
        std::vector<std::string> Choices;
        std::string              Tooltip;
    };

    enum class ENodeRole : uint8_t
    {
        Pure,        // no exec pins: a value, evaluated where it is used
        Statement,   // exec in and out: runs when control reaches it
        Event,       // exec out only: where control starts (OnTick)
        Output,      // a root of a graph with no exec flow (a material's pixel output)
    };

    struct NodeDefinition
    {
        std::string Type;           // unique within the schema: "Math.Add"
        std::string DisplayName;    // "Add"
        std::string Category;       // palette folder, '/'-separated: "Math/Trigonometry"
        std::string Keywords;       // extra search words
        std::string Tooltip;
        unsigned    Version = 1;
        ENodeRole   Role    = ENodeRole::Pure;
        bool        bUnique = false;   // at most one per graph

        std::vector<PinDefinition>      Pins;
        std::vector<PropertyDefinition> Properties;

        /* What "*" pins may resolve to, narrowest first: the first is the type
         * when nothing links the node, and when links disagree the latest of
         * theirs wins (see Schema::ResolveWildcard). */
        std::vector<std::string> WildcardTypes;

        /* The schema resolves this node's wildcard itself (a variable node
         * takes its variable's type): links and the editor never change it. */
        bool bWildcardFromSchema = false;

        /* The schema's own emit instruction -- which operation, and a template
         * for a call. Opaque to everything but that schema's compiler. */
        uint32_t    Op = 0;
        std::string Template;

        const PinDefinition*      FindPin(const std::string& InName) const;
        const PinDefinition*      FindPin(const std::string& InName, EPinDirection InDirection) const;
        const PropertyDefinition* FindProperty(const std::string& InName) const;
        bool                      HasWildcard() const { return !WildcardTypes.empty(); }
    };

    class Schema
    {
    public:
        virtual ~Schema() = default;

        const std::string& GetName() const            { return Name; }
        const std::string& GetFileTag() const         { return FileTag; }           // "material" in X.material.nmgraph
        const std::string& GetOutputExtension() const { return OutputExtension; }   // ".fsh"

        const std::vector<NodeDefinition>& GetNodes() const { return Nodes; }
        const NodeDefinition*              FindNode(const std::string& InType) const;

        const std::vector<PinTypeInfo>& GetTypes() const { return Types; }
        const PinTypeInfo*              FindType(const std::string& InName) const;

        /* Graph-wide settings (Graph::Properties) and the types a graph
         * variable may have. No variable types: the schema has no variables. */
        const std::vector<PropertyDefinition>& GetGraphProperties() const { return GraphProperties; }
        const std::vector<std::string>&        GetVariableTypes() const   { return VariableTypes; }

        /* Whether an output of InFrom may feed an input of InTo: the same type,
         * or one the compiler converts. Neither may be the wildcard. */
        bool CanConvert(const std::string& InFrom, const std::string& InTo) const;

        /* The concrete type of a pin on a node: the pin's own type, or for a
         * wildcard the node's resolved type (see ResolveWildcard). */
        std::string GetPinType(const Graph& InGraph, const Node& InNode, const PinDefinition& InPin) const;

        /* The type a wildcard node's "*" pins have; "" for a node without them. */
        std::string GetWildcardType(const Graph& InGraph, const Node& InNode) const;

        /*
         * The type a wildcard node resolves to: Node::WildcardType if it is
         * allowed; else inferred from what its wildcard pins link to; else the
         * first allowed type. A schema overrides it for nodes whose type comes
         * from elsewhere (a variable getter takes its variable's type).
         */
        virtual std::string ResolveWildcard(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef) const;

        /* Validates InGraph beyond what ValidateGraph() checks generically and
         * writes the source. Called by CompileGraph(), never directly. */
        virtual void Compile(const Graph& InGraph, CompileResult& OutResult) const = 0;

    protected:
        Schema(std::string InName, std::string InFileTag, std::string InOutputExtension);

        void            AddType(PinTypeInfo InType);
        void            AddConversion(const std::string& InFrom, const std::string& InTo);
        NodeDefinition& AddNode(NodeDefinition InNode);

        std::vector<PropertyDefinition> GraphProperties;
        std::vector<std::string>        VariableTypes;

    private:
        std::string ResolveWildcardDepth(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef, int InDepth) const;

        std::string Name;
        std::string FileTag;
        std::string OutputExtension;

        std::vector<PinTypeInfo>                Types;
        std::vector<NodeDefinition>             Nodes;
        std::unordered_map<std::string, size_t> NodeIndex;
        std::set<std::pair<std::string, std::string>> Conversions;
    };

    /* Every schema, built on first call. Material first, then Script. */
    const std::vector<const Schema*>& GetSchemas();

    /* nullptr if no schema has that name. */
    const Schema* FindSchema(const std::string& InName);

    /* nullptr if no schema uses that file tag ("material", case-insensitive). */
    const Schema* FindSchemaByFileTag(const std::string& InTag);

    /* Letters, digits and '_', not starting with a digit -- an identifier in
     * both GLSL and Lua. */
    bool IsIdentifier(const std::string& InText);
}

#endif //NODEGRAPH_SCHEMA_H
