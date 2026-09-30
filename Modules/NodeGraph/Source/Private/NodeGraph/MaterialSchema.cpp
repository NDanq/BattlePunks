#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "Public/NodeGraph/GraphCompiler.h"
#include "Private/NodeGraph/CodeWriter.h"
#include "Private/NodeGraph/GraphIndex.h"

/*
 * Material graphs -> a pixel shader in the engine's macro GLSL: what
 * Engine/Modules/GDI/Shaders/Basic.fsh looks like, so GDI's cook_shaders.py
 * takes it on every backend (GLSL for OpenGL, SPIR-V for Vulkan, and on from
 * there to HLSL and MSL) with nothing new in the toolchain.
 *
 * Pure data flow: every node is a value, evaluated once, from the Pixel Output
 * node back. Each result that is more than a name or a literal becomes a
 * `<type> tN = ...;` temporary, so a value used twice is computed once.
 *
 * ONE UNIFORM, ALWAYS: `mat4 MaterialParams`, alone in a UBO at binding 2 (0
 * and 1 are Common.glsl's view and transform blocks). Its columns are four
 * vec4 slots -- parameters take 0 to 2, sorted by name; slot 3 is the drawing
 * primitive's Tint (the Instance Tint input). A single uniform because GDI's
 * backends pack a shader's custom uniforms back to back in hash-map order,
 * with no std140 padding (VulkanDynamicGDI.cpp's PackCustomUBO, D3D12's
 * PackCustom): exactly right for one value, and wrong for two. RenderCore's
 * RenderContext fills it -- see its DrawQuadWithMaterial.
 *
 * The parameters' defaults go in a companion text file, <Name>.material, which
 * the renderer reads with the shader (MaterialFormat below).
 *
 * CONTROL FLOW. A Branch is a real `if`, and a For Loop / For Loop End pair a
 * real `for` whose body is everything between them. A value is computed in
 * the innermost block all of its uses share (AnalyseScopes): one that only a
 * Branch's True side needs is computed inside that side, and skipped when the
 * other runs; one both sides need, before the `if`. Hints ([branch],
 * [unroll], ...) are the HINT_* macros of NodeGraph/Shaders/
 * MaterialPlatform.glsl, which a shader includes only when it uses one.
 */
namespace NodeGraph
{
namespace
{
    enum EMaterialOp : uint32_t
    {
        MOP_Template  = 0,   // outputs from PinDefinition::Expression
        MOP_Pass,            // the "Value" input, unchanged (Constant / reroute)
        MOP_Parameter,       // a uniform
        MOP_Output,          // the root
        MOP_Mask,            // Component Mask: the ticked components, in order
        MOP_Swizzle,         // Swizzle: any components, any order, repeats allowed
        MOP_Branch,          // an if: True or False, by comparing A with B
        MOP_LoopBegin,       // For Loop: Index and Value, inside its loop only
        MOP_LoopEnd,         // For Loop End: Next each time round, Result after
    };

    constexpr int UNIFORM_BLOCK_BINDING = 2;
    constexpr int PARAMETER_SLOTS       = 3;   // MaterialParams columns 0-2; 3 is the tint
    const char* const PARAMS_UNIFORM    = "MaterialParams";
    const char* const PLATFORM_HEADER   = "MaterialPlatform.glsl";   // NodeGraph/Shaders/
    const char* const LOOP_TYPE         = "loop";                    // For Loop -> its For Loop End

    const std::vector<std::string> GFloatTypes  = { "float", "vec2", "vec3", "vec4" };
    const std::vector<std::string> GVectorTypes = { "vec3", "vec2", "vec4" };

    size_t Components(const std::string& InType)
    {
        if(InType == "vec2") { return 2; }
        if(InType == "vec3") { return 3; }
        if(InType == "vec4") { return 4; }
        return 1;
    }

    /* A GLSL float literal: always with a '.' or exponent, so "1" is "1.0". */
    std::string FloatLiteral(const double InValue)
    {
        std::string Text = FormatNumber(float(InValue));
        if(Text.find_first_of(".e") == std::string::npos) { Text += ".0"; }
        return Text;
    }

    /* Letters, digits, '_' and '.': a name or a member access, safe to swizzle
     * without parentheses. */
    bool IsName(const std::string& InExpr)
    {
        if(InExpr.empty()) { return false; }
        for(const char Ch : InExpr)
        {
            const bool bOk = (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') || Ch == '_' || Ch == '.';
            if(!bOk) { return false; }
        }
        return true;
    }

    std::string Swizzle(const std::string& InExpr, const char* InMask)
    {
        return (IsName(InExpr) ? InExpr : "(" + InExpr + ")") + "." + InMask;
    }

    /* InExpr of type InFrom as InTo. Wider to narrower drops components;
     * narrower to wider pads with 0 and an alpha of 1; a float splats. */
    std::string Convert(const std::string& InExpr, const std::string& InFrom, const std::string& InTo)
    {
        if(InFrom == InTo) { return InExpr; }

        const size_t From = Components(InFrom);
        const size_t To   = Components(InTo);
        static const char* const Masks[] = { "", "x", "xy", "xyz", "xyzw" };

        if(From == 1)  { return InTo + "(" + InExpr + ")"; }
        if(To < From)  { return Swizzle(InExpr, Masks[To]); }
        if(From == 2 && To == 3) { return "vec3(" + InExpr + ", 0.0)"; }
        if(From == 2 && To == 4) { return "vec4(" + InExpr + ", 0.0, 1.0)"; }
        return "vec4(" + InExpr + ", 1.0)";   // vec3 -> vec4
    }

    std::string VectorType(const size_t InComponents)
    {
        return InComponents <= 1 ? "float" : "vec" + std::to_string(InComponents > 4 ? 4 : InComponents);
    }

    /* A Component Mask's four checkboxes, and what each is off by default. */
    const char* const GMaskProperties[4] = { "R", "G", "B", "A" };
    const char* const GMaskDefaults[4]   = { "true", "false", "false", "false" };
    const char* const GSwizzleProperty   = "Swizzle";
    const char* const GSwizzleDefault    = "xyz";

    /*
     * The components a Component Mask or Swizzle selects, as GLSL letters in
     * output order -- "xz". A Swizzle may be written with rgba instead of
     * xyzw (not both at once), in either case. "" when nothing is selected or
     * the text is not 1 to 4 components.
     */
    std::string SelectedComponents(const Node& InNode, const uint32_t InOp)
    {
        std::string Out;
        if(InOp == MOP_Mask)
        {
            for(int Index = 0; Index < 4; ++Index)
            {
                if(InNode.GetProperty(GMaskProperties[Index], GMaskDefaults[Index]) == "true") { Out += "xyzw"[Index]; }
            }
            return Out;
        }

        const std::string Text = InNode.GetProperty(GSwizzleProperty, GSwizzleDefault);
        if(Text.empty() || Text.size() > 4) { return std::string(); }
        bool bXyzw = false, bRgba = false;
        for(const char Raw : Text)
        {
            const char Ch = char(std::tolower(static_cast<unsigned char>(Raw)));
            const size_t Xyzw = std::string("xyzw").find(Ch);
            const size_t Rgba = std::string("rgba").find(Ch);
            if(Xyzw != std::string::npos)      { bXyzw = true; Out += Ch; }
            else if(Rgba != std::string::npos) { bRgba = true; Out += "xyzw"[Rgba]; }
            else                               { return std::string(); }
        }
        return bXyzw && bRgba ? std::string() : Out;
    }

    /* A Branch's and a For Loop's settings. A hint of "None" leaves it to the driver. */
    const std::vector<std::string> GCompareChoices    = { ">", ">=", "<", "<=", "==", "!=" };
    const std::vector<std::string> GBranchHintChoices = { "None", "Branch", "Flatten" };
    const std::vector<std::string> GLoopHintChoices   = { "None", "Loop", "Unroll" };

    /*
     * The block an input's value is computed in when that is not its node's
     * own: a Branch's True (arm 0) and False (arm 1), a For Loop End's Next
     * (the loop body, arm 0). -1 for every other input.
     */
    int ArmOf(const NodeDefinition& InDef, const std::string& InPin)
    {
        if(InDef.Op == MOP_Branch)
        {
            if(InPin == "True")  { return 0; }
            if(InPin == "False") { return 1; }
        }
        if(InDef.Op == MOP_LoopEnd && InPin == "Next") { return 0; }
        return -1;
    }

    /* The node linked into InNode's input InPin, or nullptr. */
    const Node* NodeInto(const Graph& InGraph, const Node& InNode, const char* InPin)
    {
        for(const Link& Each : InGraph.Links)
        {
            if(Each.To.Node == InNode.Id && Each.To.Pin == InPin) { return InGraph.FindNode(Each.From.Node); }
        }
        return nullptr;
    }

    /* The first node InNode's output InPin links to, or nullptr. */
    const Node* NodeOutOf(const Graph& InGraph, const Node& InNode, const char* InPin)
    {
        for(const Link& Each : InGraph.Links)
        {
            if(Each.From.Node == InNode.Id && Each.From.Pin == InPin) { return InGraph.FindNode(Each.To.Node); }
        }
        return nullptr;
    }

    class MaterialSchema final : public Schema
    {
    public:
        MaterialSchema();
        void Compile(const Graph& InGraph, CompileResult& OutResult) const override;

        /* A mask's or swizzle's output: one component per letter selected. */
        std::string ResolvePinType(const Graph& InGraph, const Node& InNode, const PinDefinition& InPin) const override
        {
            if(InPin.Type == DERIVED_TYPE)
            {
                const NodeDefinition* Def = FindNode(InNode.Type);
                if(Def && (Def->Op == MOP_Mask || Def->Op == MOP_Swizzle))
                {
                    return VectorType(SelectedComponents(InNode, Def->Op).size());
                }
            }
            return Schema::ResolvePinType(InGraph, InNode, InPin);
        }

        /*
         * A For Loop and its For Loop End carry one value, so one type: the
         * For Loop's, which the End follows. Unless Details sets it, the For
         * Loop's type is the widest its own links or its End's suggest -- a
         * vec3 fed to the End's Next widens the loop.
         */
        std::string ResolveWildcard(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef) const override
        {
            const auto Paired = [&](const Node* InOther, const uint32_t InOp) -> const NodeDefinition*
            {
                const NodeDefinition* Def = InOther ? FindNode(InOther->Type) : nullptr;
                return Def && Def->Op == InOp ? Def : nullptr;
            };

            if(InDef.Op == MOP_LoopEnd)
            {
                const Node* Begin = NodeInto(InGraph, InNode, "Loop");
                if(const NodeDefinition* BeginDef = Paired(Begin, MOP_LoopBegin)) { return ResolveWildcard(InGraph, *Begin, *BeginDef); }
            }
            else if(InDef.Op == MOP_LoopBegin
                    && std::find(InDef.WildcardTypes.begin(), InDef.WildcardTypes.end(), InNode.WildcardType) == InDef.WildcardTypes.end())
            {
                const std::string Own = Schema::ResolveWildcard(InGraph, InNode, InDef);
                const Node* End = NodeOutOf(InGraph, InNode, "Loop");
                const NodeDefinition* EndDef = Paired(End, MOP_LoopEnd);
                if(!EndDef) { return Own; }

                const std::string Theirs = Schema::ResolveWildcard(InGraph, *End, *EndDef);
                const auto Rank = [&](const std::string& InType)
                {
                    return std::find(InDef.WildcardTypes.begin(), InDef.WildcardTypes.end(), InType) - InDef.WildcardTypes.begin();
                };
                return Rank(Theirs) > Rank(Own) && Rank(Theirs) < std::ptrdiff_t(InDef.WildcardTypes.size()) ? Theirs : Own;
            }
            return Schema::ResolveWildcard(InGraph, InNode, InDef);
        }

        /* ".xz -> vec2" -- what a mask or swizzle takes, and what it makes.
         * "vec3 . A > B . [branch]" for a Branch; "vec3 . 4 times" for a loop. */
        std::string DescribeNode(const Graph& InGraph, const Node& InNode) const override
        {
            const NodeDefinition* Def = FindNode(InNode.Type);
            if(!Def) { return std::string(); }

            const std::string Dot = " \xC2\xB7 ";   // U+00B7
            const auto Hint = [&]
            {
                std::string Value = InNode.GetProperty("Hint", "None");
                if(Value == "None") { return std::string(); }
                std::transform(Value.begin(), Value.end(), Value.begin(), [](const unsigned char Ch) { return char(std::tolower(Ch)); });
                return Dot + "[" + Value + "]";
            };

            if(Def->Op == MOP_Branch)
            {
                return GetWildcardType(InGraph, InNode) + Dot + "A " + InNode.GetProperty("Compare", ">") + " B" + Hint();
            }
            if(Def->Op == MOP_LoopBegin)
            {
                const std::string* Count = InNode.FindConstant("Count");
                const std::string Times = NodeInto(InGraph, InNode, "Count") ? std::string("Count") : Count ? *Count : std::string("4");
                return GetWildcardType(InGraph, InNode) + Dot + Times + " times" + Hint();
            }
            if(Def->Op != MOP_Mask && Def->Op != MOP_Swizzle) { return std::string(); }

            const std::string Selected = SelectedComponents(InNode, Def->Op);
            if(Selected.empty()) { return Def->Op == MOP_Mask ? "nothing selected" : "invalid"; }
            return "." + Selected + " \xE2\x86\x92 " + VectorType(Selected.size());   // U+2192
        }
    };

    // === The compiler ===========================================================

    class MaterialCompiler
    {
    public:
        MaterialCompiler(const Graph& InGraph, const Schema& InSchema, CompileResult& InResult)
            : Index(InGraph, InSchema), Result(InResult) {}

        void Run();

    private:
        struct Parameter
        {
            std::string Type;
            NodeGuid    Node;
            int         Slot = 0;
            std::string Default;
        };

        /* Where a value is computed: the blocks around it, outermost first.
         * (Branch, 0) is that Branch's True side; (For Loop End, 0) that
         * loop's body. The function's own block is the empty scope. */
        struct Frame
        {
            NodeGuid Owner;
            int      Arm = 0;
            bool operator==(const Frame& InOther) const { return Owner == InOther.Owner && Arm == InOther.Arm; }
        };
        using Scope = std::vector<Frame>;

        struct OpenLoop
        {
            std::string Value;   // the carried value's variable
            std::string Index;   // the int counter
        };

        void CollectParameters();
        void AnalyseScopes(const Node& InRoot);
        void ValidateLoops();

        std::string Output(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin);
        std::string EmitBranch(const Node& InNode, const NodeDefinition& InDef, const std::string& InType);
        std::string EmitLoop(const Node& InEnd, const NodeDefinition& InEndDef, const std::string& InType);
        std::string HintPrefix(const Node& InNode);

        void Hoist(const Node& InOwner, const NodeDefinition& InDef, int InArm);
        void HoistFrom(const Node& InNode, const PinDefinition& InPin, const Scope& InArm, std::set<NodeGuid>& InOutSeen);
        void OpenBlock();
        void CloseBlock();

        std::string Input(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin);
        std::string Literal(const Node& InNode, const PinDefinition& InPin, const std::string& InType);
        std::string Expand(const Node& InNode, const NodeDefinition& InDef, const std::string& InTemplate);

        GraphIndex     Index;
        CompileResult& Result;
        CodeWriter     Body;

        std::map<PinRef, std::string>    Values;
        std::set<NodeGuid>               InProgress;
        std::map<std::string, Parameter> Parameters;   // by name: sorted, so the block layout is stable
        int                              NextTemp = 0;

        std::map<NodeGuid, Scope>        Scopes;          // every node the output reaches; empty after a cycle
        std::map<NodeGuid, OpenLoop>     OpenLoops;       // by For Loop, while its body is written
        std::vector<std::vector<PinRef>> Blocks;          // the values cached in each open block
        int                              NextLoop = 0;
        bool                             bUsesPlatform = false;   // a HINT_* macro was written
    };

    /* Whether InInner is InOuter or a block inside it. */
    template<typename TScope>
    bool IsWithin(const TScope& InInner, const TScope& InOuter)
    {
        return InInner.size() >= InOuter.size() && std::equal(InOuter.begin(), InOuter.end(), InInner.begin());
    }

    void MaterialCompiler::Run()
    {
        const Graph& G = Index.GetGraph();

        const Node* Root = nullptr;
        const NodeDefinition* RootDef = nullptr;
        for(const Node* Each : Index.SortedNodes())
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            if(Def && Def->Op == MOP_Output)
            {
                Root = Each;
                RootDef = Def;
                break;
            }
        }
        if(!Root)
        {
            Result.Error(NodeGuid(), std::string(), "a material needs a Pixel Output node");
            return;
        }

        CollectParameters();
        AnalyseScopes(*Root);
        ValidateLoops();
        if(Result.HasErrors()) { return; }

        const PinDefinition* ColorPin = RootDef->FindPin("Color", EPinDirection::Input);
        const std::string Color = Input(*Root, *RootDef, *ColorPin);
        if(Result.HasErrors()) { return; }

        const std::string BlockName = SanitizeIdentifier(G.Name.empty() ? std::string("Material") : G.Name) + "_Block";

        CodeWriter Out;
        Out.Line("#version 440 core");
        Out.Line("// Generated by NimbleEditor from the material graph \"" + G.Name + "\". Do not edit.");
        Out.Blank();
        if(bUsesPlatform) { Out.Line("#include <" + std::string(PLATFORM_HEADER) + ">"); }   // first: it may declare an #extension
        Out.Line("#include <Common.glsl>");
        Out.Blank();
        Out.Line("OUTPUT(0, vec4, Output)");
        Out.Blank();

        /* Declared even when nothing reads it: the renderer always sets it,
         * and a shader without the block would not match that layout. */
        Out.Line("// Columns 0-2: parameters (see " + (G.Name.empty() ? std::string("<Name>") : G.Name)
                 + ".material); column 3: the primitive's tint.");
        Out.Line("UBO_BEGIN(" + std::to_string(UNIFORM_BLOCK_BINDING) + ", " + BlockName + ")");
        Out.Indent();
        Out.Line("SHADER_UNIFORM(mat4, " + std::string(PARAMS_UNIFORM) + ")");
        Out.Outdent();
        Out.Line("UBO_END(" + BlockName + ")");

        Out.Blank();
        Out.Line("void main(void)");
        Out.Line("{");
        Out.Indent();
        Out.Append(Body);
        Out.Line("Output = " + Color + ";", Root->Id);
        Out.Outdent();
        Out.Line("}");

        Result.Code      = std::move(Out.Text);
        Result.LineNodes = std::move(Out.LineNodes);

        /*
         * The companion: what the renderer loads beside the shader.
         *
         *     NimbleMaterial 1
         *     Param <Name> <Slot> <Type> <x> <y> <z> <w>
         *
         * '#' starts a comment. Unused components are 0.
         */
        std::string Material = "# Generated by NimbleEditor from the material graph \"" + G.Name + "\". Do not edit.\n"
                               "NimbleMaterial 1\n";
        for(const auto& Each : Parameters)
        {
            std::vector<double> Values;
            ParseNumbers(Each.second.Default, Components(Each.second.Type), Values);
            Values.resize(4, 0.0);
            Material += "Param " + Each.first + " " + std::to_string(Each.second.Slot) + " " + Each.second.Type;
            for(const double Value : Values) { Material += " " + FormatNumber(float(Value)); }
            Material += "\n";
        }
        Result.Companions.push_back({ ".material", Material });
    }

    /* Every Parameter node, used or not, gets its slot up front -- by name, so
     * a slot never moves because a link was drawn somewhere else. */
    void MaterialCompiler::CollectParameters()
    {
        for(const Node* Each : Index.SortedNodes())
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            if(!Def || Def->Op != MOP_Parameter) { continue; }

            const std::string Name = Each->GetProperty("Name", "Param");
            const std::string Type = Index.GetSchema().GetWildcardType(Index.GetGraph(), *Each);
            const auto Existing = Parameters.find(Name);
            if(Existing != Parameters.end())
            {
                if(Existing->second.Type != Type)
                {
                    Result.Error(Each->Id, std::string(), "parameter '" + Name + "' is " + Type + " here but "
                                 + Existing->second.Type + " elsewhere");
                }
                continue;
            }

            Parameter Added;
            Added.Type    = Type;
            Added.Node    = Each->Id;
            Added.Default = Each->GetProperty("Default", "0");
            std::vector<double> Check;
            if(!ParseNumbers(Added.Default, Components(Type), Check))
            {
                Result.Error(Each->Id, std::string(), "parameter '" + Name + "': default '" + Added.Default
                             + "' is not a " + Type + " value");
            }
            Parameters.emplace(Name, Added);
        }

        if(int(Parameters.size()) > PARAMETER_SLOTS)
        {
            Result.Error(NodeGuid(), std::string(), "a material has at most " + std::to_string(PARAMETER_SLOTS)
                         + " parameters (MaterialParams columns 0-2; column 3 is the instance tint), and this one has "
                         + std::to_string(Parameters.size()));
        }

        int Slot = 0;
        for(auto& Each : Parameters) { Each.second.Slot = Slot++; }
    }

    /*
     * Every reachable node's scope: the innermost block that all of its uses
     * share. Consumers come before what they read (reverse post-order from
     * the root), so each node's scope is final when its inputs are visited;
     * an input a Branch or For Loop End runs in one of its blocks is used
     * inside that block. A cycle leaves Scopes empty, for Output to report.
     */
    void MaterialCompiler::AnalyseScopes(const Node& InRoot)
    {
        std::vector<const Node*> Order;
        std::map<NodeGuid, int>  State;   // 1 visiting, 2 done
        bool bCycle = false;

        const std::function<void(const Node&)> Visit = [&](const Node& InNode)
        {
            State[InNode.Id] = 1;
            if(const NodeDefinition* Def = Index.FindDefinition(InNode))
            {
                for(const PinDefinition& Pin : Def->Pins)
                {
                    const Link* Into = Pin.IsInput() ? Index.LinkInto(InNode.Id, Pin.Name) : nullptr;
                    const Node* Source = Into ? Index.FindNode(Into->From.Node) : nullptr;
                    if(!Source) { continue; }

                    const int Seen = State.count(Source->Id) ? State[Source->Id] : 0;
                    if(Seen == 1) { bCycle = true; }
                    if(Seen == 0) { Visit(*Source); }
                }
            }
            State[InNode.Id] = 2;
            Order.push_back(&InNode);
        };
        Visit(InRoot);
        if(bCycle) { return; }

        Scopes[InRoot.Id] = Scope();
        for(auto It = Order.rbegin(); It != Order.rend(); ++It)
        {
            const Node& Each = **It;
            const NodeDefinition* Def = Index.FindDefinition(Each);
            if(!Def) { continue; }

            const Scope Own = Scopes[Each.Id];
            for(const PinDefinition& Pin : Def->Pins)
            {
                const Link* Into = Pin.IsInput() ? Index.LinkInto(Each.Id, Pin.Name) : nullptr;
                if(!Into || !Index.FindNode(Into->From.Node)) { continue; }

                Scope Use = Own;
                const int Arm = ArmOf(*Def, Pin.Name);
                if(Arm >= 0) { Use.push_back({ Each.Id, Arm }); }

                const auto Found = Scopes.find(Into->From.Node);
                if(Found == Scopes.end())
                {
                    Scopes.emplace(Into->From.Node, std::move(Use));
                    continue;
                }
                size_t Common = 0;
                while(Common < Found->second.size() && Common < Use.size() && Found->second[Common] == Use[Common]) { ++Common; }
                Found->second.resize(Common);
            }
        }
    }

    /*
     * A For Loop's Index and Value exist only in its body, and it closes at
     * exactly one For Loop End: each is an error on the node that breaks it,
     * found here, before any code is written.
     */
    void MaterialCompiler::ValidateLoops()
    {
        std::set<NodeGuid> Reported;
        for(const auto& Each : Scopes)
        {
            const Node* N = Index.FindNode(Each.first);
            const NodeDefinition* Def = N ? Index.FindDefinition(*N) : nullptr;
            if(!Def) { continue; }

            if(Def->Op == MOP_LoopEnd && !Index.LinkInto(N->Id, "Loop"))
            {
                Result.Error(N->Id, "Loop", "'" + Def->DisplayName + "' closes a loop: link a For Loop's Loop pin into it");
            }
            if(Def->Op == MOP_LoopBegin && Index.LinksOutOf(N->Id, "Loop").size() > 1)
            {
                Result.Error(N->Id, "Loop", "'" + Def->DisplayName + "' closes at one For Loop End, and this one links to "
                             + std::to_string(Index.LinksOutOf(N->Id, "Loop").size()));
            }

            for(const PinDefinition& Pin : Def->Pins)
            {
                const Link* Into = Pin.IsInput() ? Index.LinkInto(N->Id, Pin.Name) : nullptr;
                const Node* Begin = Into ? Index.FindNode(Into->From.Node) : nullptr;
                const NodeDefinition* BeginDef = Begin ? Index.FindDefinition(*Begin) : nullptr;
                if(!BeginDef || BeginDef->Op != MOP_LoopBegin || Into->From.Pin == "Loop") { continue; }

                const std::vector<const Link*>& Ends = Index.LinksOutOf(Begin->Id, "Loop");
                if(Ends.empty())
                {
                    if(Reported.insert(Begin->Id).second)
                    {
                        Result.Error(Begin->Id, "Loop", "'" + BeginDef->DisplayName + "' has no For Loop End: link its Loop pin"
                                     " to one, and read its Index and Value in between");
                    }
                    continue;
                }
                if(Ends.size() > 1) { continue; }   // reported above

                const auto EndScope = Scopes.find(Ends[0]->To.Node);
                Scope Body = EndScope != Scopes.end() ? EndScope->second : Scope();
                Body.push_back({ Ends[0]->To.Node, 0 });

                Scope Use = Each.second;
                const int Arm = ArmOf(*Def, Pin.Name);
                if(Arm >= 0) { Use.push_back({ N->Id, Arm }); }

                if(EndScope == Scopes.end() || !IsWithin(Use, Body))
                {
                    Result.Error(N->Id, Pin.Name, "'" + Def->DisplayName + "' reads the For Loop's " + Into->From.Pin
                                 + " outside its loop: only what leads into its For Loop End's Next is in the loop");
                }
            }
        }
    }

    std::string MaterialCompiler::Output(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin)
    {
        const PinRef Key{ InNode.Id, InPin.Name };
        const auto Cached = Values.find(Key);
        if(Cached != Values.end()) { return Cached->second; }

        if(!InProgress.insert(InNode.Id).second)
        {
            Result.Error(InNode.Id, InPin.Name, "'" + InDef.DisplayName + "' depends on its own output (a cycle)");
            return "0.0";
        }

        const std::string Type = Index.PinType(InNode, InPin);
        std::string Value;
        switch(InDef.Op)
        {
            case MOP_Parameter:
            {
                /* Its column of MaterialParams, narrowed to its type. */
                const auto Found = Parameters.find(InNode.GetProperty("Name", "Param"));
                static const char* const Masks[] = { "", ".x", ".xy", ".xyz", "" };
                Value = std::string(PARAMS_UNIFORM) + "[" + std::to_string(Found != Parameters.end() ? Found->second.Slot : 0) + "]"
                      + Masks[Components(Type)];
                break;
            }

            case MOP_Mask:
            case MOP_Swizzle:
            {
                /* A swizzle expression, used where it is needed -- no temporary. */
                const PinDefinition* In = InDef.FindPin("Vector", EPinDirection::Input);
                const std::string InType   = Index.PinType(InNode, *In);
                const size_t      Width    = Components(InType);
                const std::string Selected = SelectedComponents(InNode, InDef.Op);
                const std::string Label    = "'" + InDef.DisplayName + "'";

                if(Selected.empty())
                {
                    Result.Error(InNode.Id, std::string(), InDef.Op == MOP_Mask
                                 ? Label + ": enable at least one of R, G, B, A"
                                 : Label + ": '" + InNode.GetProperty(GSwizzleProperty, GSwizzleDefault)
                                   + "' is not 1 to 4 of x, y, z, w (or of r, g, b, a)");
                    Value = "0.0";
                    break;
                }

                bool bInRange = true;
                for(const char Ch : Selected)
                {
                    const size_t Component = std::string("xyzw").find(Ch);
                    if(Component < Width) { continue; }
                    const std::string Name = InDef.Op == MOP_Mask ? std::string(GMaskProperties[Component]) : std::string(1, Ch);
                    Result.Error(InNode.Id, "Vector", Label + " reads " + Name + ", but its input is " + InType);
                    bInRange = false;
                    break;
                }
                if(!bInRange)
                {
                    Value = "0.0";
                    break;
                }

                const std::string Source = Input(InNode, InDef, *In);
                if(Width == 1)
                {
                    Value = Selected.size() == 1 ? Source : VectorType(Selected.size()) + "(" + Source + ")";   // splat
                }
                else if(Selected == std::string("xyzw").substr(0, Width))
                {
                    Value = Source;   // everything, in order: nothing to swizzle
                }
                else
                {
                    Value = Swizzle(Source, Selected.c_str());
                }
                break;
            }

            case MOP_Pass:
            {
                const PinDefinition* In = InDef.FindPin("Value", EPinDirection::Input);
                Value = In ? Input(InNode, InDef, *In) : "0.0";
                break;
            }

            case MOP_Branch:
                Value = EmitBranch(InNode, InDef, Type);
                break;

            case MOP_LoopEnd:
                Value = EmitLoop(InNode, InDef, Type);
                break;

            case MOP_LoopBegin:
            {
                /* ValidateLoops already refused a read from outside the body;
                 * this is the backstop for a graph it could not analyse. */
                const auto Open = OpenLoops.find(InNode.Id);
                if(Open == OpenLoops.end() || InPin.Name == "Loop")
                {
                    Result.Error(InNode.Id, InPin.Name, "'" + InDef.DisplayName + "''s " + InPin.Name
                                 + " exists only inside its loop, before its For Loop End");
                    Value = "0.0";
                    break;
                }
                Value = InPin.Name == "Index" ? "float(" + Open->second.Index + ")" : Open->second.Value;
                break;
            }

            default:
            {
                const bool bHasInputs = InPin.Expression.find('{') != std::string::npos;
                Value = Expand(InNode, InDef, InPin.Expression);

                /* Anything computed gets a temporary; a bare name or built-in
                 * (Interpolants.TexCoord) is used where it is needed. */
                if(bHasInputs)
                {
                    const std::string Temp = "t" + std::to_string(NextTemp++);
                    Body.Line(Type + " " + Temp + " = " + Value + ";", InNode.Id);
                    Value = Temp;
                }
                break;
            }
        }

        InProgress.erase(InNode.Id);
        Values.emplace(Key, Value);
        if(!Blocks.empty()) { Blocks.back().push_back(Key); }   // its temporary ends with the block
        return Value;
    }

    /*
     * A Branch, as
     *
     *     vec3 t3;
     *     HINT_BRANCH if(t1 > 0.5)
     *     {
     *         ...what only True needs...
     *         t3 = <True>;
     *     }
     *     else
     *     { ... }
     */
    std::string MaterialCompiler::EmitBranch(const Node& InNode, const NodeDefinition& InDef, const std::string& InType)
    {
        const std::string A = Input(InNode, InDef, *InDef.FindPin("A", EPinDirection::Input));
        const std::string B = Input(InNode, InDef, *InDef.FindPin("B", EPinDirection::Input));
        Hoist(InNode, InDef, 0);
        Hoist(InNode, InDef, 1);

        const std::string Temp = "t" + std::to_string(NextTemp++);
        Body.Line(InType + " " + Temp + ";", InNode.Id);
        Body.Line(HintPrefix(InNode) + "if(" + A + " " + InNode.GetProperty("Compare", ">") + " " + B + ")", InNode.Id);

        for(const char* Arm : { "True", "False" })
        {
            if(Arm[0] == 'F') { Body.Line("else", InNode.Id); }
            Body.Line("{", InNode.Id);
            OpenBlock();
            const std::string Value = Input(InNode, InDef, *InDef.FindPin(Arm, EPinDirection::Input));
            Body.Line(Temp + " = " + Value + ";", InNode.Id);
            CloseBlock();
            Body.Line("}", InNode.Id);
        }
        return Temp;
    }

    /*
     * A For Loop End and its For Loop, as
     *
     *     vec3 t4 = <Initial>;
     *     HINT_UNROLL for(int i0 = 0; i0 < 4; ++i0)
     *     {
     *         ...the body, reading float(i0) and t4...
     *         t4 = <Next>;
     *     }
     *
     * and t4 is the Result.
     */
    std::string MaterialCompiler::EmitLoop(const Node& InEnd, const NodeDefinition& InEndDef, const std::string& InType)
    {
        const Link* Into = Index.LinkInto(InEnd.Id, "Loop");
        const Node* Begin = Into ? Index.FindNode(Into->From.Node) : nullptr;
        const NodeDefinition* BeginDef = Begin ? Index.FindDefinition(*Begin) : nullptr;
        if(!BeginDef || BeginDef->Op != MOP_LoopBegin)
        {
            Result.Error(InEnd.Id, "Loop", "'" + InEndDef.DisplayName + "' closes a loop: link a For Loop's Loop pin into it");
            return "0.0";
        }

        /* A constant count is written as one, so a driver (or [unroll]) sees it. */
        const PinDefinition* CountPin = BeginDef->FindPin("Count", EPinDirection::Input);
        const bool bUnroll = Begin->GetProperty("Hint", "None") == "Unroll";
        std::string Count;
        if(Index.LinkInto(Begin->Id, "Count"))
        {
            if(bUnroll)
            {
                Result.Error(Begin->Id, "Count", "'" + BeginDef->DisplayName + "' is set to Unroll, which needs a constant"
                             " Count: unlink Count, or choose another hint");
            }
            Count = "int(" + Input(*Begin, *BeginDef, *CountPin) + ")";
        }
        else
        {
            const std::string* Given = Begin->FindConstant("Count");
            const std::string Text = Given ? *Given : CountPin->Default;
            double Number = 0.0;
            if(!ParseNumber(Text, Number) || Number < 0.0 || Number != std::floor(Number) || Number > 1e6)
            {
                Result.Error(Begin->Id, "Count", "'" + BeginDef->DisplayName + "''s Count must be a whole number, 0 or more, not '"
                             + Text + "'");
                Number = 0.0;
            }
            Count = std::to_string(int64_t(Number));
        }

        Hoist(InEnd, InEndDef, 0);
        const std::string Initial = Input(*Begin, *BeginDef, *BeginDef->FindPin("Initial", EPinDirection::Input));

        const std::string Carried = "t" + std::to_string(NextTemp++);
        const std::string Counter = "i" + std::to_string(NextLoop++);
        Body.Line(InType + " " + Carried + " = " + Initial + ";", Begin->Id);
        Body.Line(HintPrefix(*Begin) + "for(int " + Counter + " = 0; " + Counter + " < " + Count + "; ++" + Counter + ")", Begin->Id);
        Body.Line("{", Begin->Id);
        OpenBlock();
        OpenLoops[Begin->Id] = { Carried, Counter };
        const std::string Next = Input(InEnd, InEndDef, *InEndDef.FindPin("Next", EPinDirection::Input));
        Body.Line(Carried + " = " + Next + ";", InEnd.Id);
        OpenLoops.erase(Begin->Id);
        CloseBlock();
        Body.Line("}", InEnd.Id);
        return Carried;
    }

    /* "HINT_UNROLL " for a node whose Hint asks for one; "" for None. */
    std::string MaterialCompiler::HintPrefix(const Node& InNode)
    {
        std::string Hint = InNode.GetProperty("Hint", "None");
        if(Hint == "None") { return std::string(); }

        bUsesPlatform = true;
        std::transform(Hint.begin(), Hint.end(), Hint.begin(), [](const unsigned char Ch) { return char(std::toupper(Ch)); });
        return "HINT_" + Hint + " ";
    }

    /*
     * Before InOwner opens its block InArm: writes, here, every value that
     * block reads but that belongs further out -- shared with the other side
     * of a Branch, or with what comes after -- so it is computed once, and
     * not inside a block the other users cannot see.
     */
    void MaterialCompiler::Hoist(const Node& InOwner, const NodeDefinition& InDef, const int InArm)
    {
        const auto Found = Scopes.find(InOwner.Id);
        if(Found == Scopes.end()) { return; }   // no analysis (a cycle): values are computed where used

        Scope Arm = Found->second;
        Arm.push_back({ InOwner.Id, InArm });

        std::set<NodeGuid> Seen;
        for(const PinDefinition& Pin : InDef.Pins)
        {
            if(Pin.IsInput() && ArmOf(InDef, Pin.Name) == InArm) { HoistFrom(InOwner, Pin, Arm, Seen); }
        }
    }

    void MaterialCompiler::HoistFrom(const Node& InNode, const PinDefinition& InPin, const Scope& InArm, std::set<NodeGuid>& InOutSeen)
    {
        const Link* Into = Index.LinkInto(InNode.Id, InPin.Name);
        const Node* Source = Into ? Index.FindNode(Into->From.Node) : nullptr;
        const NodeDefinition* SourceDef = Source ? Index.FindDefinition(*Source) : nullptr;
        const PinDefinition* SourcePin = SourceDef ? SourceDef->FindPin(Into->From.Pin, EPinDirection::Output) : nullptr;
        if(!SourcePin) { return; }

        /* A For Loop's own outputs exist only in its body, never out here;
         * what it reads is evaluated with its For Loop End, so look past it. */
        const auto Found = Scopes.find(Source->Id);
        const bool bInside = SourceDef->Op == MOP_LoopBegin || Found == Scopes.end() || IsWithin(Found->second, InArm);
        if(!bInside)
        {
            Output(*Source, *SourceDef, *SourcePin);
            return;
        }

        if(!InOutSeen.insert(Source->Id).second) { return; }
        for(const PinDefinition& Pin : SourceDef->Pins)
        {
            if(Pin.IsInput()) { HoistFrom(*Source, Pin, InArm, InOutSeen); }
        }
    }

    void MaterialCompiler::OpenBlock()
    {
        Body.Indent();
        Blocks.emplace_back();
    }

    /* The block's temporaries go out of scope with it, so their cached names do too. */
    void MaterialCompiler::CloseBlock()
    {
        for(const PinRef& Each : Blocks.back()) { Values.erase(Each); }
        Blocks.pop_back();
        Body.Outdent();
    }

    std::string MaterialCompiler::Input(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin)
    {
        const std::string Type = Index.PinType(InNode, InPin);

        const Link* Into = Index.LinkInto(InNode.Id, InPin.Name);
        if(!Into) { return Literal(InNode, InPin, Type); }

        const Node* Source = Index.FindNode(Into->From.Node);
        const NodeDefinition* SourceDef = Source ? Index.FindDefinition(*Source) : nullptr;
        const PinDefinition* SourcePin = SourceDef ? SourceDef->FindPin(Into->From.Pin, EPinDirection::Output) : nullptr;
        if(!SourcePin) { return Literal(InNode, InPin, Type); }   // ValidateGraph already reported it

        const std::string Value = Output(*Source, *SourceDef, *SourcePin);
        return Convert(Value, Index.PinType(*Source, *SourcePin), Type);
    }

    std::string MaterialCompiler::Literal(const Node& InNode, const PinDefinition& InPin, const std::string& InType)
    {
        const std::string* Given = InNode.FindConstant(InPin.Name);
        std::string Text = Given ? *Given : InPin.Default;
        if(Text.empty())
        {
            const PinTypeInfo* Info = Index.GetSchema().FindType(InType);
            Text = Info ? Info->DefaultLiteral : "0";
        }

        const size_t Count = Components(InType);
        std::vector<double> Values;
        if(!ParseNumbers(Text, Count, Values))
        {
            Result.Error(InNode.Id, InPin.Name, "'" + Text + "' is not a " + InType + " value");
            Values.assign(Count, 0.0);
        }

        if(Count == 1) { return FloatLiteral(Values[0]); }

        const bool bSplat = std::all_of(Values.begin(), Values.end(), [&](const double V) { return V == Values[0]; });
        std::string Out = InType + "(";
        for(size_t Index = 0; Index < (bSplat ? 1 : Count); ++Index)
        {
            if(Index > 0) { Out += ", "; }
            Out += FloatLiteral(Values[Index]);
        }
        return Out + ")";
    }

    /* "{A} + {B}" with each {Pin} replaced by that input's value. */
    std::string MaterialCompiler::Expand(const Node& InNode, const NodeDefinition& InDef, const std::string& InTemplate)
    {
        std::string Out;
        size_t Pos = 0;
        while(Pos < InTemplate.size())
        {
            const size_t Open = InTemplate.find('{', Pos);
            if(Open == std::string::npos)
            {
                Out += InTemplate.substr(Pos);
                break;
            }
            const size_t Close = InTemplate.find('}', Open);
            Out += InTemplate.substr(Pos, Open - Pos);

            const std::string PinName = InTemplate.substr(Open + 1, Close - Open - 1);
            const PinDefinition* Pin = InDef.FindPin(PinName, EPinDirection::Input);
            Out += Pin ? Input(InNode, InDef, *Pin) : "0.0";
            Pos = Close + 1;
        }
        return Out;
    }

    // === The schema =============================================================

    PinDefinition In(const std::string& InName, const std::string& InType, const std::string& InDefault = std::string())
    {
        PinDefinition Pin;
        Pin.Name      = InName;
        Pin.Direction = EPinDirection::Input;
        Pin.Type      = InType;
        Pin.Default   = InDefault;
        return Pin;
    }

    PinDefinition Out(const std::string& InName, const std::string& InType, const std::string& InExpression)
    {
        PinDefinition Pin;
        Pin.Name       = InName;
        Pin.Direction  = EPinDirection::Output;
        Pin.Type       = InType;
        Pin.Expression = InExpression;
        return Pin;
    }

    MaterialSchema::MaterialSchema()
        : Schema("Material", "material", ".fsh")
    {
        AddType({ "float", "Float",   0x9BD96A, "0" });
        AddType({ "vec2",  "Vector2", 0x6AC9D9, "0" });
        AddType({ "vec3",  "Vector3", 0xE8C95A, "0" });
        AddType({ "vec4",  "Vector4", 0xD9786A, "0" });
        AddType({ LOOP_TYPE, "Loop",  0xC8C8C8, "", false });   // pairs a For Loop with its End; no literal

        for(const std::string& From : GFloatTypes)
        {
            for(const std::string& To : GFloatTypes)
            {
                if(From != To) { AddConversion(From, To); }
            }
        }

        const auto Pure = [&](const std::string& InType, const std::string& InName, const std::string& InCategory,
                              std::vector<PinDefinition> InPins, std::vector<std::string> InWildcard = {},
                              const std::string& InKeywords = std::string()) -> NodeDefinition&
        {
            NodeDefinition Def;
            Def.Type          = InType;
            Def.DisplayName   = InName;
            Def.Category      = InCategory;
            Def.Keywords      = InKeywords;
            Def.Role          = ENodeRole::Pure;
            Def.Pins          = std::move(InPins);
            Def.WildcardTypes = std::move(InWildcard);
            return AddNode(std::move(Def));
        };

        // --- Output --------------------------------------------------------------
        {
            NodeDefinition Def;
            Def.Type        = "Output.Pixel";
            Def.DisplayName = "Pixel Output";
            Def.Category    = "Output";
            Def.Tooltip     = "The colour this pixel shader writes.";
            Def.Role        = ENodeRole::Output;
            Def.bUnique     = true;
            Def.Op          = MOP_Output;
            Def.Pins        = { In("Color", "vec4", "1 1 1 1") };
            AddNode(std::move(Def));
        }

        // --- Inputs --------------------------------------------------------------
        Pure("Input.TexCoord",   "Texture Coordinate", "Inputs", { Out("UV", "vec2", "Interpolants.TexCoord") }, {}, "uv");
        Pure("Input.Position",   "Interpolated Position", "Inputs", { Out("Position", "vec3", "Interpolants.Position") });
        Pure("Input.FragCoord",  "Pixel Coordinate", "Inputs", { Out("Coord", "vec4", "gl_FragCoord") }, {}, "screen frag");
        Pure("Input.Viewport",   "Viewport Size", "Inputs",
             { Out("Size", "vec2", "ViewParameters.Resolution.xy"), Out("InvSize", "vec2", "ViewParameters.Resolution.zw") },
             {}, "resolution");
        Pure("Input.InstanceTint", "Instance Tint", "Inputs",
             { Out("Tint", "vec4", std::string(PARAMS_UNIFORM) + "[3]") }, {}, "color colour primitive per-entity");
        Pure("Input.Mouse",      "Mouse", "Inputs",
             { Out("Position", "vec2", "ViewParameters.MouseCoords.xy"), Out("Down", "float", "ViewParameters.MouseCoords.z") });

        // --- Values --------------------------------------------------------------
        {
            NodeDefinition& Def = Pure("Constant", "Constant", "Values", { In("Value", WILDCARD_TYPE), Out("Out", WILDCARD_TYPE, "") },
                                       GFloatTypes, "number vector color literal");
            Def.Op = MOP_Pass;
        }
        {
            NodeDefinition& Def = Pure("Parameter", "Parameter", "Values", { Out("Value", WILDCARD_TYPE, "") }, GFloatTypes, "uniform");
            Def.Op      = MOP_Parameter;
            Def.Tooltip = "A value the renderer supplies: one column of MaterialParams, its default from <Name>.material. "
                          "At most three per material.";
            Def.Properties = { { "Name", EPropertyKind::Identifier, "Param", {}, "The parameter's name; parameters take their slots in name order" },
                               { "Default", EPropertyKind::String, "0", {}, "The value the renderer uses, e.g. \"1 0.5 0\"" } };
        }

        // --- Math: every float type, component-wise ------------------------------
        const std::string W = WILDCARD_TYPE;
        const auto Binary = [&](const std::string& InType, const std::string& InName, const std::string& InExpr,
                                const std::string& InDefaultB = "0", const std::string& InKeywords = std::string())
        {
            Pure("Math." + InType, InName, "Math", { In("A", W), In("B", W, InDefaultB), Out("Out", W, InExpr) }, GFloatTypes, InKeywords);
        };
        const auto Unary = [&](const std::string& InType, const std::string& InName, const std::string& InExpr,
                               const std::string& InCategory = "Math", const std::string& InKeywords = std::string())
        {
            Pure("Math." + InType, InName, InCategory, { In("X", W), Out("Out", W, InExpr) }, GFloatTypes, InKeywords);
        };

        Binary("Add",      "Add",      "{A} + {B}", "0", "+ plus sum");
        Binary("Subtract", "Subtract", "{A} - {B}", "0", "- minus difference");
        Binary("Multiply", "Multiply", "{A} * {B}", "1", "* times product");
        Binary("Divide",   "Divide",   "{A} / {B}", "1", "/ quotient");
        Binary("Min",      "Min",      "min({A}, {B})");
        Binary("Max",      "Max",      "max({A}, {B})");
        Binary("Power",    "Power",    "pow({A}, {B})", "1", "pow exponent");
        Binary("Mod",      "Modulo",   "mod({A}, {B})", "1", "% remainder");
        Binary("Step",     "Step",     "step({A}, {B})", "0", "threshold edge");

        Unary("Negate",   "Negate",    "-{X}");
        Unary("OneMinus", "One Minus", "1.0 - {X}", "Math", "invert 1-x");
        Unary("Abs",      "Absolute",  "abs({X})");
        Unary("Sign",     "Sign",      "sign({X})");
        Unary("Floor",    "Floor",     "floor({X})", "Math/Rounding");
        Unary("Ceil",     "Ceiling",   "ceil({X})", "Math/Rounding");
        Unary("Fract",    "Fraction",  "fract({X})", "Math/Rounding", "frac");
        Unary("Sqrt",     "Square Root", "sqrt({X})");
        Unary("Saturate", "Saturate",  "clamp({X}, 0.0, 1.0)", "Math", "clamp01");
        Unary("Sin",      "Sine",      "sin({X})", "Math/Trigonometry");
        Unary("Cos",      "Cosine",    "cos({X})", "Math/Trigonometry");
        Unary("Tan",      "Tangent",   "tan({X})", "Math/Trigonometry");

        Pure("Math.Lerp", "Lerp", "Math", { In("A", W), In("B", W, "1"), In("T", W, "0.5"), Out("Out", W, "mix({A}, {B}, {T})") },
             GFloatTypes, "mix blend interpolate");
        Pure("Math.Clamp", "Clamp", "Math", { In("X", W), In("Min", W, "0"), In("Max", W, "1"), Out("Out", W, "clamp({X}, {Min}, {Max})") },
             GFloatTypes);
        Pure("Math.Smoothstep", "Smooth Step", "Math", { In("Edge0", W, "0"), In("Edge1", W, "1"), In("X", W),
                                                         Out("Out", W, "smoothstep({Edge0}, {Edge1}, {X})") }, GFloatTypes);

        // --- Vectors -------------------------------------------------------------
        Pure("Vector.Dot",       "Dot",       "Vector", { In("A", W), In("B", W), Out("Out", "float", "dot({A}, {B})") }, GVectorTypes);
        Pure("Vector.Length",    "Length",    "Vector", { In("V", W), Out("Out", "float", "length({V})") }, GVectorTypes, "magnitude");
        Pure("Vector.Distance",  "Distance",  "Vector", { In("A", W), In("B", W), Out("Out", "float", "distance({A}, {B})") }, GVectorTypes);
        Pure("Vector.Normalize", "Normalize", "Vector", { In("V", W), Out("Out", W, "normalize({V})") }, GVectorTypes, "unit");
        Pure("Vector.Cross",     "Cross",     "Vector", { In("A", "vec3"), In("B", "vec3"), Out("Out", "vec3", "cross({A}, {B})") });

        Pure("Vector.Split", "Split", "Vector",
             { In("Vector", "vec4"), Out("X", "float", "{Vector}.x"), Out("Y", "float", "{Vector}.y"),
               Out("Z", "float", "{Vector}.z"), Out("W", "float", "{Vector}.w") }, {}, "break components rgba xyzw");
        Pure("Vector.Make2", "Make Vector2", "Vector",
             { In("X", "float"), In("Y", "float"), Out("Out", "vec2", "vec2({X}, {Y})") }, {}, "combine append");
        Pure("Vector.Make3", "Make Vector3", "Vector",
             { In("X", "float"), In("Y", "float"), In("Z", "float"), Out("Out", "vec3", "vec3({X}, {Y}, {Z})") }, {}, "combine append");
        Pure("Vector.Make4", "Make Vector4", "Vector",
             { In("X", "float"), In("Y", "float"), In("Z", "float"), In("W", "float", "1"),
               Out("Out", "vec4", "vec4({X}, {Y}, {Z}, {W})") }, {}, "combine append color rgba");

        // --- Selecting components ---------------------------------------------------
        {
            NodeDefinition& Def = Pure("Vector.ComponentMask", "Component Mask", "Vector",
                                       { In("Vector", W), Out("Out", DERIVED_TYPE, "") }, GFloatTypes,
                                       "mask channel select rgba xyzw");
            Def.Op      = MOP_Mask;
            Def.Tooltip = "The ticked components of the input, in R G B A order: R and B of a vec3 is a vec2 (.xz).";
            for(int Index = 0; Index < 4; ++Index)
            {
                Def.Properties.push_back({ GMaskProperties[Index], EPropertyKind::Bool, GMaskDefaults[Index], {},
                                           std::string("Pass the input's ") + GMaskProperties[Index] + " component through" });
            }
        }
        {
            NodeDefinition& Def = Pure("Vector.Swizzle", "Swizzle", "Vector",
                                       { In("Vector", W), Out("Out", DERIVED_TYPE, "") }, GFloatTypes,
                                       "reorder shuffle repeat rgba xyzw");
            Def.Op      = MOP_Swizzle;
            Def.Tooltip = "Components in any order, repeats allowed: 'zyx' reverses a vec3, 'xxx' spreads x.";
            Def.Properties = { { GSwizzleProperty, EPropertyKind::String, GSwizzleDefault, {},
                                 "1 to 4 of x, y, z, w (or r, g, b, a)" } };
        }

        // --- Logic: real control flow -------------------------------------------------
        {
            NodeDefinition& Def = Pure("Logic.Branch", "Branch", "Logic",
                                       { In("A", "float"), In("B", "float", "0"), In("True", W, "1"), In("False", W, "0"),
                                         Out("Out", W, "") }, GFloatTypes, "if else condition compare select");
            Def.Op      = MOP_Branch;
            Def.Tooltip = "True if A compares so with B, else False -- an if statement, so what only one side needs "
                          "is computed only when that side is taken.";
            Def.Properties = { { "Compare", EPropertyKind::Choice, ">", GCompareChoices, "How A is compared with B" },
                               { "Hint", EPropertyKind::Choice, "None", GBranchHintChoices,
                                 "Branch: really jump, skipping the side not taken. Flatten: run both and select. "
                                 "None: the driver decides. Ignored on OpenGL." } };
        }
        {
            NodeDefinition& Def = Pure("Logic.ForLoop", "For Loop", "Logic",
                                       { In("Count", "float", "4"), In("Initial", W), Out("Loop", LOOP_TYPE, ""),
                                         Out("Index", "float", ""), Out("Value", W, "") }, GFloatTypes, "repeat iterate for");
            Def.Op      = MOP_LoopBegin;
            Def.Tooltip = "Runs what lies between it and its For Loop End Count times. Value starts as Initial and "
                          "becomes Next each time; Index counts 0, 1, 2... Both exist only inside the loop.";
            Def.Properties = { { "Hint", EPropertyKind::Choice, "None", GLoopHintChoices,
                                 "Loop: keep it a loop. Unroll: repeat the body in the code (Count must be constant). "
                                 "None: the driver decides. Ignored on OpenGL." } };
        }
        {
            NodeDefinition& Def = Pure("Logic.ForLoopEnd", "For Loop End", "Logic",
                                       { In("Loop", LOOP_TYPE), In("Next", W), Out("Result", W, "") }, GFloatTypes,
                                       "repeat iterate for result");
            Def.Op                  = MOP_LoopEnd;
            Def.bWildcardFromSchema = true;   // its For Loop's type
            Def.Tooltip = "Closes a For Loop: Next is the loop's Value for the next time round, and Result its value "
                          "after the last.";
        }
    }

    void MaterialSchema::Compile(const Graph& InGraph, CompileResult& OutResult) const
    {
        MaterialCompiler(InGraph, *this, OutResult).Run();
    }
}

std::unique_ptr<Schema> CreateMaterialSchema()
{
    return std::make_unique<MaterialSchema>();
}
}
