#ifndef NODEGRAPH_GRAPHCOMPILER_H
#define NODEGRAPH_GRAPHCOMPILER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/Schema.h"

/* Graph -> source text: ValidateGraph, then the schema's compiler. Messages
 * name their node (and pin). */
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

        /* Extra files beside the output, e.g. {".material", text}. */
        std::vector<std::pair<std::string, std::string>> Companions;

        std::vector<Diagnostic> Diagnostics;

        /* Per line of Code, the node that produced it (unset if none). */
        std::vector<NodeGuid> LineNodes;

        bool HasErrors() const;

        void Error  (const NodeGuid& InNode, const std::string& InPin, const std::string& InMessage);
        void Warning(const NodeGuid& InNode, const std::string& InPin, const std::string& InMessage);
    };

    /* Schema-independent checks: node types, ids, link kinds and types, link
     * multiplicity, unique nodes, property values. */
    void ValidateGraph(const Graph& InGraph, const Schema& InSchema, CompileResult& OutResult);

    /* Validates and compiles. On any error Code is left empty. */
    void CompileGraph(const Graph& InGraph, CompileResult& OutResult);

    /* "Script graph 'Mover': 2 errors" plus one line per message -- for logs. */
    std::string FormatDiagnostics(const Graph& InGraph, const CompileResult& InResult);
}

#endif //NODEGRAPH_GRAPHCOMPILER_H
