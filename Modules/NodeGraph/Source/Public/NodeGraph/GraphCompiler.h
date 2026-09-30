#ifndef NODEGRAPH_GRAPHCOMPILER_H
#define NODEGRAPH_GRAPHCOMPILER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/Schema.h"

/*
 * Graph -> source text. CompileGraph() validates what every schema shares
 * (ValidateGraph), then hands the graph to its schema's compiler. Every
 * message names the node (and pin) it is about, so the editor can mark it.
 */
namespace NodeGraph
{
    enum class ESeverity : uint8_t { Info, Warning, Error };

    struct Diagnostic
    {
        ESeverity   Severity = ESeverity::Error;
        NodeGuid    Node;      // unset: about the graph as a whole
        std::string Pin;       // "" when not about one pin
        std::string Message;
    };

    struct CompileResult
    {
        /* The generated source; empty if compilation failed. */
        std::string Code;

        /* Further files the output needs, written beside it with the same
         * base name: {".material", text} for a material's parameter defaults. */
        std::vector<std::pair<std::string, std::string>> Companions;

        std::vector<Diagnostic> Diagnostics;

        /* For each line of Code (0-based), the node that produced it -- unset
         * for lines no single node owns. Lets the editor go from a line of the
         * code view to its node. */
        std::vector<NodeGuid> LineNodes;

        bool HasErrors() const;

        void Error  (const NodeGuid& InNode, const std::string& InPin, const std::string& InMessage);
        void Warning(const NodeGuid& InNode, const std::string& InPin, const std::string& InMessage);
    };

    /*
     * The checks every schema needs: known node types, unique ids, links that
     * join an output to an input of the same kind and compatible type, at
     * most one link into a data input or out of an exec output, unique nodes
     * that appear at most once, valid property values. Appends to OutResult.
     */
    void ValidateGraph(const Graph& InGraph, const Schema& InSchema, CompileResult& OutResult);

    /* Validates and compiles. On any error Code is left empty. */
    void CompileGraph(const Graph& InGraph, CompileResult& OutResult);

    /* "Script graph 'Mover': 2 errors" plus one line per message -- for logs. */
    std::string FormatDiagnostics(const Graph& InGraph, const CompileResult& InResult);
}

#endif //NODEGRAPH_GRAPHCOMPILER_H
