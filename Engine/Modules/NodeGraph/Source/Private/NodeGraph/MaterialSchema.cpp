#include <algorithm>
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
 * Parameters become SHADER_UNIFORMs in one UBO at binding 2 -- 0 and 1 are
 * Common.glsl's view and transform blocks, and 2 is the first per-shader slot
 * (Basic.fsh's Basic_Block uses it the same way).
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
    };

    constexpr int UNIFORM_BLOCK_BINDING = 2;

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

    class MaterialSchema final : public Schema
    {
    public:
        MaterialSchema();
        void Compile(const Graph& InGraph, CompileResult& OutResult) const override;
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
        };

        std::string Output(const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin);
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
    };

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

        const PinDefinition* ColorPin = RootDef->FindPin("Color", EPinDirection::Input);
        const std::string Color = Input(*Root, *RootDef, *ColorPin);
        if(Result.HasErrors()) { return; }

        const std::string BlockName = SanitizeIdentifier(G.Name.empty() ? std::string("Material") : G.Name) + "_Block";

        CodeWriter Out;
        Out.Line("#version 440 core");
        Out.Line("// Generated by NimbleEditor from the material graph \"" + G.Name + "\". Do not edit.");
        Out.Blank();
        Out.Line("#include <Common.glsl>");
        Out.Blank();
        Out.Line("OUTPUT(0, vec4, Output)");

        if(!Parameters.empty())
        {
            Out.Blank();
            Out.Line("UBO_BEGIN(" + std::to_string(UNIFORM_BLOCK_BINDING) + ", " + BlockName + ")");
            Out.Indent();
            for(const auto& Each : Parameters)
            {
                Out.Line("SHADER_UNIFORM(" + Each.second.Type + ", " + Each.first + ")", Each.second.Node);
            }
            Out.Outdent();
            Out.Line("UBO_END(" + BlockName + ")");
        }

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
                const std::string Name = InNode.GetProperty("Name", "Param");
                const auto Existing = Parameters.find(Name);
                if(Existing != Parameters.end() && Existing->second.Type != Type)
                {
                    Result.Error(InNode.Id, std::string(), "parameter '" + Name + "' is " + Type + " here but "
                                 + Existing->second.Type + " elsewhere");
                }
                else
                {
                    Parameters.emplace(Name, Parameter{ Type, InNode.Id });
                }
                Value = Name;
                break;
            }

            case MOP_Pass:
            {
                const PinDefinition* In = InDef.FindPin("Value", EPinDirection::Input);
                Value = In ? Input(InNode, InDef, *In) : "0.0";
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
        return Value;
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
            Def.Tooltip = "A value the renderer sets: a SHADER_UNIFORM in this shader's block.";
            Def.Properties = { { "Name", EPropertyKind::Identifier, "Param", {}, "The uniform's name" },
                               { "Default", EPropertyKind::String, "0", {}, "The value a material starts with" } };
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
