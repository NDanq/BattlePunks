#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Private/NodeGraph/CodeWriter.h"
#include "Private/NodeGraph/GraphIndex.h"
#include "Private/NodeGraph/NodeGraphLog.h"

/*
 * Script graphs -> a Lua class table, the shape Games/Sandbox/Scripts/Mover.lua
 * has by hand:
 *
 *     return {
 *         TickPhase = TickPhase.PostPhysics,
 *         Speed = 60,                              -- graph variables
 *         OnStart = function(self) ... end,
 *         OnTick = function(self, dt) ... end,
 *     }
 *
 * so the runtime needs nothing graph-specific.
 *
 * EXEC FLOW AND VALUES. Event nodes start a function; "Then" chains
 * statements. Pure nodes are re-evaluated per statement that uses them (so a
 * Get after a Set sees the new value) into `local tN`. A statement's outputs
 * are function-level locals.
 *
 * LANES. As in the Lua bindings, Vector3 / Color / AABB are 3 / 4 / 6
 * numbers, never tables; vector math is written per component.
 *
 * FUNCTIONS. A graph's own functions (Graph::Functions) are locals above the
 * table, declared first so they may call each other and themselves:
 *
 *     local Fn_Clamp
 *     Fn_Clamp = function(self, In_Value, In_Min, In_Max)
 *         ...
 *         do return r1 end
 *         return 0                                 -- fell off the end: zeros
 *     end
 *
 * The body is the chain from the function's Entry node; a Call node is
 * `local Out1 = Fn_Clamp(self, ...)`. Their nodes' pins come from the
 * function's signature (ScriptSchema::FindDefinition).
 */
namespace NodeGraph
{
namespace
{
    using Lanes = std::vector<std::string>;

    enum EScriptOp : uint32_t
    {
        SOP_Call = 0,     // Template, {Pin} = the input's lanes joined; outputs are its results in order
        SOP_Lanewise,     // Template per lane of the one output; a one-lane input is used for every lane
        SOP_Compose,      // the output's lanes are the inputs' lanes, concatenated
        SOP_Decompose,    // each output takes the next lanes of the one input
        SOP_Self,
        SOP_VarGet,
        SOP_VarSet,
        SOP_Branch,
        SOP_Sequence,
        SOP_ForLoop,
        SOP_Return,
        SOP_Emit,
        SOP_Length,
        SOP_Normalize,
        SOP_Dot,
        SOP_EventStart,
        SOP_EventTick,
        SOP_EventDestroy,
        SOP_EventOverlapBegin,
        SOP_EventOverlapEnd,
        SOP_EventCustom,
        SOP_FunctionEntry,
        SOP_FunctionReturn,
        SOP_FunctionCall,
        SOP_EventPhase,   // On <Phase>: the phase's name in the definition's Template
        SOP_UIOpen,       // a UI graph's On Document Open
        SOP_UIClose,      // On Document Close
        SOP_UIHandler,    // UI Event: a Graph: action's handler, named by its Handler property
    };

    /* What a script graph is the logic of: a prefab's entities, or a UI
     * document (a *.ui.nmgraph, its Graph setting -- UI/README.md). */
    enum class EScriptFlavour : uint8_t { Entity, UI };

    const char* const HANDLER_PROPERTY    = "Handler";
    const char* const VALUE_TYPE_PROPERTY = "ValueType";

    const char* const EXEC_IN  = "Exec";
    const char* const EXEC_OUT = "Then";
    const char* const COMPONENT_TYPE_PREFIX = "Component:";
    const char* const HANDLES_FIELD = "GraphEventHandles";
    const char* const FUNCTION_PROPERTY = "Function";

    constexpr int CUSTOM_EVENT_ARGS = 4;   // EventArgs holds four (EventBus.h)

    const std::vector<std::string> GVariableTypes = { "number", "integer", "bool", "string", "Vector3", "Color" };
    const std::vector<std::string> GTickPhases    = { "PrePhysics", "Physics", "PostPhysics", "PostUpdate" };

    bool IsComponentType(const std::string& InType)
    {
        return InType.compare(0, std::strlen(COMPONENT_TYPE_PREFIX), COMPONENT_TYPE_PREFIX) == 0;
    }

    /* A script struct's lanes (GameCore/ScriptStruct.h): one per Lua value,
     * named after its fields ("Value", "Where_X"), each of a one-value type. */
    struct StructLayout
    {
        std::vector<std::string>                         LaneNames;
        std::vector<std::string>                         LaneTypes;   // number, integer, bool or string
        std::vector<std::pair<std::string, std::string>> Fields;      // name, pin type
    };

    /* By struct name. Filled when a schema is built and never cleared, so a
     * graph still open on a retired schema compiles after a refresh, without
     * its module loaded. */
    std::map<std::string, StructLayout>& StructLayouts()
    {
        static std::map<std::string, StructLayout> Layouts;
        return Layouts;
    }

    const StructLayout* FindStructLayout(const std::string& InType)
    {
        const auto Found = StructLayouts().find(InType);
        return Found == StructLayouts().end() ? nullptr : &Found->second;
    }

    /* The lane suffixes of a multi-lane type; empty for a one-lane type. A
     * struct's are its lanes' names, even when it has one. */
    const std::vector<std::string>& LaneNames(const std::string& InType)
    {
        static const std::vector<std::string> One;
        static const std::vector<std::string> Vector = { "X", "Y", "Z" };
        static const std::vector<std::string> Color  = { "R", "G", "B", "A" };
        static const std::vector<std::string> Box    = { "MinX", "MinY", "MinZ", "MaxX", "MaxY", "MaxZ" };
        if(InType == "Vector3") { return Vector; }
        if(InType == "Color")   { return Color; }
        if(InType == "Box")     { return Box; }
        if(const StructLayout* Layout = FindStructLayout(InType)) { return Layout->LaneNames; }
        return One;
    }

    size_t LaneCount(const std::string& InType)
    {
        const size_t Count = LaneNames(InType).size();
        return Count == 0 ? 1 : Count;
    }

    /* "Name" for one lane, "Name_X", "Name_Y", ... for several. */
    Lanes LaneVariables(const std::string& InBase, const std::string& InType)
    {
        const std::vector<std::string>& Names = LaneNames(InType);
        if(Names.empty()) { return { InBase }; }

        Lanes Result;
        for(const std::string& Suffix : Names) { Result.push_back(InBase + "_" + Suffix); }
        return Result;
    }

    std::string Join(const Lanes& InLanes, const char* InSeparator = ", ")
    {
        std::string Out;
        for(size_t Index = 0; Index < InLanes.size(); ++Index)
        {
            if(Index > 0) { Out += InSeparator; }
            Out += InLanes[Index];
        }
        return Out;
    }

    std::string LuaString(const std::string& InText)
    {
        std::string Out = "\"";
        for(const unsigned char Ch : InText)
        {
            switch(Ch)
            {
                case '\\': Out += "\\\\"; break;
                case '"':  Out += "\\\""; break;
                case '\n': Out += "\\n";  break;
                case '\r': Out += "\\r";  break;
                case '\t': Out += "\\t";  break;
                default:
                    if(Ch < 0x20 || Ch == 0x7F) { Out += "\\" + std::to_string(unsigned(Ch)); }
                    else                        { Out += char(Ch); }
                    break;
            }
        }
        return Out + "\"";
    }

    std::string LuaNumber(const double InValue)
    {
        return FormatNumber(InValue);
    }

    std::string ScriptTypeOf(const GameCore::EScriptFieldType InType, const GameCore::ScriptStructID InStruct)
    {
        switch(InType)
        {
            case GameCore::EScriptFieldType::Struct:      return GameCore::GetScriptStructName(InStruct);
            case GameCore::EScriptFieldType::Float:
            case GameCore::EScriptFieldType::Double:      return "number";
            case GameCore::EScriptFieldType::Int32:
            case GameCore::EScriptFieldType::UInt8:       return "integer";
            case GameCore::EScriptFieldType::Bool:
            case GameCore::EScriptFieldType::Flag:        return "bool";
            case GameCore::EScriptFieldType::Vector3:     return "Vector3";
            case GameCore::EScriptFieldType::AABB:        return "Box";
            case GameCore::EScriptFieldType::Color:       return "Color";
            case GameCore::EScriptFieldType::FixedString: return "string";
        }
        return "Any";
    }

    std::string Trim(const std::string& InText)
    {
        const size_t Begin = InText.find_first_not_of(" \t");
        if(Begin == std::string::npos) { return std::string(); }
        const size_t End = InText.find_last_not_of(" \t");
        return InText.substr(Begin, End - Begin + 1);
    }

    /* "Name:type, Name:type" -> pins. False if any entry is malformed. */
    bool ParseSignatureList(const std::string& InText, const EPinDirection InDirection, std::vector<PinDefinition>& OutPins)
    {
        size_t Pos = 0;
        const std::string Text = Trim(InText);
        if(Text.empty()) { return true; }

        while(Pos <= Text.size())
        {
            size_t Comma = Text.find(',', Pos);
            if(Comma == std::string::npos) { Comma = Text.size(); }
            const std::string Entry = Trim(Text.substr(Pos, Comma - Pos));
            const size_t Colon = Entry.find(':');
            if(Colon == std::string::npos) { return false; }

            PinDefinition Pin;
            Pin.Name      = Trim(Entry.substr(0, Colon));
            Pin.Type      = Trim(Entry.substr(Colon + 1));
            Pin.Direction = InDirection;
            if(Pin.Name.empty()) { return false; }
            if(Pin.Type != "number" && Pin.Type != "integer" && Pin.Type != "bool" && Pin.Type != "string"
               && Pin.Type != "Vector3" && Pin.Type != "Color" && Pin.Type != "Entity" && Pin.Type != "Box"
               && !FindStructLayout(Pin.Type))
            {
                return false;
            }
            OutPins.push_back(std::move(Pin));
            Pos = Comma + 1;
        }
        return true;
    }

    /* "Params -> Results", either half optional; false if empty or malformed. */
    bool ParseSignature(const std::string& InSignature, std::vector<PinDefinition>& OutParameters, std::vector<PinDefinition>& OutResults)
    {
        const size_t Arrow = InSignature.find("->");
        return !InSignature.empty()
            && ParseSignatureList(InSignature.substr(0, Arrow), EPinDirection::Input, OutParameters)
            && (Arrow == std::string::npos || ParseSignatureList(InSignature.substr(Arrow + 2), EPinDirection::Output, OutResults));
    }

    /* "{A}, {B}" -- a call's argument list, for ExpandCall. */
    std::string ArgumentList(const std::vector<PinDefinition>& InParameters)
    {
        std::string Arguments;
        for(const PinDefinition& Each : InParameters)
        {
            Arguments += (Arguments.empty() ? "{" : ", {") + Each.Name + "}";
        }
        return Arguments;
    }

    const char* const NO_SIGNATURE_TOOLTIP = "has no signature in its binding, so it is called with no arguments";

    class ScriptSchema final : public Schema
    {
    public:
        explicit ScriptSchema(EScriptFlavour InFlavour);
        const NodeDefinition* FindDefinition(const Graph& InGraph, const Node& InNode) const override;
        std::string ResolveWildcard(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef) const override;
        std::string DescribeNode(const Graph& InGraph, const Node& InNode) const override;
        void Compile(const Graph& InGraph, CompileResult& OutResult) const override;

        bool IsUI() const { return Flavour == EScriptFlavour::UI; }

    private:
        EScriptFlavour Flavour;

        void AddComponentNodes();
        void AddLibraryNodes();
        std::vector<std::string> AddStructTypes();

        /* Function nodes' definitions, one per node type and signature: built
         * on first use and kept, so a pointer handed out stays valid. Asked
         * for from the editor and from compiles, hence the lock. */
        mutable std::mutex                            FunctionDefinitionsLock;
        mutable std::map<std::string, NodeDefinition> FunctionDefinitions;
    };

    // === The compiler ===========================================================

    class ScriptCompiler
    {
    public:
        ScriptCompiler(const Graph& InGraph, const Schema& InSchema, CompileResult& InResult, const bool bInUI)
            : Index(InGraph, InSchema), Result(InResult), bUI(bInUI) {}

        void Run();

    private:
        /* A UI graph's table: OnOpen / OnClose / OnTick and its handlers. */
        void RunUI();

        /* One generated function. */
        struct Scope
        {
            CodeWriter               Body;
            std::vector<std::string> Declared;    // statement outputs, declared `local` at the top
            std::map<PinRef, Lanes>  Available;   // statement outputs and event parameters readable here
            std::set<NodeGuid>       ExecPath;    // the statements on the current path, to catch a loop
            int                      NextTemp = 0;
            const GraphFunction*     Function = nullptr;   // the graph function this is the body of, if any
        };

        using ValueCache = std::map<PinRef, Lanes>;

        struct Resolved
        {
            const Node*           N   = nullptr;
            const NodeDefinition* Def = nullptr;
        };

        Resolved Resolve(const NodeGuid& InId) const;

        void EmitFunction(CodeWriter& Out, const std::string& InHeader, const Node* InEvent,
                          const std::vector<std::string>& InPrologue, const std::vector<std::string>& InEpilogue,
                          const std::map<std::string, Lanes>& InParameters,
                          const GraphFunction* InFunction = nullptr, const char* InClose = "end,");
        void EmitGraphFunctions(CodeWriter& Out);
        void EmitChain(Scope& S, const Node& InFrom, const std::string& InExecPin);
        void EmitStatement(Scope& S, const Node& InNode, const NodeDefinition& InDef);

        Lanes Input(Scope& S, ValueCache& Cache, const Node& InNode, const NodeDefinition& InDef, const std::string& InPin);
        Lanes Output(Scope& S, ValueCache& Cache, const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin);
        Lanes Literal(const Node& InNode, const PinDefinition& InPin, const std::string& InType);
        Lanes Convert(const Lanes& InValue, const std::string& InFrom, const std::string& InTo);

        std::string ExpandCall(Scope& S, ValueCache& Cache, const Node& InNode, const NodeDefinition& InDef);
        std::string Temp(Scope& S) { return "t" + std::to_string(++S.NextTemp); }
        std::string StatementOutputBase(const Node& InNode, const PinDefinition& InPin);

        Lanes VariableLanes(const std::string& InName) const;

        /* A graph function's local: Fn_<Name>. */
        static std::string FunctionLocal(const std::string& InName) { return "Fn_" + SanitizeIdentifier(InName); }

        /* What InFunction returns when it ends without a Return: each output's
         * type default (nil for an Entity). Empty for a function without outputs. */
        Lanes DefaultResults(const GraphFunction& InFunction);

        GraphIndex     Index;
        CompileResult& Result;
        bool           bUI = false;

        std::map<NodeGuid, int> StatementNumbers;   // for stable, readable local names
    };

    ScriptCompiler::Resolved ScriptCompiler::Resolve(const NodeGuid& InId) const
    {
        Resolved Out;
        Out.N   = Index.FindNode(InId);
        Out.Def = Out.N ? Index.FindDefinition(*Out.N) : nullptr;
        return Out;
    }

    Lanes ScriptCompiler::VariableLanes(const std::string& InName) const
    {
        const Variable* Var = Index.GetGraph().FindVariable(InName);
        const Lanes Names = LaneVariables(InName, Var ? Var->Type : std::string());
        Lanes Result;
        for(const std::string& Each : Names) { Result.push_back("self." + Each); }
        return Result;
    }

    void ScriptCompiler::Run()
    {
        if(bUI)
        {
            RunUI();
            return;
        }
        const Graph& G = Index.GetGraph();

        /* Names the generated table uses itself; a variable may not take one. */
        static const std::set<std::string> Reserved = {
            "Entity", "Root", "Instance", "TickPhase", "OnStart", "OnTick", "OnDestroy",
            "OnPrePhysics", "OnPhysics", "OnPostPhysics", "OnPostUpdate",
            "OnOverlapBegin", "OnOverlapEnd", HANDLES_FIELD
        };
        for(const Variable& Each : G.Variables)
        {
            if(Reserved.count(Each.Name) || Each.Name.compare(0, 6, "Event_") == 0)
            {
                Result.Error(NodeGuid(), std::string(), "variable '" + Each.Name + "' has a name the generated script uses itself");
            }
        }

        const Node* StartNode = nullptr;
        const Node* TickNode = nullptr;
        const Node* DestroyNode = nullptr;
        const Node* OverlapBeginNode = nullptr;
        const Node* OverlapEndNode = nullptr;
        std::map<std::string, const Node*> PhaseNodes;   // phase name -> its On <Phase> node
        std::vector<const Node*> CustomEvents;
        for(const Node* Each : Index.SortedNodes())
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            if(!Def) { continue; }
            switch(Def->Op)
            {
                case SOP_EventStart:   StartNode = Each;   break;
                case SOP_EventTick:    TickNode = Each;    break;
                case SOP_EventPhase:   PhaseNodes[Def->Template] = Each; break;
                case SOP_EventDestroy: DestroyNode = Each; break;
                case SOP_EventOverlapBegin: OverlapBeginNode = Each; break;
                case SOP_EventOverlapEnd:   OverlapEndNode = Each;   break;
                case SOP_EventCustom:  CustomEvents.push_back(Each); break;
                default: break;
            }
        }

        if(!StartNode && !TickNode && !DestroyNode && !OverlapBeginNode && !OverlapEndNode && CustomEvents.empty()
           && PhaseNodes.empty())
        {
            Result.Warning(NodeGuid(), std::string(), "the graph has no event node, so the script never runs anything");
        }

        // --- Custom event handler names -----------------------------------------
        /* On Event names its event in a property; a library's typed On <Name>
         * in its definition's Template. */
        struct Handler
        {
            const Node* N = nullptr;
            std::string Method;
            std::string EventName;
        };
        std::vector<Handler> Handlers;
        std::map<std::string, int> HandlerUse;
        for(const Node* Each : CustomEvents)
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            const std::string EventName = Def->Template.empty() ? Each->GetProperty("EventName") : Def->Template;
            if(EventName.empty())
            {
                Result.Error(Each->Id, std::string(), "'On Event' needs an event name");
                continue;
            }
            std::string Method = "Event_" + SanitizeIdentifier(EventName);
            const int Use = ++HandlerUse[Method];
            if(Use > 1) { Method += "_" + std::to_string(Use); }
            Handlers.push_back({ Each, Method, EventName });
        }

        CodeWriter Out;
        Out.Line("-- Generated by NimbleEditor from the script graph \"" + G.Name + "\". Do not edit.");
        EmitGraphFunctions(Out);
        Out.Line("return {");
        Out.Indent();
        Out.Line("TickPhase = TickPhase." + G.GetProperty("TickPhase", "PrePhysics") + ",");

        if(!G.Variables.empty())
        {
            Out.Blank();
            for(const Variable& Each : G.Variables)
            {
                PinDefinition Pin;
                Pin.Name = Each.Name;
                Node Holder;
                Holder.Constants[Each.Name] = Each.Default;
                const Lanes Values = Literal(Holder, Pin, Each.Type);
                const Lanes Names  = LaneVariables(Each.Name, Each.Type);
                for(size_t Lane = 0; Lane < Names.size() && Lane < Values.size(); ++Lane)
                {
                    Out.Line(Names[Lane] + " = " + Values[Lane] + ",");
                }
            }
        }

        // --- OnStart: subscriptions, then the user's chain --------------------------
        if(StartNode || !Handlers.empty())
        {
            std::vector<std::string> Prologue;
            if(!Handlers.empty())
            {
                Prologue.push_back("self." + std::string(HANDLES_FIELD) + " = {");
                for(const Handler& Each : Handlers)
                {
                    Prologue.push_back("    Events.On(" + LuaString(Each.EventName)
                                       + ", function(...) self:" + Each.Method + "(...) end),");
                }
                Prologue.push_back("}");
            }
            Out.Blank();
            EmitFunction(Out, "OnStart = function(self)", StartNode, Prologue, {}, {});
        }

        if(TickNode)
        {
            Out.Blank();
            EmitFunction(Out, "OnTick = function(self, dt)", TickNode, {}, {}, { { "DeltaSeconds", { "dt" } } });
        }

        /* A prefab's phase functions, in phase order. Each one present puts
         * the instance in that phase's list; an absent one costs nothing. */
        for(const std::string& Phase : GTickPhases)
        {
            const auto Found = PhaseNodes.find(Phase);
            if(Found == PhaseNodes.end()) { continue; }
            Out.Blank();
            EmitFunction(Out, "On" + Phase + " = function(self, dt)", Found->second, {}, {}, { { "DeltaSeconds", { "dt" } } });
        }

        /* Called by the engine's overlap pass (GameCore's CollisionComponent):
         * mine is the script's own entity that overlapped. */
        if(OverlapBeginNode)
        {
            Out.Blank();
            EmitFunction(Out, "OnOverlapBegin = function(self, other, mine)", OverlapBeginNode, {}, {},
                         { { "Other", { "other" } }, { "Mine", { "mine" } } });
        }
        if(OverlapEndNode)
        {
            Out.Blank();
            EmitFunction(Out, "OnOverlapEnd = function(self, other, mine)", OverlapEndNode, {}, {},
                         { { "Other", { "other" } }, { "Mine", { "mine" } } });
        }

        if(DestroyNode || !Handlers.empty())
        {
            /* Unsubscribed first: a handler must not run on a destroyed entity. */
            std::vector<std::string> Prologue;
            if(!Handlers.empty())
            {
                Prologue.push_back("for _, Handle in ipairs(self." + std::string(HANDLES_FIELD) + " or {}) do Events.Off(Handle) end");
                Prologue.push_back("self." + std::string(HANDLES_FIELD) + " = nil");
            }
            Out.Blank();
            EmitFunction(Out, "OnDestroy = function(self)", DestroyNode, Prologue, {}, {});
        }

        for(const Handler& Each : Handlers)
        {
            /* The node's data outputs are the event's arguments, in order. An
             * integer arrives from the bus as a float, so it is floored. */
            std::map<std::string, Lanes> Parameters;
            std::string Header = Each.Method + " = function(self";
            for(int Arg = 1; Arg <= CUSTOM_EVENT_ARGS; ++Arg) { Header += ", Arg" + std::to_string(Arg); }

            int Arg = 0;
            for(const PinDefinition& Pin : Index.FindDefinition(*Each.N)->Pins)
            {
                if(Pin.IsExec() || !Pin.IsOutput() || ++Arg > CUSTOM_EVENT_ARGS) { continue; }
                const std::string Name = "Arg" + std::to_string(Arg);
                Parameters[Pin.Name] = { Pin.Type == "integer" ? "math.floor(" + Name + " or 0)" : Name };
            }
            Out.Blank();
            EmitFunction(Out, Header + ")", Each.N, {}, {}, Parameters);
        }

        Out.Outdent();
        Out.Line("}");

        Result.Code      = std::move(Out.Text);
        Result.LineNodes = std::move(Out.LineNodes);
    }

    void ScriptCompiler::RunUI()
    {
        const Graph& G = Index.GetGraph();

        /* Names the generated table uses itself: neither a variable nor a handler may take one. */
        static const std::set<std::string> Reserved = { "Document", "OnOpen", "OnClose", "OnTick", HANDLES_FIELD };
        const auto IsReserved = [](const std::string& InName)
        {
            return Reserved.count(InName) || InName.compare(0, 6, "Event_") == 0;
        };
        std::set<std::string> Variables;
        for(const Variable& Each : G.Variables)
        {
            Variables.insert(Each.Name);
            if(IsReserved(Each.Name))
            {
                Result.Error(NodeGuid(), std::string(), "variable '" + Each.Name + "' has a name the generated script uses itself");
            }
        }

        const Node* OpenNode = nullptr;
        const Node* CloseNode = nullptr;
        const Node* TickNode = nullptr;
        std::vector<const Node*> UIHandlers;
        std::vector<const Node*> CustomEvents;
        for(const Node* Each : Index.SortedNodes())
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            if(!Def) { continue; }
            switch(Def->Op)
            {
                case SOP_UIOpen:        OpenNode = Each;  break;
                case SOP_UIClose:       CloseNode = Each; break;
                case SOP_EventTick:     TickNode = Each;  break;
                case SOP_UIHandler:     UIHandlers.push_back(Each); break;
                case SOP_EventCustom:   CustomEvents.push_back(Each); break;
                default: break;
            }
        }
        if(!OpenNode && !CloseNode && !TickNode && UIHandlers.empty() && CustomEvents.empty())
        {
            Result.Warning(NodeGuid(), std::string(), "the graph has no event node, so the document's logic never runs anything");
        }

        /* UI Event handlers: their names are what Graph: actions call. */
        std::set<std::string> HandlerNames;
        for(const Node* Each : UIHandlers)
        {
            const std::string Name = Each->GetProperty(HANDLER_PROPERTY, "Handler");
            if(Name.empty() || SanitizeIdentifier(Name) != Name)
            {
                Result.Error(Each->Id, std::string(), "'" + Name + "' is not a handler name (letters, digits and _)");
            }
            else if(IsReserved(Name) || Variables.count(Name))
            {
                Result.Error(Each->Id, std::string(), "'" + Name + "' is a name the generated script already uses");
            }
            else if(!HandlerNames.insert(Name).second)
            {
                Result.Error(Each->Id, std::string(), "two UI Events are named '" + Name + "'");
            }
        }

        /* On Event subscriptions live while the document is open. */
        struct Subscription
        {
            const Node* N = nullptr;
            std::string Method;
            std::string EventName;
        };
        std::vector<Subscription> Subscriptions;
        std::map<std::string, int> MethodUse;
        for(const Node* Each : CustomEvents)
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            const std::string EventName = Def->Template.empty() ? Each->GetProperty("EventName") : Def->Template;
            if(EventName.empty())
            {
                Result.Error(Each->Id, std::string(), "'On Event' needs an event name");
                continue;
            }
            std::string Method = "Event_" + SanitizeIdentifier(EventName);
            const int Use = ++MethodUse[Method];
            if(Use > 1) { Method += "_" + std::to_string(Use); }
            Subscriptions.push_back({ Each, Method, EventName });
        }

        CodeWriter Out;
        Out.Line("-- Generated by NimbleEditor from the UI graph \"" + G.Name + "\". Do not edit.");
        EmitGraphFunctions(Out);
        Out.Line("return {");
        Out.Indent();
        for(const Variable& Each : G.Variables)
        {
            PinDefinition Pin;
            Pin.Name = Each.Name;
            Node Holder;
            Holder.Constants[Each.Name] = Each.Default;
            const Lanes Values = Literal(Holder, Pin, Each.Type);
            const Lanes Names  = LaneVariables(Each.Name, Each.Type);
            for(size_t Lane = 0; Lane < Names.size() && Lane < Values.size(); ++Lane)
            {
                Out.Line(Names[Lane] + " = " + Values[Lane] + ",");
            }
        }

        if(OpenNode || !Subscriptions.empty())
        {
            std::vector<std::string> Prologue;
            if(!Subscriptions.empty())
            {
                Prologue.push_back("self." + std::string(HANDLES_FIELD) + " = {");
                for(const Subscription& Each : Subscriptions)
                {
                    Prologue.push_back("    Events.On(" + LuaString(Each.EventName) + ", function(...) self:" + Each.Method + "(...) end),");
                }
                Prologue.push_back("}");
            }
            Out.Blank();
            EmitFunction(Out, "OnOpen = function(self)", OpenNode, Prologue, {}, {});
        }

        if(CloseNode || !Subscriptions.empty())
        {
            std::vector<std::string> Prologue;
            if(!Subscriptions.empty())
            {
                Prologue.push_back("for _, Handle in ipairs(self." + std::string(HANDLES_FIELD) + " or {}) do Events.Off(Handle) end");
                Prologue.push_back("self." + std::string(HANDLES_FIELD) + " = nil");
            }
            Out.Blank();
            EmitFunction(Out, "OnClose = function(self)", CloseNode, Prologue, {}, {});
        }

        if(TickNode)
        {
            Out.Blank();
            EmitFunction(Out, "OnTick = function(self, dt)", TickNode, {}, {}, { { "DeltaSeconds", { "dt" } } });
        }

        /* Graph:<Handler>(value, sender), from UISystem. A check box's value is 1 or 0. */
        for(const Node* Each : UIHandlers)
        {
            const std::string Name = Each->GetProperty(HANDLER_PROPERTY, "Handler");
            if(Name.empty() || SanitizeIdentifier(Name) != Name) { continue; }
            const bool bBool = Each->GetProperty(VALUE_TYPE_PROPERTY) == "Bool";
            Out.Blank();
            EmitFunction(Out, Name + " = function(self, Value, Sender)", Each, {}, {},
                         { { "Value", { bBool ? "(Value ~= 0)" : "Value" } }, { "Sender", { "Sender" } } });
        }

        for(const Subscription& Each : Subscriptions)
        {
            std::map<std::string, Lanes> Parameters;
            std::string Header = Each.Method + " = function(self";
            for(int Arg = 1; Arg <= CUSTOM_EVENT_ARGS; ++Arg) { Header += ", Arg" + std::to_string(Arg); }
            int Arg = 0;
            for(const PinDefinition& Pin : Index.FindDefinition(*Each.N)->Pins)
            {
                if(Pin.IsExec() || !Pin.IsOutput() || ++Arg > CUSTOM_EVENT_ARGS) { continue; }
                const std::string ArgName = "Arg" + std::to_string(Arg);
                Parameters[Pin.Name] = { Pin.Type == "integer" ? "math.floor(" + ArgName + " or 0)" : ArgName };
            }
            Out.Blank();
            EmitFunction(Out, Header + ")", Each.N, {}, {}, Parameters);
        }

        Out.Outdent();
        Out.Line("}");

        Result.Code      = std::move(Out.Text);
        Result.LineNodes = std::move(Out.LineNodes);
    }

    void ScriptCompiler::EmitFunction(CodeWriter& Out, const std::string& InHeader, const Node* InEvent,
                                      const std::vector<std::string>& InPrologue, const std::vector<std::string>& InEpilogue,
                                      const std::map<std::string, Lanes>& InParameters,
                                      const GraphFunction* InFunction, const char* InClose)
    {
        Scope S;
        S.Function = InFunction;
        if(InEvent)
        {
            for(const auto& Each : InParameters) { S.Available[PinRef{ InEvent->Id, Each.first }] = Each.second; }
            EmitChain(S, *InEvent, EXEC_OUT);
        }

        const NodeGuid Owner = InEvent ? InEvent->Id : NodeGuid();
        Out.Line(InHeader, Owner);
        Out.Indent();
        for(const std::string& Line : InPrologue) { Out.Line(Line, Owner); }
        if(!S.Declared.empty()) { Out.Line("local " + Join(S.Declared), Owner); }
        Out.Append(S.Body);
        for(const std::string& Line : InEpilogue) { Out.Line(Line, Owner); }
        Out.Outdent();
        Out.Line(InClose, Owner);
    }

    void ScriptCompiler::EmitGraphFunctions(CodeWriter& Out)
    {
        const Graph& G = Index.GetGraph();
        if(G.Functions.empty()) { return; }

        /* Each function's Entry, and whether anything calls it. */
        std::map<std::string, const Node*> Entries;
        std::set<std::string> Called;
        for(const Node* Each : Index.SortedNodes())
        {
            const NodeDefinition* Def = Index.FindDefinition(*Each);
            if(!Def) { continue; }
            const std::string Name = Each->GetProperty(FUNCTION_PROPERTY);
            if(Def->Op == SOP_FunctionCall) { Called.insert(Name); }
            if(Def->Op != SOP_FunctionEntry) { continue; }

            if(!Entries.emplace(Name, Each).second)
            {
                Result.Error(Each->Id, std::string(), "function '" + Name + "' has more than one Entry node");
            }
        }

        /* An Entity, a struct or any variable type; never a component or Any. */
        const std::vector<std::string>& VariableTypes = Index.GetSchema().GetVariableTypes();
        const auto IsAllowed = [&](const std::string& InType)
        {
            return InType == "Entity" || std::find(VariableTypes.begin(), VariableTypes.end(), InType) != VariableTypes.end();
        };

        std::vector<std::string> Locals;
        for(const GraphFunction& Each : G.Functions) { Locals.push_back(FunctionLocal(Each.Name)); }
        Out.Line("local " + Join(Locals));

        for(const GraphFunction& Function : G.Functions)
        {
            const auto Entry = Entries.find(Function.Name);
            if(Entry == Entries.end())
            {
                Result.Error(NodeGuid(), std::string(), "function '" + Function.Name + "' has no Entry node to start its body");
                continue;
            }
            if(!Called.count(Function.Name))
            {
                Result.Warning(Entry->second->Id, std::string(), "function '" + Function.Name + "' is never called");
            }

            for(const std::vector<FunctionPin>* List : { &Function.Inputs, &Function.Outputs })
            {
                for(const FunctionPin& Pin : *List)
                {
                    if(Pin.Name == EXEC_IN || Pin.Name == EXEC_OUT)
                    {
                        Result.Error(Entry->second->Id, std::string(), "function '" + Function.Name + "': '" + Pin.Name
                                     + "' is the name of an execution pin");
                    }
                    if(!IsAllowed(Pin.Type))
                    {
                        Result.Error(Entry->second->Id, std::string(), "function '" + Function.Name + "': '" + Pin.Name
                                     + "' has the type '" + Pin.Type + "', which a function cannot take or give");
                    }
                }
            }

            /* Each input is its lanes, as parameters: In_Pos_X, In_Pos_Y, ... */
            std::string Header = FunctionLocal(Function.Name) + " = function(self";
            std::map<std::string, Lanes> Parameters;
            for(const FunctionPin& Pin : Function.Inputs)
            {
                const Lanes Names = LaneVariables("In_" + SanitizeIdentifier(Pin.Name), Pin.Type);
                for(const std::string& Lane : Names) { Header += ", " + Lane; }
                Parameters[Pin.Name] = Names;
            }

            std::vector<std::string> Epilogue;
            const Lanes Defaults = DefaultResults(Function);
            if(!Defaults.empty()) { Epilogue.push_back("return " + Join(Defaults)); }

            EmitFunction(Out, Header + ")", Entry->second, {}, Epilogue, Parameters, &Function, "end");
        }
        Out.Blank();
    }

    Lanes ScriptCompiler::DefaultResults(const GraphFunction& InFunction)
    {
        Lanes Out;
        const Node NoValues;
        for(const FunctionPin& Each : InFunction.Outputs)
        {
            PinDefinition Pin;
            Pin.Name = Each.Name;
            const Lanes Value = Each.Type == "Entity" ? Lanes{ "nil" } : Literal(NoValues, Pin, Each.Type);
            Out.insert(Out.end(), Value.begin(), Value.end());
        }
        return Out;
    }

    void ScriptCompiler::EmitChain(Scope& S, const Node& InFrom, const std::string& InExecPin)
    {
        const std::vector<const Link*>& Next = Index.LinksOutOf(InFrom.Id, InExecPin);
        if(Next.empty()) { return; }

        const Resolved Target = Resolve(Next.front()->To.Node);
        if(!Target.Def) { return; }
        if(Target.Def->Role != ENodeRole::Statement)
        {
            Result.Error(Target.N->Id, std::string(), "'" + Target.Def->DisplayName + "' cannot be run from an execution pin");
            return;
        }
        EmitStatement(S, *Target.N, *Target.Def);
    }

    std::string ScriptCompiler::StatementOutputBase(const Node& InNode, const PinDefinition& InPin)
    {
        const auto Found = StatementNumbers.find(InNode.Id);
        const int Number = Found != StatementNumbers.end()
                         ? Found->second
                         : StatementNumbers.emplace(InNode.Id, int(StatementNumbers.size()) + 1).first->second;
        return SanitizeIdentifier(InPin.Name) + std::to_string(Number);
    }

    void ScriptCompiler::EmitStatement(Scope& S, const Node& InNode, const NodeDefinition& InDef)
    {
        if(!S.ExecPath.insert(InNode.Id).second)
        {
            Result.Error(InNode.Id, std::string(), "execution loops back to '" + InDef.DisplayName + "'; use a For Loop to repeat");
            return;
        }

        ValueCache Cache;
        CodeWriter& W = S.Body;

        switch(InDef.Op)
        {
            case SOP_Branch:
            {
                const Lanes Condition = Input(S, Cache, InNode, InDef, "Condition");
                W.Line("if " + Condition[0] + " then", InNode.Id);
                W.Indent();
                EmitChain(S, InNode, "True");
                W.Outdent();
                if(!Index.LinksOutOf(InNode.Id, "False").empty())
                {
                    W.Line("else", InNode.Id);
                    W.Indent();
                    EmitChain(S, InNode, "False");
                    W.Outdent();
                }
                W.Line("end", InNode.Id);
                break;
            }

            case SOP_Sequence:
            {
                for(const PinDefinition& Pin : InDef.Pins)
                {
                    if(Pin.IsExec() && Pin.IsOutput()) { EmitChain(S, InNode, Pin.Name); }
                }
                break;
            }

            case SOP_ForLoop:
            {
                const Lanes First = Input(S, Cache, InNode, InDef, "First");
                const Lanes Last  = Input(S, Cache, InNode, InDef, "Last");
                const PinDefinition* IndexPin = InDef.FindPin("Index", EPinDirection::Output);
                const std::string Counter = StatementOutputBase(InNode, *IndexPin);

                W.Line("for " + Counter + " = " + First[0] + ", " + Last[0] + " do", InNode.Id);
                W.Indent();
                S.Available[PinRef{ InNode.Id, "Index" }] = { Counter };
                EmitChain(S, InNode, "Body");
                S.Available.erase(PinRef{ InNode.Id, "Index" });   // the loop variable ends with the loop
                W.Outdent();
                W.Line("end", InNode.Id);
                EmitChain(S, InNode, "Completed");
                break;
            }

            case SOP_Return:
            {
                /* In a function body, a plain Return gives the defaults. */
                const Lanes Defaults = S.Function ? DefaultResults(*S.Function) : Lanes();
                W.Line(Defaults.empty() ? "do return end" : "do return " + Join(Defaults) + " end", InNode.Id);
                break;
            }

            case SOP_FunctionReturn:
            {
                const std::string Name = InNode.GetProperty(FUNCTION_PROPERTY);
                if(!S.Function || S.Function->Name != Name)
                {
                    Result.Error(InNode.Id, std::string(), "'" + InDef.DisplayName + "' is reached from outside function '" + Name
                                 + "'; it can only end that function's own body");
                    break;
                }

                Lanes Values;
                for(const PinDefinition& Pin : InDef.Pins)
                {
                    if(Pin.IsExec() || !Pin.IsInput()) { continue; }
                    const Lanes Value = Input(S, Cache, InNode, InDef, Pin.Name);
                    Values.insert(Values.end(), Value.begin(), Value.end());
                }
                W.Line(Values.empty() ? "do return end" : "do return " + Join(Values) + " end", InNode.Id);
                break;
            }

            case SOP_VarSet:
            {
                const std::string Name = InNode.GetProperty("Variable");
                const Lanes Value = Input(S, Cache, InNode, InDef, "Value");
                W.Line(Join(VariableLanes(Name)) + " = " + Join(Value), InNode.Id);
                EmitChain(S, InNode, EXEC_OUT);
                break;
            }

            case SOP_Emit:
            {
                Lanes Arguments;
                if(!InDef.Template.empty())
                {
                    /* A library's typed Emit <Name>: every parameter, linked or
                     * not, since each has a typed default. */
                    Arguments.push_back(LuaString(InDef.Template));
                    for(const PinDefinition& Pin : InDef.Pins)
                    {
                        if(Pin.IsExec() || !Pin.IsInput()) { continue; }
                        const Lanes Value = Input(S, Cache, InNode, InDef, Pin.Name);
                        Arguments.insert(Arguments.end(), Value.begin(), Value.end());
                    }
                    W.Line("Events.Emit(" + Join(Arguments) + ")", InNode.Id);
                    EmitChain(S, InNode, EXEC_OUT);
                    break;
                }

                /* Up to the last linked argument: a trailing nil arrives as 0. */
                const Lanes Name = Input(S, Cache, InNode, InDef, "Event");
                int LastArg = 0;
                for(int Arg = 1; Arg <= CUSTOM_EVENT_ARGS; ++Arg)
                {
                    if(Index.LinkInto(InNode.Id, "Arg " + std::to_string(Arg))) { LastArg = Arg; }
                }
                Arguments = Name;
                for(int Arg = 1; Arg <= LastArg; ++Arg)
                {
                    const Lanes Value = Input(S, Cache, InNode, InDef, "Arg " + std::to_string(Arg));
                    Arguments.insert(Arguments.end(), Value.begin(), Value.end());
                }
                W.Line("Events.Emit(" + Join(Arguments) + ")", InNode.Id);
                EmitChain(S, InNode, EXEC_OUT);
                break;
            }

            case SOP_FunctionCall:
            default:   // SOP_Call
            {
                std::string Call;
                if(InDef.Op == SOP_FunctionCall)
                {
                    /* Fn_<Name>(self, <each input's lanes>) */
                    Call = FunctionLocal(InNode.GetProperty(FUNCTION_PROPERTY)) + "(self";
                    for(const PinDefinition& Pin : InDef.Pins)
                    {
                        if(Pin.IsExec() || !Pin.IsInput()) { continue; }
                        const Lanes Value = Input(S, Cache, InNode, InDef, Pin.Name);
                        for(const std::string& Lane : Value) { Call += ", " + Lane; }
                    }
                    Call += ")";
                }
                else
                {
                    Call = ExpandCall(S, Cache, InNode, InDef);
                }

                Lanes Targets;
                for(const PinDefinition& Pin : InDef.Pins)
                {
                    if(!Pin.IsOutput() || Pin.IsExec()) { continue; }
                    const Lanes Names = LaneVariables(StatementOutputBase(InNode, Pin), Index.PinType(InNode, Pin));
                    for(const std::string& Each : Names)
                    {
                        if(std::find(S.Declared.begin(), S.Declared.end(), Each) == S.Declared.end()) { S.Declared.push_back(Each); }
                    }
                    Targets.insert(Targets.end(), Names.begin(), Names.end());
                    S.Available[PinRef{ InNode.Id, Pin.Name }] = Names;
                }

                if(!InDef.Tooltip.empty() && InDef.Tooltip.find("no signature") != std::string::npos)
                {
                    Result.Warning(InNode.Id, std::string(), "'" + InDef.DisplayName + "' " + InDef.Tooltip);
                }

                W.Line(Targets.empty() ? Call : Join(Targets) + " = " + Call, InNode.Id);
                EmitChain(S, InNode, EXEC_OUT);
                break;
            }
        }

        S.ExecPath.erase(InNode.Id);
    }

    /* A Call template with each {Pin} replaced by that input's lanes. */
    std::string ScriptCompiler::ExpandCall(Scope& S, ValueCache& Cache, const Node& InNode, const NodeDefinition& InDef)
    {
        const std::string& T = InDef.Template;
        std::string Out;
        size_t Pos = 0;
        while(Pos < T.size())
        {
            const size_t Open = T.find('{', Pos);
            if(Open == std::string::npos) { Out += T.substr(Pos); break; }
            const size_t Close = T.find('}', Open);
            Out += T.substr(Pos, Open - Pos);
            const std::string Name = T.substr(Open + 1, Close - Open - 1);
            if(!Name.empty() && Name[0] == '@')
            {
                /* {@Property}: the node's setting, as a Lua string (a UI graph's widget name). */
                const PropertyDefinition* Property = InDef.FindProperty(Name.substr(1));
                Out += LuaString(InNode.GetProperty(Name.substr(1), Property ? Property->Default : std::string()));
            }
            else
            {
                Out += Join(Input(S, Cache, InNode, InDef, Name));
            }
            Pos = Close + 1;
        }
        return Out;
    }

    Lanes ScriptCompiler::Input(Scope& S, ValueCache& Cache, const Node& InNode, const NodeDefinition& InDef, const std::string& InPin)
    {
        const PinDefinition* Pin = InDef.FindPin(InPin, EPinDirection::Input);
        if(!Pin) { return { "nil" }; }
        const std::string Type = Index.PinType(InNode, *Pin);

        const Link* Into = Index.LinkInto(InNode.Id, InPin);
        if(!Into) { return Literal(InNode, *Pin, Type); }

        const Resolved Source = Resolve(Into->From.Node);
        const PinDefinition* SourcePin = Source.Def ? Source.Def->FindPin(Into->From.Pin, EPinDirection::Output) : nullptr;
        if(!SourcePin) { return Literal(InNode, *Pin, Type); }

        const Lanes Value = Output(S, Cache, *Source.N, *Source.Def, *SourcePin);
        return Convert(Value, Index.PinType(*Source.N, *SourcePin), Type);
    }

    Lanes ScriptCompiler::Output(Scope& S, ValueCache& Cache, const Node& InNode, const NodeDefinition& InDef, const PinDefinition& InPin)
    {
        const PinRef Key{ InNode.Id, InPin.Name };
        const std::string Type = Index.PinType(InNode, InPin);
        const Lanes Fallback(LaneCount(Type), "nil");

        /* A statement's or event's output: only where it has run. */
        if(InDef.Role != ENodeRole::Pure)
        {
            const auto Found = S.Available.find(Key);
            if(Found != S.Available.end()) { return Found->second; }
            Result.Error(InNode.Id, InPin.Name, "'" + InDef.DisplayName + "'s " + InPin.Name
                         + " is used where it has not run -- in another event, or outside its loop");
            return Fallback;
        }

        /* Marks a node mid-evaluation, to detect cycles. */
        static const Lanes InFlight = { "\x01" };

        const auto Cached = Cache.find(Key);
        if(Cached != Cache.end())
        {
            if(Cached->second == InFlight)
            {
                Result.Error(InNode.Id, InPin.Name, "'" + InDef.DisplayName + "' depends on its own output (a cycle)");
                return Fallback;
            }
            return Cached->second;
        }
        Cache[Key] = InFlight;

        Lanes Value;
        switch(InDef.Op)
        {
            case SOP_Self:
                Value = { "self.Entity" };
                break;

            case SOP_VarGet:
                Value = VariableLanes(InNode.GetProperty("Variable"));
                break;

            case SOP_Compose:
            {
                for(const PinDefinition& Pin : InDef.Pins)
                {
                    if(!Pin.IsInput()) { continue; }
                    const Lanes Part = Input(S, Cache, InNode, InDef, Pin.Name);
                    Value.insert(Value.end(), Part.begin(), Part.end());
                }
                break;
            }

            case SOP_Decompose:
            {
                const PinDefinition* Source = nullptr;
                for(const PinDefinition& Pin : InDef.Pins) { if(Pin.IsInput()) { Source = &Pin; break; } }
                const Lanes All = Source ? Input(S, Cache, InNode, InDef, Source->Name) : Lanes();

                size_t Offset = 0;
                for(const PinDefinition& Pin : InDef.Pins)
                {
                    if(!Pin.IsOutput()) { continue; }
                    const size_t Count = LaneCount(Index.PinType(InNode, Pin));
                    Lanes Part;
                    for(size_t Lane = 0; Lane < Count; ++Lane) { Part.push_back(Offset + Lane < All.size() ? All[Offset + Lane] : "0"); }
                    Offset += Count;
                    Cache[PinRef{ InNode.Id, Pin.Name }] = Part;
                    if(Pin.Name == InPin.Name) { Value = Part; }
                }
                return Value;
            }

            case SOP_Lanewise:
            {
                std::map<std::string, Lanes> Inputs;
                for(const PinDefinition& Pin : InDef.Pins)
                {
                    if(Pin.IsInput()) { Inputs[Pin.Name] = Input(S, Cache, InNode, InDef, Pin.Name); }
                }

                Lanes Expressions;
                for(size_t Lane = 0; Lane < LaneCount(Type); ++Lane)
                {
                    const std::string& T = InDef.Template;
                    std::string Expr;
                    size_t Pos = 0;
                    while(Pos < T.size())
                    {
                        const size_t Open = T.find('{', Pos);
                        if(Open == std::string::npos) { Expr += T.substr(Pos); break; }
                        const size_t Close = T.find('}', Open);
                        Expr += T.substr(Pos, Open - Pos);
                        const Lanes& Arg = Inputs[T.substr(Open + 1, Close - Open - 1)];
                        Expr += Arg.empty() ? "nil" : (Arg.size() == 1 ? Arg[0] : Arg[std::min(Lane, Arg.size() - 1)]);
                        Pos = Close + 1;
                    }
                    Expressions.push_back(Expr);
                }

                const Lanes Names = LaneVariables(Temp(S), Type);
                S.Body.Line("local " + Join(Names) + " = " + Join(Expressions), InNode.Id);
                Value = Names;
                break;
            }

            case SOP_Length:
            case SOP_Normalize:
            case SOP_Dot:
            {
                const bool bDot = InDef.Op == SOP_Dot;
                const Lanes A = Input(S, Cache, InNode, InDef, bDot ? "A" : "V");
                const Lanes B = bDot ? Input(S, Cache, InNode, InDef, "B") : A;
                std::string Sum;
                for(size_t Lane = 0; Lane < A.size() && Lane < B.size(); ++Lane)
                {
                    if(Lane > 0) { Sum += " + "; }
                    Sum += A[Lane] + " * " + B[Lane];
                }

                if(InDef.Op == SOP_Normalize)
                {
                    const std::string Length = Temp(S);
                    S.Body.Line("local " + Length + " = math.sqrt(" + Sum + ")", InNode.Id);
                    Lanes Parts;
                    for(const std::string& Each : A) { Parts.push_back(Length + " > 0 and " + Each + " / " + Length + " or 0"); }
                    const Lanes Names = LaneVariables(Temp(S), "Vector3");
                    S.Body.Line("local " + Join(Names) + " = " + Join(Parts), InNode.Id);
                    Value = Names;
                }
                else
                {
                    const std::string Name = Temp(S);
                    S.Body.Line("local " + Name + " = " + (bDot ? Sum : "math.sqrt(" + Sum + ")"), InNode.Id);
                    Value = { Name };
                }
                break;
            }

            default:   // SOP_Call: every output at once, from one call
            {
                const std::string Call = ExpandCall(S, Cache, InNode, InDef);
                const std::string Base = Temp(S);

                Lanes Targets;
                for(const PinDefinition& Pin : InDef.Pins)
                {
                    if(!Pin.IsOutput()) { continue; }
                    const bool bOnly = std::count_if(InDef.Pins.begin(), InDef.Pins.end(), [](const PinDefinition& P) { return P.IsOutput(); }) == 1;
                    const Lanes Names = LaneVariables(bOnly ? Base : Base + "_" + SanitizeIdentifier(Pin.Name), Index.PinType(InNode, Pin));
                    Targets.insert(Targets.end(), Names.begin(), Names.end());
                    Cache[PinRef{ InNode.Id, Pin.Name }] = Names;
                    if(Pin.Name == InPin.Name) { Value = Names; }
                }
                S.Body.Line("local " + Join(Targets) + " = " + Call, InNode.Id);
                return Value;
            }
        }

        Cache[Key] = Value;
        return Value;
    }

    Lanes ScriptCompiler::Literal(const Node& InNode, const PinDefinition& InPin, const std::string& InType)
    {
        if(InType == "Entity") { return { "self.Entity" }; }
        if(InType == "Any")    { return { "nil" }; }
        if(IsComponentType(InType))
        {
            Result.Error(InNode.Id, InPin.Name, "'" + InPin.Name + "' needs a " + InType.substr(std::strlen(COMPONENT_TYPE_PREFIX))
                         + " component linked to it");
            return { "nil" };
        }

        /* A struct has no literal of its own (Make <Struct> builds one): unlinked,
         * each lane is its type's zero. */
        if(const StructLayout* Layout = FindStructLayout(InType))
        {
            Lanes Out;
            for(const std::string& LaneType : Layout->LaneTypes)
            {
                Out.push_back(LaneType == "bool" ? "false" : LaneType == "string" ? "\"\"" : "0");
            }
            return Out;
        }

        const std::string* Given = InNode.FindConstant(InPin.Name);
        std::string Text = Given ? *Given : InPin.Default;
        if(Text.empty() && !Given)
        {
            const PinTypeInfo* Info = Index.GetSchema().FindType(InType);
            Text = Info ? Info->DefaultLiteral : std::string();
        }

        if(InType == "string") { return { LuaString(Text) }; }
        if(InType == "bool")
        {
            if(Text == "true" || Text == "1")               { return { "true" }; }
            if(Text == "false" || Text == "0" || Text.empty()) { return { "false" }; }
            Result.Error(InNode.Id, InPin.Name, "'" + Text + "' is not true or false");
            return { "false" };
        }

        const size_t Count = LaneCount(InType);
        std::vector<double> Values;
        if(!ParseNumbers(Text, Count, Values))
        {
            Result.Error(InNode.Id, InPin.Name, "'" + Text + "' is not a " + InType + " value");
            Values.assign(Count, 0.0);
        }

        Lanes Out;
        for(const double Each : Values)
        {
            if(InType == "integer")    { Out.push_back(std::to_string((long long)std::llround(Each))); }
            else if(InType == "Color") { Out.push_back(std::to_string((long long)std::llround(std::min(255.0, std::max(0.0, Each))))); }
            else                       { Out.push_back(LuaNumber(Each)); }
        }
        return Out;
    }

    Lanes ScriptCompiler::Convert(const Lanes& InValue, const std::string& InFrom, const std::string& InTo)
    {
        if(InFrom == InTo || InTo == "Any" || InFrom == "Any" || (InFrom == "integer" && InTo == "number"))
        {
            return InValue;
        }
        if(InFrom == "number" && InTo == "integer") { return { "math.floor(" + InValue[0] + ")" }; }
        if(InTo == "string")
        {
            if(InValue.size() == 1) { return { "tostring(" + InValue[0] + ")" }; }
            std::string Format;
            for(size_t Lane = 0; Lane < InValue.size(); ++Lane) { Format += Lane > 0 ? ", %g" : "%g"; }
            return { "string.format(\"(" + Format + ")\", " + Join(InValue) + ")" };
        }
        return InValue;   // ValidateGraph refused anything else
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

    PinDefinition Out(const std::string& InName, const std::string& InType)
    {
        PinDefinition Pin;
        Pin.Name      = InName;
        Pin.Direction = EPinDirection::Output;
        Pin.Type      = InType;
        return Pin;
    }

    PinDefinition ExecIn(const std::string& InName = EXEC_IN)
    {
        PinDefinition Pin;
        Pin.Name      = InName;
        Pin.Direction = EPinDirection::Input;
        Pin.Kind      = EPinKind::Exec;
        return Pin;
    }

    PinDefinition ExecOut(const std::string& InName = EXEC_OUT)
    {
        PinDefinition Pin;
        Pin.Name      = InName;
        Pin.Direction = EPinDirection::Output;
        Pin.Kind      = EPinKind::Exec;
        return Pin;
    }

    ScriptSchema::ScriptSchema(const EScriptFlavour InFlavour)
        : Schema(InFlavour == EScriptFlavour::UI ? "UI" : "Script", InFlavour == EScriptFlavour::UI ? "ui" : "script", ".lua")
        , Flavour(InFlavour)
    {
        AddType({ "number",  "Number",  0x9BD96A, "0" });
        AddType({ "integer", "Integer", 0x3FC1A0, "0" });
        AddType({ "bool",    "Boolean", 0xD95A5A, "false" });
        AddType({ "string",  "String",  0xE07AD0, "" });
        AddType({ "Vector3", "Vector3", 0xE8C95A, "0 0 0" });
        AddType({ "Color",   "Color",   0x6A8FE8, "255 255 255 255" });
        AddType({ "Box",     "Box",     0xB08A5A, "0 0 0 0 0 0" });
        AddType({ "Entity",  "Entity",  0x5AB4E8, "", false });
        AddType({ "Any",     "Any",     0xBDBDBD, "", false });

        AddConversion("integer", "number");
        AddConversion("number", "integer");
        for(const char* Each : { "number", "integer", "bool", "Entity", "Vector3", "Color", "Any" }) { AddConversion(Each, "string"); }
        for(const char* Each : { "number", "integer", "bool", "string", "Entity" })
        {
            AddConversion(Each, "Any");
            AddConversion("Any", Each);
        }

        /* Script structs first: variables and signatures may use them. */
        VariableTypes = GVariableTypes;
        for(std::string& Each : AddStructTypes()) { VariableTypes.push_back(std::move(Each)); }
        if(IsUI())
        {
            GraphProperties.push_back({ "Document", EPropertyKind::String, "", {},
                                        "The UI document this is the logic of (its Graph setting names this graph)" });
        }
        else
        {
            GraphProperties.push_back({ "TickPhase", EPropertyKind::Choice, "PrePhysics", GTickPhases,
                                        "When this script's OnTick runs relative to the World's systems" });
        }

        const auto Add = [&](const std::string& InType, const std::string& InName, const std::string& InCategory,
                             const ENodeRole InRole, const uint32_t InOp, std::vector<PinDefinition> InPins,
                             const std::string& InTemplate = std::string(), std::vector<std::string> InWildcard = {},
                             const std::string& InKeywords = std::string()) -> NodeDefinition&
        {
            NodeDefinition Def;
            Def.Type          = InType;
            Def.DisplayName   = InName;
            Def.Category      = InCategory;
            Def.Role          = InRole;
            Def.Op            = InOp;
            Def.Template      = InTemplate;
            Def.Keywords      = InKeywords;
            Def.WildcardTypes = std::move(InWildcard);
            if(InRole == ENodeRole::Statement)
            {
                InPins.insert(InPins.begin(), ExecIn());
                InPins.insert(InPins.begin() + 1, ExecOut());
            }
            Def.Pins = std::move(InPins);
            return AddNode(std::move(Def));
        };
        const auto Pure = [&](const std::string& InType, const std::string& InName, const std::string& InCategory,
                              const uint32_t InOp, std::vector<PinDefinition> InPins, const std::string& InTemplate = std::string(),
                              std::vector<std::string> InWildcard = {}, const std::string& InKeywords = std::string()) -> NodeDefinition&
        {
            return Add(InType, InName, InCategory, ENodeRole::Pure, InOp, std::move(InPins), InTemplate, std::move(InWildcard), InKeywords);
        };
        const auto Statement = [&](const std::string& InType, const std::string& InName, const std::string& InCategory,
                                   const uint32_t InOp, std::vector<PinDefinition> InPins, const std::string& InTemplate = std::string(),
                                   const std::string& InKeywords = std::string()) -> NodeDefinition&
        {
            return Add(InType, InName, InCategory, ENodeRole::Statement, InOp, std::move(InPins), InTemplate, {}, InKeywords);
        };
        const auto Event = [&](const std::string& InType, const std::string& InName, const uint32_t InOp,
                               std::vector<PinDefinition> InPins, const bool bInUnique) -> NodeDefinition&
        {
            InPins.insert(InPins.begin(), ExecOut());
            NodeDefinition& Def = Add(InType, InName, "Events", ENodeRole::Event, InOp, std::move(InPins));
            Def.bUnique = bInUnique;
            return Def;
        };

        // --- Events --------------------------------------------------------------
        Event("Event.OnTick", "On Tick", SOP_EventTick, { Out("DeltaSeconds", "number") }, true).Tooltip =
            IsUI() ? "Runs every frame while the document is open, paused or not." : "Runs every frame.";
        if(IsUI())
        {
            Event("UI.Event.Open", "On Document Open", SOP_UIOpen, {}, true).Tooltip =
                "Runs once when the document opens, after that frame's input: fill the data store it binds to.";
            Event("UI.Event.Close", "On Document Close", SOP_UIClose, {}, true).Tooltip = "Runs as the document closes.";
            NodeDefinition& Handler = Event("UI.Event.Handler", "UI Event", SOP_UIHandler,
                                            { Out("Value", "number"), Out("Sender", "string") }, false);
            Handler.Tooltip = "Runs when a widget's action names it -- OnClick=\"Graph:Resume_OnClick\". "
                              "Value is the widget's ($value: a slider's value, a check box's), Sender its name. "
                              "In the UI Designer, + > Graph handler on an event makes one.";
            Handler.Keywords   = "click changed button slider check box widget handler on";
            Handler.Properties = {
                { HANDLER_PROPERTY, EPropertyKind::Identifier, "Handler", {}, "The name actions call it by: Graph:<Handler>" },
                { "Widget", EPropertyKind::WidgetName, "", {}, "The widget it was made for (a note; the action is what calls it)" },
                { "Event", EPropertyKind::String, "", {}, "The widget's event it was made for (a note)" },
                { VALUE_TYPE_PROPERTY, EPropertyKind::Choice, "Number", { "Number", "Bool" }, "Value as a number, or a bool (a check box)" },
            };
        }
        if(!IsUI())
        {
            Event("Event.OnStart",   "On Start",   SOP_EventStart,   {}, true).Tooltip = "Runs once, before this entity's first tick.";
            Event("Event.OnDestroy", "On Destroy", SOP_EventDestroy, {}, true).Tooltip = "Runs when the entity or its script goes away.";
            /* A prefab's per-phase functions: define only the phases it needs. */
            for(const std::string& Phase : GTickPhases)
            {
                static const std::map<std::string, std::string> Labels = {
                    { "PrePhysics", "Pre-Physics" }, { "Physics", "Physics" }, { "PostPhysics", "Post-Physics" }, { "PostUpdate", "Post Update" } };
                NodeDefinition& Def = Event("Event.On" + Phase, "On " + Labels.at(Phase), SOP_EventPhase, { Out("DeltaSeconds", "number") }, true);
                Def.Template = Phase;
                Def.Tooltip  = "Runs every frame in the " + Labels.at(Phase) + " phase, on a prefab's script.";
                Def.Keywords = "tick update frame phase";
            }
            {
                NodeDefinition& Begin = Event("Event.OnOverlapBegin", "On Overlap Begin", SOP_EventOverlapBegin,
                                              { Out("Other", "Entity"), Out("Mine", "Entity") }, true);
                Begin.Tooltip  = "Runs when one of this script's colliders starts overlapping another (start of PostPhysics). "
                                 "Mine is which of its own entities it was.";
                Begin.Keywords = "collision collide touch trigger enter hit";
                NodeDefinition& End = Event("Event.OnOverlapEnd", "On Overlap End", SOP_EventOverlapEnd,
                                            { Out("Other", "Entity"), Out("Mine", "Entity") }, true);
                End.Tooltip  = "Runs when an overlap ends -- Other may no longer exist (Is Valid).";
                End.Keywords = "collision collide untouch trigger exit leave";
            }
        }
        {
            std::vector<PinDefinition> Args;
            for(int Arg = 1; Arg <= CUSTOM_EVENT_ARGS; ++Arg) { Args.push_back(Out("Arg " + std::to_string(Arg), "Any")); }
            NodeDefinition& Def = Event("Event.Custom", "On Event", SOP_EventCustom, std::move(Args), false);
            Def.Tooltip    = "Runs whenever the named event is emitted (Events.Emit).";
            Def.Properties = { { "EventName", EPropertyKind::String, "MyEvent", {}, "The event to listen for" } };
            Def.Keywords   = "listen subscribe on";
        }

        // --- Flow ----------------------------------------------------------------
        Add("Flow.Branch", "Branch", "Flow", ENodeRole::Statement, SOP_Branch,
            { ExecIn(), In("Condition", "bool"), ExecOut("True"), ExecOut("False") }, std::string(), {}, "if else");
        Add("Flow.Sequence", "Sequence", "Flow", ENodeRole::Statement, SOP_Sequence,
            { ExecIn(), ExecOut("Then 1"), ExecOut("Then 2"), ExecOut("Then 3"), ExecOut("Then 4") }, std::string(), {}, "then");
        Add("Flow.ForLoop", "For Loop", "Flow", ENodeRole::Statement, SOP_ForLoop,
            { ExecIn(), In("First", "integer", "1"), In("Last", "integer", "10"), ExecOut("Body"), Out("Index", "integer"),
              ExecOut("Completed") }, std::string(), {}, "repeat loop");
        Add("Flow.Return", "Return", "Flow", ENodeRole::Statement, SOP_Return, { ExecIn() }, std::string(), {}, "stop exit");

        // --- Functions -----------------------------------------------------------
        /* Pins here are only the exec ones: FindDefinition adds the named
         * function's inputs and outputs. */
        {
            const auto Function = [&](const std::string& InType, const std::string& InName, const ENodeRole InRole, const uint32_t InOp,
                                      std::vector<PinDefinition> InPins, const std::string& InTooltip, const std::string& InKeywords)
            {
                NodeDefinition Def;
                Def.Type        = InType;
                Def.DisplayName = InName;
                Def.Category    = "Functions";
                Def.Role        = InRole;
                Def.Op          = InOp;
                Def.Pins        = std::move(InPins);
                Def.Tooltip     = InTooltip;
                Def.Keywords    = InKeywords;
                Def.Properties  = { { FUNCTION_PROPERTY, EPropertyKind::Function, "", {}, "The graph function" } };
                AddNode(std::move(Def));
            };
            Function("Function.Entry", "Function Entry", ENodeRole::Event, SOP_FunctionEntry, { ExecOut() },
                     "Where a graph function's body starts; its outputs are the function's inputs.", "define body start");
            Function("Function.Return", "Return from Function", ENodeRole::Statement, SOP_FunctionReturn, { ExecIn() },
                     "Ends a graph function, giving its outputs.", "result exit");
            Function("Function.Call", "Call Function", ENodeRole::Statement, SOP_FunctionCall, { ExecIn(), ExecOut() },
                     "Runs a graph function and gives its outputs.", "run invoke");
        }

        // --- Variables -----------------------------------------------------------
        {
            NodeDefinition& Get = Pure("Variable.Get", "Get Variable", "Variables", SOP_VarGet, { Out("Value", WILDCARD_TYPE) },
                                       std::string(), VariableTypes);
            Get.Properties = { { "Variable", EPropertyKind::Variable, "", {}, "The graph variable to read" } };
            Get.bWildcardFromSchema = true;

            NodeDefinition& Set = Statement("Variable.Set", "Set Variable", "Variables", SOP_VarSet, { In("Value", WILDCARD_TYPE) });
            Set.WildcardTypes = VariableTypes;
            Set.Properties = { { "Variable", EPropertyKind::Variable, "", {}, "The graph variable to write" } };
            Set.bWildcardFromSchema = true;
        }

        // --- Entity and World ----------------------------------------------------
        if(!IsUI()) { Pure("Entity.Self", "Self", "Entity", SOP_Self, { Out("Entity", "Entity") }, std::string(), {}, "this me owner"); }
        Pure("Entity.GetName", "Get Name", "Entity", SOP_Call, { In("Entity", "Entity"), Out("Name", "string") }, "{Entity}:GetName()");
        Pure("Entity.IsValid", "Is Valid", "Entity", SOP_Call, { In("Entity", "Entity"), Out("Valid", "bool") }, "{Entity}:IsValid()", {}, "alive");
        Statement("Entity.SetName", "Set Name", "Entity", SOP_Call, { In("Entity", "Entity"), In("Name", "string") }, "{Entity}:SetName({Name})");
        Statement("Entity.Destroy", "Destroy", "Entity", SOP_Call, { In("Entity", "Entity") }, "{Entity}:Destroy()", "delete remove kill");
        /* A prefab's script: its root, its entities by template name, and how it ticks. */
        if(!IsUI())
        {
            Pure("Prefab.Root", "Prefab Root", "Prefab", SOP_Call, { Out("Root", "Entity") }, "self.Root", {}, "instance self owner");
            Pure("Prefab.Find", "Find Prefab Entity", "Prefab", SOP_Call, { In("Name", "string"), Out("Entity", "Entity") },
                 "self.Instance:Find({Name})", {}, "instance child template lookup");
            Statement("Prefab.SetTickMode", "Set Tick Mode", "Prefab", SOP_Call, { In("Mode", "string", "EventsOnly") },
                      "self.Instance:SetTickMode({Mode})", "instance tick pause sleep events off");
            Statement("Prefab.SetPhaseEnabled", "Set Phase Enabled", "Prefab", SOP_Call,
                      { In("Phase", "string", "PrePhysics"), In("Enabled", "bool", "true") },
                      "self.Instance:SetPhaseEnabled(TickPhase[{Phase}], {Enabled})", "instance tick pause phase");
            Statement("Prefab.Destroy", "Destroy Prefab", "Prefab", SOP_Call, {}, "self.Instance:Destroy()", "instance remove kill");
        }
        if(IsUI())
        {
            /* The document this graph is the logic of: self.Document is its path,
             * which the UI library's functions take first. */
            Pure("UI.Document", "This Document", "Document", SOP_Call, { Out("Path", "string") }, "self.Document", {},
                 "self path ui document").Tooltip = "The path of the document this graph is the logic of: the UI library's Path.";
            Statement("UI.CloseDocument", "Close This Document", "Document", SOP_Call, {}, "UI.Close(self.Document)",
                      "close dismiss exit").Tooltip = "Closes the document (its On Document Close runs).";
            Pure("UI.Key", "Key", "Document", SOP_Call, { In("Key", "string", "Name"), Out("Full", "string") },
                 "((self.Prefix or \"\") .. {Key})", {}, "binding prefix include store")
                .Tooltip = "A data store key as this document's {Bind:Key} reads it: its Include's Prefix + Key.";
            const auto Widget = [](const char* InName, const char* InTooltip)
            {
                return PropertyDefinition{ InName, EPropertyKind::WidgetName, "", {}, InTooltip };
            };
            const auto WidgetStatement = [&](const std::string& InType, const std::string& InName, std::vector<PinDefinition> InPins,
                                             const std::string& InTemplate, const std::string& InKeywords, const std::string& InTooltip)
            {
                NodeDefinition& Def = Statement(InType, InName, "Document", SOP_Call, std::move(InPins), InTemplate, InKeywords);
                Def.Properties = { Widget("Widget", "A widget of this document, by Name") };
                Def.Tooltip    = InTooltip;
            };
            WidgetStatement("UI.Widget.SetText", "Set Widget Text", { In("Text", "string") },
                            "UI.SetText(self.Document, {@Widget}, {Text})", "label caption", "Sets a Text widget's Text.");
            WidgetStatement("UI.Widget.SetVisible", "Set Widget Visible", { In("Visible", "bool", "true") },
                            "UI.SetVisible(self.Document, {@Widget}, {Visible})", "show hide collapse", "Shows it, or collapses it.");
            WidgetStatement("UI.Widget.SetEnabled", "Set Widget Enabled", { In("Enabled", "bool", "true") },
                            "UI.SetEnabled(self.Document, {@Widget}, {Enabled})", "disable grey", "Enables it, or greys it out.");
            WidgetStatement("UI.Widget.SetFocus", "Set Widget Focus", {}, "UI.SetFocus(self.Document, {@Widget})", "focus select",
                            "Gives it focus (keys and gamepad act on it).");
            NodeDefinition& Page = Statement("UI.Widget.ShowPage", "Show Page", "Document", SOP_Call, { In("Page", "string", "0") },
                                             "UI.ShowPage(self.Document, {@Switcher}, {Page})", "switcher tab page");
            Page.Properties = { Widget("Switcher", "A Switcher of this document, by Name") };
            Page.Tooltip    = "Shows a Switcher's page, by its Name or number (0 first).";
        }
        Pure("Entity.GetParent", "Get Parent", "Entity", SOP_Call, { In("Entity", "Entity"), Out("Parent", "Entity") }, "{Entity}:GetParent()", {}, "hierarchy attach owner");
        Pure("Entity.GetChildCount", "Get Child Count", "Entity", SOP_Call, { In("Entity", "Entity"), Out("Count", "integer") }, "{Entity}:GetChildCount()", {}, "hierarchy children");
        Pure("Entity.GetChild", "Get Child", "Entity", SOP_Call, { In("Entity", "Entity"), In("Index", "integer", "1"), Out("Child", "Entity") }, "{Entity}:GetChild({Index})", {}, "hierarchy children");
        Statement("Entity.SetParent", "Set Parent", "Entity", SOP_Call, { In("Entity", "Entity"), In("Parent", "Entity"), In("KeepWorld", "bool", "true") },
                  "{Entity}:SetParent({Parent}, {KeepWorld})", "attach detach hierarchy");

        Pure("World.FindEntity", "Find Entity", "World", SOP_Call, { In("Name", "string"), Out("Entity", "Entity") }, "World.FindEntity({Name})", {}, "lookup by name");
        Pure("World.GetEntityCount", "Entity Count", "World", SOP_Call, { Out("Count", "integer") }, "World.GetEntityCount()");
        Statement("World.CreateEntity", "Create Entity", "World", SOP_Call, { In("Name", "string"), Out("Entity", "Entity") },
                  "World.CreateEntity({Name})", "spawn new");
        Statement("World.SpawnPrefab", "Spawn Prefab", "World", SOP_Call, { In("Prefab", "string"), In("Position", "Vector3"), Out("Root", "Entity") },
                  "World.SpawnPrefab({Prefab}, {Position})", "spawn instantiate create prefab");
        Statement("World.Raycast", "Raycast", "World", SOP_Call,
                  { In("Origin", "Vector3"), In("Direction", "Vector3", "1 0 0"), In("MaxDistance", "number", "1000"),
                    Out("Hit", "Entity"), Out("Distance", "number") },
                  "World.Raycast({Origin}, {Direction}, {MaxDistance})", "trace line hit");

        {
            std::vector<PinDefinition> Pins = { In("Event", "string", "MyEvent") };
            for(int Arg = 1; Arg <= CUSTOM_EVENT_ARGS; ++Arg) { Pins.push_back(In("Arg " + std::to_string(Arg), "Any")); }
            Statement("Events.Emit", "Emit Event", "Events", SOP_Emit, std::move(Pins), std::string(), "send broadcast fire");
        }

        Statement("Debug.Print", "Print", "Debug", SOP_Call, { In("Value", "string", "Hello") }, "print({Value})", "log");
        Pure("Utility.IsSet", "Is Set", "Utility", SOP_Lanewise, { In("Value", "Any"), Out("Set", "bool") }, "{Value} ~= nil", {}, "nil null exists");

        // --- Math ----------------------------------------------------------------
        const std::string W = WILDCARD_TYPE;
        const std::vector<std::string> NumberOrVector = { "number", "Vector3" };
        const auto Binary = [&](const std::string& InType, const std::string& InName, const std::string& InExpr,
                                const std::string& InDefaultB, const std::string& InKeywords)
        {
            Pure("Math." + InType, InName, "Math", SOP_Lanewise, { In("A", W), In("B", W, InDefaultB), Out("Out", W) }, InExpr,
                 NumberOrVector, InKeywords);
        };
        Binary("Add",      "Add",      "{A} + {B}", "0", "+ plus sum");
        Binary("Subtract", "Subtract", "{A} - {B}", "0", "- minus difference");
        Binary("Multiply", "Multiply", "{A} * {B}", "1", "* times product");
        Binary("Divide",   "Divide",   "{A} / {B}", "1", "/ quotient");
        Pure("Math.Lerp", "Lerp", "Math", SOP_Lanewise, { In("A", W), In("B", W, "1"), In("T", "number", "0.5"), Out("Out", W) },
             "{A} + ({B} - {A}) * {T}", NumberOrVector, "mix blend interpolate");
        Pure("Math.Negate", "Negate", "Math", SOP_Lanewise, { In("X", W), Out("Out", W) }, "-{X}", NumberOrVector);

        const auto Number1 = [&](const std::string& InType, const std::string& InName, const std::string& InExpr, const std::string& InCategory)
        {
            Pure("Math." + InType, InName, InCategory, SOP_Lanewise, { In("X", "number"), Out("Out", "number") }, InExpr);
        };
        const auto Number2 = [&](const std::string& InType, const std::string& InName, const std::string& InExpr,
                                 const std::string& InDefaultB, const std::string& InKeywords)
        {
            Pure("Math." + InType, InName, "Math", SOP_Lanewise, { In("A", "number"), In("B", "number", InDefaultB), Out("Out", "number") },
                 InExpr, {}, InKeywords);
        };
        Number2("Modulo", "Modulo", "{A} % {B}", "1", "% remainder mod");
        Number2("Min", "Min", "math.min({A}, {B})", "0", "smallest");
        Number2("Max", "Max", "math.max({A}, {B})", "0", "largest");
        Number2("Power", "Power", "{A} ^ {B}", "2", "pow exponent");
        Number1("Abs",   "Absolute",    "math.abs({X})",   "Math");
        Number1("Floor", "Floor",       "math.floor({X})", "Math/Rounding");
        Number1("Ceil",  "Ceiling",     "math.ceil({X})",  "Math/Rounding");
        Number1("Sqrt",  "Square Root", "math.sqrt({X})",  "Math");
        Number1("Sin",   "Sine",        "math.sin({X})",   "Math/Trigonometry");
        Number1("Cos",   "Cosine",      "math.cos({X})",   "Math/Trigonometry");
        Pure("Math.Clamp", "Clamp", "Math", SOP_Lanewise,
             { In("X", "number"), In("Min", "number", "0"), In("Max", "number", "1"), Out("Out", "number") },
             "math.max({Min}, math.min({Max}, {X}))");
        Pure("Math.Random", "Random", "Math", SOP_Call, { Out("Value", "number") }, "math.random()", {}, "rand noise");

        // --- Comparison and logic ------------------------------------------------
        const auto Compare = [&](const std::string& InType, const std::string& InName, const std::string& InOperator, const std::string& InKeywords)
        {
            Pure("Compare." + InType, InName, "Logic", SOP_Lanewise, { In("A", "number"), In("B", "number"), Out("Out", "bool") },
                 "{A} " + InOperator + " {B}", {}, InKeywords);
        };
        Compare("Less",         "Less",             "<",  "<");
        Compare("LessEqual",    "Less or Equal",    "<=", "<=");
        Compare("Greater",      "Greater",          ">",  ">");
        Compare("GreaterEqual", "Greater or Equal", ">=", ">=");
        /* Narrowest first: an integer compared with a number compares as numbers. */
        const std::vector<std::string> Comparable = { "integer", "number", "bool", "string" };
        Pure("Compare.Equal", "Equal", "Logic", SOP_Lanewise, { In("A", W), In("B", W), Out("Out", "bool") }, "{A} == {B}", Comparable, "== same");
        Pure("Compare.NotEqual", "Not Equal", "Logic", SOP_Lanewise, { In("A", W), In("B", W), Out("Out", "bool") }, "{A} ~= {B}", Comparable, "~= != different");
        Pure("Logic.And", "And", "Logic", SOP_Lanewise, { In("A", "bool"), In("B", "bool"), Out("Out", "bool") }, "{A} and {B}", {}, "&&");
        Pure("Logic.Or",  "Or",  "Logic", SOP_Lanewise, { In("A", "bool"), In("B", "bool"), Out("Out", "bool") }, "{A} or {B}", {}, "||");
        Pure("Logic.Not", "Not", "Logic", SOP_Lanewise, { In("X", "bool"), Out("Out", "bool") }, "not {X}", {}, "! invert");

        Pure("String.Append", "Append", "String", SOP_Lanewise, { In("A", "string"), In("B", "string"), Out("Out", "string") },
             "{A} .. {B}", {}, "concat join ..");

        // --- Vectors, colours, boxes ---------------------------------------------
        Pure("Vector.Make", "Make Vector3", "Vector", SOP_Compose,
             { In("X", "number"), In("Y", "number"), In("Z", "number"), Out("Vector", "Vector3") }, std::string(), {}, "combine");
        Pure("Vector.Break", "Break Vector3", "Vector", SOP_Decompose,
             { In("Vector", "Vector3"), Out("X", "number"), Out("Y", "number"), Out("Z", "number") }, std::string(), {}, "split components");
        Pure("Vector.Scale", "Scale Vector", "Vector", SOP_Lanewise, { In("Vector", "Vector3"), In("Scale", "number", "1"), Out("Out", "Vector3") },
             "{Vector} * {Scale}", {}, "multiply");
        Pure("Vector.Length", "Length", "Vector", SOP_Length, { In("V", "Vector3"), Out("Length", "number") }, std::string(), {}, "magnitude");
        Pure("Vector.Normalize", "Normalize", "Vector", SOP_Normalize, { In("V", "Vector3"), Out("Out", "Vector3") }, std::string(), {}, "unit direction");
        Pure("Vector.Dot", "Dot", "Vector", SOP_Dot, { In("A", "Vector3"), In("B", "Vector3"), Out("Dot", "number") });

        Pure("Color.Make", "Make Color", "Color", SOP_Compose,
             { In("R", "integer", "255"), In("G", "integer", "255"), In("B", "integer", "255"), In("A", "integer", "255"),
               Out("Color", "Color") }, std::string(), {}, "rgba");
        Pure("Color.Break", "Break Color", "Color", SOP_Decompose,
             { In("Color", "Color"), Out("R", "integer"), Out("G", "integer"), Out("B", "integer"), Out("A", "integer") }, std::string(), {}, "rgba");
        Pure("Box.Make", "Make Box", "Vector", SOP_Compose, { In("Min", "Vector3"), In("Max", "Vector3"), Out("Box", "Box") },
             std::string(), {}, "aabb bounds");
        Pure("Box.Break", "Break Box", "Vector", SOP_Decompose, { In("Box", "Box"), Out("Min", "Vector3"), Out("Max", "Vector3") },
             std::string(), {}, "aabb bounds");

        AddComponentNodes();
        AddLibraryNodes();
    }

    /* Per component type: get/has/add/remove, a getter/setter per field and a
     * call per method. Late-registered components appear next editor start. */
    void ScriptSchema::AddComponentNodes()
    {
        for(size_t Id = 0; Id < GameCore::GetComponentTypeCount(); ++Id)
        {
            const GameCore::ComponentTypeInfo* Info = GameCore::GetComponentType(GameCore::ComponentTypeID(Id));
            if(!Info || Info->Name[0] == '\0') { continue; }

            const std::string Name     = Info->Name;
            const std::string TypeName = COMPONENT_TYPE_PREFIX + Name;
            const std::string Prefix   = "Component." + Name + ".";
            const std::string Category = "Components/" + Name;
            const std::string Quoted   = LuaString(Name);

            AddType({ TypeName, Name, 0x4F9FD9, "", false });
            AddConversion(TypeName, "Any");

            const auto Make = [&](const std::string& InType, const std::string& InName, const ENodeRole InRole,
                                  std::vector<PinDefinition> InPins, const std::string& InTemplate)
            {
                NodeDefinition Def;
                Def.Type        = Prefix + InType;
                Def.DisplayName = InName;
                Def.Category    = Category;
                Def.Keywords    = Name;
                Def.Role        = InRole;
                Def.Op          = SOP_Call;
                Def.Template    = InTemplate;
                if(InRole == ENodeRole::Statement)
                {
                    InPins.insert(InPins.begin(), ExecIn());
                    InPins.insert(InPins.begin() + 1, ExecOut());
                }
                Def.Pins = std::move(InPins);
                AddNode(std::move(Def));
            };

            Make("Get",    "Get " + Name,    ENodeRole::Pure,      { In("Entity", "Entity"), Out(Name, TypeName) }, "{Entity}:GetComponent(" + Quoted + ")");
            Make("Has",    "Has " + Name,    ENodeRole::Pure,      { In("Entity", "Entity"), Out("Has", "bool") }, "{Entity}:HasComponent(" + Quoted + ")");
            Make("Add",    "Add " + Name,    ENodeRole::Statement, { In("Entity", "Entity"), Out(Name, TypeName) }, "{Entity}:AddComponent(" + Quoted + ")");
            Make("Remove", "Remove " + Name, ENodeRole::Statement, { In("Entity", "Entity") }, "{Entity}:RemoveComponent(" + Quoted + ")");
            Make("GetEntity", Name + ": Get Entity", ENodeRole::Pure, { In("Target", TypeName), Out("Entity", "Entity") }, "{Target}:GetEntity()");

            size_t Count = 0;
            const GameCore::ScriptBindingEntry* Entries = GameCore::FindComponentScript(GameCore::ComponentTypeID(Id), Count);
            for(size_t Index = 0; Index < Count; ++Index)
            {
                const GameCore::ScriptBindingEntry& Entry = Entries[Index];
                const std::string Member = Entry.Name;

                if(!Entry.bMethod)
                {
                    const std::string FieldType = ScriptTypeOf(Entry.FieldType, Entry.FieldStruct);
                    Make("Get" + Member, Name + ": Get " + Member, ENodeRole::Pure,
                         { In("Target", TypeName), Out(Member, FieldType) }, "{Target}:Get" + Member + "()");
                    if(!Entry.bReadOnly)
                    {
                        Make("Set" + Member, Name + ": Set " + Member, ENodeRole::Statement,
                             { In("Target", TypeName), In(Member, FieldType) }, "{Target}:Set" + Member + "({" + Member + "})");
                    }
                    continue;
                }

                /* A method: its pins from the binding's signature. */
                std::vector<PinDefinition> Pins = { In("Target", TypeName) };
                std::vector<PinDefinition> Parameters, Results;
                const std::string Signature = Entry.Signature;
                const bool bParsed = ParseSignature(Signature, Parameters, Results);

                std::string Arguments;
                if(bParsed)
                {
                    Arguments = ArgumentList(Parameters);
                    Pins.insert(Pins.end(), Parameters.begin(), Parameters.end());
                    Pins.insert(Pins.end(), Results.begin(), Results.end());
                }

                Make(Member, Name + ": " + Member, Entry.bPure ? ENodeRole::Pure : ENodeRole::Statement, std::move(Pins),
                     "{Target}:" + Member + "(" + Arguments + ")");
                if(!bParsed)
                {
                    NodeDefinition& Def = const_cast<NodeDefinition&>(*FindNode(Prefix + Member));
                    Def.Tooltip = Signature.empty()
                                ? NO_SIGNATURE_TOOLTIP
                                : "has an unreadable signature ('" + Signature + "') in its binding -- no signature -- so it is called with no arguments";
                }
            }
        }
    }

    /* Per registered script struct (GameCore/ScriptStruct.h): a pin type,
     * which must be linked or is all zeros, and Make / Break nodes. Records the
     * lane layout the compiler writes it with. Returns the type names. */
    std::vector<std::string> ScriptSchema::AddStructTypes()
    {
        std::vector<std::string> Names;
        for(size_t Id = 0; Id < GameCore::GetScriptStructCount(); ++Id)
        {
            const GameCore::ScriptStructInfo* Info = GameCore::GetScriptStruct(GameCore::ScriptStructID(Id));
            if(!Info || Info->FieldCount == 0) { continue; }

            const std::string Name = Info->Name;
            StructLayout Layout;
            std::vector<PinDefinition> FieldInputs, FieldOutputs;
            for(uint16_t Index = 0; Index < Info->FieldCount; ++Index)
            {
                const GameCore::ScriptStructField& Field = Info->Fields[Index];
                const std::string FieldName = Field.Name;
                const std::string PinType   = ScriptTypeOf(Field.Type, GameCore::SCRIPT_STRUCT_INVALID);
                Layout.Fields.emplace_back(FieldName, PinType);

                const std::vector<std::string>& Suffixes = LaneNames(PinType);
                const std::string LaneType = PinType == "Color" ? "integer" : Suffixes.empty() ? PinType : "number";
                if(Suffixes.empty())
                {
                    Layout.LaneNames.push_back(FieldName);
                    Layout.LaneTypes.push_back(LaneType);
                }
                for(const std::string& Suffix : Suffixes)
                {
                    Layout.LaneNames.push_back(FieldName + "_" + Suffix);
                    Layout.LaneTypes.push_back(LaneType);
                }

                FieldInputs.push_back(In(FieldName, PinType));
                FieldOutputs.push_back(Out(FieldName, PinType));
            }
            StructLayouts()[Name] = std::move(Layout);

            AddType({ Name, Name, 0xC77DFF, "", false });
            Names.push_back(Name);

            const auto Make = [&](const std::string& InType, const std::string& InDisplay, const uint32_t InOp,
                                  std::vector<PinDefinition> InPins, const std::string& InTooltip)
            {
                NodeDefinition Def;
                Def.Type        = "Struct." + Name + "." + InType;
                Def.DisplayName = InDisplay;
                Def.Category    = "Structs";
                Def.Keywords    = Name + " struct";
                Def.Tooltip     = InTooltip;
                Def.Role        = ENodeRole::Pure;
                Def.Op          = InOp;
                Def.Pins        = std::move(InPins);
                AddNode(std::move(Def));
            };

            FieldInputs.push_back(Out(Name, Name));
            Make("Make", "Make " + Name, SOP_Compose, std::move(FieldInputs), "A " + Name + " from its fields");
            FieldOutputs.insert(FieldOutputs.begin(), In(Name, Name));
            Make("Break", "Break " + Name, SOP_Decompose, std::move(FieldOutputs), "A " + Name + "'s fields");
        }
        return Names;
    }

    /* Per script library (GameCore/ScriptLibrary.h), under the library's name
     * in the palette: a call per function, Get/Set per variable, and On/Emit
     * per event. Types are Library.<Lib>.<Member>, so a graph keeps its links
     * when the library is reloaded, and loads placeholders while it is not. */
    void ScriptSchema::AddLibraryNodes()
    {
        for(size_t LibraryIndex = 0; LibraryIndex < GameCore::GetScriptLibraryCount(); ++LibraryIndex)
        {
            GameCore::ScriptLibraryInfo Info;
            if(!GameCore::GetScriptLibrary(LibraryIndex, Info)) { continue; }

            const std::string Library = Info.Name;
            const std::string Prefix  = "Library." + Library + ".";

            const auto Make = [&](const std::string& InType, const std::string& InName, const ENodeRole InRole, const uint32_t InOp,
                                  std::vector<PinDefinition> InPins, const std::string& InTemplate, const std::string& InTooltip)
            {
                NodeDefinition Def;
                Def.Type        = Prefix + InType;
                Def.DisplayName = InName;
                Def.Category    = Library;
                Def.Keywords    = Library;
                Def.Tooltip     = InTooltip;
                Def.Role        = InRole;
                Def.Op          = InOp;
                Def.Template    = InTemplate;
                if(InRole == ENodeRole::Statement)
                {
                    InPins.insert(InPins.begin(), ExecIn());
                    InPins.insert(InPins.begin() + 1, ExecOut());
                }
                else if(InRole == ENodeRole::Event)
                {
                    InPins.insert(InPins.begin(), ExecOut());
                }
                Def.Pins = std::move(InPins);
                AddNode(std::move(Def));
            };

            for(size_t MemberIndex = 0; MemberIndex < Info.Count; ++MemberIndex)
            {
                const GameCore::ScriptLibraryMember& Entry = Info.Members[MemberIndex];
                const std::string Member  = Entry.Name;
                const std::string Tooltip = Entry.Tooltip;
                const std::string Call    = Library + "." + Member;

                switch(Entry.Kind)
                {
                case GameCore::EScriptLibraryMember::Function:
                case GameCore::EScriptLibraryMember::Pure:
                {
                    std::vector<PinDefinition> Parameters, Results;
                    const bool bParsed = ParseSignature(Entry.Signature, Parameters, Results);
                    if(!bParsed && Entry.Signature[0] != '\0')
                    {
                        NODEGRAPH_LOG_WARN("'" + Call + "' has an unreadable signature ('" + Entry.Signature + "'); its node takes no arguments")
                    }

                    std::vector<PinDefinition> Pins = Parameters;
                    Pins.insert(Pins.end(), Results.begin(), Results.end());
                    const bool bPure = Entry.Kind == GameCore::EScriptLibraryMember::Pure;
                    Make(Member, Member, bPure ? ENodeRole::Pure : ENodeRole::Statement, SOP_Call, std::move(Pins),
                         Call + "(" + ArgumentList(Parameters) + ")", bParsed ? Tooltip : std::string(NO_SIGNATURE_TOOLTIP));
                    break;
                }

                case GameCore::EScriptLibraryMember::Variable:
                {
                    const std::string Type = ScriptTypeOf(Entry.FieldType, Entry.FieldStruct);
                    Make("Get" + Member, "Get " + Member, ENodeRole::Pure, SOP_Call, { Out(Member, Type) },
                         Library + ".Get" + Member + "()", Tooltip);
                    if(!Entry.bReadOnly)
                    {
                        Make("Set" + Member, "Set " + Member, ENodeRole::Statement, SOP_Call, { In(Member, Type) },
                             Library + ".Set" + Member + "({" + Member + "})", Tooltip);
                    }
                    break;
                }

                case GameCore::EScriptLibraryMember::Event:
                {
                    /* One Lua value per parameter, at most four: what an
                     * EventBus event carries. */
                    std::vector<PinDefinition> Parameters, Unused;
                    bool bValid = Entry.Signature[0] == '\0'
                               || (ParseSignature(Entry.Signature, Parameters, Unused) && Unused.empty());
                    bValid = bValid && Parameters.size() <= size_t(CUSTOM_EVENT_ARGS);
                    for(const PinDefinition& Each : Parameters) { bValid = bValid && LaneCount(Each.Type) == 1 && Each.Type != "Box"; }
                    if(!bValid)
                    {
                        NODEGRAPH_LOG_WARN("event '" + Call + "' needs at most " + std::to_string(CUSTOM_EVENT_ARGS)
                                           + " parameters of number, integer, bool, string or Entity ('" + Entry.Signature
                                           + "'); it has no nodes")
                        break;
                    }

                    std::vector<PinDefinition> Outputs;
                    for(const PinDefinition& Each : Parameters) { Outputs.push_back(Out(Each.Name, Each.Type)); }
                    Make("On" + Member, "On " + Member, ENodeRole::Event, SOP_EventCustom, std::move(Outputs), Member,
                         Tooltip.empty() ? "Runs whenever " + Member + " is emitted." : Tooltip);
                    Make("Emit" + Member, "Emit " + Member, ENodeRole::Statement, SOP_Emit, std::move(Parameters), Member,
                         Tooltip.empty() ? "Emits " + Member + " on the World's event bus." : Tooltip);
                    break;
                }
                }
            }
        }
    }

    const NodeDefinition* ScriptSchema::FindDefinition(const Graph& InGraph, const Node& InNode) const
    {
        const NodeDefinition* Base = Schema::FindDefinition(InGraph, InNode);
        if(Base && Base->Op == SOP_UIHandler && InNode.GetProperty(VALUE_TYPE_PROPERTY) == "Bool")
        {
            /* A check box's handler: Value is a bool. */
            const std::lock_guard<std::mutex> Lock(FunctionDefinitionsLock);
            const std::string Key = Base->Type + "\nBool";
            const auto Found = FunctionDefinitions.find(Key);
            if(Found != FunctionDefinitions.end()) { return &Found->second; }
            NodeDefinition Def = *Base;
            for(PinDefinition& Pin : Def.Pins) { if(Pin.Name == "Value") Pin.Type = "bool"; }
            return &FunctionDefinitions.emplace(Key, std::move(Def)).first->second;
        }
        if(!Base || (Base->Op != SOP_FunctionEntry && Base->Op != SOP_FunctionReturn && Base->Op != SOP_FunctionCall))
        {
            return Base;
        }

        /* A function the graph lacks leaves the exec pins alone; ValidateGraph says why. */
        const GraphFunction* Function = InGraph.FindFunction(InNode.GetProperty(FUNCTION_PROPERTY));
        if(!Function) { return Base; }

        std::string Key = Base->Type + "\n" + Function->Name;
        for(const FunctionPin& Each : Function->Inputs)  { Key += "\n<" + Each.Name + ":" + Each.Type; }
        for(const FunctionPin& Each : Function->Outputs) { Key += "\n>" + Each.Name + ":" + Each.Type; }

        const std::lock_guard<std::mutex> Lock(FunctionDefinitionsLock);
        const auto Found = FunctionDefinitions.find(Key);
        if(Found != FunctionDefinitions.end()) { return &Found->second; }

        NodeDefinition Def = *Base;
        switch(Base->Op)
        {
            case SOP_FunctionEntry:
                Def.DisplayName = Function->Name;
                for(const FunctionPin& Each : Function->Inputs) { Def.Pins.push_back(Out(Each.Name, Each.Type)); }
                break;
            case SOP_FunctionReturn:
                Def.DisplayName = "Return from " + Function->Name;
                for(const FunctionPin& Each : Function->Outputs) { Def.Pins.push_back(In(Each.Name, Each.Type)); }
                break;
            default:   // SOP_FunctionCall
                Def.DisplayName = "Call " + Function->Name;
                for(const FunctionPin& Each : Function->Inputs)  { Def.Pins.push_back(In(Each.Name, Each.Type)); }
                for(const FunctionPin& Each : Function->Outputs) { Def.Pins.push_back(Out(Each.Name, Each.Type)); }
                break;
        }
        return &FunctionDefinitions.emplace(Key, std::move(Def)).first->second;
    }

    std::string ScriptSchema::ResolveWildcard(const Graph& InGraph, const Node& InNode, const NodeDefinition& InDef) const
    {
        if(InDef.Op == SOP_VarGet || InDef.Op == SOP_VarSet)
        {
            const Variable* Var = InGraph.FindVariable(InNode.GetProperty("Variable"));
            if(Var) { return Var->Type; }
        }
        return Schema::ResolveWildcard(InGraph, InNode, InDef);
    }

    std::string ScriptSchema::DescribeNode(const Graph& InGraph, const Node& InNode) const
    {
        const NodeDefinition* Def = Schema::FindDefinition(InGraph, InNode);
        if(Def && Def->Op == SOP_UIHandler)
        {
            /* Where the designer made it: "Resume . OnClick". */
            const std::string Widget = InNode.GetProperty("Widget"), On = InNode.GetProperty("Event");
            if(!Widget.empty() || !On.empty()) { return Widget + (Widget.empty() || On.empty() ? "" : " \xC2\xB7 ") + On; }
        }
        return Schema::DescribeNode(InGraph, InNode);
    }

    void ScriptSchema::Compile(const Graph& InGraph, CompileResult& OutResult) const
    {
        ScriptCompiler(InGraph, *this, OutResult, IsUI()).Run();
    }
}

std::unique_ptr<Schema> CreateScriptSchema()
{
    return std::make_unique<ScriptSchema>(EScriptFlavour::Entity);
}

std::unique_ptr<Schema> CreateUIScriptSchema()
{
    return std::make_unique<ScriptSchema>(EScriptFlavour::UI);
}
}
