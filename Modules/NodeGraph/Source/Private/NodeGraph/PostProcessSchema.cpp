#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

#include "Public/NodeGraph/GraphCompiler.h"
#include "Private/NodeGraph/GraphIndex.h"

/*
 * The PostProcess schema: a post process stack (*.pp.nmgraph). Each node is
 * one effect and its settings -- no pins, nothing to link. The renderer runs
 * the effects in a fixed order (RenderCore's RENDERPIPELINE.md, "Post
 * process"), so a node's place on the canvas means nothing; a stack without a
 * node leaves that effect alone.
 *
 * The output is ".ppstack": a line per effect, read by RenderCore's
 * ParsePostProcessStack from the graph's <gen_code> at runtime --
 *
 *     Effect Bloom Threshold=0.8 Intensity=0.6 Radius=0.75 Levels=5 Tint=1,1,1
 *     Material Stage=After Order=0 Path=Content/Materials/Negative.material.nmgraph
 *
 * Numbers are written as parsed, lists with commas; a Material's path comes
 * last and runs to the end of the line.
 */

namespace NodeGraph
{
namespace
{
    /* A numeric setting: its property, its range, and how many numbers. */
    struct NumberSetting
    {
        const char* Name;
        const char* Default;
        double      Min;
        double      Max;
        size_t      Count;   // 1, or 3 for a colour
        const char* Tooltip;
    };

    struct EffectSpec
    {
        const char*                 Type;          // "Effect.Bloom"
        const char*                 Key;           // "Bloom", in the .ppstack
        const char*                 DisplayName;
        const char*                 Tooltip;
        std::vector<NumberSetting>  Numbers;
        std::vector<PropertyDefinition> Others;    // choices and switches, written as given
    };

    const std::vector<EffectSpec>& GetEffects()
    {
        static const std::vector<EffectSpec> Effects = {
            { "Effect.DepthOfField", "DepthOfField", "Depth of Field",
              "Blurs what is nearer or farther than the focus. Uses the scene depth.",
              { { "Focus Distance", "800", 0.0, 1.0e7, 1, "World units from the camera to the sharpest plane" },
                { "Focus Range",    "400", 1.0, 1.0e7, 1, "How far either side of the focus stays sharp before the blur starts" },
                { "Max Blur",       "8",   0.0, 32.0,  1, "The widest blur, in pixels at full resolution" } },
              { { "Near Blur", EPropertyKind::Bool, "true", {}, "Blur what is nearer than the focus too, not only what is farther" } } },
            { "Effect.MotionBlur", "MotionBlur", "Motion Blur",
              "Smears the image along the camera's own motion since the last frame (not moving objects').",
              { { "Intensity",  "0.5", 0.0, 2.0,   1, "The fraction of a frame's motion the blur spans" },
                { "Max Length", "32",  0.0, 128.0, 1, "The longest streak, in pixels" },
                { "Samples",    "8",   2.0, 32.0,  1, "Taps along each streak: more is smoother and slower" } },
              {} },
            { "Effect.Bloom", "Bloom", "Bloom",
              "Bright areas glow: what passes Threshold is blurred through a chain of smaller targets and added back.",
              { { "Threshold", "0.8",   0.0, 1.0,  1, "How bright (0-1, the scene is not HDR) a pixel must be to glow" },
                { "Intensity", "0.6",   0.0, 8.0,  1, "How strongly the glow is added" },
                { "Radius",    "0.75",  0.0, 1.0,  1, "How far the glow spreads: how much each smaller level adds back" },
                { "Levels",    "5",     1.0, 6.0,  1, "Downsample steps, each half the size: more is wider" },
                { "Tint",      "1 1 1", 0.0, 8.0,  3, "The glow's colour, multiplied in" } },
              {} },
            { "Effect.Antialiasing", "Antialiasing", "Antialiasing",
              "Smooths jagged edges after every other effect.",
              { { "Edge Threshold", "0.125", 0.0, 1.0, 1, "The contrast an edge needs before it is smoothed: lower catches more" } },
              { { "Method",  EPropertyKind::Choice, "FXAA",   { "FXAA" }, "The technique" },
                { "Quality", EPropertyKind::Choice, "Medium", { "Low", "Medium", "High" }, "How far along an edge it searches" } } },
        };
        return Effects;
    }

    /* "Focus Distance" -> "FocusDistance". */
    std::string KeyOf(const std::string& InName)
    {
        std::string Key;
        for(const char C : InName) { if(C != ' ') { Key += C; } }
        return Key;
    }

    class PostProcessSchema final : public Schema
    {
    public:
        PostProcessSchema() : Schema("PostProcess", "pp", ".ppstack")
        {
            for(const EffectSpec& Spec : GetEffects())
            {
                NodeDefinition Def;
                Def.Type        = Spec.Type;
                Def.DisplayName = Spec.DisplayName;
                Def.Category    = "Effects";
                Def.Tooltip     = Spec.Tooltip;
                Def.Role        = ENodeRole::Output;
                Def.bUnique     = true;
                for(const NumberSetting& Number : Spec.Numbers)
                {
                    Def.Properties.push_back({ Number.Name, EPropertyKind::String, Number.Default, {}, Number.Tooltip });
                }
                for(const PropertyDefinition& Other : Spec.Others)
                {
                    Def.Properties.push_back(Other);
                }
                AddNode(std::move(Def));
            }

            NodeDefinition Material;
            Material.Type        = "Effect.Material";
            Material.DisplayName = "Material";
            Material.Category    = "Effects";
            Material.Keywords    = "custom pass postprocess";
            Material.Tooltip     = "A PostProcess-domain material run full screen, before or after the built-in effects. "
                                   "It samples the scene so far, and the scene depth.";
            Material.Role        = ENodeRole::Output;
            Material.Properties  = {
                { "Material", EPropertyKind::String, "", {}, "A PostProcess material graph: Content/Materials/X.material.nmgraph" },
                { "Stage",    EPropertyKind::Choice, "After Effects", { "Before Effects", "After Effects" },
                  "Before Depth of Field, or after Bloom (always before Antialiasing)" },
                { "Order",    EPropertyKind::String, "0", {}, "Lower runs first, among materials at the same stage" },
            };
            AddNode(std::move(Material));
        }

        void Compile(const Graph& InGraph, CompileResult& OutResult) const override
        {
            /* Never empty, so a stack with no nodes still stores an output. */
            std::ostringstream Text;
            Text << "# Post process stack " << InGraph.Name << '\n';

            /* The effects in the renderer's order, whatever the canvas says. */
            for(const EffectSpec& Spec : GetEffects())
            {
                const auto Found = std::find_if(InGraph.Nodes.begin(), InGraph.Nodes.end(),
                                                [&Spec](const Node& InNode) { return InNode.Type == Spec.Type; });
                if(Found == InGraph.Nodes.end())
                {
                    continue;
                }
                const Node& Each = *Found;
                Text << "Effect " << Spec.Key;
                for(const NumberSetting& Number : Spec.Numbers)
                {
                    std::vector<double> Values;
                    const std::string   Value = Each.GetProperty(Number.Name, Number.Default);
                    if(!ParseNumbers(Value, Number.Count, Values))
                    {
                        OutResult.Error(Each.Id, std::string(), std::string(Spec.DisplayName) + ": " + Number.Name + " '" + Value
                                        + "' is not " + (Number.Count == 1 ? "a number" : std::to_string(Number.Count) + " numbers"));
                        continue;
                    }
                    Text << ' ' << KeyOf(Number.Name) << '=';
                    for(size_t Index = 0; Index < Values.size(); ++Index)
                    {
                        if(Values[Index] < Number.Min || Values[Index] > Number.Max)
                        {
                            OutResult.Error(Each.Id, std::string(), std::string(Spec.DisplayName) + ": " + Number.Name + " "
                                            + FormatNumber(Values[Index]) + " is outside " + FormatNumber(Number.Min) + " to "
                                            + FormatNumber(Number.Max));
                        }
                        Text << (Index ? "," : "") << FormatNumber(Values[Index]);
                    }
                }
                for(const PropertyDefinition& Other : Spec.Others)
                {
                    Text << ' ' << KeyOf(Other.Name) << '=' << KeyOf(Each.GetProperty(Other.Name, Other.Default));
                }
                Text << '\n';
            }

            /* Materials, by stage and order; ties by GUID, so output is stable. */
            struct MaterialPass { const Node* Each; int Stage; double Order; };
            std::vector<MaterialPass> Passes;
            for(const Node& Each : InGraph.Nodes)
            {
                if(Each.Type != "Effect.Material")
                {
                    continue;
                }
                const std::string Path  = Each.GetProperty("Material");
                const std::string Ext   = ".material.nmgraph";
                double            Order = 0.0;
                if(Path.size() <= Ext.size() || Path.compare(Path.size() - Ext.size(), Ext.size(), Ext) != 0)
                {
                    OutResult.Error(Each.Id, std::string(), "Material: '" + Path + "' is not a material graph (*.material.nmgraph)");
                    continue;
                }
                if(!ParseNumber(Each.GetProperty("Order", "0"), Order))
                {
                    OutResult.Error(Each.Id, std::string(), "Material: Order '" + Each.GetProperty("Order") + "' is not a number");
                    continue;
                }
                Passes.push_back({ &Each, Each.GetProperty("Stage", "After Effects") == "Before Effects" ? 0 : 1, Order });
            }
            std::sort(Passes.begin(), Passes.end(), [](const MaterialPass& InA, const MaterialPass& InB)
            {
                if(InA.Stage != InB.Stage) { return InA.Stage < InB.Stage; }
                if(InA.Order != InB.Order) { return InA.Order < InB.Order; }
                return InA.Each->Id < InB.Each->Id;
            });
            for(const MaterialPass& Pass : Passes)
            {
                Text << "Material Stage=" << (Pass.Stage == 0 ? "Before" : "After") << " Order=" << FormatNumber(Pass.Order)
                     << " Path=" << Pass.Each->GetProperty("Material") << '\n';
            }

            if(!OutResult.HasErrors())
            {
                OutResult.Code = Text.str();
            }
        }
    };
}

std::unique_ptr<Schema> CreatePostProcessSchema()
{
    return std::make_unique<PostProcessSchema>();
}
}
