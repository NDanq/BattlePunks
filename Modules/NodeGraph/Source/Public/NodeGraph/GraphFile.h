#ifndef NODEGRAPH_GRAPHFILE_H
#define NODEGRAPH_GRAPHFILE_H

#include <string>
#include <vector>

#include "Public/NodeGraph/Graph.h"

/*
 * .nmgraph files: XML, one graph each, the schema named by both the compound
 * extension and the Schema attribute (a mismatch fails the load):
 *
 *     Scripts/Mover.script.nmgraph      <gen_code>: .lua
 *     Materials/Glow.material.nmgraph   <gen_code>: .mtl.glsl, .material
 *
 * The generated code is stored in the graph file itself, at its end
 * (<gen_code>, one <Output Extension="..."> CDATA each): nothing is written
 * beside the graph. A game's cook and the runtime read it from there.
 *
 * Tools route on the extension; a bare *.nmgraph falls back to the attribute.
 * Written deterministically for clean diffs. Format: README.md.
 */
namespace NodeGraph
{
    class Schema;
    struct CompileResult;

    constexpr const char* GRAPH_EXTENSION   = ".nmgraph";
    constexpr int         GRAPH_FILE_FORMAT = 1;

    // === Paths ==================================================================

    /* "material" for "a/Glow.material.nmgraph" (lower-cased); "" when the name
     * does not end in .<tag>.nmgraph. */
    std::string GetGraphFileTag(const std::string& InPath);

    /* The schema the extension names; nullptr for a bare or unknown tag. */
    const Schema* FindSchemaForPath(const std::string& InPath);

    /* "a/Glow.material.nmgraph" or "a/Glow.nmgraph" -> "a/Glow". */
    std::string StripGraphExtension(const std::string& InPath);

    /* "a/Glow" -> "a/Glow.material.nmgraph". */
    std::string MakeGraphPath(const std::string& InBasePath, const Schema& InSchema);

    /* The generated source's name, were it a file: "a/Glow.material.nmgraph" -> "a/Glow.mtl.glsl". */
    std::string GetGraphOutputPath(const std::string& InGraphPath, const Schema& InSchema);

    /* The file name without directory or extension: "a/Glow.material.nmgraph" -> "Glow". */
    std::string GetGraphBaseName(const std::string& InPath);

    // === Text ===================================================================

    /* The file's bytes for InGraph. */
    std::string WriteGraphText(const Graph& InGraph);

    /* False, with OutError, on unrecoverable problems; recoverable ones load
     * with OutWarnings. */
    bool ReadGraphText(const std::string& InText, Graph& OutGraph, std::string& OutError,
                       std::vector<std::string>* OutWarnings = nullptr);

    // === Files ==================================================================

    /* Reads InPath and checks its extension against its Schema attribute.
     * Sets Graph::Name to the file's base name. */
    bool LoadGraphFile(const std::string& InPath, Graph& OutGraph, std::string& OutError,
                       std::vector<std::string>* OutWarnings = nullptr);

    /* Writes through "<path>.tmp" and a replacing rename, creating missing
     * directories; leaves the file untouched when the bytes are unchanged. */
    bool SaveGraphFile(const std::string& InPath, const Graph& InGraph, std::string& OutError);

    /* Writes InText to InPath the same way, if it differs from what is there. */
    bool WriteTextFileIfChanged(const std::string& InPath, const std::string& InText, std::string& OutError);

    /* InResult (clean) as InOutGraph's GenCode: the schema's output, then the
     * companions. False, GenCode untouched, if InResult has errors. */
    bool StoreGeneratedCode(Graph& InOutGraph, const CompileResult& InResult);

    /* Compiles InGraphPath and rewrites its <gen_code> (the file is untouched
     * when nothing changed). False only on I/O errors; compile errors return
     * true with OutResult set and the old <gen_code> kept. */
    bool GenerateGraphOutput(const std::string& InGraphPath, CompileResult& OutResult, std::string& OutError);

    // === Clipboard ==============================================================

    /* The selected nodes and the links among them, as a <NodeGraphFragment>. */
    std::string WriteGraphFragment(const Graph& InGraph, const std::vector<NodeGuid>& InSelection);

    /* Pastes with fresh GUIDs, offset by (InOffsetX, InOffsetY), links
     * remapped. False if not a fragment for this schema. */
    bool PasteGraphFragment(const std::string& InText, Graph& InOutGraph, float InOffsetX, float InOffsetY,
                            std::vector<NodeGuid>& OutNewNodes);
}

#endif //NODEGRAPH_GRAPHFILE_H
