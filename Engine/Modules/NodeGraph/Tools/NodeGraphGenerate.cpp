#include <cstdio>
#include <cstring>
#include <string>

#include "Public/Core/CorePlatform.h"
#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/NodeGraph/Schema.h"

/*
 * NodeGraphGenerate [-module <game module>]... <graph.nmgraph>...
 *
 * What the editor does on save, from the command line: compiles each graph
 * and rewrites the generated code stored in it, <gen_code> (GenerateGraphOutput).
 * Prints each graph's diagnostics; exits 1 if any graph failed to compile or
 * write.
 *
 * -module loads a game module (SandboxGame.dll) for the script declarations
 * it registers -- its components, script libraries and script structs -- as
 * the editor does at startup, so graphs using them compile. Its dependencies
 * must resolve: run from the game's binaries folder.
 */
int main(const int InArgc, char** InArgv)
{
    if(InArgc < 2)
    {
        std::fprintf(stderr, "usage: NodeGraphGenerate [-module <game module>]... <graph.nmgraph>...\n");
        return 2;
    }

    /* Loaded first, so their static registrations exist when the schemas are
     * rebuilt; closed last, after the schemas no longer need them. */
    EngineCore::DLLHandle Modules[16] = {};
    int ModuleCount = 0;
    for(int Arg = 1; Arg + 1 < InArgc; ++Arg)
    {
        if(std::strcmp(InArgv[Arg], "-module") != 0) { continue; }
        if(ModuleCount == 16) { std::fprintf(stderr, "too many -module arguments\n"); return 2; }
        Modules[ModuleCount] = EngineCore::appDLLOpen(InArgv[Arg + 1]);
        if(!Modules[ModuleCount])
        {
            std::fprintf(stderr, "%s: %s\n", InArgv[Arg + 1], EngineCore::appDLLGetLastError().c_str());
            return 1;
        }
        ++ModuleCount;
    }
    if(ModuleCount > 0) { NodeGraph::RefreshSchemas(); }

    int Failures = 0;
    for(int Arg = 1; Arg < InArgc; ++Arg)
    {
        if(std::strcmp(InArgv[Arg], "-module") == 0) { ++Arg; continue; }
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

    for(int Index = ModuleCount - 1; Index >= 0; --Index) { EngineCore::appDLLClose(Modules[Index]); }
    return Failures == 0 ? 0 : 1;
}
