/*
 * NodeGraphTests -- format, paths, validation and both code generators.
 *
 * GOLDEN FILES in Tests/Data/; a missing one is written and fails once. Set
 * NODEGRAPH_UPDATE_GOLDEN=1 to rewrite them after an intended change.
 *
 * The generated Lua is also run on a ticked entity.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/NodeGraph/Schema.h"

using namespace NodeGraph;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

namespace
{
    // === Helpers ================================================================

    NodeGuid Id(const uint64_t InNumber)
    {
        NodeGuid Guid;
        Guid.Hi = 0x6e6f646567726170ull;   // fixed, so golden files are stable
        Guid.Lo = InNumber;
        return Guid;
    }

    Node& AddNode(Graph& InGraph, const uint64_t InId, const char* InType, const float InX = 0.0f, const float InY = 0.0f)
    {
        Node Added;
        Added.Id   = Id(InId);
        Added.Type = InType;
        Added.X    = InX;
        Added.Y    = InY;
        InGraph.Nodes.push_back(Added);
        return InGraph.Nodes.back();
    }

    void Connect(Graph& InGraph, const uint64_t InFrom, const char* InFromPin, const uint64_t InTo, const char* InToPin)
    {
        InGraph.Links.push_back({ PinRef{ Id(InFrom), InFromPin }, PinRef{ Id(InTo), InToPin } });
    }

    std::string TempDir()
    {
        const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "NimbleNodeGraphTests";
        std::error_code Ignored;
        std::filesystem::create_directories(Dir, Ignored);
        return Dir.generic_string();
    }

    std::string ReadFile(const std::string& InPath)
    {
        std::ifstream Stream(InPath, std::ios::binary);
        std::stringstream Buffer;
        Buffer << Stream.rdbuf();
        return Buffer.str();
    }

    void WriteFile(const std::string& InPath, const std::string& InText)
    {
        std::ofstream(InPath, std::ios::binary | std::ios::trunc) << InText;
    }

    void ExpectGolden(const std::string& InName, const std::string& InText)
    {
        const std::string Path = std::string(NODEGRAPH_TEST_DATA_DIR) + "/" + InName;
        const bool bUpdate = std::getenv("NODEGRAPH_UPDATE_GOLDEN") != nullptr;

        if(bUpdate || !std::filesystem::exists(Path))
        {
            std::filesystem::create_directories(std::filesystem::path(Path).parent_path());
            WriteFile(Path, InText);
            Expect(bUpdate, "golden file " + InName + " did not exist; written -- review it and run again");
            return;
        }

        const std::string Expected = ReadFile(Path);
        Expect(Expected == InText, "output matches golden file " + InName);
        if(Expected != InText)
        {
            std::printf("----- expected (%s)\n%s----- got\n%s-----\n", InName.c_str(), Expected.c_str(), InText.c_str());
        }
    }

    bool HasErrorOn(const CompileResult& InResult, const NodeGuid& InNode)
    {
        for(const Diagnostic& Each : InResult.Diagnostics)
        {
            if(Each.Severity == ESeverity::Error && Each.Node == InNode) { return true; }
        }
        return false;
    }

    // === Sample graphs ==========================================================

    /* Games/Sandbox/Scripts/Mover.lua as a graph: slides along +x at Speed. */
    Graph MakeMoverGraph()
    {
        Graph G;
        G.SchemaName = "Script";
        G.Name       = "Mover";
        G.Properties["TickPhase"] = "PostPhysics";
        G.Variables.push_back({ "Speed", "number", "60" });

        AddNode(G, 1, "Event.OnStart", -400, -120);
        AddNode(G, 2, "Debug.Print", -120, -120);
        AddNode(G, 3, "Entity.GetName", -400, -40);
        AddNode(G, 4, "String.Append", -260, -40).Constants["A"] = "Mover started on ";
        Connect(G, 1, "Then", 2, "Exec");
        Connect(G, 3, "Name", 4, "B");
        Connect(G, 4, "Out", 2, "Value");

        AddNode(G, 10, "Event.OnTick", -400, 80);
        AddNode(G, 11, "Variable.Get", -400, 200).Properties["Variable"] = "Speed";
        AddNode(G, 12, "Math.Multiply", -200, 160);
        AddNode(G, 13, "Vector.Make", -40, 160);
        AddNode(G, 14, "Component.Transform.Get", -40, 60);
        AddNode(G, 15, "Component.Transform.Translate", 200, 80);
        Connect(G, 10, "Then", 15, "Exec");
        Connect(G, 11, "Value", 12, "A");
        Connect(G, 10, "DeltaSeconds", 12, "B");
        Connect(G, 12, "Out", 13, "X");
        Connect(G, 14, "Transform", 15, "Target");
        Connect(G, 13, "Vector", 15, "Delta");

        G.Comments.push_back({ -420, 40, 820, 260, "Slide along +x" });
        return G;
    }

    /* Branches, a loop, a statement output and a custom event. */
    Graph MakeFlowGraph()
    {
        Graph G;
        G.SchemaName = "Script";
        G.Name       = "Flow";
        G.Variables.push_back({ "Hits", "integer", "0" });
        G.Variables.push_back({ "Home", "Vector3", "1 2 3" });

        AddNode(G, 1, "Event.OnTick");
        AddNode(G, 2, "World.Raycast");
        AddNode(G, 3, "Utility.IsSet");
        AddNode(G, 4, "Flow.Branch");
        AddNode(G, 5, "Events.Emit").Constants["Event"] = "Hit";
        AddNode(G, 6, "Variable.Set").Properties["Variable"] = "Hits";
        AddNode(G, 7, "Variable.Get").Properties["Variable"] = "Hits";
        AddNode(G, 8, "Math.Add");
        Connect(G, 1, "Then", 2, "Exec");
        Connect(G, 2, "Then", 4, "Exec");
        Connect(G, 2, "Hit", 3, "Value");
        Connect(G, 3, "Set", 4, "Condition");
        Connect(G, 4, "True", 5, "Exec");
        Connect(G, 2, "Hit", 5, "Arg 1");
        Connect(G, 2, "Distance", 5, "Arg 2");
        Connect(G, 5, "Then", 6, "Exec");
        Connect(G, 7, "Value", 8, "A");
        G.FindNode(Id(8))->Constants["B"] = "1";
        Connect(G, 8, "Out", 6, "Value");

        AddNode(G, 20, "Event.Custom").Properties["EventName"] = "Reset";
        AddNode(G, 21, "Flow.ForLoop").Constants["Last"] = "3";
        AddNode(G, 22, "Debug.Print");
        AddNode(G, 23, "Variable.Set").Properties["Variable"] = "Home";
        AddNode(G, 24, "Vector.Normalize");
        AddNode(G, 25, "Variable.Get").Properties["Variable"] = "Home";
        Connect(G, 20, "Then", 21, "Exec");
        Connect(G, 21, "Body", 22, "Exec");
        Connect(G, 21, "Index", 22, "Value");
        Connect(G, 21, "Completed", 23, "Exec");
        Connect(G, 25, "Value", 24, "V");
        Connect(G, 24, "Out", 23, "Value");
        return G;
    }

    /* A tinted, pulsing radial gradient. */
    Graph MakeGlowGraph()
    {
        Graph G;
        G.SchemaName = "Material";
        G.Name       = "Glow";

        AddNode(G, 1, "Output.Pixel", 400, 0);
        AddNode(G, 2, "Input.TexCoord", -400, 0);
        AddNode(G, 3, "Vector.Distance", -200, 0).Constants["B"] = "0.5";
        AddNode(G, 4, "Math.OneMinus", -40, 0);
        AddNode(G, 5, "Parameter", -200, 120).Properties["Name"] = "Tint";
        G.FindNode(Id(5))->WildcardType = "vec3";
        G.FindNode(Id(5))->Properties["Default"] = "1 0.6 0.2";
        AddNode(G, 6, "Math.Multiply", 120, 40);
        AddNode(G, 7, "Math.Saturate", 260, 40);
        Connect(G, 2, "UV", 3, "A");
        Connect(G, 3, "Out", 4, "X");
        Connect(G, 5, "Value", 6, "A");
        Connect(G, 4, "Out", 6, "B");
        Connect(G, 6, "Out", 7, "X");
        Connect(G, 7, "Out", 1, "Color");
        return G;
    }

    // === Tests ==================================================================

    void TestPaths()
    {
        std::printf("[paths]\n");

        Expect(GetGraphFileTag("Scripts/Mover.script.nmgraph") == "script", "the tag of a script graph");
        Expect(GetGraphFileTag("a.b/Glow.MATERIAL.NMGRAPH") == "material", "tags are case-insensitive");
        Expect(GetGraphFileTag("Glow.nmgraph").empty(), "a bare .nmgraph has no tag");
        Expect(GetGraphFileTag("Glow.material.lua").empty(), "not a graph file");

        const Schema* Material = FindSchemaForPath("Shaders/Glow.material.nmgraph");
        const Schema* Script   = FindSchemaForPath("Scripts/Mover.Script.nmgraph");
        Expect(Material && Material->GetName() == "Material", "*.material.nmgraph is a Material graph");
        Expect(Script && Script->GetName() == "Script", "*.script.nmgraph is a Script graph");
        Expect(FindSchemaForPath("x.anim.nmgraph") == nullptr, "an unknown tag has no schema");

        Expect(StripGraphExtension("Shaders/Glow.material.nmgraph") == "Shaders/Glow", "the whole compound extension comes off");
        Expect(StripGraphExtension("Levels.v2/Level.1.nmgraph") == "Levels.v2/Level.1", "an unknown tag is part of the name");
        Expect(StripGraphExtension("Scripts/Foo.script.nmgraph.script.nmgraph") == "Scripts/Foo.script.nmgraph",
               "a doubled extension (as the Windows save dialog makes) strips one at a time");
        Expect(StripGraphExtension(StripGraphExtension("Scripts/Foo.script.nmgraph.script.nmgraph")) == "Scripts/Foo",
               "and twice down to the base name");
        Expect(Material && GetGraphOutputPath("Shaders/Glow.material.nmgraph", *Material) == "Shaders/Glow.mtl.glsl", "a material outputs .mtl.glsl");
        Expect(Script && GetGraphOutputPath("Scripts/Mover.Script.NMGRAPH", *Script) == "Scripts/Mover.lua", "a script outputs .lua");
        Expect(Script && MakeGraphPath("Scripts/Mover", *Script) == "Scripts/Mover.script.nmgraph", "MakeGraphPath adds the tag");
        Expect(GetGraphBaseName("a/b/Glow.material.nmgraph") == "Glow", "the base name");
    }

    void TestFormat()
    {
        std::printf("[format]\n");

        const Graph Mover = MakeMoverGraph();
        const std::string Text = WriteGraphText(Mover);
        ExpectGolden("Mover.script.nmgraph", Text);

        Graph Loaded;
        Loaded.Name = "Mover";
        std::string Error;
        std::vector<std::string> Warnings;
        Expect(ReadGraphText(Text, Loaded, Error, &Warnings), "a written graph reads back: " + Error);
        Expect(Warnings.empty(), "and with no warnings");
        Expect(WriteGraphText(Loaded) == Text, "save -> load -> save is byte-identical");
        Expect(Loaded.Nodes.size() == Mover.Nodes.size() && Loaded.Links.size() == Mover.Links.size()
               && Loaded.Variables.size() == 1 && Loaded.Comments.size() == 1, "every part survives");

        /* Node order in memory does not change the bytes. */
        Graph Shuffled = Mover;
        std::reverse(Shuffled.Nodes.begin(), Shuffled.Nodes.end());
        std::reverse(Shuffled.Links.begin(), Shuffled.Links.end());
        Expect(WriteGraphText(Shuffled) == Text, "output does not depend on creation order");

        // --- An unknown node type is kept, links and all -------------------------
        std::string WithUnknown = Text;
        const std::string Marker = "Type=\"Debug.Print\"";
        WithUnknown.replace(WithUnknown.find(Marker), Marker.size(), "Type=\"Legacy.Print\"");
        Graph Placeholder;
        Warnings.clear();
        Expect(ReadGraphText(WithUnknown, Placeholder, Error, &Warnings), "a graph with an unknown node type still loads");
        Expect(!Warnings.empty(), "and says so");
        const Node* Kept = Placeholder.FindNode(Id(2));
        Expect(Kept && Kept->IsPlaceholder(), "the unknown node is a placeholder");
        Expect(WriteGraphText(Placeholder) == WithUnknown, "a placeholder saves back unchanged");
        CompileResult PlaceholderResult;
        CompileGraph(Placeholder, PlaceholderResult);
        Expect(PlaceholderResult.HasErrors() && HasErrorOn(PlaceholderResult, Id(2)), "a placeholder fails compilation, on its node");

        // --- Files: the extension must agree with the Schema attribute -----------
        const std::string Dir = TempDir();
        const std::string Good = Dir + "/Mover.script.nmgraph";
        const std::string Wrong = Dir + "/Mover.material.nmgraph";
        std::remove(Good.c_str());
        Expect(SaveGraphFile(Good, Mover, Error), "SaveGraphFile writes: " + Error);
        const auto Stamp = std::filesystem::last_write_time(Good);
        Expect(SaveGraphFile(Good, Mover, Error) && std::filesystem::last_write_time(Good) == Stamp,
               "saving unchanged bytes leaves the file alone");

        Graph FromFile;
        Expect(LoadGraphFile(Good, FromFile, Error) && FromFile.Name == "Mover", "LoadGraphFile names the graph after the file");
        WriteFile(Wrong, Text);
        Expect(!LoadGraphFile(Wrong, FromFile, Error) && Error.find("Material") != std::string::npos,
               "a script graph named *.material.nmgraph is refused");

        // --- Clipboard ------------------------------------------------------------
        const std::string Fragment = WriteGraphFragment(Mover, { Id(11), Id(12), Id(13) });
        Graph Target = Mover;
        std::vector<NodeGuid> Pasted;
        Expect(PasteGraphFragment(Fragment, Target, 50, 50, Pasted) && Pasted.size() == 3, "a fragment pastes three nodes");
        bool bFresh = true;
        for(const NodeGuid& Each : Pasted) { bFresh = bFresh && !Mover.FindNode(Each); }
        Expect(bFresh, "pasted nodes get fresh ids");
        Expect(Target.Links.size() == Mover.Links.size() + 2, "the two links among them are copied, and only those");
        size_t Remapped = 0;
        for(const Link& Each : Target.Links)
        {
            const bool bFrom = std::find(Pasted.begin(), Pasted.end(), Each.From.Node) != Pasted.end();
            const bool bTo   = std::find(Pasted.begin(), Pasted.end(), Each.To.Node) != Pasted.end();
            if(bFrom && bTo) { ++Remapped; }
        }
        Expect(Remapped == 2, "copied links join the copies, not the originals");
        Graph OtherSchema;
        OtherSchema.SchemaName = "Material";
        Expect(!PasteGraphFragment(Fragment, OtherSchema, 0, 0, Pasted), "a script fragment does not paste into a material");
    }

    void TestScriptCodegen()
    {
        std::printf("[script codegen]\n");

        CompileResult Mover;
        CompileGraph(MakeMoverGraph(), Mover);
        Expect(!Mover.HasErrors(), "the Mover graph compiles");
        if(Mover.HasErrors()) { std::printf("%s\n", FormatDiagnostics(MakeMoverGraph(), Mover).c_str()); }
        ExpectGolden("Mover.lua", Mover.Code);

        size_t Lines = 0;
        for(const char Ch : Mover.Code) { Lines += Ch == '\n'; }
        Expect(Mover.LineNodes.size() == Lines, "every generated line has a node entry");

        CompileResult Flow;
        CompileGraph(MakeFlowGraph(), Flow);
        Expect(!Flow.HasErrors(), "the Flow graph compiles");
        if(Flow.HasErrors()) { std::printf("%s\n", FormatDiagnostics(MakeFlowGraph(), Flow).c_str()); }
        ExpectGolden("Flow.lua", Flow.Code);

        // --- Errors name their nodes ----------------------------------------------
        {
            Graph G = MakeMoverGraph();
            G.Links.erase(std::remove_if(G.Links.begin(), G.Links.end(),
                                         [](const Link& L) { return L.To.Pin == "Target"; }), G.Links.end());
            CompileResult R;
            CompileGraph(G, R);
            Expect(HasErrorOn(R, Id(15)) && R.Code.empty(), "an unlinked component input is an error on that node");
        }
        {
            Graph G = MakeMoverGraph();
            Connect(G, 3, "Name", 13, "Y");   // string into number
            CompileResult R;
            CompileGraph(G, R);
            Expect(HasErrorOn(R, Id(13)), "a type mismatch is an error on the receiving node");
        }
        {
            Graph G = MakeMoverGraph();
            AddNode(G, 30, "Event.OnTick");
            CompileResult R;
            CompileGraph(G, R);
            Expect(HasErrorOn(R, Id(30)) || HasErrorOn(R, Id(10)), "a second On Tick is an error");
        }
        {
            Graph G = MakeMoverGraph();
            AddNode(G, 31, "Flow.Sequence");
            G.Links.erase(std::remove_if(G.Links.begin(), G.Links.end(),
                                         [](const Link& L) { return L.From.Pin == "Then" && L.From.Node == Id(10); }), G.Links.end());
            Connect(G, 10, "Then", 31, "Exec");
            Connect(G, 31, "Then 1", 15, "Exec");
            Connect(G, 15, "Then", 31, "Exec");
            CompileResult R;
            CompileGraph(G, R);
            Expect(HasErrorOn(R, Id(31)), "an execution loop is an error");
        }
        {
            Graph G = MakeFlowGraph();
            Connect(G, 2, "Distance", 22, "Value");   // Raycast (OnTick) into a Print (Reset handler)
            G.Links.erase(std::remove_if(G.Links.begin(), G.Links.end(),
                                         [](const Link& L) { return L.From.Pin == "Index"; }), G.Links.end());
            CompileResult R;
            CompileGraph(G, R);
            Expect(HasErrorOn(R, Id(2)), "a statement's output used in another event is an error");
        }
    }

    void TestScriptRuntime()
    {
        std::printf("[script runtime]\n");

        CompileResult Compiled;
        CompileGraph(MakeMoverGraph(), Compiled);
        const std::string Path = TempDir() + "/Mover.lua";
        WriteFile(Path, Compiled.Code);

        GameCore::World W;
        GameCore::ScriptEngine Scripts;
        Expect(Scripts.Initialize(), "ScriptEngine::Initialize");
        Scripts.BindWorld(&W);

        GameCore::Entity Mover = W.CreateEntity("Mover");
        Mover.AddComponent<GameCore::TransformComponent>();
        Expect(Mover.SetScript(Path.c_str()), "the generated script attaches");

        for(int Frame = 0; Frame < 10; ++Frame) { W.Tick(0.1f); }

        const GameCore::TransformComponent* T = Mover.GetComponent<GameCore::TransformComponent>();
        const double X = T ? double(T->Position.GetX()) : -1.0;
        Expect(std::fabs(X - 60.0) < 0.01, "ten 0.1s ticks at Speed 60 move the entity 60 units (got " + std::to_string(X) + ")");
        Expect(Scripts.GetErrorCount() == 0, "the generated script raised no Lua errors");

        /* The Flow graph at least loads and runs its OnTick without error. */
        CompileResult Flow;
        CompileGraph(MakeFlowGraph(), Flow);
        const std::string FlowPath = TempDir() + "/Flow.lua";
        WriteFile(FlowPath, Flow.Code);
        GameCore::Entity Flowing = W.CreateEntity("Flowing");
        Flowing.AddComponent<GameCore::TransformComponent>();
        Flowing.SetScript(FlowPath.c_str());
        W.Tick(0.1f);
        W.Tick(0.1f);
        Expect(Scripts.RunString("Events.Emit('Reset')", "emit-reset"), "emitting the handled event runs");
        W.DestroyEntity(Flowing.GetID());
        W.Tick(0.1f);
        Expect(Scripts.GetErrorCount() == 0, "the Flow script raised no Lua errors");

        Scripts.UnbindWorld();
        Scripts.Shutdown();
    }

    void TestMaterialCodegen()
    {
        std::printf("[material codegen]\n");

        CompileResult Glow;
        CompileGraph(MakeGlowGraph(), Glow);
        Expect(!Glow.HasErrors(), "the Glow graph compiles");
        if(Glow.HasErrors()) { std::printf("%s\n", FormatDiagnostics(MakeGlowGraph(), Glow).c_str()); }
        ExpectGolden("Glow.mtl.glsl", Glow.Code);
        Expect(Glow.Code.find("UBO_BLOCK_BEGIN(2, Glow_Block)") != std::string::npos, "the parameters are one std140 block at binding 2");
        Expect(Glow.Code.find("Param_Tint * ") != std::string::npos, "a parameter reads its member of the block");
        Expect(Glow.Code.find("Surface.Emissive = t") != std::string::npos && Glow.Code.find("Surface.Opacity = t") != std::string::npos,
               "Pixel Output's colour is an Unlit, Translucent surface");
        Expect(Glow.Companions.size() == 1 && Glow.Companions[0].first == ".material", "a material writes a .material companion");
        if(!Glow.Companions.empty()) { ExpectGolden("Glow.material", Glow.Companions[0].second); }

        {
            /* Offsets follow names, not graph order, and any number fit. */
            Graph G = MakeGlowGraph();
            const char* Names[] = { "Zeta", "Alpha", "Mid" };
            for(uint64_t Index = 0; Index < 3; ++Index)
            {
                Node& Added = AddNode(G, 40 + Index, "Parameter");
                Added.Properties["Name"] = Names[Index];
            }
            CompileResult R;
            CompileGraph(G, R);
            Expect(!R.HasErrors(), "four parameters fit in the block");

            G.RemoveNode(Id(42));   // "Mid" goes: Alpha, Tint, Zeta
            G.RemoveNode(Id(40));   // "Zeta" goes: Alpha, Tint
            CompileResult Two;
            CompileGraph(G, Two);
            Expect(!Two.HasErrors() && Two.Code.find("float Param_Alpha;\n    vec3  Param_Tint;") != std::string::npos,
                   "Tint is declared after Alpha");
            Expect(!Two.Companions.empty() && Two.Companions[0].second.find("Param Alpha float 32") != std::string::npos
                   && Two.Companions[0].second.find("Param Tint vec3 48") != std::string::npos,
                   "and the companion gives Alpha the first offset, Tint the next 16-byte one");
        }

        /* float + vec3 resolves to vec3 regardless of link order, and a
         * round-tripped graph compiles identically. */
        {
            Graph Reordered = MakeGlowGraph();
            std::reverse(Reordered.Links.begin(), Reordered.Links.end());
            CompileResult R;
            CompileGraph(Reordered, R);
            Expect(R.Code == Glow.Code, "link order does not change the generated code");

            Graph Loaded;
            Loaded.Name = "Glow";
            std::string Error;
            ReadGraphText(WriteGraphText(MakeGlowGraph()), Loaded, Error);
            CompileResult FromText;
            CompileGraph(Loaded, FromText);
            Expect(FromText.Code == Glow.Code, "a saved and reloaded graph generates the same code");
        }

        {
            Graph G = MakeGlowGraph();
            G.RemoveNode(Id(1));
            CompileResult R;
            CompileGraph(G, R);
            Expect(R.HasErrors(), "a material without an output fails");
        }
        {
            Graph G = MakeGlowGraph();
            Connect(G, 7, "Out", 3, "B");   // Saturate feeds Distance feeds ... Saturate
            CompileResult R;
            CompileGraph(G, R);
            Expect(R.HasErrors(), "a data cycle fails");
        }
        {
            Graph G = MakeGlowGraph();
            AddNode(G, 9, "Parameter").Properties["Name"] = "Tint";   // float Tint beside the vec3 one
            G.FindNode(Id(9))->WildcardType = "float";
            Connect(G, 9, "Value", 6, "B");
            G.Links.erase(std::remove_if(G.Links.begin(), G.Links.end(),
                                         [](const Link& L) { return L.From.Node == Id(4) && L.To.Pin == "B"; }), G.Links.end());
            CompileResult R;
            CompileGraph(G, R);
            Expect(HasErrorOn(R, Id(9)) || HasErrorOn(R, Id(5)), "one parameter name with two types fails");
        }
        {
            Graph G = MakeGlowGraph();
            G.FindNode(Id(5))->Properties["Name"] = "3D";
            CompileResult R;
            CompileGraph(G, R);
            Expect(HasErrorOn(R, Id(5)), "a parameter name that is not an identifier fails");
        }
    }

    /* Position -> Component Mask (R, B) -> Add 0.5 -> Swizzle "yxyx" -> output. */
    Graph MakeMasksGraph()
    {
        Graph G;
        G.SchemaName = "Material";
        G.Name       = "Masks";

        AddNode(G, 1, "Output.Pixel", 600, 0);
        AddNode(G, 2, "Input.Position", -300, 0);
        Node& Mask = AddNode(G, 3, "Vector.ComponentMask", -100, 0);
        Mask.Properties["R"] = "true";
        Mask.Properties["B"] = "true";
        AddNode(G, 4, "Math.Add", 100, 0).Constants["B"] = "0.5";
        AddNode(G, 5, "Vector.Swizzle", 300, 0).Properties["Swizzle"] = "yxyx";
        Connect(G, 2, "Position", 3, "Vector");
        Connect(G, 3, "Out", 4, "A");
        Connect(G, 4, "Out", 5, "Vector");
        Connect(G, 5, "Out", 1, "Color");
        return G;
    }

    /* One node of InType reading InSource's InPin, into a Pixel Output. */
    Graph MakeSelectGraph(const char* InSource, const char* InPin, const char* InType,
                          const std::map<std::string, std::string>& InProperties)
    {
        Graph G;
        G.SchemaName = "Material";
        G.Name       = "Select";
        AddNode(G, 1, "Output.Pixel");
        AddNode(G, 2, InSource);
        AddNode(G, 3, InType).Properties = InProperties;
        Connect(G, 2, InPin, 3, "Vector");
        Connect(G, 3, "Out", 1, "Color");
        return G;
    }

    void TestComponentSelection()
    {
        std::printf("[mask and swizzle]\n");

        const Schema* S = FindSchema("Material");
        const Graph Masks = MakeMasksGraph();
        const Node* Mask    = Masks.FindNode(Id(3));
        const Node* Add     = Masks.FindNode(Id(4));
        const Node* Swizzle = Masks.FindNode(Id(5));

        Expect(S->GetPinType(Masks, *Mask, *S->FindNode(Mask->Type)->FindPin("Out")) == "vec2",
               "R and B ticked make the mask's output a vec2");
        Expect(S->GetWildcardType(Masks, *Add) == "vec2", "a wildcard node takes the mask's derived type");
        Expect(S->GetPinType(Masks, *Swizzle, *S->FindNode(Swizzle->Type)->FindPin("Out")) == "vec4",
               "four letters make the swizzle's output a vec4");
        Expect(S->DescribeNode(Masks, *Mask) == ".xz \xE2\x86\x92 vec2", "the mask describes itself for its header");

        CompileResult R;
        CompileGraph(Masks, R);
        Expect(!R.HasErrors(), "the mask graph compiles");
        if(R.HasErrors()) { std::printf("%s\n", FormatDiagnostics(Masks, R).c_str()); }
        ExpectGolden("Masks.mtl.glsl", R.Code);
        if(!R.Companions.empty()) { ExpectGolden("Masks.material", R.Companions[0].second); }
        Expect(R.Code.find("Inputs.ClipPosition.xz") != std::string::npos, "the mask is a swizzle of its input");
        Expect(R.Code.find("t0.yxyx") != std::string::npos, "the swizzle reorders and repeats");

        Graph Loaded;
        Loaded.Name = "Masks";
        std::string Error;
        Expect(ReadGraphText(WriteGraphText(Masks), Loaded, Error) && WriteGraphText(Loaded) == WriteGraphText(Masks),
               "mask and swizzle settings round-trip");

        const auto Compile = [](const Graph& InGraph, CompileResult& OutResult) { CompileGraph(InGraph, OutResult); };
        {
            CompileResult All;
            Compile(MakeSelectGraph("Input.FragCoord", "Coord", "Vector.ComponentMask",
                                    { { "R", "true" }, { "G", "true" }, { "B", "true" }, { "A", "true" } }), All);
            Expect(!All.HasErrors() && All.Code.find("Surface.Emissive = Inputs.FragCoord.rgb;") != std::string::npos,
                   "all four of a vec4 pass it through untouched");
        }
        {
            CompileResult None;
            Compile(MakeSelectGraph("Input.TexCoord", "UV", "Vector.ComponentMask", { { "R", "false" } }), None);
            Expect(HasErrorOn(None, Id(3)), "a mask with nothing ticked is an error on it");

            CompileResult Wide;
            Compile(MakeSelectGraph("Input.TexCoord", "UV", "Vector.ComponentMask", { { "B", "true" } }), Wide);
            Expect(HasErrorOn(Wide, Id(3)), "B on a vec2 is an error on the mask");
        }
        {
            CompileResult Rgba;
            Compile(MakeSelectGraph("Input.Position", "Position", "Vector.Swizzle", { { "Swizzle", "BGR" } }), Rgba);
            Expect(!Rgba.HasErrors() && Rgba.Code.find("Inputs.ClipPosition.zyx") != std::string::npos,
                   "rgba letters, in either case, mean xyzw");

            CompileResult Splat;
            Compile(MakeSelectGraph("Input.Mouse", "Down", "Vector.Swizzle", { { "Swizzle", "xxx" } }), Splat);
            Expect(!Splat.HasErrors() && Splat.Code.find("vec3(ViewParameters.MouseCoords.z)") != std::string::npos,
                   "swizzling a float spreads it with a constructor");

            for(const char* Bad : { "xq", "xr", "xyzwx", "" })
            {
                CompileResult Invalid;
                Compile(MakeSelectGraph("Input.Position", "Position", "Vector.Swizzle", { { "Swizzle", Bad } }), Invalid);
                Expect(HasErrorOn(Invalid, Id(3)), std::string("swizzle '") + Bad + "' is refused");
            }

            CompileResult Narrow;
            Compile(MakeSelectGraph("Input.TexCoord", "UV", "Vector.Swizzle", { { "Swizzle", "xz" } }), Narrow);
            Expect(HasErrorOn(Narrow, Id(3)), "z of a vec2 is an error on the swizzle");
        }
        {
            CompileResult BadBool;
            Compile(MakeSelectGraph("Input.Position", "Position", "Vector.ComponentMask", { { "G", "yes" } }), BadBool);
            Expect(HasErrorOn(BadBool, Id(3)), "a checkbox property holds true or false");
        }
    }

    /* UV.x > 0.5 ? sin(UV) * 2 : sin(UV), then 4x Value += Index * sin(UV).
     * Shared sin(UV) is hoisted; the * 2 stays inside True. */
    Graph MakeLogicGraph(const char* InBranchHint = "Branch", const char* InLoopHint = "Unroll")
    {
        Graph G;
        G.SchemaName = "Material";
        G.Name       = "Logic";

        AddNode(G, 1, "Output.Pixel", 900, 0);
        AddNode(G, 2, "Input.TexCoord", -500, 0);
        AddNode(G, 3, "Vector.ComponentMask", -300, -100);                              // R: UV.x
        AddNode(G, 4, "Math.Sin", -300, 100);
        AddNode(G, 5, "Logic.Branch", 100, 0).Properties["Hint"] = InBranchHint;
        AddNode(G, 6, "Math.Multiply", -100, 0).Constants["B"] = "2";
        AddNode(G, 7, "Logic.ForLoop", 300, 0).Properties["Hint"] = InLoopHint;
        AddNode(G, 8, "Math.Add", 500, 0);
        AddNode(G, 9, "Math.Multiply", 500, 150);
        AddNode(G, 10, "Logic.ForLoopEnd", 700, 0);
        G.FindNode(Id(5))->Constants["B"] = "0.5";

        Connect(G, 2, "UV", 3, "Vector");
        Connect(G, 2, "UV", 4, "X");
        Connect(G, 3, "Out", 5, "A");
        Connect(G, 4, "Out", 6, "A");
        Connect(G, 6, "Out", 5, "True");
        Connect(G, 4, "Out", 5, "False");
        Connect(G, 5, "Out", 7, "Initial");
        Connect(G, 7, "Value", 8, "A");
        Connect(G, 7, "Index", 9, "A");
        Connect(G, 4, "Out", 9, "B");
        Connect(G, 9, "Out", 8, "B");
        Connect(G, 7, "Loop", 10, "Loop");
        Connect(G, 8, "Out", 10, "Next");
        Connect(G, 10, "Result", 1, "Color");
        return G;
    }

    void TestLogic()
    {
        std::printf("[branch and loop]\n");

        const Schema* S = FindSchema("Material");
        const Graph Logic = MakeLogicGraph();
        Expect(S->GetWildcardType(Logic, *Logic.FindNode(Id(5))) == "vec2", "a Branch takes its sides' type");
        Expect(S->GetWildcardType(Logic, *Logic.FindNode(Id(10))) == "vec2", "a For Loop End takes its For Loop's type");
        Expect(S->DescribeNode(Logic, *Logic.FindNode(Id(5))) == "vec2 \xC2\xB7 A > B \xC2\xB7 [branch]",
               "the Branch describes its test and hint");
        Expect(S->DescribeNode(Logic, *Logic.FindNode(Id(7))) == "vec2 \xC2\xB7 4 times \xC2\xB7 [unroll]",
               "the For Loop describes its count and hint");

        CompileResult R;
        CompileGraph(Logic, R);
        Expect(!R.HasErrors(), "the logic graph compiles");
        if(R.HasErrors()) { std::printf("%s\n", FormatDiagnostics(Logic, R).c_str()); }
        ExpectGolden("Logic.mtl.glsl", R.Code);
        if(!R.Companions.empty()) { ExpectGolden("Logic.material", R.Companions[0].second); }

        const auto At = [&](const char* InText) { return R.Code.find(InText); };
        Expect(At("#include") == std::string::npos, "a material function includes nothing: the templates bring MaterialPlatform.glsl");
        Expect(At("HINT_BRANCH if(Inputs.TexCoord.x > 0.5)") != std::string::npos, "a Branch is a hinted if");
        Expect(At("HINT_UNROLL for(int i0 = 0; i0 < 4; ++i0)") != std::string::npos, "a For Loop is a hinted for");
        Expect(At("sin(Inputs.TexCoord)") < At("if("), "a value both sides use is computed before the if");
        Expect(At("* vec2(2.0)") > At("if(") && At("* vec2(2.0)") < At("else"), "a value only True uses is computed inside it");
        Expect(At("vec2(float(i0))") > At("for("), "the loop's Index is read inside the loop");

        Graph Loaded;
        Loaded.Name = "Logic";
        std::string Error;
        Expect(ReadGraphText(WriteGraphText(Logic), Loaded, Error) && WriteGraphText(Loaded) == WriteGraphText(Logic),
               "branch and loop settings round-trip");

        {
            CompileResult Plain;
            CompileGraph(MakeLogicGraph("None", "None"), Plain);
            Expect(!Plain.HasErrors() && Plain.Code.find("MaterialPlatform") == std::string::npos
                   && Plain.Code.find("HINT_") == std::string::npos && Plain.Code.find("    if(") != std::string::npos,
                   "without hints: a plain if and for, and no platform header");
        }
        {
            Graph Ternary = MakeLogicGraph("None", "None");
            Ternary.FindNode(Id(5))->Properties["Ternary"] = "true";
            CompileResult T;
            CompileGraph(Ternary, T);
            Expect(!T.HasErrors() && T.Code.find("vec2 t2 = Inputs.TexCoord.x > 0.5 ? t1 : t0;") != std::string::npos
                   && T.Code.find("if(") == std::string::npos, "a ternary Branch is one expression, both sides computed before it");
            Expect(S->DescribeNode(Ternary, *Ternary.FindNode(Id(5))) == "vec2 \xC2\xB7 A > B \xC2\xB7 ? :",
                   "a ternary Branch says so in its header");

            Ternary.FindNode(Id(5))->Properties["Hint"] = "Branch";
            CompileResult Hinted;
            CompileGraph(Ternary, Hinted);
            const bool bWarned = std::any_of(Hinted.Diagnostics.begin(), Hinted.Diagnostics.end(), [](const Diagnostic& D)
            {
                return D.Severity == ESeverity::Warning && D.Node == Id(5);
            });
            Expect(!Hinted.HasErrors() && bWarned && Hinted.Code.find("HINT_BRANCH") == std::string::npos,
                   "a hint on a ternary is a warning, and not written");
        }
        {
            Graph Wide = MakeLogicGraph();
            Wide.Links.erase(std::remove_if(Wide.Links.begin(), Wide.Links.end(),
                                            [](const Link& L) { return L.To.Node == Id(7) && L.To.Pin == "Initial"; }),
                             Wide.Links.end());
            AddNode(Wide, 11, "Input.Position");
            Wide.Links.erase(std::remove_if(Wide.Links.begin(), Wide.Links.end(),
                                            [](const Link& L) { return L.To.Node == Id(10) && L.To.Pin == "Next"; }),
                             Wide.Links.end());
            Connect(Wide, 11, "Position", 10, "Next");
            Expect(S->GetWildcardType(Wide, *Wide.FindNode(Id(7))) == "vec3", "a vec3 into the End's Next widens the loop");
        }
        {
            Graph Outside = MakeLogicGraph();
            Outside.Links.pop_back();                      // Result -> Color
            Connect(Outside, 8, "Out", 1, "Color");        // the body's Add, read after the loop
            CompileResult Bad;
            CompileGraph(Outside, Bad);
            Expect(HasErrorOn(Bad, Id(8)) || HasErrorOn(Bad, Id(1)), "reading the loop's Value outside the loop is an error");
        }
        {
            Graph Linked = MakeLogicGraph();
            Connect(Linked, 3, "Out", 7, "Count");
            CompileResult Bad;
            CompileGraph(Linked, Bad);
            Expect(HasErrorOn(Bad, Id(7)), "Unroll with a linked Count is an error on the For Loop");

            Graph Dynamic = MakeLogicGraph("Branch", "Loop");
            Connect(Dynamic, 3, "Out", 7, "Count");
            CompileResult Good;
            CompileGraph(Dynamic, Good);
            Expect(!Good.HasErrors() && Good.Code.find("< int(Inputs.TexCoord.x);") != std::string::npos,
                   "a linked Count is a run-time bound");
        }
        {
            Graph Fraction = MakeLogicGraph();
            Fraction.FindNode(Id(7))->Constants["Count"] = "2.5";
            CompileResult Bad;
            CompileGraph(Fraction, Bad);
            Expect(HasErrorOn(Bad, Id(7)), "a Count that is not a whole number is an error");
        }
        {
            Graph Open = MakeLogicGraph();
            Open.Links.erase(std::remove_if(Open.Links.begin(), Open.Links.end(),
                                            [](const Link& L) { return L.To.Pin == "Loop"; }),
                             Open.Links.end());
            CompileResult Bad;
            CompileGraph(Open, Bad);
            Expect(HasErrorOn(Bad, Id(10)), "a For Loop End with no For Loop is an error on it");
        }
        {
            Graph Unclosed = MakeLogicGraph();
            Unclosed.Links.erase(std::remove_if(Unclosed.Links.begin(), Unclosed.Links.end(),
                                                [](const Link& L) { return L.To.Node == Id(1); }),
                                 Unclosed.Links.end());
            Unclosed.Links.erase(std::remove_if(Unclosed.Links.begin(), Unclosed.Links.end(),
                                                [](const Link& L) { return L.To.Pin == "Loop"; }),
                                 Unclosed.Links.end());
            Connect(Unclosed, 8, "Out", 1, "Color");
            CompileResult Bad;
            CompileGraph(Unclosed, Bad);
            Expect(HasErrorOn(Bad, Id(7)), "a For Loop read without a For Loop End is an error on it");
        }
    }

    /* A disc: discard where distance(UV, 0.5) > 0.4; the colour is 1 - that distance. */
    Graph MakeCutoutGraph()
    {
        Graph G;
        G.SchemaName = "Material";
        G.Name       = "Cutout";

        AddNode(G, 1, "Output.Pixel", 400, 0);
        AddNode(G, 2, "Input.TexCoord", -300, 0);
        AddNode(G, 3, "Vector.Distance", -100, 0).Constants["B"] = "0.5";
        Node& Discard = AddNode(G, 4, "Output.Discard", 200, 150);
        Discard.Constants["B"]       = "0.4";
        Discard.Properties["Compare"] = ">";
        AddNode(G, 5, "Math.OneMinus", 200, 0);
        Connect(G, 2, "UV", 3, "A");
        Connect(G, 3, "Out", 4, "A");
        Connect(G, 3, "Out", 5, "X");
        Connect(G, 5, "Out", 1, "Color");
        return G;
    }

    void TestDiscard()
    {
        std::printf("[discard]\n");

        const Schema* S = FindSchema("Material");
        const Graph Cutout = MakeCutoutGraph();
        Expect(S->DescribeNode(Cutout, *Cutout.FindNode(Id(4))) == "if A > B", "a Discard describes its test");

        CompileResult R;
        CompileGraph(Cutout, R);
        Expect(!R.HasErrors(), "the cutout graph compiles");
        if(R.HasErrors()) { std::printf("%s\n", FormatDiagnostics(Cutout, R).c_str()); }
        ExpectGolden("Cutout.mtl.glsl", R.Code);
        if(!R.Companions.empty()) { ExpectGolden("Cutout.material", R.Companions[0].second); }

        const size_t Discard = R.Code.find("if(t0 > 0.4) { discard; }");
        Expect(Discard != std::string::npos, "a Discard is an if around discard");
        Expect(Discard < R.Code.find("1.0 - t0"), "the discard comes before the colour");
        Expect(R.Code.find("distance(") == R.Code.rfind("distance("), "a value the discard and the colour share is computed once");

        Graph Loaded;
        Loaded.Name = "Cutout";
        std::string Error;
        Expect(ReadGraphText(WriteGraphText(Cutout), Loaded, Error) && WriteGraphText(Loaded) == WriteGraphText(Cutout),
               "a Discard round-trips");

        {
            Graph Two = MakeCutoutGraph();
            AddNode(Two, 6, "Output.Discard").Constants["B"] = "0.1";   // A unlinked: 0 < 0.1, always
            Two.FindNode(Id(6))->Constants["A"] = "0";
            CompileResult Both;
            CompileGraph(Two, Both);
            const size_t First  = Both.Code.find("if(t0 > 0.4) { discard; }");
            const size_t Second = Both.Code.find("if(0.0 < 0.1) { discard; }");
            Expect(!Both.HasErrors() && First != std::string::npos && Second != std::string::npos && First < Second,
                   "several Discards are allowed, each written, in GUID order");
        }
        {
            Graph Only;
            Only.SchemaName = "Material";
            AddNode(Only, 1, "Output.Discard");
            CompileResult None;
            CompileGraph(Only, None);
            Expect(None.HasErrors(), "a Discard does not replace the Pixel Output");
        }
    }

    /*
     * A Lit, Masked material whose surface and World Position Offset share a
     * value: Height * sin(Time) moves each vertex along its normal and is the
     * Roughness too. BaseColor is a Color parameter times the vertex colour;
     * Opacity Mask is the UV's x.
     */
    Graph MakeWaveGraph()
    {
        Graph G;
        G.SchemaName = "Material";
        G.Name       = "Wave";

        Node& Output = AddNode(G, 1, "Output.Material", 700, 0);
        Output.Properties["Domain"]    = "Masked";
        Output.Properties["MaxOffset"] = "2";
        AddNode(G, 2, "Input.Time", -400, 0);
        AddNode(G, 3, "Math.Sin", -250, 0);
        Node& Height = AddNode(G, 4, "Parameter", -250, 120);
        Height.Properties["Name"]    = "Height";
        Height.Properties["Default"] = "0.5";
        Height.WildcardType          = "float";
        AddNode(G, 5, "Math.Multiply", -100, 60);
        AddNode(G, 6, "Input.VertexNormal", -100, 200);
        AddNode(G, 7, "Math.Multiply", 100, 200);
        Node& Color = AddNode(G, 8, "Parameter", -100, -200);
        Color.Properties["Name"]    = "Color";
        Color.Properties["Default"] = "0.2 0.6 1";
        Color.WildcardType          = "vec3";
        AddNode(G, 9, "Input.VertexColor", -100, -100);
        AddNode(G, 10, "Math.Multiply", 100, -150);
        AddNode(G, 11, "Input.TexCoord", 100, 350);
        AddNode(G, 12, "Vector.Split", 300, 350);

        Connect(G, 2, "Time", 3, "X");
        Connect(G, 4, "Value", 5, "A");
        Connect(G, 3, "Out", 5, "B");
        Connect(G, 6, "Normal", 7, "A");
        Connect(G, 5, "Out", 7, "B");
        Connect(G, 7, "Out", 1, "WorldPositionOffset");
        Connect(G, 5, "Out", 1, "Roughness");
        Connect(G, 8, "Value", 10, "A");
        Connect(G, 9, "RGB", 10, "B");
        Connect(G, 10, "Out", 1, "BaseColor");
        Connect(G, 11, "UV", 12, "Vector");
        Connect(G, 12, "X", 1, "OpacityMask");
        return G;
    }

    size_t Count(const std::string& InText, const std::string& InWhat)
    {
        size_t Found = 0;
        for(size_t At = InText.find(InWhat); At != std::string::npos; At = InText.find(InWhat, At + 1)) { ++Found; }
        return Found;
    }

    void TestMaterialStages()
    {
        std::printf("[material stages]\n");

        const Graph Wave = MakeWaveGraph();
        CompileResult R;
        CompileGraph(Wave, R);
        Expect(!R.HasErrors(), "the Wave graph compiles");
        if(R.HasErrors()) { std::printf("%s\n", FormatDiagnostics(Wave, R).c_str()); }
        ExpectGolden("Wave.mtl.glsl", R.Code);
        const std::string Data = R.Companions.empty() ? std::string() : R.Companions[0].second;
        if(!Data.empty()) { ExpectGolden("Wave.material", Data); }

        Expect(R.Code.find("vec3 CalcMaterialOffset(in MaterialInputs Inputs)") != std::string::npos
               && R.Code.find("#if VERTEX_SHADER") != std::string::npos, "World Position Offset makes a vertex function");
        Expect(Count(R.Code, "sin(Scene_Time.x)") == 2, "a value both stages use is computed in each");
        Expect(R.Code.find("Surface.BaseColor = ") != std::string::npos && R.Code.find("Surface.Roughness = ") != std::string::npos
               && R.Code.find("Surface.OpacityMask = ") != std::string::npos, "each linked pin sets its surface field");
        Expect(R.Code.find("Surface.Metallic") == std::string::npos, "an unset pin keeps the template's default");
        Expect(R.Code.find("Inputs.Color.rgb") != std::string::npos && R.Code.find("Inputs.WorldNormal") != std::string::npos,
               "Vertex Color and Vertex Normal read MaterialInputs");
        Expect(Data.find("Domain Masked") != std::string::npos && Data.find("Shading Lit") != std::string::npos
               && Data.find("HasOffset 1") != std::string::npos && Data.find("MaxOffset 2") != std::string::npos,
               "the .material describes the domain, shading and offset");
        Expect(Data.find("Param Color vec3 32") != std::string::npos && Data.find("Param Height float 44") != std::string::npos
               && Data.find("BlockSize 48") != std::string::npos, "a float packs into a vec3's last four bytes (std140)");

        {
            /* std140 alignment: float 4, vec2 8, vec3 and vec4 16. */
            Graph G;
            G.SchemaName = "Material";
            G.Name       = "Layout";
            AddNode(G, 1, "Output.Material");
            const char* Names[] = { "A", "B", "C", "D" };
            const char* Types[] = { "float", "vec3", "vec2", "vec4" };
            for(uint64_t Index = 0; Index < 4; ++Index)
            {
                Node& Added = AddNode(G, 50 + Index, "Parameter");
                Added.Properties["Name"] = Names[Index];
                Added.WildcardType       = Types[Index];
            }
            CompileResult Layout;
            CompileGraph(G, Layout);
            const std::string Text = Layout.Companions.empty() ? std::string() : Layout.Companions[0].second;
            Expect(!Layout.HasErrors() && Text.find("Param A float 32") != std::string::npos
                   && Text.find("Param B vec3 48") != std::string::npos && Text.find("Param C vec2 64") != std::string::npos
                   && Text.find("Param D vec4 80") != std::string::npos, "each parameter sits at its std140 offset");
        }

        // --- Stage restrictions ------------------------------------------------------
        const Schema* S = FindSchema("Material");
        {
            Graph G = MakeWaveGraph();
            AddNode(G, 20, "Input.PixelDepth");
            const Node& Depth  = *G.FindNode(Id(20));
            const Node& Offset = *G.FindNode(Id(7));
            const Node& Base   = *G.FindNode(Id(10));
            const Node& Shared = *G.FindNode(Id(5));

            std::string Why;
            Expect(!S->CanLinkStages(G, Depth, Offset, "B", &Why) && Why.find("vertex") != std::string::npos,
                   "Pixel Depth cannot be linked toward World Position Offset");
            Expect(!S->CanLinkStages(G, Depth, Shared, "A"), "nor into a node that reaches it further on");
            Expect(S->CanLinkStages(G, Depth, Base, "A"), "but it can feed the surface");
            Expect(S->GetRequiredStages(G, Shared, "A") == 3u, "a value both stages use needs both");

            Connect(G, 20, "Linear", 5, "A");   // what the editor refuses, as a file could hold it
            G.Links.erase(std::remove_if(G.Links.begin(), G.Links.end(),
                                         [](const Link& L) { return L.From.Node == Id(4); }), G.Links.end());
            CompileResult Bad;
            CompileGraph(G, Bad);
            Expect(HasErrorOn(Bad, Id(20)) && Bad.Code.empty(), "a pixel-only node feeding the vertex stage is an error on it");
        }
        {
            Graph G = MakeWaveGraph();
            G.FindNode(Id(1))->Properties["Shading"] = "Unlit";
            CompileResult Unlit;
            CompileGraph(G, Unlit);
            bool bWarned = false;
            for(const Diagnostic& Each : Unlit.Diagnostics)
            {
                bWarned |= Each.Severity == ESeverity::Warning && Each.Pin == "BaseColor";
            }
            Expect(!Unlit.HasErrors() && bWarned, "a pin the shading ignores is a warning");
        }
        {
            Graph G = MakeWaveGraph();
            G.FindNode(Id(1))->Properties["Domain"] = "PostProcess";
            CompileResult Post;
            CompileGraph(G, Post);
            Expect(HasErrorOn(Post, Id(1)), "a PostProcess material is Unlit and has no offset");
        }
        {
            Graph G;
            G.SchemaName = "Material";
            G.Name       = "Invert";
            Node& Output = AddNode(G, 1, "Output.Material");
            Output.Properties["Domain"]  = "PostProcess";
            Output.Properties["Shading"] = "Unlit";
            AddNode(G, 2, "Input.SceneTexture");
            AddNode(G, 3, "Math.OneMinus");
            Connect(G, 2, "Color", 3, "X");
            Connect(G, 3, "Out", 1, "Emissive");
            CompileResult Post;
            CompileGraph(G, Post);
            Expect(!Post.HasErrors() && Post.Code.find("texture(Material_SceneColor, Inputs.TexCoord)") != std::string::npos,
                   "Scene Texture reads the scene in a PostProcess material");

            G.FindNode(Id(1))->Properties["Domain"] = "Opaque";
            CompileResult Opaque;
            CompileGraph(G, Opaque);
            Expect(HasErrorOn(Opaque, Id(2)), "and is an error anywhere else");
        }
        {
            Graph G = MakeWaveGraph();
            AddNode(G, 30, "Output.Pixel");
            CompileResult Two;
            CompileGraph(G, Two);
            Expect(Two.HasErrors(), "a material has one output node");
        }
    }

    /*
     * Textures: a Brick Texture Object (20) read by two Texture Samples (one
     * slot, two samples; RGB and A of the first share one); Brick again,
     * Nearest and Clamp (21), sampled at UV * 4 (a second slot); and a height
     * map with a space in its path (22) moving each vertex along its normal
     * (the vertex stage: textureLod).
     */
    Graph MakeTexturedGraph()
    {
        Graph G;
        G.SchemaName = "Material";
        G.Name       = "Textured";

        Node& Output = AddNode(G, 1, "Output.Material", 700, 0);
        Output.Properties["MaxOffset"] = "1";
        AddNode(G, 2, "Texture.Sample", 300, -200).Version = 2;
        AddNode(G, 3, "Texture.Sample", 300, 0).Version = 2;
        AddNode(G, 4, "Input.TexCoord", -100, 200);
        AddNode(G, 5, "Math.Multiply", 100, 200).Constants["B"] = "4";
        AddNode(G, 6, "Texture.Sample", 300, 200).Version = 2;
        AddNode(G, 7, "Texture.Sample", 300, 400).Version = 2;
        AddNode(G, 8, "Math.Multiply", 500, 400);
        AddNode(G, 9, "Input.VertexNormal", 300, 550);
        AddNode(G, 20, "Texture.Object", 0, -100).Properties["Texture"] = "Content/Textures/Brick.nmtexture";
        Node& Tiled = AddNode(G, 21, "Texture.Object", 0, 200);
        Tiled.Properties["Texture"] = "Content/Textures/Brick.nmtexture";
        Tiled.Properties["Filter"]  = "Nearest";
        Tiled.Properties["Wrap"]    = "Clamp";
        AddNode(G, 22, "Texture.Object", 0, 400).Properties["Texture"] = "Content/Textures/Height Map.nmtexture";

        Connect(G, 20, "Texture", 2, "Texture");
        Connect(G, 20, "Texture", 3, "Texture");
        Connect(G, 21, "Texture", 6, "Texture");
        Connect(G, 22, "Texture", 7, "Texture");
        Connect(G, 2, "RGB", 1, "BaseColor");
        Connect(G, 2, "A", 1, "AmbientOcclusion");
        Connect(G, 3, "R", 1, "Roughness");
        Connect(G, 4, "UV", 5, "A");
        Connect(G, 5, "Out", 6, "UV");
        Connect(G, 6, "G", 1, "Metallic");
        Connect(G, 9, "Normal", 8, "A");
        Connect(G, 7, "R", 8, "B");
        Connect(G, 8, "Out", 1, "WorldPositionOffset");
        return G;
    }

    void TestTextureSample()
    {
        std::printf("[texture sample]\n");

        const Graph Textured = MakeTexturedGraph();
        CompileResult R;
        CompileGraph(Textured, R);
        Expect(!R.HasErrors(), "the Textured graph compiles");
        if(R.HasErrors()) { std::printf("%s\n", FormatDiagnostics(Textured, R).c_str()); }
        ExpectGolden("Textured.mtl.glsl", R.Code);
        const std::string Data = R.Companions.empty() ? std::string() : R.Companions[0].second;
        if(!Data.empty()) { ExpectGolden("Textured.material", Data); }

        Expect(Count(R.Code, "MATERIAL_SAMPLER2D(") == 3 && R.Code.find("MATERIAL_SAMPLER2D(5, Material_Texture1)") != std::string::npos
               && R.Code.find("MATERIAL_SAMPLER2D(7, Material_Texture3)") != std::string::npos,
               "each Texture Object with its own texture and sampler is one slot, from slot 1 (binding 5)");
        Expect(Count(R.Code, "texture(Material_Texture1, Inputs.TexCoord)") == 2,
               "two Texture Samples read one Texture Object, a sample each; one node's RGB and A share its sample");
        Expect(R.Code.find("texture(Material_Texture2, t") != std::string::npos, "a linked UV is sampled at");
        Expect(R.Code.find("textureLod(Material_Texture3, Inputs.TexCoord, 0.0)") != std::string::npos
               && R.Code.find("texture(Material_Texture3") == std::string::npos,
               "World Position Offset reads level 0 (the vertex stage has no derivatives)");
        Expect(Data.find("Texture 1 Default Default Content/Textures/Brick.nmtexture\n") != std::string::npos
               && Data.find("Texture 2 Nearest Clamp Content/Textures/Brick.nmtexture\n") != std::string::npos
               && Data.find("Texture 3 Default Default Content/Textures/Height Map.nmtexture\n") != std::string::npos,
               "the .material names each slot's file and the sampler settings its Texture Object overrides");

        const Schema* Material = FindSchema("Material");
        const NodeDefinition* Def = Material ? Material->FindNode("Texture.Sample") : nullptr;
        Expect(Def && Def->FindPin("UV") && Def->FindPin("UV")->Implicit == "Texture Coordinate",
               "an unlinked UV is the texture coordinate, not a constant");
        const PinTypeInfo* TextureType = Material ? Material->FindType("texture") : nullptr;
        Expect(Def && Def->FindPin("Texture") && Def->FindPin("Texture")->Type == "texture" && TextureType && !TextureType->bHasLiteral
               && !Material->CanConvert("texture", "vec4") && !Material->CanConvert("vec4", "texture"),
               "a Texture Sample's Texture is a texture: linked from a Texture Object, never a value");
        Expect(Material && Material->DescribeNode(Textured, *Textured.FindNode(Id(21))).find("Nearest") != std::string::npos
               && Material->DescribeNode(Textured, *Textured.FindNode(Id(21))).find("Clamp") != std::string::npos,
               "a Texture Object describes the sampler settings it overrides");

        {
            /* One Texture Object, sampled twice at different UVs and once at its size's texel offset. */
            Graph G;
            G.SchemaName = "Material";
            G.Name       = "Blur";
            AddNode(G, 1, "Output.Material").Properties["Shading"] = "Unlit";
            AddNode(G, 2, "Texture.Object").Properties["Texture"] = "Content/Textures/Brick.nmtexture";
            AddNode(G, 3, "Texture.Sample").Version = 2;
            AddNode(G, 4, "Texture.Sample").Version = 2;
            AddNode(G, 5, "Input.TexCoord");
            AddNode(G, 6, "Math.Divide").Constants["A"] = "1";
            AddNode(G, 7, "Math.Add");
            AddNode(G, 8, "Math.Add");
            Connect(G, 2, "Texture", 3, "Texture");
            Connect(G, 2, "Texture", 4, "Texture");
            Connect(G, 2, "Size", 6, "B");
            Connect(G, 5, "UV", 7, "A");
            Connect(G, 6, "Out", 7, "B");
            Connect(G, 7, "Out", 4, "UV");
            Connect(G, 3, "RGB", 8, "A");
            Connect(G, 4, "RGB", 8, "B");
            Connect(G, 8, "Out", 1, "Emissive");
            CompileResult Blur;
            CompileGraph(G, Blur);
            Expect(!Blur.HasErrors() && Count(Blur.Code, "MATERIAL_SAMPLER2D(") == 1
                   && Count(Blur.Code, "texture(Material_Texture1, ") == 2
                   && Blur.Code.find("textureSize(Material_Texture1, 0)") != std::string::npos,
                   "one Texture Object, many samples at their own UVs: one slot; Size is its size in texels");
            if(Blur.HasErrors()) { std::printf("%s\n", FormatDiagnostics(G, Blur).c_str()); }
        }
        {
            Graph G = MakeTexturedGraph();
            G.FindNode(Id(3))->Constants["UV"] = "0.5 0.25";
            CompileResult Given;
            CompileGraph(G, Given);
            Expect(!Given.HasErrors() && Given.Code.find("texture(Material_Texture1, vec2(0.5, 0.25))") != std::string::npos,
                   "a UV given a constant samples there");
        }
        {
            Graph G = MakeTexturedGraph();
            G.Links.erase(G.Links.begin());   // 20 -> 2's Texture
            CompileResult Unlinked;
            CompileGraph(G, Unlinked);
            Expect(Unlinked.HasErrors(), "a Texture Sample with no Texture Object linked is an error");
        }
        {
            /* Two Texture Objects naming the same texture and sampler share a slot. */
            Graph G = MakeTexturedGraph();
            G.FindNode(Id(22))->Properties["Texture"] = "Content/Textures/Brick.nmtexture";
            CompileResult Shared;
            CompileGraph(G, Shared);
            Expect(!Shared.HasErrors() && Count(Shared.Code, "MATERIAL_SAMPLER2D(") == 2
                   && Shared.Code.find("textureLod(Material_Texture1, ") != std::string::npos,
                   "two Texture Objects with the same texture and sampler are one slot");
        }
        {
            Graph G = MakeTexturedGraph();
            G.FindNode(Id(20))->Properties["Texture"] = "";
            CompileResult Empty;
            CompileGraph(G, Empty);
            Expect(Empty.HasErrors(), "a Texture Object without a texture is an error");
        }
        {
            Graph G = MakeTexturedGraph();
            G.FindNode(Id(20))->Properties["Texture"] = "Content/Textures/Brick.png";
            CompileResult Wrong;
            CompileGraph(G, Wrong);
            Expect(Wrong.HasErrors(), "a texture must be a .nmtexture");
        }
        {
            Graph G;
            G.SchemaName = "Material";
            G.Name       = "Many";
            AddNode(G, 1, "Output.Material");
            for(uint64_t Index = 0; Index < 8; ++Index)
            {
                AddNode(G, 10 + Index, "Texture.Object").Properties["Texture"] = "Content/T" + std::to_string(Index) + ".nmtexture";
            }
            CompileResult Eight;
            CompileGraph(G, Eight);
            Expect(Eight.HasErrors(), "eight distinct textures are one more than a material's seven slots");
            G.Nodes.pop_back();
            CompileResult Seven;
            CompileGraph(G, Seven);
            Expect(!Seven.HasErrors() && Count(Seven.Code, "MATERIAL_SAMPLER2D(") == 7, "seven fit");
        }
        {
            /* A PostProcess material's scene is slot 0; its textures start at 1 all the same. */
            Graph G;
            G.SchemaName = "Material";
            G.Name       = "Overlay";
            Node& Out = AddNode(G, 1, "Output.Material");
            Out.Properties["Domain"]  = "PostProcess";
            Out.Properties["Shading"] = "Unlit";
            AddNode(G, 2, "Texture.Sample").Version = 2;
            AddNode(G, 5, "Texture.Object").Properties["Texture"] = "Content/Textures/Vignette.nmtexture";
            AddNode(G, 3, "Input.SceneTexture");
            AddNode(G, 4, "Math.Multiply");
            Connect(G, 5, "Texture", 2, "Texture");
            Connect(G, 3, "Color", 4, "A");
            Connect(G, 2, "RGBA", 4, "B");
            Connect(G, 4, "Out", 1, "Emissive");
            CompileResult Post;
            CompileGraph(G, Post);
            Expect(!Post.HasErrors() && Post.Code.find("MATERIAL_SAMPLER2D(5, Material_Texture1)") != std::string::npos,
                   "a PostProcess material samples textures beside the scene");
        }
        {
            /*
             * A graph from before Texture Objects: version 1 Texture Samples
             * named their texture themselves. Reading it gives each distinct
             * texture and sampler a Texture Object, linked in, and compiles as
             * before -- the same way every time it is read.
             */
            const std::string Old =
                "<?xml version=\"1.0\"?>\n<NodeGraph Version=\"1\" Schema=\"Material\">\n<Nodes>\n"
                "<Node Id=\"00000000000000000000000000000001\" Type=\"Output.Material\" Version=\"1\" X=\"400\" Y=\"0\" />\n"
                "<Node Id=\"00000000000000000000000000000002\" Type=\"Texture.Sample\" Version=\"1\" X=\"0\" Y=\"0\">\n"
                "  <Prop Name=\"Texture\" Value=\"Content/Textures/Brick.nmtexture\" /></Node>\n"
                "<Node Id=\"00000000000000000000000000000003\" Type=\"Texture.Sample\" Version=\"1\" X=\"0\" Y=\"200\">\n"
                "  <Prop Name=\"Texture\" Value=\"Content/Textures/Brick.nmtexture\" /></Node>\n"
                "<Node Id=\"00000000000000000000000000000004\" Type=\"Texture.Sample\" Version=\"1\" X=\"0\" Y=\"400\">\n"
                "  <Prop Name=\"Texture\" Value=\"Content/Textures/Brick.nmtexture\" /><Prop Name=\"Filter\" Value=\"Nearest\" /></Node>\n"
                "</Nodes>\n<Links>\n"
                "<Link From=\"00000000000000000000000000000002.RGB\" To=\"00000000000000000000000000000001.BaseColor\" />\n"
                "<Link From=\"00000000000000000000000000000003.R\" To=\"00000000000000000000000000000001.Roughness\" />\n"
                "<Link From=\"00000000000000000000000000000004.G\" To=\"00000000000000000000000000000001.Metallic\" />\n"
                "</Links>\n</NodeGraph>\n";
            Graph Upgraded, Again;
            std::string Error;
            std::vector<std::string> Warnings;
            const bool bRead = ReadGraphText(Old, Upgraded, Error, &Warnings) && ReadGraphText(Old, Again, Error);

            size_t Objects = 0, Samples = 0, Leftover = 0;
            for(const Node& Each : Upgraded.Nodes)
            {
                Objects += Each.Type == "Texture.Object";
                if(Each.Type == "Texture.Sample")
                {
                    ++Samples;
                    Leftover += Each.Version != 2 || Each.Properties.count("Texture") || Each.Properties.count("Filter");
                }
            }
            Expect(bRead && Objects == 2 && Samples == 3 && Leftover == 0,
                   "an old graph's Texture Samples gain a Texture Object per distinct texture and sampler");
            bool bSameIds = Upgraded.Nodes.size() == Again.Nodes.size();
            for(size_t Index = 0; bSameIds && Index < Upgraded.Nodes.size(); ++Index)
            {
                bSameIds = Upgraded.Nodes[Index].Id == Again.Nodes[Index].Id;
            }
            Expect(bSameIds, "and the same ones each time it is read, so slots stay put");
            Expect(!Warnings.empty() && Warnings.back().find("Texture Object") != std::string::npos, "the reader says it upgraded");

            CompileResult Compiled;
            CompileGraph(Upgraded, Compiled);
            const std::string Material = Compiled.Companions.empty() ? std::string() : Compiled.Companions[0].second;
            Expect(!Compiled.HasErrors() && Count(Compiled.Code, "MATERIAL_SAMPLER2D(") == 2
                   && Material.find("Nearest Default Content/Textures/Brick.nmtexture") != std::string::npos,
                   "and compiles as it did");
            if(Compiled.HasErrors()) { std::printf("%s\n", FormatDiagnostics(Upgraded, Compiled).c_str()); }

            /* Written back, it is the new form. */
            const std::string Written = WriteGraphText(Upgraded);
            Expect(Written.find("Type=\"Texture.Object\"") != std::string::npos
                   && Written.find("Type=\"Texture.Sample\" Version=\"2\"") != std::string::npos,
                   "saving writes the Texture Objects");
        }
    }

    void TestSchemas()
    {
        std::printf("[schemas]\n");

        const Schema* Script = FindSchema("Script");
        Expect(Script && Script->FindNode("Component.Transform.GetPosition"), "component fields become nodes");
        Expect(Script && Script->FindNode("Component.Transform.SetPosition"), "a writable field gets a setter");
        Expect(Script && !Script->FindNode("Component.Bounds.SetWorld"), "a read-only field does not");
        const NodeDefinition* Translate = Script ? Script->FindNode("Component.Transform.Translate") : nullptr;
        Expect(Translate && Translate->FindPin("Delta") && Translate->FindPin("Delta")->Type == "Vector3",
               "a method's pins come from its binding's signature");

        for(const Schema* Each : GetSchemas())
        {
            for(const NodeDefinition& Def : Each->GetNodes())
            {
                for(const PinDefinition& Pin : Def.Pins)
                {
                    if(Pin.IsExec() || Pin.Type == WILDCARD_TYPE || Pin.Type == DERIVED_TYPE) { continue; }
                    Expect(Each->FindType(Pin.Type) != nullptr, Each->GetName() + " " + Def.Type + "." + Pin.Name + " has a known type");
                }
            }
        }
    }
}

int main()
{
    std::printf("NodeGraphTests\n");

    TestPaths();
    TestSchemas();
    TestFormat();
    TestScriptCodegen();
    TestScriptRuntime();
    TestMaterialCodegen();
    TestComponentSelection();
    TestLogic();
    TestDiscard();
    TestMaterialStages();
    TestTextureSample();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
