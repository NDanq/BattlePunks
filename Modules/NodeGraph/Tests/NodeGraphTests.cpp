/*
 * NodeGraphTests -- the .nmgraph format, the path rules, validation, and both
 * code generators, in Modules/Core/Tests/SmokeMain.cpp's style: an Expect()
 * counter and a nonzero exit status.
 *
 * GOLDEN FILES. Generated code is compared with Tests/Data/<name>; a missing
 * golden file is written and reported as a failure, so a new case has to be
 * looked at once. Set NODEGRAPH_UPDATE_GOLDEN=1 to rewrite them all after an
 * intended change to the output, then review the diff.
 *
 * The generated Lua is also RUN: a World, a ScriptEngine and an entity with
 * the script attached, ticked, and its Transform checked.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
        Expect(Material && GetGraphOutputPath("Shaders/Glow.material.nmgraph", *Material) == "Shaders/Glow.fsh", "a material outputs .fsh");
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
        ExpectGolden("Glow.fsh", Glow.Code);
        Expect(Glow.Code.find("SHADER_UNIFORM(mat4, MaterialParams)") != std::string::npos, "one mat4 carries every parameter");
        Expect(Glow.Code.find("MaterialParams[0].xyz") != std::string::npos, "a vec3 parameter reads its column's xyz");
        Expect(Glow.Companions.size() == 1 && Glow.Companions[0].first == ".material", "a material writes a .material companion");
        if(!Glow.Companions.empty()) { ExpectGolden("Glow.material", Glow.Companions[0].second); }

        {
            /* Slots follow names, not graph order; a fourth parameter is refused. */
            Graph G = MakeGlowGraph();
            const char* Names[] = { "Zeta", "Alpha", "Mid" };
            for(uint64_t Index = 0; Index < 3; ++Index)
            {
                Node& Added = AddNode(G, 40 + Index, "Parameter");
                Added.Properties["Name"] = Names[Index];
            }
            CompileResult R;
            CompileGraph(G, R);
            Expect(R.HasErrors(), "four parameters are one too many");

            G.RemoveNode(Id(42));   // "Mid" goes: Alpha, Tint, Zeta
            G.RemoveNode(Id(40));   // "Zeta" goes: Alpha, Tint
            CompileResult Two;
            CompileGraph(G, Two);
            Expect(!Two.HasErrors() && Two.Code.find("MaterialParams[1].xyz") != std::string::npos,
                   "Tint sorts after Alpha, into slot 1");
            Expect(!Two.Companions.empty() && Two.Companions[0].second.find("Param Alpha 0 float") != std::string::npos,
                   "and the companion lists Alpha in slot 0");
        }

        /* A wildcard node fed a float and a vec3 is vec3 whichever link is
         * stored first -- and a saved-then-loaded graph (links sorted) makes
         * the same code as the one in memory. */
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
            Expect(R.HasErrors(), "a material without a Pixel Output fails");
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
                    if(Pin.IsExec() || Pin.Type == WILDCARD_TYPE) { continue; }
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

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
