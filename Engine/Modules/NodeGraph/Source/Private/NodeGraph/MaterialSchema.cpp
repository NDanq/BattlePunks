#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "Public/NodeGraph/GraphCompiler.h"
#include "Private/NodeGraph/CodeWriter.h"
#include "Private/NodeGraph/GraphIndex.h"

/*
 * Material graphs -> a material function (<Name>.mtl.glsl) that the engine's
 * templates (Engine/Content/Shaders/MaterialTemplate_VS/PS.glsl) include, and
 * <Name>.material describing it. See RenderCore/MATERIALS.md.
 *
 * TWO STAGES. World Position Offset is the vertex stage's only root; every
 * other Material Output pin and every Discard is the pixel stage's. Each stage
 * is compiled on its own, from its roots back, so a node both reach is
 * emitted in both functions. A node whose StageMask lacks a stage it is
 * reached from is an error (CheckStages). Anything more than a name or
 * literal becomes a `<type> tN` temporary.
 *
 * UNIFORMS: one std140 block at binding 2 -- the engine's 160 bytes (tint,
 * clip value, two-sided, and a draw's baked lighting: RenderCore's
 * MaterialDescription.h), then every Parameter by name, at offsets computed
 * here and written to the .material. The renderer fills it byte for byte.
 *
 * TEXTURES: a Texture Object names a texture and its sampler settings; each
 * distinct one takes a material texture slot from 1 (slot 0 is a PostProcess
 * material's scene), declared as Material_Texture<slot>, which its Texture
 * output is. Any number of Texture Samples read one at their own UVs. The
 * .material says which file and sampler fill each slot.
 *
 * CONTROL FLOW. Branch is a real `if`; For Loop / For Loop End a real `for`.
 * Each value is computed in the innermost block all its uses share
 * (AnalyseScopes). Hints are MaterialPlatform.glsl's HINT_* macros, which the
 * templates always include.
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
        MOP_Output,          // the legacy root: Pixel Output's Color (Unlit, Translucent)
        MOP_Mask,            // Component Mask: the ticked components, in order
        MOP_Swizzle,         // Swizzle: any components, any order, repeats allowed
        MOP_Branch,          // an if: True or False, by comparing A with B
        MOP_LoopBegin,       // For Loop: Index and Value, inside its loop only
        MOP_LoopEnd,         // For Loop End: Next each time round, Result after
        MOP_Discard,         // a second kind of root: discard the pixel when A compares so with B
        MOP_MaterialOutput,  // the root: Material Output's surface pins and World Position Offset
        MOP_SceneTexture,    // PostProcess only
        MOP_TextureSample,   // its Texture Object, at UV (the texture coordinate unlinked)
        MOP_TextureObject,   // a texture asset and its sampler: a material texture slot
        MOP_Reflect,         // a scene capture's render target, sampled as a reflection (a slot, as a Texture Object)
        MOP_NormalMap,       // a tangent-space normal map's colour, to a world-space normal (Material_NormalMap)
    };

    /* NodeDefinition::StageMask / PinDefinition::Stage bits. */
    constexpr uint32_t STAGE_VERTEX = 1;
    constexpr uint32_t STAGE_PIXEL  = 2;

    constexpr int UNIFORM_BLOCK_BINDING = 2;       // GDI_MATERIAL_BLOCK_BINDING
    constexpr int ENGINE_BLOCK_BYTES    = 160;     // Material_Tint .. Material_AmbientCube[5]
    constexpr int MAX_BLOCK_BYTES       = 1024;    // GDI_MATERIAL_BLOCK_MAX_BYTES
    const char* const PARAM_PREFIX      = "Param_";
    const char* const TINT_UNIFORM      = "Material_Tint";
    const char* const OFFSET_PIN        = "WorldPositionOffset";
    const char* const LOOP_TYPE         = "loop";  // For Loop -> its For Loop End

    /* Material texture slots: GDI binding 4 + slot (GDI_MATERIAL_TEXTURE_BINDING). */
    constexpr int TEXTURE_BINDING       = 4;
    constexpr int TEXTURE_SLOT_FIRST    = 1;       // slot 0 is Scene Color (MaterialCommon.glsl)
    constexpr int TEXTURE_SLOT_COUNT    = 8;       // GDI_MATERIAL_TEXTURE_SLOTS
    const char* const TEXTURE_PREFIX    = "Material_Texture";
    const char* const TEXTURE_TYPE      = "texture";   // a Texture Object's output: a sampler, not a value
    const char* const TEXTURE_OBJECT    = "Texture.Object";
    const char* const TEXTURE_SAMPLE    = "Texture.Sample";
    const char* const TEXTURE_REFLECT   = "Texture.Reflect";
    const char* const NORMAL_MAP        = "Texture.NormalMap";

    /* A render target path's kind, as RenderCore's RenderTargetAsset.h tells
     * it: true and bOutCube for an .nmrt2d (false) or .nmrt3d (true). */
    bool RenderTargetKind(const std::string& InPath, bool& bOutCube)
    {
        const auto EndsWith = [&InPath](const char* InSuffix)
        {
            const std::string Suffix(InSuffix);
            return InPath.size() > Suffix.size() && InPath.compare(InPath.size() - Suffix.size(), Suffix.size(), Suffix) == 0;
        };
        if(EndsWith(".nmrt2d")) { bOutCube = false; return true; }
        if(EndsWith(".nmrt3d")) { bOutCube = true;  return true; }
        return false;
    }

    /* A Texture Sample's sampler settings; Default keeps the texture asset's own. */
    const std::vector<std::string> GFilterChoices = { "Default", "Nearest", "Linear" };
    const std::vector<std::string> GWrapChoices   = { "Default", "Repeat", "Clamp", "Mirror" };

    const std::vector<std::string> GDomainChoices  = { "Opaque", "Masked", "Translucent", "PostProcess" };
    const std::vector<std::string> GShadingChoices = { "Lit", "Unlit" };

    /* std140 base alignment and size of a float type. */
    int Std140Align(const std::string& InType) { return InType == "float" ? 4 : InType == "vec2" ? 8 : 16; }
    int Std140Size(const std::string& InType)  { return InType == "float" ? 4 : InType == "vec2" ? 8 : InType == "vec3" ? 12 : 16; }

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

    /* A name or member access: safe to swizzle without parentheses. */
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

    /* A Component Mask's or Swizzle's selection as GLSL letters ("xz").
     * Swizzles accept xyzw or rgba, any case. "" if empty or invalid. */
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

    /* The block an input is computed in, if not its node's: a Branch's True
     * (0) / False (1), a For Loop End's Next (0). Otherwise -1 (including
     * ternary Branches). */
    bool IsTernary(const Node& InNode) { return InNode.GetProperty("Ternary", "false") == "true"; }

    int ArmOf(const Node& InNode, const NodeDefinition& InDef, const std::string& InPin)
    {
        if(InDef.Op == MOP_Branch && !IsTernary(InNode))
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

        void UpgradeGraph(Graph& InOutGraph, std::vector<std::string>& OutWarnings) const override;

        std::string GetStageName(const uint32_t InStageBit) const override
        {
            return InStageBit == STAGE_VERTEX ? "vertex" : InStageBit == STAGE_PIXEL ? "pixel" : std::string();
        }

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

        /* A For Loop and its End share one type: the widest suggested by
         * either's links, unless set in Details. */
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
                return GetWildcardType(InGraph, InNode) + Dot + "A " + InNode.GetProperty("Compare", ">") + " B"
                       + (IsTernary(InNode) ? Dot + "? :" : Hint());
            }
            if(Def->Op == MOP_LoopBegin)
            {
                const std::string* Count = InNode.FindConstant("Count");
                const std::string Times = NodeInto(InGraph, InNode, "Count") ? std::string("Count") : Count ? *Count : std::string("4");
                return GetWildcardType(InGraph, InNode) + Dot + Times + " times" + Hint();
            }
            if(Def->Op == MOP_Discard) { return "if A " + InNode.GetProperty("Compare", "<") + " B"; }
            if(Def->Op == MOP_TextureObject)
            {
                /* The overridden sampler settings: "Nearest Â· Clamp". */
                std::string Settings;
                for(const char* Name : { "Filter", "Wrap" })
                {
                    const std::string Value = InNode.GetProperty(Name, "Default");
                    if(Value != "Default") { Settings += (Settings.empty() ? std::string() : Dot) + Value; }
                }
                return InNode.GetProperty("Texture", "").empty() ? std::string("no texture") : Settings;
            }
            if(Def->Op != MOP_Mask && Def->Op != MOP_Swizzle) { return std::string(); }

            const std::string Selected = SelectedComponents(InNode, Def->Op);
            if(Selected.empty()) { return Def->Op == MOP_Mask ? "nothing selected" : "invalid"; }
            return "." + Selected + " \xE2\x86\x92 " + VectorType(Selected.size());   // U+2192
        }
    };

    /*
     * Texture Sample version 1 named its texture and sampler itself; version 2
     * reads them from a Texture Object. Each old sample gets one, placed to its
     * left and linked into its Texture -- one per distinct texture and settings,
     * shared by the samples that named them. Its id derives from the first such
     * sample's, so reading the same file twice gives the same graph (and slots).
     */
    void MaterialSchema::UpgradeGraph(Graph& InOutGraph, std::vector<std::string>& OutWarnings) const
    {
        std::vector<Node*> Samples;
        for(Node& Each : InOutGraph.Nodes)
        {
            if(Each.Type == TEXTURE_SAMPLE && Each.Version < 2) { Samples.push_back(&Each); }
        }
        if(Samples.empty()) { return; }
        std::sort(Samples.begin(), Samples.end(), [](const Node* InA, const Node* InB) { return InA->Id < InB->Id; });

        /* Each hex digit of InId flipped: stable, and nothing a GUID generator makes on purpose. */
        const auto Derived = [](const NodeGuid& InId)
        {
            std::string Text = GuidToString(InId);
            for(char& Ch : Text)
            {
                const char* Digits = "0123456789abcdef";
                const char* At = std::strchr(Digits, std::tolower(static_cast<unsigned char>(Ch)));
                if(At && *At) { Ch = Digits[(At - Digits) ^ 0xA]; }
            }
            NodeGuid Out;
            return GuidFromString(Text, Out) ? Out : NodeGuid::Generate();
        };

        std::map<std::string, NodeGuid> Objects;   // texture, filter, wrap -> its Texture Object
        std::vector<Node> Added;
        for(Node* Sample : Samples)
        {
            const std::string Path   = Sample->GetProperty("Texture", "");
            const std::string Filter = Sample->GetProperty("Filter", "Default");
            const std::string Wrap   = Sample->GetProperty("Wrap", "Default");
            Sample->Properties.erase("Texture");
            Sample->Properties.erase("Filter");
            Sample->Properties.erase("Wrap");
            Sample->Version = 2;

            const std::string Key = Path + "\n" + Filter + "\n" + Wrap;
            auto Found = Objects.find(Key);
            if(Found == Objects.end())
            {
                Node Object;
                Object.Id      = Derived(Sample->Id);
                Object.Type    = TEXTURE_OBJECT;
                Object.Version = 1;
                Object.X       = Sample->X - 260.0f;
                Object.Y       = Sample->Y;
                if(!Path.empty())        { Object.Properties["Texture"] = Path; }
                if(Filter != "Default")  { Object.Properties["Filter"]  = Filter; }
                if(Wrap != "Default")    { Object.Properties["Wrap"]    = Wrap; }
                Found = Objects.emplace(Key, Object.Id).first;
                Added.push_back(std::move(Object));
            }
            InOutGraph.Links.push_back({ PinRef{ Found->second, "Texture" }, PinRef{ Sample->Id, "Texture" } });
        }
        for(Node& Each : Added) { InOutGraph.Nodes.push_back(std::move(Each)); }   // after: Samples point into Nodes

        OutWarnings.push_back(std::to_string(Samples.size()) + " Texture Sample node(s) from an older editor now read "
                              + std::to_string(Added.size()) + " Texture Object node(s); save the graph to keep it so");
    }

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
            int         Offset = 0;   // bytes into the material block
            std::string Default;
        };

        /* A material texture slot: one texture with one set of sampler settings. */
        struct TextureSlot
        {
            int         Slot = 0;
            std::string Path;
            std::string Filter = "Default";
            std::string Wrap   = "Default";
            NodeGuid    Node;     // the first Texture Object to name it
        };

        /* Enclosing blocks, outermost first: (Branch, 0) is its True side,
         * (For Loop End, 0) the loop body. Empty is the function block. */
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
        void CollectTextures();
        std::string EmitTextureSample(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin);

        /* TEXTURE STREAMING: how a UV relates to the mesh's UV0, as far as
         * the graph says -- Texture Coordinate, times constants and
         * Parameters, Multiply by Multiply. False: anything else. */
        struct UVScale
        {
            double                   Constant = 1.0;
            std::vector<std::string> Params;
        };
        bool ScaleOfUV(const PinRef& InFrom, UVScale& InOutScale, int InDepth) const;
        void NoteSampleUV(const Node& InNode, int InSlot);
        std::string EmitReflect(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin);
        void CheckStages(const Node& InRoot, const NodeDefinition& InRootDef);
        void BeginStage(const std::vector<const Node*>& InRoots, const std::set<std::string>& InRootPins);
        void AnalyseScopes(const std::vector<const Node*>& InRoots);
        void ValidateLoops();
        bool IsRootPinSkipped(const Node& InNode, const PinDefinition& InPin) const;
        bool IsPinSet(const Node& InNode, const PinDefinition& InPin) const;

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
        std::vector<TextureSlot>         Textures;     // by slot
        std::map<NodeGuid, int>          TextureSlotOf;   // each Texture Object's slot
        /* Each slot's samples' UV scales (NoteSampleUV); a slot with any
         * sample it could not follow is in UVUnknown: no line for it. */
        std::map<int, std::vector<UVScale>> TextureUVs;
        std::set<int>                    UVUnknown;
        bool                             bVertexStage = false;   // textureLod: no derivatives there
        int                              BlockBytes = ENGINE_BLOCK_BYTES;
        int                              NextTemp = 0;

        std::string                      Domain  = "Opaque";
        std::string                      Shading = "Lit";

        /* The stage being compiled: its Output node, and the pins of it that
         * are this stage's roots (AnalyseScopes looks only through those). */
        const Node*                      RootNode = nullptr;
        std::set<std::string>            RootPins;

        std::map<NodeGuid, Scope>        Scopes;          // every node the output reaches; empty after a cycle
        std::map<NodeGuid, OpenLoop>     OpenLoops;       // by For Loop, while its body is written
        std::vector<std::vector<PinRef>> Blocks;          // the values cached in each open block
        int                              NextLoop = 0;
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
            if(!Def || (Def->Op != MOP_Output && Def->Op != MOP_MaterialOutput)) { continue; }
            if(Root)
            {
                Result.Error(Each->Id, std::string(), "a material has one output: this '" + Def->DisplayName
                             + "' and a '" + RootDef->DisplayName + "' are both here");
                continue;
            }
            Root = Each;
            RootDef = Def;
        }
        if(!Root)
        {
            Result.Error(NodeGuid(), std::string(), "a material needs a Material Output node");
            return;
        }

        /* The legacy Pixel Output is Unlit and Translucent: how every material drew before. */
        const bool bLegacy = RootDef->Op == MOP_Output;
        Domain  = bLegacy ? "Translucent" : Root->GetProperty("Domain", "Opaque");
        Shading = bLegacy ? "Unlit" : Root->GetProperty("Shading", "Lit");

        double ClipValue = 0.5, MaxOffset = 0.0;
        if(!bLegacy)
        {
            if(!ParseNumber(Root->GetProperty("ClipValue", "0.5"), ClipValue))
            {
                Result.Error(Root->Id, std::string(), "Clip Value must be a number");
            }
            if(!ParseNumber(Root->GetProperty("MaxOffset", "0"), MaxOffset) || MaxOffset < 0.0)
            {
                Result.Error(Root->Id, std::string(), "Max Offset must be a number, 0 or more");
            }
        }
        const bool bTwoSided = !bLegacy && Root->GetProperty("TwoSided", "false") == "true";

        const PinDefinition* OffsetPin = RootDef->FindPin(OFFSET_PIN, EPinDirection::Input);
        const bool bHasOffset = OffsetPin && IsPinSet(*Root, *OffsetPin);

        if(Domain == "PostProcess" && Shading == "Lit")
        {
            Result.Error(Root->Id, std::string(), "a PostProcess material is Unlit: set Shading to Unlit");
        }
        if(Domain == "PostProcess" && bHasOffset)
        {
            Result.Error(Root->Id, OFFSET_PIN, "a PostProcess material draws a full-screen pass: it has no World Position Offset");
        }
        if(!bLegacy)
        {
            /* Pins this domain or shading never reads: a link to one is a mistake worth a word. */
            for(const PinDefinition& Pin : RootDef->Pins)
            {
                if(!Pin.IsInput() || !Index.LinkInto(Root->Id, Pin.Name)) { continue; }
                const bool bLitOnly = Pin.Name == "BaseColor" || Pin.Name == "Metallic" || Pin.Name == "Roughness"
                                   || Pin.Name == "Specular" || Pin.Name == "Normal" || Pin.Name == "AmbientOcclusion";
                std::string Why;
                if(bLitOnly && Shading == "Unlit")                       { Why = "an Unlit material"; }
                if(Pin.Name == "Opacity" && Domain != "Translucent")    { Why = "a " + Domain + " material (only Translucent blends)"; }
                if(Pin.Name == "OpacityMask" && Domain != "Masked")     { Why = "a " + Domain + " material (only Masked clips)"; }
                if(!Why.empty()) { Result.Warning(Root->Id, Pin.Name, Pin.Name + " is unused by " + Why); }
            }
        }

        CollectParameters();
        CollectTextures();
        CheckStages(*Root, *RootDef);
        if(Result.HasErrors()) { return; }

        // --- Pixel stage: the Discards, in GUID order, then the surface --------
        std::vector<const Node*> PixelRoots;
        for(const Node* Each : Index.SortedNodes())
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            if(Def && Def->Op == MOP_Discard) { PixelRoots.push_back(Each); }
        }
        PixelRoots.push_back(Root);

        std::set<std::string> PixelPins;
        for(const PinDefinition& Pin : RootDef->Pins)
        {
            if(Pin.IsInput() && Pin.Stage == STAGE_PIXEL) { PixelPins.insert(Pin.Name); }
        }

        bVertexStage = false;
        BeginStage(PixelRoots, PixelPins);
        if(Result.HasErrors()) { return; }

        for(const Node* Each : PixelRoots)
        {
            if(Each == Root) { continue; }
            const NodeDefinition& Def = *Index.FindDefinition(*Each);
            const std::string A = Input(*Each, Def, *Def.FindPin("A", EPinDirection::Input));
            const std::string B = Input(*Each, Def, *Def.FindPin("B", EPinDirection::Input));
            Body.Line("if(" + A + " " + Each->GetProperty("Compare", "<") + " " + B + ") { discard; }", Each->Id);
        }

        if(bLegacy)
        {
            std::string Color = Input(*Root, *RootDef, *RootDef->FindPin("Color", EPinDirection::Input));
            if(!IsName(Color))
            {
                const std::string Temp = "t" + std::to_string(NextTemp++);
                Body.Line("vec4 " + Temp + " = " + Color + ";", Root->Id);
                Color = Temp;
            }
            Body.Line("Surface.Emissive = " + Color + ".rgb;", Root->Id);
            Body.Line("Surface.Opacity = " + Color + ".a;", Root->Id);
        }
        else
        {
            /* Only what is linked or set: the rest keeps DefaultMaterialSurface's value. */
            for(const PinDefinition& Pin : RootDef->Pins)
            {
                if(!Pin.IsInput() || Pin.Stage != STAGE_PIXEL || !IsPinSet(*Root, Pin)) { continue; }
                const std::string Value = Input(*Root, *RootDef, Pin);
                Body.Line("Surface." + Pin.Name + " = " + Value + ";", Root->Id);
            }
        }
        if(Result.HasErrors()) { return; }
        const CodeWriter PixelBody = Body;

        // --- Vertex stage: World Position Offset ------------------------------
        CodeWriter VertexBody;
        if(bHasOffset)
        {
            bVertexStage = true;
            BeginStage({ Root }, { OFFSET_PIN });
            if(Result.HasErrors()) { return; }
            const std::string Offset = Input(*Root, *RootDef, *OffsetPin);
            Body.Line("return " + Offset + ";", Root->Id);
            if(Result.HasErrors()) { return; }
            VertexBody = Body;
        }

        // --- The file -----------------------------------------------------------
        const std::string Name      = G.Name.empty() ? std::string("Material") : G.Name;
        const std::string BlockName = SanitizeIdentifier(Name) + "_Block";

        CodeWriter Out;
        Out.Line("// Generated by NimbleEditor from the material graph \"" + G.Name + "\". Do not edit.");
        Out.Line("// " + Domain + ", " + Shading + ". Included by Engine/Content/Shaders/MaterialTemplate_VS/PS.glsl.");
        Out.Blank();

        /* Declared in both stages that include it, so both see the same block. */
        Out.Line("// Bytes 0-159 are the engine's; the parameters' offsets are in " + Name + ".material.");
        Out.Line("UBO_BLOCK_BEGIN(" + std::to_string(UNIFORM_BLOCK_BINDING) + ", " + BlockName + ")");
        Out.Indent();
        Out.Line("vec4  Material_Tint;");
        Out.Line("float Material_ClipValue;");
        Out.Line("float Material_TwoSided;");
        Out.Line("float Material_Reserved0;");
        Out.Line("float Material_Reserved1;");
        Out.Line("vec4  Material_LightmapScaleOffset;");
        Out.Line("vec4  Material_BakedLighting;");
        Out.Line("vec4  Material_AmbientCube[6];");
        for(const auto& Each : Parameters)
        {
            Out.Line(Each.second.Type + std::string(Each.second.Type == "float" ? " " : "  ") + PARAM_PREFIX + Each.first + ";",
                     Each.second.Node);
        }
        Out.Outdent();
        Out.Line("UBO_BLOCK_END(" + BlockName + ")");

        if(!Textures.empty())
        {
            Out.Blank();
            Out.Line("// Textures, by material texture slot; " + Name + ".material names each one's file and sampler.");
            for(const TextureSlot& Each : Textures)
            {
                Out.Line("MATERIAL_SAMPLER2D(" + std::to_string(TEXTURE_BINDING + Each.Slot) + ", " + TEXTURE_PREFIX
                         + std::to_string(Each.Slot) + ")", Each.Node);
            }
        }

        if(bHasOffset)
        {
            Out.Blank();
            Out.Line("#if VERTEX_SHADER");
            Out.Line("vec3 CalcMaterialOffset(in MaterialInputs Inputs)");
            Out.Line("{");
            Out.Indent();
            Out.Append(VertexBody);
            Out.Outdent();
            Out.Line("}");
            Out.Line("#endif");
        }

        Out.Blank();
        Out.Line("#if PIXEL_SHADER");
        Out.Line("void CalcMaterial(in MaterialInputs Inputs, inout MaterialSurface Surface)");
        Out.Line("{");
        Out.Indent();
        Out.Append(PixelBody);
        Out.Outdent();
        Out.Line("}");
        Out.Line("#endif");

        Result.Code      = std::move(Out.Text);
        Result.LineNodes = std::move(Out.LineNodes);

        /*
         * The companion: what the renderer loads beside the shader.
         *
         *     NimbleMaterial 3
         *     Domain <Opaque|Masked|Translucent|PostProcess>
         *     Shading <Lit|Unlit>
         *     TwoSided <0|1>
         *     ClipValue <x>
         *     MaxOffset <x>
         *     HasOffset <0|1>
         *     BlockSize <bytes>
         *     Param <Name> <Type> <Offset> <x> <y> <z> <w>
         *     Texture <Slot> <Default|Nearest|Linear> <Default|Repeat|Clamp|Mirror> <Path>
         *     TextureUV <Slot> <x> [<Param> ...]
         *
         * '#' starts a comment. Unused components are 0. A Texture's path is
         * the rest of its line (it may hold spaces); Default keeps the
         * texture asset's own filter or wrap. TextureUV (texture streaming):
         * a sample of the slot reads UV0 times x times each named parameter's
         * largest component; one line per sample, none for a slot whose UV
         * the compiler could not follow, or that is UV0 unscaled.
         */
        std::string Material = "# Generated by NimbleEditor from the material graph \"" + G.Name + "\". Do not edit.\n"
                               "NimbleMaterial 3\n";
        Material += "Domain " + Domain + "\n";
        Material += "Shading " + Shading + "\n";
        Material += std::string("TwoSided ") + (bTwoSided ? "1" : "0") + "\n";
        Material += "ClipValue " + FormatNumber(ClipValue) + "\n";
        Material += "MaxOffset " + FormatNumber(MaxOffset) + "\n";
        Material += std::string("HasOffset ") + (bHasOffset ? "1" : "0") + "\n";
        Material += "BlockSize " + std::to_string(BlockBytes) + "\n";
        for(const auto& Each : Parameters)
        {
            std::vector<double> Values;
            ParseNumbers(Each.second.Default, Components(Each.second.Type), Values);
            Values.resize(4, 0.0);
            Material += "Param " + Each.first + " " + Each.second.Type + " " + std::to_string(Each.second.Offset);
            for(const double Value : Values) { Material += " " + FormatNumber(float(Value)); }
            Material += "\n";
        }
        for(const TextureSlot& Each : Textures)
        {
            Material += "Texture " + std::to_string(Each.Slot) + " " + Each.Filter + " " + Each.Wrap + " " + Each.Path + "\n";
        }
        for(const auto& [Slot, Scales] : TextureUVs)
        {
            const bool bPlain = std::all_of(Scales.begin(), Scales.end(), [](const UVScale& InScale)
            {
                return InScale.Constant == 1.0 && InScale.Params.empty();
            });
            if(UVUnknown.count(Slot) || bPlain) { continue; }
            std::set<std::string> Written;   // a sample emitted in both stages is one line
            for(const UVScale& Scale : Scales)
            {
                std::string Line = "TextureUV " + std::to_string(Slot) + " " + FormatNumber(Scale.Constant);
                for(const std::string& Param : Scale.Params) { Line += " " + Param; }
                if(Written.insert(Line).second) { Material += Line + "\n"; }
            }
        }
        Result.Companions.push_back({ ".material", Material });
    }

    /* Whether an Output pin says anything: linked, or given a constant other
     * than its default. An unset pin keeps the template's default. */
    bool MaterialCompiler::IsPinSet(const Node& InNode, const PinDefinition& InPin) const
    {
        if(Index.LinkInto(InNode.Id, InPin.Name)) { return true; }
        const std::string* Given = InNode.FindConstant(InPin.Name);
        if(!Given) { return false; }

        std::vector<double> A, B;
        const size_t Count = Components(InPin.Type);
        return !(ParseNumbers(*Given, Count, A) && ParseNumbers(InPin.Default, Count, B) && A == B);
    }

    /* AnalyseScopes and the Hoist walk look into the Output node only through
     * the current stage's pins. */
    bool MaterialCompiler::IsRootPinSkipped(const Node& InNode, const PinDefinition& InPin) const
    {
        return &InNode == RootNode && !RootPins.count(InPin.Name);
    }

    /* Starts a stage afresh: its own temporaries, scopes and blocks. */
    void MaterialCompiler::BeginStage(const std::vector<const Node*>& InRoots, const std::set<std::string>& InRootPins)
    {
        Body = CodeWriter();
        Values.clear();
        InProgress.clear();
        Scopes.clear();
        OpenLoops.clear();
        Blocks.clear();
        NextTemp = 0;
        NextLoop = 0;

        RootNode = InRoots.empty() ? nullptr : InRoots.back();
        RootPins = InRootPins;

        AnalyseScopes(InRoots);
        ValidateLoops();
    }

    /*
     * Every node a stage's root pins reach must have that stage in its
     * StageMask. Reported once per node, on the node, naming the root pin --
     * the editor refuses such links (Schema::CanLinkStages), but a file can
     * still hold one.
     */
    void MaterialCompiler::CheckStages(const Node& InRoot, const NodeDefinition& InRootDef)
    {
        std::set<NodeGuid> Reported;
        for(const PinDefinition& RootPin : InRootDef.Pins)
        {
            if(!RootPin.IsInput() || RootPin.Stage == 0) { continue; }

            std::set<NodeGuid> Seen;
            const std::function<void(const Node&, const PinDefinition&)> Walk = [&](const Node& InNode, const PinDefinition& InPin)
            {
                const Link* Into = Index.LinkInto(InNode.Id, InPin.Name);
                const Node* Source = Into ? Index.FindNode(Into->From.Node) : nullptr;
                const NodeDefinition* SourceDef = Source ? Index.FindDefinition(*Source) : nullptr;
                if(!SourceDef || !Seen.insert(Source->Id).second) { return; }

                if(!(SourceDef->StageMask & RootPin.Stage) && Reported.insert(Source->Id).second)
                {
                    const std::string Stage = RootPin.Stage == STAGE_VERTEX ? "vertex" : "pixel";
                    Result.Error(Source->Id, std::string(), "'" + SourceDef->DisplayName + "' is not available in the " + Stage
                                 + " stage, and it feeds " + RootPin.Name);
                }
                for(const PinDefinition& Pin : SourceDef->Pins)
                {
                    if(Pin.IsInput()) { Walk(*Source, Pin); }
                }
            };
            Walk(InRoot, RootPin);
        }
    }

    /* Every Parameter, used or not, gets a slot by name, so slots are stable. */
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

        /* std140, in name order: each at its base alignment after the engine's bytes. */
        int Offset = ENGINE_BLOCK_BYTES;
        for(auto& Each : Parameters)
        {
            const int Align = Std140Align(Each.second.Type);
            Offset = (Offset + Align - 1) / Align * Align;
            Each.second.Offset = Offset;
            Offset += Std140Size(Each.second.Type);
        }
        BlockBytes = (Offset + 15) / 16 * 16;

        if(BlockBytes > MAX_BLOCK_BYTES)
        {
            Result.Error(NodeGuid(), std::string(), "the material's parameters take " + std::to_string(BlockBytes)
                         + " bytes; a material block holds at most " + std::to_string(MAX_BLOCK_BYTES));
        }
    }

    /*
     * Every Texture Object, used or not, in GUID order, so slots are stable:
     * one slot per distinct texture and sampler settings, from slot 1 -- two
     * Objects naming the same share it.
     */
    void MaterialCompiler::CollectTextures()
    {
        for(const Node* Each : Index.SortedNodes())
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            if(!Def || (Def->Op != MOP_TextureObject && Def->Op != MOP_Reflect)) { continue; }

            TextureSlot Wanted;
            Wanted.Path   = Each->GetProperty(Def->Op == MOP_Reflect ? "Target" : "Texture", "");
            bool bCube = false;
            if(Def->Op == MOP_Reflect && !Wanted.Path.empty() && !RenderTargetKind(Wanted.Path, bCube))
            {
                Result.Error(Each->Id, std::string(), "'" + Def->DisplayName + "' reflects a scene capture: its Target must be a "
                             "RenderTargetTex2D (.nmrt2d, a planar mirror) or RenderTargetTex3D (.nmrt3d, omnidirectional)");
                continue;
            }
            Wanted.Filter = Each->GetProperty("Filter", "Default");
            Wanted.Wrap   = Each->GetProperty("Wrap", "Default");
            Wanted.Node   = Each->Id;
            if(Wanted.Path.empty())
            {
                Result.Error(Each->Id, std::string(), Def->Op == MOP_Reflect
                             ? "'" + Def->DisplayName + "' has no Target: choose an .nmrt2d or .nmrt3d in its Details"
                             : "'" + Def->DisplayName + "' has no texture: choose a .nmtexture in its Details");
                continue;
            }

            const auto Same = std::find_if(Textures.begin(), Textures.end(), [&](const TextureSlot& InOther)
            {
                return InOther.Path == Wanted.Path && InOther.Filter == Wanted.Filter && InOther.Wrap == Wanted.Wrap;
            });
            if(Same != Textures.end())
            {
                TextureSlotOf[Each->Id] = Same->Slot;
                continue;
            }

            /* A PostProcess material's last slot is the scene depth (Scene Depth). */
            const int SlotCount = Domain == "PostProcess" ? TEXTURE_SLOT_COUNT - 1 : TEXTURE_SLOT_COUNT;
            Wanted.Slot = TEXTURE_SLOT_FIRST + int(Textures.size());
            if(Wanted.Slot >= SlotCount)
            {
                Result.Error(Each->Id, std::string(), "a " + Domain + " material samples at most " + std::to_string(SlotCount - TEXTURE_SLOT_FIRST)
                             + " textures (each distinct texture and sampler setting is one); this would be one more");
                continue;
            }
            TextureSlotOf[Each->Id] = Wanted.Slot;
            Textures.push_back(Wanted);
        }
    }

    /*
     * One sample per node per block, as a vec4 temporary; RGB, R, G, B and A
     * swizzle it. The sampler is its Texture input's -- a Texture Object's
     * slot. The vertex stage has no derivatives, so it reads level 0.
     */
    std::string MaterialCompiler::EmitTextureSample(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin)
    {
        const PinRef Whole{ InNode.Id, "RGBA" };
        std::string Sample;
        const auto Cached = Values.find(Whole);
        if(Cached != Values.end())
        {
            Sample = Cached->second;
        }
        else
        {
            const PinDefinition* TexturePin = InDef.FindPin("Texture", EPinDirection::Input);
            const std::string Sampler = TexturePin && Index.LinkInto(InNode.Id, "Texture") ? Input(InNode, InDef, *TexturePin)
                                                                                          : std::string();
            if(!TexturePin || !Index.LinkInto(InNode.Id, "Texture"))
            {
                Result.Error(InNode.Id, "Texture", "'" + InDef.DisplayName + "' samples a Texture Object: link one into its Texture");
                Sample = "vec4(0.0)";
            }
            else if(Sampler.empty())
            {
                Sample = "vec4(0.0)";   // its Texture Object has no slot: CollectTextures reported why
            }
            else
            {
                const PinDefinition* UVPin = InDef.FindPin("UV", EPinDirection::Input);
                const bool bGiven = UVPin && (Index.LinkInto(InNode.Id, "UV") || InNode.FindConstant("UV"));
                const std::string UV = bGiven ? Input(InNode, InDef, *UVPin) : std::string("Inputs.TexCoord");
                if(const Link* Texture = Index.LinkInto(InNode.Id, "Texture"))
                {
                    const auto Slot = TextureSlotOf.find(Texture->From.Node);
                    if(Slot != TextureSlotOf.end()) { NoteSampleUV(InNode, Slot->second); }
                }
                Sample = "t" + std::to_string(NextTemp++);
                Body.Line("vec4 " + Sample + " = " + (bVertexStage ? "textureLod(" + Sampler + ", " + UV + ", 0.0)"
                                                                   : "texture(" + Sampler + ", " + UV + ")") + ";", InNode.Id);
            }
            if(InPin.Name != Whole.Pin)
            {
                Values.emplace(Whole, Sample);
                if(!Blocks.empty()) { Blocks.back().push_back(Whole); }
            }
        }

        if(InPin.Name == "RGBA") { return Sample; }
        if(InPin.Name == "RGB")  { return Swizzle(Sample, "rgb"); }
        const std::string Channel(1, char(std::tolower(static_cast<unsigned char>(InPin.Name[0]))));
        return Swizzle(Sample, Channel.c_str());
    }

    bool MaterialCompiler::ScaleOfUV(const PinRef& InFrom, UVScale& InOutScale, const int InDepth) const
    {
        const Node* From = Index.FindNode(InFrom.Node);
        if(!From || InDepth > 8) { return false; }
        if(From->Type == "Input.TexCoord") { return true; }
        if(From->Type != "Math.Multiply") { return false; }

        /* One side the UV, the other a factor: a Parameter, or a constant. */
        bool bUV = false;
        for(const char* Side : { "A", "B" })
        {
            const Link* Into = Index.LinkInto(From->Id, Side);
            if(Into)
            {
                const Node* Other = Index.FindNode(Into->From.Node);
                if(Other && Other->Type == "Parameter")
                {
                    InOutScale.Params.push_back(Other->GetProperty("Name"));
                    continue;
                }
                if(bUV || !ScaleOfUV(Into->From, InOutScale, InDepth + 1)) { return false; }
                bUV = true;
                continue;
            }
            const std::string* Given = From->FindConstant(Side);
            std::istringstream Numbers(Given ? *Given : std::string(Side[0] == 'B' ? "1" : "0"));
            double Largest = 0.0, Each = 0.0;
            while(Numbers >> Each) { Largest = std::max(Largest, std::abs(Each)); }
            InOutScale.Constant *= Largest;
        }
        return bUV;
    }

    void MaterialCompiler::NoteSampleUV(const Node& InNode, const int InSlot)
    {
        const Link* UV = Index.LinkInto(InNode.Id, "UV");
        if(!UV)
        {
            if(InNode.FindConstant("UV")) { UVUnknown.insert(InSlot); }   // one fixed texel: no scale to speak of
            else                          { TextureUVs[InSlot].push_back(UVScale()); }
            return;
        }
        UVScale Scale;
        if(ScaleOfUV(UV->From, Scale, 0)) { TextureUVs[InSlot].push_back(Scale); }
        else                              { UVUnknown.insert(InSlot); }
    }

    /*
     * Reflect: one sample per node per block, as Texture Sample's. A planar
     * mirror's target (.nmrt2d) at the mirrored screen point, nudged by
     * Distortion; an omnidirectional one (.nmrt3d) along the view reflected
     * about Normal (the pixel's normal unlinked). MaterialCommon.glsl.
     */
    std::string MaterialCompiler::EmitReflect(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin)
    {
        const PinRef Whole{ InNode.Id, "RGBA" };
        std::string Sample;
        const auto Cached = Values.find(Whole);
        if(Cached != Values.end())
        {
            Sample = Cached->second;
        }
        else
        {
            const auto Slot = TextureSlotOf.find(InNode.Id);
            bool bCube = false;
            RenderTargetKind(InNode.GetProperty("Target", ""), bCube);
            if(Slot == TextureSlotOf.end())
            {
                Sample = "vec4(0.0)";   // CollectTextures reported why
            }
            else
            {
                const std::string Sampler = TEXTURE_PREFIX + std::to_string(Slot->second);
                Sample = "t" + std::to_string(NextTemp++);
                if(bCube)
                {
                    const PinDefinition* NormalPin = InDef.FindPin("Normal", EPinDirection::Input);
                    const bool bGiven = NormalPin && (Index.LinkInto(InNode.Id, "Normal") || InNode.FindConstant("Normal"));
                    const std::string Normal = bGiven ? Input(InNode, InDef, *NormalPin) : std::string("Inputs.WorldNormal");
                    Body.Line("vec4 " + Sample + " = Material_SampleCubeStrip(" + Sampler + ", Material_ReflectionVector(Inputs, "
                              + Normal + "));", InNode.Id);
                }
                else
                {
                    const PinDefinition* DistortionPin = InDef.FindPin("Distortion", EPinDirection::Input);
                    const bool bGiven = DistortionPin && (Index.LinkInto(InNode.Id, "Distortion") || InNode.FindConstant("Distortion"));
                    const std::string Offset = bGiven ? " + " + Input(InNode, InDef, *DistortionPin) : std::string();
                    Body.Line("vec4 " + Sample + " = texture(" + Sampler + ", Material_MirrorUV(Inputs)" + Offset + ");", InNode.Id);
                }
            }
            if(InPin.Name != Whole.Pin)
            {
                Values.emplace(Whole, Sample);
                if(!Blocks.empty()) { Blocks.back().push_back(Whole); }
            }
        }
        return InPin.Name == "RGB" ? Swizzle(Sample, "rgb") : Sample;
    }

    /*
     * Each reachable node's scope: the innermost block all its uses share.
     * Visited in reverse post-order from the roots, so a node's scope is final
     * before its inputs. A cycle leaves Scopes empty, for Output to report.
     */
    void MaterialCompiler::AnalyseScopes(const std::vector<const Node*>& InRoots)
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
                    if(IsRootPinSkipped(InNode, Pin)) { continue; }
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
        for(const Node* Root : InRoots) { if(!State.count(Root->Id)) { Visit(*Root); } }
        if(bCycle) { return; }

        for(const Node* Root : InRoots) { Scopes[Root->Id] = Scope(); }   // every root is the function's own block
        for(auto It = Order.rbegin(); It != Order.rend(); ++It)
        {
            const Node& Each = **It;
            const NodeDefinition* Def = Index.FindDefinition(Each);
            if(!Def) { continue; }

            const Scope Own = Scopes[Each.Id];
            for(const PinDefinition& Pin : Def->Pins)
            {
                if(IsRootPinSkipped(Each, Pin)) { continue; }
                const Link* Into = Pin.IsInput() ? Index.LinkInto(Each.Id, Pin.Name) : nullptr;
                if(!Into || !Index.FindNode(Into->From.Node)) { continue; }

                Scope Use = Own;
                const int Arm = ArmOf(Each, *Def, Pin.Name);
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

    /* A For Loop's outputs are body-only and it needs exactly one End;
     * violations are reported here, before codegen. */
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
                const int Arm = ArmOf(*N, *Def, Pin.Name);
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
                /* Its member of the material block. */
                Value = PARAM_PREFIX + InNode.GetProperty("Name", "Param");
                break;

            case MOP_TextureSample:
                Value = EmitTextureSample(InNode, InDef, InPin);
                break;

            case MOP_Reflect:
                Value = EmitReflect(InNode, InDef, InPin);
                break;

            case MOP_NormalMap:
            {
                /* The map's colour about a tangent frame from derivatives of the position and UV
                 * (MaterialCommon.glsl): UV must be what the map was sampled at. */
                const PinDefinition* ColorPin    = InDef.FindPin("Color", EPinDirection::Input);
                const PinDefinition* StrengthPin = InDef.FindPin("Strength", EPinDirection::Input);
                const PinDefinition* UVPin       = InDef.FindPin("UV", EPinDirection::Input);
                const bool bUV = UVPin && (Index.LinkInto(InNode.Id, "UV") || InNode.FindConstant("UV"));
                const std::string Color    = Input(InNode, InDef, *ColorPin);
                const std::string Strength = Input(InNode, InDef, *StrengthPin);
                const std::string UV       = bUV ? Input(InNode, InDef, *UVPin) : std::string("Inputs.TexCoord");
                const bool bDirectX = InNode.GetProperty("Format", "OpenGL") == "DirectX";
                const std::string Temp = "t" + std::to_string(NextTemp++);
                Body.Line("vec3 " + Temp + " = Material_NormalMap(Inputs, " + Color + ", " + Strength + ", " + UV + ", "
                          + (bDirectX ? "1.0" : "-1.0") + ");", InNode.Id);
                Value = Temp;
                break;
            }

            case MOP_TextureObject:
            {
                /* Its slot's sampler, which a Texture Sample reads; and its size in texels. */
                const auto Slot = TextureSlotOf.find(InNode.Id);
                const std::string Sampler = Slot == TextureSlotOf.end() ? std::string() : TEXTURE_PREFIX + std::to_string(Slot->second);
                if(InPin.Name == "Size")
                {
                    Value = Sampler.empty() ? std::string("vec2(1.0)") : "vec2(textureSize(" + Sampler + ", 0))";
                }
                else
                {
                    Value = Sampler;   // "" reported by CollectTextures
                }
                break;
            }

            case MOP_SceneTexture:
                if(Domain != "PostProcess")
                {
                    Result.Error(InNode.Id, InPin.Name, "'" + InDef.DisplayName + "' is the scene a PostProcess material "
                                 "draws over; this material is " + Domain);
                }
                Value = InPin.Expression;
                break;

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
                /* Backstop for what ValidateLoops could not analyse. */
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

                /* Computed values get a temporary; bare names are inlined. */
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
     *
     * or, set to Ternary, `vec3 t3 = t1 > 0.5 ? t2 : t0;` -- both sides
     * computed first, then one chosen.
     */
    std::string MaterialCompiler::EmitBranch(const Node& InNode, const NodeDefinition& InDef, const std::string& InType)
    {
        const std::string A = Input(InNode, InDef, *InDef.FindPin("A", EPinDirection::Input));
        const std::string B = Input(InNode, InDef, *InDef.FindPin("B", EPinDirection::Input));
        const std::string Test = A + " " + InNode.GetProperty("Compare", ">") + " " + B;

        if(IsTernary(InNode))
        {
            if(InNode.GetProperty("Hint", "None") != "None")
            {
                Result.Warning(InNode.Id, std::string(), "'" + InDef.DisplayName + "' is a ternary, an expression, which"
                               " takes no Hint; its Hint is ignored");
            }
            const std::string True  = Input(InNode, InDef, *InDef.FindPin("True", EPinDirection::Input));
            const std::string False = Input(InNode, InDef, *InDef.FindPin("False", EPinDirection::Input));
            const std::string Temp  = "t" + std::to_string(NextTemp++);
            Body.Line(InType + " " + Temp + " = " + Test + " ? " + True + " : " + False + ";", InNode.Id);
            return Temp;
        }

        Hoist(InNode, InDef, 0);
        Hoist(InNode, InDef, 1);

        const std::string Temp = "t" + std::to_string(NextTemp++);
        Body.Line(InType + " " + Temp + ";", InNode.Id);
        Body.Line(HintPrefix(InNode) + "if(" + Test + ")", InNode.Id);

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

        std::transform(Hint.begin(), Hint.end(), Hint.begin(), [](const unsigned char Ch) { return char(std::toupper(Ch)); });
        return "HINT_" + Hint + " ";
    }

    /* Before InOwner opens block InArm, emits the values that block reads but
     * that belong to an outer scope. */
    void MaterialCompiler::Hoist(const Node& InOwner, const NodeDefinition& InDef, const int InArm)
    {
        const auto Found = Scopes.find(InOwner.Id);
        if(Found == Scopes.end()) { return; }   // no analysis (a cycle): values are computed where used

        Scope Arm = Found->second;
        Arm.push_back({ InOwner.Id, InArm });

        std::set<NodeGuid> Seen;
        for(const PinDefinition& Pin : InDef.Pins)
        {
            if(Pin.IsInput() && ArmOf(InOwner, InDef, Pin.Name) == InArm) { HoistFrom(InOwner, Pin, Arm, Seen); }
        }
    }

    void MaterialCompiler::HoistFrom(const Node& InNode, const PinDefinition& InPin, const Scope& InArm, std::set<NodeGuid>& InOutSeen)
    {
        const Link* Into = Index.LinkInto(InNode.Id, InPin.Name);
        const Node* Source = Into ? Index.FindNode(Into->From.Node) : nullptr;
        const NodeDefinition* SourceDef = Source ? Index.FindDefinition(*Source) : nullptr;
        const PinDefinition* SourcePin = SourceDef ? SourceDef->FindPin(Into->From.Pin, EPinDirection::Output) : nullptr;
        if(!SourcePin) { return; }

        /* A For Loop's inputs are evaluated with its End; look past it. */
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
        : Schema("Material", "material", ".mtl.glsl")
    {
        AddType({ "float", "Float",   0x9BD96A, "0" });
        AddType({ "vec2",  "Vector2", 0x6AC9D9, "0" });
        AddType({ "vec3",  "Vector3", 0xE8C95A, "0" });
        AddType({ "vec4",  "Vector4", 0xD9786A, "0" });
        AddType({ LOOP_TYPE, "Loop",  0xC8C8C8, "", false });   // pairs a For Loop with its End; no literal
        AddType({ TEXTURE_TYPE, "Texture", 0xD48CE8, "", false });   // a Texture Object's sampler; no literal, no conversions

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
        const auto Staged = [](PinDefinition InPin, const uint32_t InStage, const std::string& InTooltip)
        {
            InPin.Stage   = InStage;
            InPin.Tooltip = InTooltip;
            return InPin;
        };
        {
            NodeDefinition Def;
            Def.Type        = "Output.Material";
            Def.DisplayName = "Material Output";
            Def.Category    = "Output";
            Def.Keywords    = "surface pbr result";
            Def.Tooltip     = "The surface: what the pixel stage shades, and how far the vertex stage moves each vertex. "
                              "An unlinked pin keeps its default.";
            Def.Role        = ENodeRole::Output;
            Def.bUnique     = true;
            Def.Op          = MOP_MaterialOutput;
            Def.StageMask   = STAGE_VERTEX | STAGE_PIXEL;
            Def.Pins        = {
                Staged(In("BaseColor", "vec3", "1 1 1"), STAGE_PIXEL, "Lit: the surface's albedo (metals: their reflectance)"),
                Staged(In("Metallic", "float", "0"), STAGE_PIXEL, "Lit: 0 dielectric, 1 metal"),
                Staged(In("Roughness", "float", "0.5"), STAGE_PIXEL, "Lit: 0 mirror, 1 fully rough"),
                Staged(In("Specular", "float", "0.5"), STAGE_PIXEL, "Lit: a dielectric's reflectance; 0.5 is 4%"),
                Staged(In("Emissive", "vec3", "0 0 0"), STAGE_PIXEL, "Light the surface gives off; an Unlit material's colour"),
                Staged(In("Opacity", "float", "1"), STAGE_PIXEL, "Translucent: 0 clear, 1 solid"),
                Staged(In("OpacityMask", "float", "1"), STAGE_PIXEL, "Masked: the pixel is discarded below Clip Value"),
                Staged(In("Normal", "vec3", "0 0 -1"), STAGE_PIXEL, "Lit: the world-space normal; unlinked, the vertex normal"),
                Staged(In("AmbientOcclusion", "float", "1"), STAGE_PIXEL, "Lit: how much ambient light reaches the surface"),
                Staged(In(OFFSET_PIN, "vec3", "0 0 0"), STAGE_VERTEX,
                       "World-space offset added to each vertex. Vertex stage: pixel-only nodes cannot feed it"),
            };
            Def.Properties  = {
                { "Domain", EPropertyKind::Choice, "Opaque", GDomainChoices,
                  "Opaque, Masked (clips at Clip Value), Translucent (blends, drawn after) or PostProcess (a full-screen pass)" },
                { "Shading", EPropertyKind::Choice, "Lit", GShadingChoices, "Lit: physically based, by the scene's light. Unlit: Emissive only" },
                { "TwoSided", EPropertyKind::Bool, "false", {}, "Light back faces with a flipped normal" },
                { "ClipValue", EPropertyKind::String, "0.5", {}, "Masked: Opacity Mask below this discards the pixel" },
                { "MaxOffset", EPropertyKind::String, "0", {},
                  "The farthest World Position Offset moves a vertex, in world units: pads the culling bounds" },
            };
            AddNode(std::move(Def));
        }
        {
            NodeDefinition Def;
            Def.Type        = "Output.Pixel";
            Def.DisplayName = "Pixel Output";
            Def.Category    = "Output/Legacy";
            Def.Tooltip     = "The first material output: one colour, drawn Unlit and Translucent (alpha blends). "
                              "Material Output supersedes it.";
            Def.Role        = ENodeRole::Output;
            Def.bUnique     = true;
            Def.Op          = MOP_Output;
            Def.StageMask   = STAGE_PIXEL;
            Def.Pins        = { Staged(In("Color", "vec4", "1 1 1 1"), STAGE_PIXEL, "RGB is the colour, A the opacity") };
            AddNode(std::move(Def));
        }
        {
            /* A root, like Pixel Output; any number per graph. */
            NodeDefinition Def;
            Def.Type        = "Output.Discard";
            Def.DisplayName = "Discard";
            Def.Category    = "Output";
            Def.Keywords    = "clip kill cutout alpha mask test";
            Def.Tooltip     = "Throws the pixel away -- nothing is written -- when A compares so with B: "
                              "with the default '<', when A is below B (an alpha below 0.5).";
            Def.Role        = ENodeRole::Output;
            Def.Op          = MOP_Discard;
            Def.StageMask   = STAGE_PIXEL;
            Def.Pins        = { Staged(In("A", "float"), STAGE_PIXEL, std::string()), Staged(In("B", "float", "0.5"), STAGE_PIXEL, std::string()) };
            Def.Properties  = { { "Compare", EPropertyKind::Choice, "<", GCompareChoices, "Discard when A compares so with B" } };
            AddNode(std::move(Def));
        }

        // --- Inputs --------------------------------------------------------------
        /* Inputs read MaterialInputs (MaterialCommon.glsl), which each stage's
         * template fills -- so one expression serves both stages. */
        const auto PixelOnly = [](NodeDefinition& InDef, const std::string& InTooltip)
        {
            InDef.StageMask = STAGE_PIXEL;
            InDef.Tooltip   = InTooltip + " Pixel stage only: it cannot feed World Position Offset.";
        };

        Pure("Input.TexCoord",   "Texture Coordinate", "Inputs", { Out("UV", "vec2", "Inputs.TexCoord") }, {}, "uv");
        Pure("Input.VertexNormal", "Vertex Normal", "Inputs", { Out("Normal", "vec3", "Inputs.WorldNormal") }, {}, "world normal")
            .Tooltip = "The interpolated world-space normal (a quad's faces -Z before its rotation).";
        Pure("Input.VertexColor", "Vertex Color", "Inputs",
             { Out("Color", "vec4", "Inputs.Color"), Out("RGB", "vec3", "Inputs.Color.rgb"), Out("A", "float", "Inputs.Color.a") },
             {}, "colour rgba painted")
            .Tooltip = "The mesh's per-vertex colour; white when it has none (and on quads). A particle system's "
                       "mesh: times each particle's colour, which fades over its life.";
        Pure("Input.WorldPosition", "World Position", "Inputs", { Out("Position", "vec3", "Inputs.WorldPosition") }, {}, "absolute")
            .Tooltip = "Where this point is in the world. In the vertex stage, before World Position Offset moves it.";
        Pure("Input.CameraPosition", "Camera Position", "Inputs", { Out("Position", "vec3", "Scene_CameraPosition.xyz") }, {},
             "eye view camera fresnel")
            .Tooltip = "Where the view's eye is in the world: with World Position, the direction to the camera (Fresnel).";
        PixelOnly(Pure("Input.Position", "Clip Position", "Inputs", { Out("Position", "vec3", "Inputs.ClipPosition") }, {},
                       "interpolated projected"),
                  "The interpolated clip-space position (before the divide by w).");
        PixelOnly(Pure("Input.FragCoord", "Pixel Coordinate", "Inputs", { Out("Coord", "vec4", "Inputs.FragCoord") }, {}, "screen frag"),
                  "gl_FragCoord: the pixel's window coordinates from the target's top-left, depth and 1/w.");
        PixelOnly(Pure("Input.PixelDepth", "Pixel Depth", "Inputs",
                       { Out("Linear", "float", "Inputs.ViewDepth"), Out("Device", "float", "Inputs.DeviceDepth") }, {}, "z distance"),
                  "Linear: distance in front of the camera, in world units. Device: the depth buffer's 0-1 value.");
        Pure("Input.Viewport",   "Viewport Size", "Inputs",
             { Out("Size", "vec2", "ViewParameters.Resolution.xy"), Out("InvSize", "vec2", "ViewParameters.Resolution.zw") },
             {}, "resolution");
        Pure("Input.InstanceTint", "Instance Tint", "Inputs",
             { Out("Tint", "vec4", TINT_UNIFORM) }, {}, "color colour primitive per-entity");
        Pure("Input.Mouse",      "Mouse", "Inputs",
             { Out("Position", "vec2", "ViewParameters.MouseCoords.xy"), Out("Down", "float", "ViewParameters.MouseCoords.z") });
        Pure("Input.Time", "Time", "Inputs", { Out("Time", "float", "Scene_Time.x"), Out("Delta", "float", "Scene_Time.y") },
             {}, "seconds clock animate elapsed")
            .Tooltip = "Seconds since the renderer started, and the last frame's length.";
        {
            NodeDefinition& Def = Pure("Input.SceneTexture", "Scene Texture", "Inputs",
                                       { Out("Color", "vec4", "texture(Material_SceneColor, Inputs.TexCoord)") }, {},
                                       "scene color screen post process");
            Def.Op = MOP_SceneTexture;
            PixelOnly(Def, "PostProcess only: the scene drawn so far, at this pixel.");
        }
        {
            NodeDefinition& Def = Pure("Input.SceneDepth", "Scene Depth", "Inputs",
                                       { Out("Depth", "float", "Material_SceneDepthAt(Inputs.FragCoord.xy)"),
                                         Out("Distance", "float", "Material_SceneDistanceAt(Inputs.ClipPosition.xy, Inputs.FragCoord.xy)") },
                                       {}, "scene depth distance z buffer post process fog");
            Def.Op = MOP_SceneTexture;
            PixelOnly(Def, "PostProcess only: the scene's depth at this pixel -- Depth as stored (0 near, 1 far), "
                           "Distance in world units from the camera.");
        }

        // --- Textures ------------------------------------------------------------
        {
            NodeDefinition& Def = Pure(TEXTURE_OBJECT, "Texture Object", "Textures",
                                       { Out("Texture", TEXTURE_TYPE, ""), Out("Size", "vec2", "") }, {},
                                       "texture image bitmap asset sampler");
            Def.Op      = MOP_TextureObject;
            Def.Tooltip = "A texture asset and how it is sampled. Link Texture into any number of Texture Samples, each "
                          "at its own UV; Size is its width and height in texels (one texel is 1 / Size in UV).";
            Def.Properties = {
                { "Texture", EPropertyKind::Texture, "", {}, "The .nmtexture, by its path in the game's runtime "
                  "folder: Content/Textures/Brick.nmtexture. Drop one from the Content Browser" },
                { "Filter", EPropertyKind::Choice, "Default", GFilterChoices,
                  "Default: the texture's own (set when it was imported). Nearest: hard texels. Linear: blended" },
                { "Wrap", EPropertyKind::Choice, "Default", GWrapChoices,
                  "Outside 0-1. Default: the texture's own. Repeat tiles; Clamp stretches the edge; Mirror tiles reflected" },
            };
        }
        {
            PinDefinition UV = In("UV", "vec2");
            UV.Implicit = "Texture Coordinate";
            UV.Tooltip  = "Where to sample, 0-1 across the texture. Unlinked: the vertex's Texture Coordinate";
            PinDefinition Texture = In("Texture", TEXTURE_TYPE);
            Texture.Tooltip = "The Texture Object to sample";
            NodeDefinition& Def = Pure(TEXTURE_SAMPLE, "Texture Sample", "Textures",
                                       { Texture, UV, Out("RGBA", "vec4", ""), Out("RGB", "vec3", ""), Out("R", "float", ""),
                                         Out("G", "float", ""), Out("B", "float", ""), Out("A", "float", "") }, {},
                                       "texture image bitmap sampler map lookup");
            Def.Op      = MOP_TextureSample;
            Def.Version = 2;   // 1 named its texture itself (UpgradeGraph)
            Def.Tooltip = "A Texture Object's colour at UV. Colour textures read as authored (display space), like "
                          "every colour a material uses. In World Position Offset it reads the full-size level.";
        }

        {
            PinDefinition Normal = In("Normal", "vec3");
            Normal.Tooltip = "Omnidirectional: the surface normal the view reflects about (world space). Unlinked: the pixel's";
            PinDefinition Distortion = In("Distortion", "vec2");
            Distortion.Tooltip = "Planar mirror: an offset in screen UV (ripples). Unlinked: none";
            NodeDefinition& Def = Pure(TEXTURE_REFLECT, "Reflect", "Textures",
                                       { Normal, Distortion, Out("RGBA", "vec4", ""), Out("RGB", "vec3", "") }, {},
                                       "reflection mirror chrome environment cubemap scene capture probe");
            Def.Op = MOP_Reflect;
            PixelOnly(Def, "The scene as a Scene Capture Probe sees it, reflected at this pixel. A RenderTargetTex2D target is a "
                           "planar mirror's capture, read at the mirrored screen point; a RenderTargetTex3D target is an "
                           "omnidirectional capture, read along the view reflected about Normal. Feed RGB to Emissive (or Base "
                           "Color) for a mirror or chrome.");
            Def.Properties = {
                { "Target", EPropertyKind::Texture, "", {}, "The render target its probe captures into: Content/RenderTargets/Pool.nmrt2d "
                  "(a planar mirror) or Content/RenderTargets/Ball.nmrt3d (omnidirectional)" },
            };
        }

        {
            PinDefinition Color = In("Color", "vec3", "0.5 0.5 1");
            Color.Tooltip = "The normal map's colour: a Texture Sample's RGB. Unlinked: flat";
            PinDefinition Strength = In("Strength", "float", "1");
            Strength.Tooltip = "How far the map tilts the normal: 0 flat, 1 as authored, more exaggerates";
            PinDefinition UV = In("UV", "vec2");
            UV.Implicit = "Texture Coordinate";
            UV.Tooltip  = "The UV the map was sampled at, for its tangent frame. Unlinked: the vertex's Texture Coordinate";
            NodeDefinition& Def = Pure(NORMAL_MAP, "Normal Map", "Textures",
                                       { Color, Strength, UV, Out("Normal", "vec3", "") }, {},
                                       "normal map bump tangent space detail");
            Def.Op = MOP_NormalMap;
            PixelOnly(Def, "A tangent-space normal map's colour as a world-space normal, for Material Output's Normal. The "
                           "tangent frame comes from how the position and UV change across the pixel, so any mesh works "
                           "without tangents. Import the map as linear (not sRGB).");
            Def.Properties = {
                { "Format", EPropertyKind::Choice, "OpenGL", { "OpenGL", "DirectX" },
                  "Which way the map's green points: OpenGL (Y+, up the image; Blender, Substance's OpenGL export) or "
                  "DirectX (Y-, down it; Unreal, 3ds Max)" },
            };
        }

        // --- Values --------------------------------------------------------------
        {
            NodeDefinition& Def = Pure("Constant", "Constant", "Values", { In("Value", WILDCARD_TYPE), Out("Out", WILDCARD_TYPE, "") },
                                       GFloatTypes, "number vector color literal");
            Def.Op = MOP_Pass;
        }
        {
            NodeDefinition& Def = Pure("Parameter", "Parameter", "Values", { Out("Value", WILDCARD_TYPE, "") }, GFloatTypes, "uniform");
            Def.Op      = MOP_Parameter;
            Def.Tooltip = "A value the renderer supplies: a member of the material's uniform block, its default from "
                          "<Name>.material. Any number, within the block's 1024 bytes.";
            Def.Properties = { { "Name", EPropertyKind::Identifier, "Param", {}, "The parameter's name; parameters are laid out in name order" },
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

        // --- Terrain ---------------------------------------------------------------
        /* A terrain's four paint layers (RenderCore's Terrain component puts
         * their weights in the vertex colour, R G B A = layers 0 1 2 3),
         * blended: each input times its weight, over the weights' sum. */
        Pure("Terrain.LayerBlend", "Terrain Layer Blend", "Terrain",
             { In("Layer0", W), In("Layer1", W), In("Layer2", W), In("Layer3", W),
               Out("Out", W, "(({Layer0}) * Inputs.Color.r + ({Layer1}) * Inputs.Color.g + ({Layer2}) * Inputs.Color.b"
                             " + ({Layer3}) * Inputs.Color.a) / max(Inputs.Color.r + Inputs.Color.g + Inputs.Color.b"
                             " + Inputs.Color.a, 0.0001)") },
             GFloatTypes, "landscape splat paint weights mix")
            .Tooltip = "A Terrain's paint layers mixed by their weights (the vertex colour's R G B A). "
                       "Feed it a colour, roughness or normal per layer.";

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
                                 "None: the driver decides. Ignored on OpenGL, and by a ternary." },
                               { "Ternary", EPropertyKind::Bool, "false", {},
                                 "Write one expression, A > B ? True : False, instead of an if block: both sides are "
                                 "computed, then one is chosen" } };
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

        /* A node both stages reach is checked twice; say each thing once. */
        std::vector<Diagnostic> Unique;
        for(const Diagnostic& Each : OutResult.Diagnostics)
        {
            const bool bSeen = std::any_of(Unique.begin(), Unique.end(), [&](const Diagnostic& InOther)
            {
                return InOther.Severity == Each.Severity && InOther.Node == Each.Node && InOther.Pin == Each.Pin
                    && InOther.Message == Each.Message;
            });
            if(!bSeen) { Unique.push_back(Each); }
        }
        OutResult.Diagnostics = std::move(Unique);
    }
}

std::unique_ptr<Schema> CreateMaterialSchema()
{
    return std::make_unique<MaterialSchema>();
}
}
