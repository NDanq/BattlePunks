#ifndef NODEGRAPH_GRAPHFILE_H
#define NODEGRAPH_GRAPHFILE_H

#include <string>
#include <vector>

#include "Public/NodeGraph/Graph.h"

/*
 * .nmgraph files: XML, one graph per file, the schema named twice -- by the
 * compound extension and by the root's Schema attribute:
 *
 *     Scripts/Mover.script.nmgraph      -> Scripts/Mover.lua
 *     Shaders/Glow.material.nmgraph     -> Shaders/Glow.fsh
 *
 * The extension is what tools route on (File > Open, Cook Content's search,
 * the output's name); the attribute is a check, and a mismatch is a load
 * error. A bare *.nmgraph falls back to the attribute, with a warning.
 *
 * Written deterministically -- nodes by GUID, links by their ends, fixed
 * attribute order, shortest round-trip numbers -- so an unchanged graph saves
 * to the same bytes and a changed one diffs as the change. See README.md for
 * the element-by-element format.
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

    /* "a/Glow.material.nmgraph" -> "a/Glow": the whole compound extension off
     * the file name (not just ".nmgraph"); a bare "a/Glow.nmgraph" -> "a/Glow". */
    std::string StripGraphExtension(const std::string& InPath);

    /* "a/Glow" -> "a/Glow.material.nmgraph". */
    std::string MakeGraphPath(const std::string& InBasePath, const Schema& InSchema);

    /* Where the generated source goes: "a/Glow.material.nmgraph" -> "a/Glow.fsh". */
    std::string GetGraphOutputPath(const std::string& InGraphPath, const Schema& InSchema);

    /* The file name without directory or extension: "a/Glow.material.nmgraph" -> "Glow". */
    std::string GetGraphBaseName(const std::string& InPath);

    // === Text ===================================================================

    /* The file's bytes for InGraph. */
    std::string WriteGraphText(const Graph& InGraph);

    /*
     * Parses a file's text. False, with OutError, on bad XML, an unknown
     * schema or a malformed element that cannot be skipped. Recoverable
     * problems (a link to a missing node, an unknown node type) load anyway
     * and are described in OutWarnings.
     */
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

    /*
     * Loads InGraphPath, compiles it and writes the output -- and any
     * companion files (CompileResult::Companions) -- beside it, each only if
     * its text changed. False with OutError when the file cannot be read or
     * written; a graph that fails to COMPILE returns true with the errors in
     * OutResult and the old output left in place.
     */
    bool GenerateGraphOutput(const std::string& InGraphPath, CompileResult& OutResult, std::string& OutError);

    // === Clipboard ==============================================================

    /* The selected nodes and the links among them, as a <NodeGraphFragment>. */
    std::string WriteGraphFragment(const Graph& InGraph, const std::vector<NodeGuid>& InSelection);

    /*
     * Adds a fragment to InOutGraph: every node gets a fresh GUID and is
     * offset by (InOffsetX, InOffsetY); links are remapped onto the copies.
     * False if the text is not a fragment for InOutGraph's schema. OutNewNodes
     * receives the copies' GUIDs.
     */
    bool PasteGraphFragment(const std::string& InText, Graph& InOutGraph, float InOffsetX, float InOffsetY,
                            std::vector<NodeGuid>& OutNewNodes);
}

#endif //NODEGRAPH_GRAPHFILE_H
