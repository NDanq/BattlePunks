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
#include "Public/GameCore/ScriptLibrary.h"
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

// === A script struct, as a game module declares one =============================

struct TestStats
{
    GAMECORE_SCRIPT_STRUCT_BODY()
    int32_t             Hits   = 0;
    float               Scale  = 1.0f;
    EngineCore::Vector3 Where;
    uint8_t             bLucky = 0;
};
GAMECORE_DEFINE_SCRIPT_STRUCT(TestStats, "TestStats")
{
    Struct.Field("Hits",  &TestStats::Hits);
    Struct.Field("Scale", &TestStats::Scale);
    Struct.Field("Where", &TestStats::Where);
    Struct.Flag("Lucky",  &TestStats::bLucky);
}

/* A component holding one, as a field, and returning one from a member. */
struct StatsHolderComponent
{
    GAMECORE_COMPONENT_BODY()
    TestStats Stats;

    TestStats Bumped(const int32_t InBy) const
    {
        TestStats Out = Stats;
        Out.Hits += InBy;
        return Out;
    }
};
GAMECORE_DEFINE_COMPONENT(StatsHolderComponent, "TestStatsHolder")
GAMECORE_DEFINE_COMPONENT_SCRIPT(StatsHolderComponent)
{
    Bind.Field("Stats", &StatsHolderComponent::Stats);
    Bind.Native<&StatsHolderComponent::Bumped>("Bumped", "By -> Stats");
}

static TestStats GTestStats;

static TestStats NativeMerge(const TestStats& InA, const TestStats& InB)
{
    TestStats Out;
    Out.Hits   = InA.Hits + InB.Hits;
    Out.Scale  = InA.Scale + InB.Scale;
    Out.Where  = InA.Where + InB.Where;
    Out.bLucky = uint8_t(InA.bLucky | InB.bLucky);
    return Out;
}

// === A script library, as a game module declares one ===========================

static int32_t GTestScore    = 0;
static int32_t GTestVersion  = 3;
static int32_t GTestRecorded = -1;

/* Plain C++ functions, bound with Native: no ScriptCall in sight. */
static double      NativeLerp(const double InA, const double InB, const double InT) { return InA + (InB - InA) * InT; }
static std::string NativeGreet(const std::string& InName, const int32_t InTimes)
{
    std::string Out;
    for(int32_t Index = 0; Index < InTimes; ++Index) { Out += "hi " + InName + (Index + 1 < InTimes ? ", " : ""); }
    return Out;
}
static GameCore::EntityID NativeSpawnAt(GameCore::World& InWorld, const EngineCore::Vector3& InAt)
{
    GameCore::TransformComponent Transform;
    Transform.Position = InAt;
    const GameCore::Entity Spawned = InWorld.CreateEntity("Spawned");
    Spawned.AddComponent(Transform);
    return Spawned.GetID();
}
static void NativeFail(GameCore::ScriptCall& InCall) { InCall.Error("refused"); }

GAMECORE_DEFINE_SCRIPT_LIBRARY(TestLib)
{
    Lib.Variable("Score", &GTestScore);
    Lib.ReadOnly("Version", &GTestVersion);

    Lib.Function("AddScore", [](GameCore::ScriptCall& Call)
    {
        const int32_t Amount = Call.GetInt(1);
        if(Call.HasError()) { return; }
        GTestScore += Amount;
        Call.PushInteger(GTestScore);
    }, "Amount:integer -> Total:integer").Tooltip("Adds Amount to Score");

    Lib.Pure("Doubled", [](GameCore::ScriptCall& Call) { Call.PushNumber(Call.GetNumber(1) * 2.0); },
             "Value:number -> Result:number");

    Lib.Function("Nudge", [](GameCore::ScriptCall& Call)
    {
        const GameCore::EntityID Target = Call.GetEntity(1);
        const EngineCore::Vector3 Delta = Call.GetVector3(2);
        if(Call.HasError()) { return; }
        if(GameCore::TransformComponent* T = Call.GetWorld().GetEntity(Target).GetComponent<GameCore::TransformComponent>())
        {
            T->Position = T->Position + Delta;
        }
    }, "Target:Entity, Delta:Vector3");

    Lib.Function("Record", [](GameCore::ScriptCall& Call) { GTestRecorded = Call.GetInt(1); }, "Value:integer");

    Lib.Event("Scored", "Who:Entity, Total:integer");
    Lib.Event("TooWide", "Where:Vector3");   // not one value per parameter: no nodes

    Lib.NativePure<&NativeLerp>("Lerp", "A, B, T -> Out");
    Lib.NativePure<&NativeGreet>("Greet");                           // unnamed: Arg1, Arg2, Result
    Lib.Native<&NativeSpawnAt>("SpawnAt", "At -> Spawned");          // World& is filled in
    Lib.Native<&NativeFail>("Fail");

    Lib.Variable("Stats", &GTestStats);                              // a script struct variable
    Lib.NativePure<&NativeMerge>("Merge", "A, B -> Sum");            // script structs in and out
}

/* A component whose bindings are withdrawn and registered again, as a game
 * module's are when it unloads and loads. */
struct ReloadProbeComponent
{
    GAMECORE_COMPONENT_BODY()
    float Speed = 1.0f;

    /* Native members: bound as they are, no glue written. */
    double              Scaled(const double InFactor) const { return Speed * InFactor; }
    void                SetSpeedTo(const float InSpeed)     { Speed = InSpeed; }
    EngineCore::Vector3 Along(const EngineCore::Vector3& InDirection, const EngineCore::Color& InWeight) const
    {
        return InDirection * (double(Speed) * InWeight.A / 255.0);
    }
};
GAMECORE_DEFINE_COMPONENT(ReloadProbeComponent, "TestReloadProbe")
GAMECORE_DEFINE_COMPONENT_SCRIPT(ReloadProbeComponent)
{
    Bind.Method("Ping", [](ReloadProbeComponent&, GameCore::ScriptCall& Call) { Call.PushBool(true); }, "-> Ok:bool");
    Bind.Native<&ReloadProbeComponent::Scaled>("Scaled", "Factor -> Value");
    Bind.Native<&ReloadProbeComponent::SetSpeedTo>("SetSpeedTo", "Speed");
    Bind.Native<&ReloadProbeComponent::Along>("Along", "Direction, Weight -> Offset");
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

    /* TestLib's nodes: a pure call into a statement, a variable read, an
     * entity and a vector argument, and a typed event emitted and handled. */
    Graph MakeLibraryGraph()
    {
        Graph G;
        G.SchemaName = "Script";
        G.Name       = "Scorer";

        AddNode(G, 1, "Event.OnTick");
        AddNode(G, 2, "Library.TestLib.Doubled").Constants["Value"] = "2";
        AddNode(G, 3, "Library.TestLib.AddScore");
        AddNode(G, 4, "Library.TestLib.Nudge").Constants["Delta"] = "1 0 0";
        AddNode(G, 5, "Library.TestLib.EmitScored");
        AddNode(G, 6, "Entity.Self");
        Connect(G, 1, "Then", 3, "Exec");
        Connect(G, 2, "Result", 3, "Amount");   // number into integer
        Connect(G, 3, "Then", 4, "Exec");
        Connect(G, 4, "Then", 5, "Exec");
        Connect(G, 6, "Entity", 5, "Who");
        Connect(G, 3, "Total", 5, "Total");

        AddNode(G, 10, "Library.TestLib.OnScored");
        AddNode(G, 11, "Library.TestLib.Record");
        AddNode(G, 12, "Library.TestLib.GetVersion");
        AddNode(G, 13, "Math.Add");
        Connect(G, 10, "Then", 11, "Exec");
        Connect(G, 10, "Total", 13, "A");
        Connect(G, 12, "Version", 13, "B");
        Connect(G, 13, "Out", 11, "Value");
        return G;
    }

    /* TestStats through a graph: a component field read, broken, changed,
     * made again and written back; a struct variable merged into each tick. */
    Graph MakeStatsGraph()
    {
        Graph G;
        G.SchemaName = "Script";
        G.Name       = "Stats";
        G.Variables.push_back({ "Best", "TestStats", "" });

        AddNode(G, 1, "Event.OnTick");
        AddNode(G, 2, "Component.TestStatsHolder.Get");
        AddNode(G, 3, "Component.TestStatsHolder.GetStats");
        AddNode(G, 4, "Struct.TestStats.Break");
        AddNode(G, 5, "Math.Add").Constants["B"] = "1";
        AddNode(G, 6, "Struct.TestStats.Make").Constants["Lucky"] = "true";
        AddNode(G, 7, "Component.TestStatsHolder.SetStats");
        AddNode(G, 8, "Variable.Set").Properties["Variable"] = "Best";
        AddNode(G, 9, "Library.TestLib.Merge");
        AddNode(G, 10, "Variable.Get").Properties["Variable"] = "Best";
        AddNode(G, 11, "Library.TestLib.SetStats");
        Connect(G, 1, "Then", 7, "Exec");
        Connect(G, 2, "TestStatsHolder", 3, "Target");
        Connect(G, 2, "TestStatsHolder", 7, "Target");
        Connect(G, 3, "Stats", 4, "TestStats");
        Connect(G, 4, "Hits", 5, "A");
        Connect(G, 5, "Out", 6, "Hits");
        Connect(G, 4, "Scale", 6, "Scale");
        Connect(G, 4, "Where", 6, "Where");
        Connect(G, 6, "TestStats", 7, "Stats");
        Connect(G, 7, "Then", 8, "Exec");
        Connect(G, 10, "Value", 9, "A");
        Connect(G, 3, "Stats", 9, "B");
        Connect(G, 9, "Sum", 8, "Value");
        Connect(G, 8, "Then", 11, "Exec");
        Connect(G, 10, "Value", 11, "Stats");
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
        ExpectGolden("Masks.fsh", R.Code);
        Expect(R.Code.find("Interpolants.Position.xz") != std::string::npos, "the mask is a swizzle of its input");
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
            Expect(!All.HasErrors() && All.Code.find("Output = gl_FragCoord;") != std::string::npos,
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
            Expect(!Rgba.HasErrors() && Rgba.Code.find("Interpolants.Position.zyx") != std::string::npos,
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
        ExpectGolden("Logic.fsh", R.Code);

        const auto At = [&](const char* InText) { return R.Code.find(InText); };
        Expect(At("#include <MaterialPlatform.glsl>") < At("#include <Common.glsl>"), "hints include the platform header first");
        Expect(At("HINT_BRANCH if(Interpolants.TexCoord.x > 0.5)") != std::string::npos, "a Branch is a hinted if");
        Expect(At("HINT_UNROLL for(int i0 = 0; i0 < 4; ++i0)") != std::string::npos, "a For Loop is a hinted for");
        Expect(At("sin(Interpolants.TexCoord)") < At("if("), "a value both sides use is computed before the if");
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
            Expect(!T.HasErrors() && T.Code.find("vec2 t2 = Interpolants.TexCoord.x > 0.5 ? t1 : t0;") != std::string::npos
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
            Expect(!Good.HasErrors() && Good.Code.find("< int(Interpolants.TexCoord.x);") != std::string::npos,
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
        ExpectGolden("Cutout.fsh", R.Code);

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

    void TestLibraries()
    {
        std::printf("[script libraries]\n");

        // --- Nodes ----------------------------------------------------------------
        const Schema* Script = FindSchema("Script");
        const NodeDefinition* AddScore = Script ? Script->FindNode("Library.TestLib.AddScore") : nullptr;
        Expect(AddScore && AddScore->Role == ENodeRole::Statement && AddScore->Category == "TestLib",
               "a library function is a statement in the library's palette category");
        Expect(AddScore && AddScore->FindPin("Amount", EPinDirection::Input) && AddScore->FindPin("Total", EPinDirection::Output)
               && AddScore->Tooltip == "Adds Amount to Score", "its pins come from its signature, its tooltip from the binder");
        const NodeDefinition* Doubled = Script ? Script->FindNode("Library.TestLib.Doubled") : nullptr;
        Expect(Doubled && Doubled->Role == ENodeRole::Pure, "a Pure function is a value node");
        Expect(Script && Script->FindNode("Library.TestLib.GetScore") && Script->FindNode("Library.TestLib.SetScore"),
               "a variable gets a getter and a setter");
        Expect(Script && Script->FindNode("Library.TestLib.GetVersion") && !Script->FindNode("Library.TestLib.SetVersion"),
               "a read-only variable gets only a getter");
        const NodeDefinition* OnScored = Script ? Script->FindNode("Library.TestLib.OnScored") : nullptr;
        Expect(OnScored && OnScored->Role == ENodeRole::Event && OnScored->FindPin("Who", EPinDirection::Output)
               && OnScored->FindPin("Who", EPinDirection::Output)->Type == "Entity",
               "an event is an event node with typed outputs");
        Expect(Script && Script->FindNode("Library.TestLib.EmitScored"), "and an Emit statement");
        Expect(Script && !Script->FindNode("Library.TestLib.OnTooWide"), "an event with a multi-value parameter has no nodes");

        // --- Code -------------------------------------------------------------------
        CompileResult Scorer;
        CompileGraph(MakeLibraryGraph(), Scorer);
        Expect(!Scorer.HasErrors(), "the library graph compiles");
        if(Scorer.HasErrors()) { std::printf("%s\n", FormatDiagnostics(MakeLibraryGraph(), Scorer).c_str()); }
        ExpectGolden("Scorer.lua", Scorer.Code);

        // --- Run --------------------------------------------------------------------
        const std::string Path = TempDir() + "/Scorer.lua";
        WriteFile(Path, Scorer.Code);
        {
            GameCore::World W;
            GameCore::ScriptEngine Scripts;
            Scripts.Initialize();
            Scripts.BindWorld(&W);

            GTestScore = 0;
            GTestRecorded = -1;
            GameCore::Entity Scoring = W.CreateEntity("Scoring");
            Scoring.AddComponent<GameCore::TransformComponent>();
            Scoring.SetScript(Path.c_str());
            for(int Frame = 0; Frame < 3; ++Frame) { W.Tick(0.1f); }

            const GameCore::TransformComponent* T = Scoring.GetComponent<GameCore::TransformComponent>();
            Expect(GTestScore == 12, "three ticks of AddScore(Doubled(2)) score 12 (got " + std::to_string(GTestScore) + ")");
            Expect(T && std::fabs(double(T->Position.GetX()) - 3.0) < 1e-6, "Nudge took an Entity and a Vector3");
            Expect(GTestRecorded == 15, "On Scored received the typed Total and Record ran (got " + std::to_string(GTestRecorded) + ")");
            Expect(Scripts.RunString("assert(TestLib.GetVersion() == 3 and TestLib.SetVersion == nil)", "readonly"),
                   "a read-only variable has no setter in Lua");
            Expect(Scripts.RunString("TestLib.SetScore(40) assert(TestLib.GetScore() == 40)", "variable") && GTestScore == 40,
                   "a variable is the C++ object itself");
            Expect(Scripts.RunString("assert(TestLib.Scored == 'Scored')", "event-name"), "an event's name is published");
            Expect(Scripts.GetErrorCount() == 0, "the library script raised no Lua errors");
            Expect(!Scripts.RunString("TestLib.AddScore('x')", "bad-arg"), "a bad argument is a Lua error, not a crash");

            Scripts.UnbindWorld();
            Scripts.Shutdown();
        }

        // --- Registration -------------------------------------------------------
        GameCore::ScriptLibraryMember Ping;
        std::snprintf(Ping.Name, sizeof(Ping.Name), "Ping");
        Ping.Kind     = GameCore::EScriptLibraryMember::Pure;
        Ping.Function = [](GameCore::ScriptCall& Call) { Call.PushBool(true); };
        std::snprintf(Ping.Signature, sizeof(Ping.Signature), "-> Ok:bool");

        Expect(GameCore::RegisterScriptLibrary("math", &Ping, 1, 0) == GameCore::SCRIPT_LIBRARY_INVALID,
               "a library may not replace a Lua standard library");
        Expect(GameCore::RegisterScriptLibrary("TestLib", &Ping, 1, 0) == GameCore::SCRIPT_LIBRARY_INVALID,
               "nor take a registered library's name");

        const GameCore::ScriptLibraryHandle Late = GameCore::RegisterScriptLibrary("LateLib", &Ping, 1, 0);
        const Schema* Before = FindSchema("Script");
        Expect(Late != GameCore::SCRIPT_LIBRARY_INVALID && Before && !Before->FindNode("Library.LateLib.Ping"),
               "a library registered after the schemas were built is not in them yet");
        RefreshSchemas();
        const Schema* After = FindSchema("Script");
        Expect(After && After != Before && After->FindNode("Library.LateLib.Ping"), "RefreshSchemas picks it up");
        Expect(Before->FindNode("Component.Transform.Translate") != nullptr, "and the old schema stays usable");

        {
            GameCore::World W;
            GameCore::ScriptEngine Scripts;
            Scripts.Initialize();
            Scripts.BindWorld(&W);
            Expect(Scripts.RunString("assert(LateLib.Ping())", "late"), "a library registered before BindWorld is in Lua");
            GameCore::UnregisterScriptLibrary(Late);
            Expect(!Scripts.RunString("LateLib.Ping()", "unloaded"), "calling an unregistered library is a Lua error");
            Scripts.UnbindWorld();
            Scripts.Shutdown();
        }
        RefreshSchemas();
        Expect(!FindSchema("Script")->FindNode("Library.LateLib.Ping"), "and after a refresh its nodes are gone");

        // --- Native bindings ------------------------------------------------------
        {
            const auto LibrarySignature = [](const char* InMember) -> std::string
            {
                for(size_t Index = 0; Index < GameCore::GetScriptLibraryCount(); ++Index)
                {
                    GameCore::ScriptLibraryInfo Info;
                    if(!GameCore::GetScriptLibrary(Index, Info) || std::string(Info.Name) != "TestLib") { continue; }
                    for(size_t M = 0; M < Info.Count; ++M)
                    {
                        if(std::string(Info.Members[M].Name) == InMember) { return Info.Members[M].Signature; }
                    }
                }
                return "<missing>";
            };
            const auto MethodSignature = [](const char* InMember) -> std::string
            {
                size_t Count = 0;
                const GameCore::ScriptBindingEntry* Entries = GameCore::FindComponentScript(ReloadProbeComponent::StaticType().ID, Count);
                for(size_t Index = 0; Index < Count; ++Index)
                {
                    if(std::string(Entries[Index].Name) == InMember) { return Entries[Index].Signature; }
                }
                return "<missing>";
            };

            Expect(LibrarySignature("Lerp") == "A:number, B:number, T:number -> Out:number", "Native: types from C++, names from the binder");
            Expect(LibrarySignature("Greet") == "Arg1:string, Arg2:integer -> Result:string", "Native: unnamed parameters are Arg1.. and Result");
            Expect(LibrarySignature("SpawnAt") == "At:Vector3 -> Spawned:Entity", "Native: a World& parameter is not a script argument");
            Expect(LibrarySignature("Fail") == "", "Native: no parameters and no result is an empty signature");
            Expect(MethodSignature("Scaled") == "Factor:number -> Value:number", "Bind.Native: a const member function");
            Expect(MethodSignature("SetSpeedTo") == "Speed:number", "Bind.Native: a void member function");
            Expect(MethodSignature("Along") == "Direction:Vector3, Weight:Color -> Offset:Vector3", "Bind.Native: multi-value types");

            const Schema* Now = FindSchema("Script");
            const NodeDefinition* Lerp = Now ? Now->FindNode("Library.TestLib.Lerp") : nullptr;
            Expect(Lerp && Lerp->Role == ENodeRole::Pure && Lerp->FindPin("T", EPinDirection::Input), "a NativePure is a value node with named pins");
            const NodeDefinition* Along = Now ? Now->FindNode("Component.TestReloadProbe.Along") : nullptr;
            Expect(Along && Along->FindPin("Weight", EPinDirection::Input) && Along->FindPin("Weight", EPinDirection::Input)->Type == "Color",
                   "a Bind.Native method is a node with typed pins");

            GameCore::World W;
            GameCore::ScriptEngine Scripts;
            Scripts.Initialize();
            Scripts.BindWorld(&W);
            W.CreateEntity("NativeProbe").AddComponent<ReloadProbeComponent>();
            Expect(Scripts.RunString("assert(TestLib.Lerp(10, 20, 0.25) == 12.5)", "native-lerp"), "a native pure function runs");
            Expect(Scripts.RunString("assert(TestLib.Greet('bo', 2) == 'hi bo, hi bo')", "native-greet"), "std::string in and out");
            Expect(Scripts.RunString("local e = TestLib.SpawnAt(1, 2, 3) local x, y, z = e:GetComponent('Transform'):GetPosition() "
                                     "assert(e:GetName() == 'Spawned' and x == 1 and y == 2 and z == 3)", "native-spawn"),
                   "World& filled in, a Vector3 read from three values, an EntityID returned");
            Expect(Scripts.RunString("local p = World.FindEntity('NativeProbe'):GetComponent('TestReloadProbe') "
                                     "p:SetSpeedTo(4) assert(p:Scaled(2) == 8) "
                                     "local x, y, z = p:Along(1, 0, 0, 0, 0, 0, 255) assert(x == 4 and y == 0 and z == 0)", "native-member"),
                   "native members run, with a Color's four values before the next argument");
            const uint32_t ErrorsBefore = Scripts.GetErrorCount();
            Expect(!Scripts.RunString("TestLib.Lerp(1, 'x', 0)", "native-bad"), "a bad argument to a native is a Lua error");
            Expect(!Scripts.RunString("TestLib.Fail()", "native-fail"), "ScriptCall& parameter: Error() fails the call");
            Expect(Scripts.GetErrorCount() == ErrorsBefore + 2, "both counted as script errors");
            Scripts.UnbindWorld();
            Scripts.Shutdown();
        }

        // --- Component bindings across an unload ----------------------------------
        {
            const GameCore::ComponentTypeID Type = ReloadProbeComponent::StaticType().ID;
            size_t Count = 0;
            const GameCore::ScriptBindingEntry* First = GameCore::FindComponentScript(Type, Count);

            GameCore::World W;
            GameCore::ScriptEngine Scripts;
            Scripts.Initialize();
            Scripts.BindWorld(&W);
            W.CreateEntity("Probe").AddComponent<ReloadProbeComponent>();
            const char* const Call = "assert(World.FindEntity('Probe'):GetComponent('TestReloadProbe'):Ping())";
            Expect(Scripts.RunString(Call, "probe"), "a component method runs");

            GameCore::UnregisterComponentScript(Type);
            Expect(!Scripts.RunString(Call, "probe-unloaded"), "after an unregister it is a Lua error, not a jump into unloaded code");

            GameCore::ComponentScriptBinder<ReloadProbeComponent> Again;
            Again.Method("Ping", [](ReloadProbeComponent&, GameCore::ScriptCall& C) { C.PushBool(true); }, "-> Ok:bool");
            Expect(Again.Commit(), "the type can register again, as a reloaded module does");
            size_t AgainCount = 0;
            Expect(GameCore::FindComponentScript(Type, AgainCount) == First && AgainCount == 1 && Count > 1,
                   "into the same entries (a smaller set fits), so reloads do not fill the table");
            Expect(Scripts.RunString(Call, "probe-reloaded"), "and the method runs again");

            Scripts.UnbindWorld();
            Scripts.Shutdown();
        }
    }

    void TestScriptStructs()
    {
        std::printf("[script structs]\n");

        // --- Registry ---------------------------------------------------------------
        const GameCore::ScriptStructID Id = TestStats::StaticScriptStruct();
        const GameCore::ScriptStructInfo* Info = GameCore::GetScriptStruct(Id);
        Expect(Info && std::string(Info->Name) == "TestStats" && Info->FieldCount == 4 && Info->Lanes == 6,
               "a struct registers its fields; a Vector3 field is three lanes");
        Expect(GameCore::FindOrAddScriptStruct("TestStats") == Id, "an id is found by name, so a reload keeps it");

        // --- Nodes ------------------------------------------------------------------
        const Schema* S = FindSchema("Script");
        const PinTypeInfo* Type = S ? S->FindType("TestStats") : nullptr;
        Expect(Type && !Type->bHasLiteral, "a struct is a pin type, built rather than typed in");
        const NodeDefinition* MakeDef = S ? S->FindNode("Struct.TestStats.Make") : nullptr;
        Expect(MakeDef && MakeDef->FindPin("Where", EPinDirection::Input) && MakeDef->FindPin("Where", EPinDirection::Input)->Type == "Vector3"
               && MakeDef->FindPin("Lucky", EPinDirection::Input)->Type == "bool"
               && MakeDef->FindPin("TestStats", EPinDirection::Output), "Make has a typed input per field");
        Expect(S && S->FindNode("Struct.TestStats.Break"), "and Break the outputs");
        const NodeDefinition* GetStats = S ? S->FindNode("Component.TestStatsHolder.GetStats") : nullptr;
        Expect(GetStats && GetStats->FindPin("Stats", EPinDirection::Output)->Type == "TestStats", "a component's struct field is a struct pin");
        const NodeDefinition* Merge = S ? S->FindNode("Library.TestLib.Merge") : nullptr;
        Expect(Merge && Merge->FindPin("Sum", EPinDirection::Output) && Merge->FindPin("Sum", EPinDirection::Output)->Type == "TestStats",
               "a Native signature names the struct");
        const std::vector<std::string>& VarTypes = S->GetVariableTypes();
        Expect(std::find(VarTypes.begin(), VarTypes.end(), "TestStats") != VarTypes.end(), "a graph variable may be a struct");

        // --- Code -------------------------------------------------------------------
        CompileResult Stats;
        CompileGraph(MakeStatsGraph(), Stats);
        Expect(!Stats.HasErrors(), "the struct graph compiles");
        if(Stats.HasErrors()) { std::printf("%s\n", FormatDiagnostics(MakeStatsGraph(), Stats).c_str()); }
        ExpectGolden("Stats.lua", Stats.Code);

        // --- Run --------------------------------------------------------------------
        const std::string Path = TempDir() + "/Stats.lua";
        WriteFile(Path, Stats.Code);
        {
            GameCore::World W;
            GameCore::ScriptEngine Scripts;
            Scripts.Initialize();
            Scripts.BindWorld(&W);

            GTestStats = TestStats();
            GameCore::Entity Holder = W.CreateEntity("Holder");
            Holder.AddComponent<StatsHolderComponent>();
            Holder.SetScript(Path.c_str());
            for(int Frame = 0; Frame < 3; ++Frame) { W.Tick(0.1f); }

            const StatsHolderComponent* C = Holder.GetComponent<StatsHolderComponent>();
            Expect(C && C->Stats.Hits == 3 && C->Stats.bLucky == 1 && std::fabs(C->Stats.Scale - 1.0f) < 1e-6f,
                   "Break, change, Make and set: three ticks count three hits");
            Expect(GTestStats.Hits == 6 && std::fabs(GTestStats.Scale - 3.0f) < 1e-6f && GTestStats.bLucky == 1,
                   "a struct variable merged each tick, then written to the library (got Hits "
                   + std::to_string(GTestStats.Hits) + ")");
            Expect(Scripts.GetErrorCount() == 0, "the struct script raised no Lua errors");

            Expect(Scripts.RunString(
                "local h = World.FindEntity('Holder'):GetComponent('TestStatsHolder') "
                "h:SetStats(5, 0.5, 1, 2, 3, false) "
                "local hits, scale, x, y, z, lucky = h:GetStats() "
                "assert(hits == 5 and scale == 0.5 and x == 1 and y == 2 and z == 3 and lucky == false) "
                "local b = { h:Bumped(10) } assert(#b == 6 and b[1] == 15) "
                "local m = { TestLib.Merge(1, 1, 0, 0, 0, false, 2, 1, 1, 1, 1, true) } "
                "assert(m[1] == 3 and m[2] == 2 and m[3] == 1 and m[6] == true)", "struct-lua"),
                   "in Lua a struct is its fields' values, in order, for fields, Native members and functions");
            Expect(!Scripts.RunString("TestLib.Merge(1, 'x')", "struct-bad"), "a bad struct argument is a Lua error");

            // --- Across an unload ---------------------------------------------------
            GameCore::UnregisterScriptStruct(Id);
            Expect(GameCore::GetScriptStruct(Id) == nullptr && std::string(GameCore::GetScriptStructName(Id)) == "TestStats",
                   "unregistering drops the fields and keeps the name");
            Expect(!Scripts.RunString("TestLib.GetStats()", "struct-unloaded"), "reading an unloaded struct is a Lua error");
            GameCore::ScriptStructBinder<TestStats> Again;
            Again.Field("Hits", &TestStats::Hits);
            Again.Field("Scale", &TestStats::Scale);
            Again.Field("Where", &TestStats::Where);
            Again.Flag("Lucky", &TestStats::bLucky);
            Expect(Again.Commit() && GameCore::GetScriptStruct(Id) != nullptr, "and it registers again under the same id");
            Expect(Scripts.RunString("assert(select('#', TestLib.GetStats()) == 6)", "struct-reloaded"), "and works again");

            Scripts.UnbindWorld();
            Scripts.Shutdown();
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
    TestLibraries();
    TestScriptStructs();
    TestMaterialCodegen();
    TestComponentSelection();
    TestLogic();
    TestDiscard();

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
