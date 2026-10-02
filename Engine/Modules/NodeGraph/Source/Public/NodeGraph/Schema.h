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
 * One kind of graph (Material, Script): pin types and conversions, node
 * definitions, and the compiler. Built on first use (GetSchemas()) and again
 * by RefreshSchemas(); the script schema generates component and library
 * nodes from GameCore's script bindings.
 */
namespace NodeGraph
{
    struct CompileResult;

    enum class EPinDirection : uint8_t { Input, Output };
    enum class EPinKind      : uint8_t { Data, Exec };

    /* A pin type that each node resolves to one of NodeDefinition::WildcardTypes
     * -- Math.Add is float, vec2, vec3 or vec4 in a material, as its links say. */
    constexpr const char* WILDCARD_TYPE = "*";

    /* Type set by the node's settings (a mask's output width), computed by
     * Schema::ResolvePinType; WildcardTypes lists the options. */
    constexpr const char* DERIVED_TYPE = "?";

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

        /* An Output node's input: the stage bits (the schema's, e.g. a
         * material's vertex or pixel) whatever feeds it is computed in.
         * 0: no requirement. */
        uint32_t      Stage = 0;

        /* An input with no literal: unlinked, the schema supplies this --
         * "Texture Coordinate" for a Texture Sample's UV. Tools show the
         * words instead of a constant editor. "" for an ordinary input. */
        std::string   Implicit;

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
        Bool,         // "true" or "false" -- a checkbox
        Texture,      // a texture asset's runtime-relative path ("Content/Textures/Brick.nmtexture"), or ""
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

        /* Narrowest first; the first is the unlinked default (ResolveWildcard). */
        std::vector<std::string> WildcardTypes;

        /* The schema fixes the type (e.g. a variable's); links can't change it. */
        bool bWildcardFromSchema = false;

        /* The schema's own emit instruction -- which operation, and a template
         * for a call. Opaque to everything but that schema's compiler. */
        uint32_t    Op = 0;
        std::string Template;

        /* The stages (PinDefinition::Stage bits) this node can be computed
         * in. All by default; a material's Pixel Depth is pixel-only. */
        uint32_t    StageMask = ~0u;

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

        /*
         * STAGES. A value linked into InNode's input InPin must be computable
         * in every stage GetRequiredStages returns: the PinDefinition::Stage
         * bits of the Output pins it eventually reaches (0: none). A node's
         * outputs can be computed in GetAvailableStages: its own StageMask
         * and every node upstream of it.
         */
        uint32_t GetRequiredStages(const Graph& InGraph, const Node& InNode, const std::string& InPin) const;
        uint32_t GetAvailableStages(const Graph& InGraph, const Node& InNode) const;

        /* Whether linking InFromNode's output into InToNode's input InToPin
         * respects stages; OutWhy says which stage cannot be had. */
        bool CanLinkStages(const Graph& InGraph, const Node& InFromNode, const Node& InToNode, const std::string& InToPin,
                           std::string* OutWhy = nullptr) const;

        /* A stage bit's name for messages ("vertex"); "" for none. */
        virtual std::string GetStageName(uint32_t InStageBit) const { return std::string(); }

        /* The concrete type of a pin on a node -- ResolvePinType's answer. */
        std::string GetPinType(const Graph& InGraph, const Node& InNode, const PinDefinition& InPin) const;

        /* InPin's concrete type: its own, the resolved wildcard, or (override)
         * a DERIVED_TYPE. All pin-type queries go through here. */
        virtual std::string ResolvePinType(const Graph& InGraph, const Node& InNode, const PinDefinition& InPin) const;

        /* A short setup summary for the node header (".xz -> vec2"); "" shows
         * the resolved wildcard type. */
        virtual std::string DescribeNode(const Graph& InGraph, const Node& InNode) const;

        /* The type a wildcard node's "*" pins have; "" for a node without them. */
        std::string GetWildcardType(const Graph& InGraph, const Node& InNode) const;

        /* Node::WildcardType if allowed, else inferred from links, else the
         * first allowed type. Overridden for schema-fixed types. */
        virtual std::string ResolveWildcard(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef) const;

        /* Schema-specific validation and codegen. Called by CompileGraph(). */
        virtual void Compile(const Graph& InGraph, CompileResult& OutResult) const = 0;

        /* Brings a graph an older editor saved up to date as it is read (a
         * file, or a pasted fragment): nodes whose Version is below their
         * definition's are rewritten -- a material's version 1 Texture Sample
         * gains a Texture Object, say. OutWarnings says what changed. */
        virtual void UpgradeGraph(Graph& InOutGraph, std::vector<std::string>& OutWarnings) const
        {
            (void)InOutGraph;
            (void)OutWarnings;
        }

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

    /*
     * Rebuilds every schema from GameCore's registries as they stand now --
     * component bindings and script libraries (ScriptLibrary.h) -- so the
     * declarations of a game module loaded since appear as nodes. Call it
     * while that module is loaded: what it declared is copied, and stays after
     * the module unloads. Main thread only.
     *
     * A Schema* or NodeDefinition* obtained earlier stays valid, but describes
     * the old declarations: a graph opened before the refresh keeps its
     * schema until it is reopened.
     */
    void RefreshSchemas();

    /* nullptr if no schema has that name. */
    const Schema* FindSchema(const std::string& InName);

    /* nullptr if no schema uses that file tag ("material", case-insensitive). */
    const Schema* FindSchemaByFileTag(const std::string& InTag);

    /* Letters, digits and '_', not starting with a digit -- an identifier in
     * both GLSL and Lua. */
    bool IsIdentifier(const std::string& InText);
}

#endif //NODEGRAPH_SCHEMA_H
