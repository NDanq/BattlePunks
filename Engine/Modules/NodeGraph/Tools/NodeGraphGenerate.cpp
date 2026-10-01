#include <cstdio>
#include <string>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/GraphFile.h"

/*
 * NodeGraphGenerate <graph.nmgraph>...
 *
 * What the editor does on save, from the command line: compiles each graph
 * and writes its changed output and companions beside it (GenerateGraphOutput).
 * Prints each graph's diagnostics; exits 1 if any graph failed to compile or
 * write.
 */
int main(const int InArgc, char** InArgv)
{
    if(InArgc < 2)
    {
        std::fprintf(stderr, "usage: NodeGraphGenerate <graph.nmgraph>...\n");
        return 2;
    }

    int Failures = 0;
    for(int Arg = 1; Arg < InArgc; ++Arg)
    {
        const std::string Path = InArgv[Arg];

        NodeGraph::CompileResult Result;
        std::string Error;
        if(!NodeGraph::GenerateGraphOutput(Path, Result, Error))
        {
            std::fprintf(stderr, "%s: %s\n", Path.c_str(), Error.c_str());
            ++Failures;
            continue;
        }

        NodeGraph::Graph Loaded;
        std::string LoadError;
        NodeGraph::LoadGraphFile(Path, Loaded, LoadError);
        if(!Result.Diagnostics.empty())
        {
            std::printf("%s\n", NodeGraph::FormatDiagnostics(Loaded, Result).c_str());
        }
        if(Result.HasErrors())
        {
            ++Failures;
            continue;
        }
        std::printf("generated %s\n", Path.c_str());
    }
    return Failures == 0 ? 0 : 1;
}
