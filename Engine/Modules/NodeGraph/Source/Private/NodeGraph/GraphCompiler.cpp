#include "Public/NodeGraph/GraphCompiler.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "Private/NodeGraph/GraphIndex.h"

namespace NodeGraph
{
    // === CompileResult ==========================================================

    bool CompileResult::HasErrors() const
    {
        return std::any_of(Diagnostics.begin(), Diagnostics.end(),
                           [](const Diagnostic& Each) { return Each.Severity == ESeverity::Error; });
    }

    void CompileResult::Error(const NodeGuid& InNode, const std::string& InPin, const std::string& InMessage)
    {
        Diagnostics.push_back({ ESeverity::Error, InNode, InPin, InMessage });
    }

    void CompileResult::Warning(const NodeGuid& InNode, const std::string& InPin, const std::string& InMessage)
    {
        Diagnostics.push_back({ ESeverity::Warning, InNode, InPin, InMessage });
    }

    // === Validation =============================================================

    namespace
    {
        std::string NodeLabel(const Node& InNode, const NodeDefinition* InDef)
        {
            return "'" + (InDef ? InDef->DisplayName : InNode.Type) + "'";
        }

        void ValidateProperties(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef, CompileResult& OutResult)
        {
            for(const PropertyDefinition& Property : InDef.Properties)
            {
                const std::string Value = InNode.GetProperty(Property.Name, Property.Default);
                switch(Property.Kind)
                {
                    case EPropertyKind::String:
                        break;
                    case EPropertyKind::Identifier:
                        if(!IsIdentifier(Value))
                        {
                            OutResult.Error(InNode.Id, std::string(), NodeLabel(InNode, &InDef) + ": " + Property.Name + " '" + Value
                                            + "' must be letters, digits and '_', not starting with a digit");
                        }
                        break;
                    case EPropertyKind::Choice:
                        if(std::find(Property.Choices.begin(), Property.Choices.end(), Value) == Property.Choices.end())
                        {
                            OutResult.Error(InNode.Id, std::string(), NodeLabel(InNode, &InDef) + ": '" + Value
                                            + "' is not a valid " + Property.Name);
                        }
                        break;
                    case EPropertyKind::Variable:
                        if(!InGraph.FindVariable(Value))
                        {
                            OutResult.Error(InNode.Id, std::string(), NodeLabel(InNode, &InDef) + ": no variable named '" + Value + "'");
                        }
                        break;
                    case EPropertyKind::Function:
                        if(!InGraph.FindFunction(Value))
                        {
                            OutResult.Error(InNode.Id, std::string(), NodeLabel(InNode, &InDef) + ": no function named '" + Value + "'");
                        }
                        break;
                    case EPropertyKind::Bool:
                        if(Value != "true" && Value != "false")
                        {
                            OutResult.Error(InNode.Id, std::string(), NodeLabel(InNode, &InDef) + ": " + Property.Name + " must be true or false, not '"
                                            + Value + "'");
                        }
                        break;
                    case EPropertyKind::Texture:
                    {
                        /* Empty is the schema's to judge; anything else must be a .nmtexture on one line. */
                        const std::string Suffix = ".nmtexture";
                        std::string Lower = Value;
                        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](const unsigned char Ch) { return char(std::tolower(Ch)); });
                        const bool bTexture = Lower.size() > Suffix.size() && Lower.compare(Lower.size() - Suffix.size(), Suffix.size(), Suffix) == 0;
                        if(!Value.empty() && (!bTexture || Value.find_first_of("\r\n") != std::string::npos))
                        {
                            OutResult.Error(InNode.Id, std::string(), NodeLabel(InNode, &InDef) + ": " + Property.Name + " '" + Value
                                            + "' is not a texture asset (a .nmtexture path)");
                        }
                        break;
                    }
                }
            }
        }
    }

    void ValidateGraph(const Graph& InGraph, const Schema& InSchema, CompileResult& OutResult)
    {
        const GraphIndex Index(InGraph, InSchema);

        // --- Nodes ---------------------------------------------------------------
        std::set<NodeGuid>                     Seen;
        std::map<std::string, const Node*>     UniqueSeen;
        for(const Node& Each : InGraph.Nodes)
        {
            if(!Each.Id.IsSet())
            {
                OutResult.Error(Each.Id, std::string(), "a node has no id");
                continue;
            }
            if(!Seen.insert(Each.Id).second)
            {
                OutResult.Error(Each.Id, std::string(), "two nodes share the id " + GuidToString(Each.Id));
                continue;
            }

            const NodeDefinition* Def = Index.FindDefinition(Each);
            if(!Def)
            {
                OutResult.Error(Each.Id, std::string(), "unknown node type '" + Each.Type + "' -- kept as a placeholder;"
                                " delete it or restore the type");
                continue;
            }

            if(Each.Version > Def->Version)
            {
                OutResult.Warning(Each.Id, std::string(), NodeLabel(Each, Def) + " was saved by a newer version ("
                                  + std::to_string(Each.Version) + " > " + std::to_string(Def->Version) + ")");
            }

            if(Def->bUnique)
            {
                const auto Inserted = UniqueSeen.emplace(Def->Type, &Each);
                if(!Inserted.second)
                {
                    OutResult.Error(Each.Id, std::string(), NodeLabel(Each, Def) + " may appear only once in a graph");
                }
            }

            if(Def->HasWildcard() && !Each.WildcardType.empty()
               && std::find(Def->WildcardTypes.begin(), Def->WildcardTypes.end(), Each.WildcardType) == Def->WildcardTypes.end())
            {
                OutResult.Warning(Each.Id, std::string(), NodeLabel(Each, Def) + " cannot be '" + Each.WildcardType
                                  + "'; using '" + InSchema.GetWildcardType(InGraph, Each) + "'");
            }

            for(const auto& Constant : Each.Constants)
            {
                const PinDefinition* Pin = Def->FindPin(Constant.first, EPinDirection::Input);
                if(!Pin || Pin->IsExec())
                {
                    OutResult.Warning(Each.Id, Constant.first, NodeLabel(Each, Def) + " has a value for '" + Constant.first
                                      + "', which is not one of its inputs; it is ignored");
                }
            }

            ValidateProperties(InGraph, Each, *Def, OutResult);
        }

        // --- Links ---------------------------------------------------------------
        std::set<PinRef> DataInputsFed;
        std::set<PinRef> ExecOutputsUsed;
        for(const Link& Each : InGraph.Links)
        {
            const Node* From = Index.FindNode(Each.From.Node);
            const Node* To   = Index.FindNode(Each.To.Node);
            if(!From || !To)
            {
                OutResult.Error(From ? From->Id : (To ? To->Id : NodeGuid()), std::string(), "a link points at a node that does not exist");
                continue;
            }

            const NodeDefinition* FromDef = Index.FindDefinition(*From);
            const NodeDefinition* ToDef   = Index.FindDefinition(*To);
            if(!FromDef || !ToDef) { continue; }   // a placeholder is already an error

            const PinDefinition* FromPin = FromDef->FindPin(Each.From.Pin, EPinDirection::Output);
            const PinDefinition* ToPin   = ToDef->FindPin(Each.To.Pin, EPinDirection::Input);
            if(!FromPin)
            {
                OutResult.Error(From->Id, Each.From.Pin, NodeLabel(*From, FromDef) + " has no output '" + Each.From.Pin + "'");
                continue;
            }
            if(!ToPin)
            {
                OutResult.Error(To->Id, Each.To.Pin, NodeLabel(*To, ToDef) + " has no input '" + Each.To.Pin + "'");
                continue;
            }
            if(From == To)
            {
                OutResult.Error(From->Id, Each.From.Pin, NodeLabel(*From, FromDef) + " is linked to itself");
                continue;
            }
            if(FromPin->Kind != ToPin->Kind)
            {
                OutResult.Error(To->Id, Each.To.Pin, "an execution pin is linked to a value pin");
                continue;
            }

            if(FromPin->IsExec())
            {
                if(!ExecOutputsUsed.insert(Each.From).second)
                {
                    OutResult.Error(From->Id, Each.From.Pin, NodeLabel(*From, FromDef) + "'s '" + Each.From.Pin
                                    + "' leads to more than one node; use a Sequence");
                }
                continue;
            }

            if(!DataInputsFed.insert(Each.To).second)
            {
                OutResult.Error(To->Id, Each.To.Pin, NodeLabel(*To, ToDef) + "'s input '" + Each.To.Pin + "' has more than one link");
            }

            const std::string FromType = Index.PinType(*From, *FromPin);
            const std::string ToType   = Index.PinType(*To, *ToPin);
            if(!InSchema.CanConvert(FromType, ToType))
            {
                OutResult.Error(To->Id, Each.To.Pin, NodeLabel(*To, ToDef) + "'s input '" + Each.To.Pin + "' is "
                                + ToType + " and cannot take the " + FromType + " from " + NodeLabel(*From, FromDef));
            }
        }

        // --- Variables -----------------------------------------------------------
        std::set<std::string> VariableNames;
        for(const Variable& Each : InGraph.Variables)
        {
            if(!IsIdentifier(Each.Name))
            {
                OutResult.Error(NodeGuid(), std::string(), "variable '" + Each.Name + "' must be letters, digits and '_', not starting with a digit");
            }
            if(!VariableNames.insert(Each.Name).second)
            {
                OutResult.Error(NodeGuid(), std::string(), "two variables are named '" + Each.Name + "'");
            }
            const std::vector<std::string>& Allowed = InSchema.GetVariableTypes();
            if(std::find(Allowed.begin(), Allowed.end(), Each.Type) == Allowed.end())
            {
                OutResult.Error(NodeGuid(), std::string(), "variable '" + Each.Name + "' has the unknown type '" + Each.Type + "'");
            }
        }

        // --- Functions -----------------------------------------------------------
        /* Their pin types are the schema's to judge, in its Compile. */
        std::set<std::string> FunctionNames;
        for(const GraphFunction& Each : InGraph.Functions)
        {
            if(!IsIdentifier(Each.Name))
            {
                OutResult.Error(NodeGuid(), std::string(), "function '" + Each.Name + "' must be letters, digits and '_', not starting with a digit");
            }
            if(!FunctionNames.insert(Each.Name).second)
            {
                OutResult.Error(NodeGuid(), std::string(), "two functions are named '" + Each.Name + "'");
            }

            std::set<std::string> PinNames;
            for(const std::vector<FunctionPin>* List : { &Each.Inputs, &Each.Outputs })
            {
                for(const FunctionPin& Pin : *List)
                {
                    if(!IsIdentifier(Pin.Name))
                    {
                        OutResult.Error(NodeGuid(), std::string(), "function '" + Each.Name + "': '" + Pin.Name
                                        + "' must be letters, digits and '_', not starting with a digit");
                    }
                    else if(!PinNames.insert(Pin.Name).second)
                    {
                        OutResult.Error(NodeGuid(), std::string(), "function '" + Each.Name + "' has two inputs or outputs named '" + Pin.Name + "'");
                    }
                }
            }
        }

        for(const PropertyDefinition& Property : InSchema.GetGraphProperties())
        {
            const std::string Value = InGraph.GetProperty(Property.Name, Property.Default);
            if(Property.Kind == EPropertyKind::Choice
               && std::find(Property.Choices.begin(), Property.Choices.end(), Value) == Property.Choices.end())
            {
                OutResult.Error(NodeGuid(), std::string(), "'" + Value + "' is not a valid " + Property.Name);
            }
        }
    }

    void CompileGraph(const Graph& InGraph, CompileResult& OutResult)
    {
        OutResult.Code.clear();
        OutResult.LineNodes.clear();
        OutResult.Companions.clear();

        const Schema* S = FindSchema(InGraph.SchemaName);
        if(!S)
        {
            OutResult.Error(NodeGuid(), std::string(), "unknown graph schema '" + InGraph.SchemaName + "'");
            return;
        }

        ValidateGraph(InGraph, *S, OutResult);
        if(OutResult.HasErrors()) { return; }

        S->Compile(InGraph, OutResult);
        if(OutResult.HasErrors())
        {
            OutResult.Code.clear();
            OutResult.LineNodes.clear();
            OutResult.Companions.clear();
        }
    }

    std::string FormatDiagnostics(const Graph& InGraph, const CompileResult& InResult)
    {
        size_t Errors = 0, Warnings = 0;
        for(const Diagnostic& Each : InResult.Diagnostics)
        {
            if(Each.Severity == ESeverity::Error)   { ++Errors; }
            if(Each.Severity == ESeverity::Warning) { ++Warnings; }
        }

        std::string Text = InGraph.SchemaName + " graph '" + InGraph.Name + "': "
                         + std::to_string(Errors) + " error(s), " + std::to_string(Warnings) + " warning(s)";
        for(const Diagnostic& Each : InResult.Diagnostics)
        {
            Text += "\n  ";
            Text += Each.Severity == ESeverity::Error ? "error: " : (Each.Severity == ESeverity::Warning ? "warning: " : "note: ");
            Text += Each.Message;
            if(Each.Node.IsSet()) { Text += " [node " + ShortGuid(Each.Node) + "]"; }
        }
        return Text;
    }
}
