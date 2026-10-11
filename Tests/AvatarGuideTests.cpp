/*
 * NimbleEngineAvatarGuideTests -- Engine/Source/AVATARS_GUIDE.md's examples,
 * built and run, so the guide stays true: the Walker game's C++ (an input
 * map, a pawn component, a simulation, two systems, a native prefab and an
 * NetGameMode), the pawn's Lua script, and the pawn's script graph.
 *
 * Between the "GUIDE" markers is the guide's code (the guide splits
 * WalkerMode across two steps); the rest drives it.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentProperties.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/NodeGraph/Schema.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Renderer.h"

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

/* Where the test writes the guide's Lua, and its graph's Lua. */
static std::string LuaScriptPath()   { return (std::filesystem::temp_directory_path() / "NimbleWalkerPawn.lua").generic_string(); }
static std::string GraphScriptPath() { return (std::filesystem::temp_directory_path() / "NimbleWalkerPawn.graph.lua").generic_string(); }

// ==== GUIDE: C++ ================================================================

// --- The input map ----------------------------------------------------------
NIMBLE_DEFINE_INPUT_MAP(Walker)
{
    Map.Axis  ("MoveX",    "D, Right, -A, -Left, Gamepad.LeftX");
    Map.Axis  ("MoveZ",    "W, Up, -S, -Down, -Gamepad.LeftY");
    Map.Action("Jump",     "Space, Gamepad.South");
    Map.Action("Interact", "E, Gamepad.West");
}

// --- The pawn's own state ---------------------------------------------------
struct WalkerPawnComponent
{
    GAMECORE_COMPONENT_BODY()

    float   Speed = 400.0f;   // world units a second
    int32_t Jumps = 0;        // the server's count, replicated
};

GAMECORE_DEFINE_COMPONENT(WalkerPawnComponent, "WalkerPawn")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(WalkerPawnComponent, 1)
{
    Ar.Field("Speed", C.Speed);
}

GAMECORE_DEFINE_COMPONENT_REPLICATION(WalkerPawnComponent)
{
    Rep.Field("Jumps", &WalkerPawnComponent::Jumps);
}

GAMECORE_DEFINE_COMPONENT_SCRIPT(WalkerPawnComponent)
{
    Bind.Field("Speed",    &WalkerPawnComponent::Speed);
    Bind.ReadOnly("Jumps", &WalkerPawnComponent::Jumps);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(WalkerPawnComponent)
{
    Props.Field("Speed", &WalkerPawnComponent::Speed);
}
#endif

// --- Movement: one frame of input, the same on server and predicting client --
NIMBLE_DEFINE_AVATAR_SIMULATION(WalkerMove, 0.1f)
{
    static const int MoveX = NimbleEngine::FindInputAxis("Walker", "MoveX");
    static const int MoveZ = NimbleEngine::FindInputAxis("Walker", "MoveZ");

    GameCore::Entity Pawn = InWorld.GetEntity(InAvatar);
    auto* Transform = Pawn.GetComponent<GameCore::TransformComponent>();
    auto* State     = Pawn.GetComponent<WalkerPawnComponent>();
    if(!Transform || !State || MoveX < 0 || MoveZ < 0) { return; }

    const double Step = double(State->Speed) * double(InFrame.Seconds);
    Transform->Position = EngineCore::Vector3(Transform->Position.GetX() + double(InFrame.Axes[MoveX]) * Step,
                                              Transform->Position.GetY(),
                                              Transform->Position.GetZ() + double(InFrame.Axes[MoveZ]) * Step);
}

// --- Gameplay: what a press does, decided by the server ----------------------
static void WalkerJumpSystem(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* /*InUserData*/)
{
    if(!NimbleEngine::HasAuthority()) { return; }
    InWorld.Each<NimbleEngine::AvatarHarnessComponent>([](GameCore::Entity InAvatar, NimbleEngine::AvatarHarnessComponent& InHarness)
    {
        if(!InHarness.WasPressed("Jump")) { return; }
        if(WalkerPawnComponent* State = InAvatar.GetComponent<WalkerPawnComponent>()) { ++State->Jumps; }
    });
}

// --- The prefab, in code (or author Content/Prefabs/Pawn.nmprefab) ------------
static void BuildWalkerPawn(GameCore::PrefabBuilder& B)
{
    GameCore::PrefabBuilder::Node Root = B.Root("Pawn");
    Root.Add<GameCore::TransformComponent>();

    GameCore::NetComponent Net;
    Net.bPredicted = 1;                                   // its owner moves it at once
    Root.Add(Net);

    NimbleEngine::AvatarHarnessComponent Harness;
    std::snprintf(Harness.InputMap,   sizeof(Harness.InputMap),   "%s", "Walker");
    std::snprintf(Harness.Simulation, sizeof(Harness.Simulation), "%s", "WalkerMove");
    Root.Add(Harness);

    Root.Add<WalkerPawnComponent>();

    /* Its camera: inactive, so only its own player's view uses it. */
    GameCore::PrefabBuilder::Node Eye = Root.Child("Eye");
    GameCore::TransformComponent At;
    At.Position = EngineCore::Vector3(0.0, 200.0, -500.0);
    Eye.Add(At);
    RenderCore::CameraComponent Camera;
    Camera.Projection = RenderCore::CP_Perspective;
    Camera.bActive    = 0;
    Eye.Add(Camera);
}
GAMECORE_DEFINE_NATIVE_PREFAB(WalkerPawn, "Walker.Pawn", &BuildWalkerPawn)

// --- The mode ------------------------------------------------------------------
class WalkerMode : public NimbleEngine::NetGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        NetGameMode::OnLoad(InSession);
        GameCore::World& World = InSession.GetWorld();
        World.RegisterSystem("Walker.Jump",     GameCore::TP_PrePhysics, &WalkerJumpSystem);
        World.RegisterSystem("Walker.Interact", GameCore::TP_PrePhysics, &InteractSystem, this);
        return true;
    }

    void OnTick(const NimbleEngine::FrameContext& /*InFrame*/) override {}

    void OnAvatarPossessed(const GameCore::EntityID InPlayer, const GameCore::EntityID InAvatar) override
    {
        GameCore::World& World = Session->GetWorld();
        CORE_LOG(std::string("[Walker] ") + World.GetEntity(InPlayer).GetName() + " drives " + World.GetEntity(InAvatar).GetName())
    }

protected:
    const char* GetAvatarPrefab(GameCore::EntityID /*InPlayer*/) override { return "native:Walker.Pawn"; }

    void OnAvatarSpawned(const GameCore::EntityID InPlayer, const GameCore::EntityID InAvatar, const uint32_t InOrdinal) override
    {
        GameCore::World& World = Session->GetWorld();
        World.GetEntity(InAvatar).SetName(("Pawn " + std::to_string(InOrdinal + 1)).c_str());
        Pawns[InPlayer.Pack()] = InAvatar;
    }

private:
    /* Interact: into the nearest free vehicle (a Requestable harness)
     * within reach, or out of it, back to the player's own pawn. */
    static void InteractSystem(GameCore::World& InWorld, float /*InDeltaSeconds*/, void* InUserData)
    {
        if(!NimbleEngine::HasAuthority()) { return; }
        WalkerMode& Self = *static_cast<WalkerMode*>(InUserData);

        /* Collected first: Possess changes harnesses under Each. */
        std::vector<std::pair<GameCore::EntityID, GameCore::EntityID>> Asking;   // player, what it drives
        InWorld.Each<NimbleEngine::AvatarHarnessComponent>([&](GameCore::Entity InAvatar, NimbleEngine::AvatarHarnessComponent& InHarness)
        {
            if(InHarness.WasPressed("Interact")) { Asking.push_back({ InHarness.GetPossessor(), InAvatar.GetID() }); }
        });

        for(const auto& [Player, Driven] : Asking)
        {
            const GameCore::EntityID Own = Self.Pawns[Player.Pack()];
            if(Driven != Own && InWorld.IsAlive(Own))
            {
                NimbleEngine::Possess(InWorld, Player, Own);   // out
                continue;
            }
            const GameCore::EntityID Vehicle = Self.FindVehicleNear(InWorld, Driven, 300.0);
            if(Vehicle.IsSet()) { NimbleEngine::Possess(InWorld, Player, Vehicle); }   // in
        }
    }

    GameCore::EntityID FindVehicleNear(GameCore::World& InWorld, const GameCore::EntityID InFrom, const double InReach)
    {
        const EngineCore::Vector3 From = InWorld.GetEntity(InFrom).GetWorldPose().Position;
        GameCore::EntityID Best;
        double BestDistance = InReach;
        InWorld.Each<NimbleEngine::AvatarHarnessComponent>([&](GameCore::Entity InAvatar, NimbleEngine::AvatarHarnessComponent& InHarness)
        {
            if(!InHarness.bRequestable || InHarness.IsPossessed()) { return; }
            const EngineCore::Vector3 Delta = InAvatar.GetWorldPose().Position - From;
            const double Distance = std::sqrt(double(Delta.GetX()) * double(Delta.GetX()) + double(Delta.GetZ()) * double(Delta.GetZ()));
            if(Distance <= BestDistance) { BestDistance = Distance; Best = InAvatar.GetID(); }
        });
        return Best;
    }

    std::unordered_map<uint64_t, GameCore::EntityID> Pawns;   // each player's own, by packed id
};

// ==== END GUIDE: C++ ============================================================

/* The pawn with the guide's Lua, and with its graph: what Pawn.nmprefab's
 * Script field does for an authored prefab. */
static void BuildLuaPawn(GameCore::PrefabBuilder& B)   { BuildWalkerPawn(B); static const std::string P = LuaScriptPath();   B.Script(P.c_str()); }
static void BuildGraphPawn(GameCore::PrefabBuilder& B) { BuildWalkerPawn(B); static const std::string P = GraphScriptPath(); B.Script(P.c_str()); }
GAMECORE_DEFINE_NATIVE_PREFAB(WalkerLuaPawn, "WalkerTest.LuaPawn", &BuildLuaPawn)
GAMECORE_DEFINE_NATIVE_PREFAB(WalkerGraphPawn, "WalkerTest.GraphPawn", &BuildGraphPawn)

/* The guide's mode, spawning one of those instead. */
class ScriptedWalkerMode : public WalkerMode
{
public:
    explicit ScriptedWalkerMode(const char* InPrefab) : Prefab(InPrefab) {}
protected:
    const char* GetAvatarPrefab(GameCore::EntityID) override { return Prefab; }
    const char* Prefab;
};

// ==== GUIDE: Lua ================================================================
static const char* const PawnLua = R"LUA(-- Scripts/WalkerPawn.lua: the pawn prefab's Script.
return {
    TickPhase = TickPhase.PrePhysics,

    OnStart = function(self)
        self.Harness = self.Entity:GetComponent("AvatarHarness")
    end,

    -- Possession is announced on every process; on a client, as it replicates.
    OnPossessed = function(self, player)
        self.Entity:SetName(player:GetName() .. "'s pawn")
    end,

    OnUnpossessed = function(self, player)
        self.Entity:SetName("Empty pawn")
    end,

    OnTick = function(self, dt)
        if not self.Harness:IsPossessed() then return end

        -- Rules are the server's: on a client this reads the same press,
        -- and the score arrives by replication.
        if Net.HasAuthority() and self.Harness:WasPressed("Jump") then
            Game.AddScore(self.Harness:GetPossessor(), 1)
        end

        -- Feedback only the player at the keys should get.
        if self.Harness:IsLocallyControlled() and self.Harness:GetAxis("MoveX") ~= 0 then
            self.Moving = true
        end
    end,
}
)LUA";
// ==== END GUIDE: Lua ============================================================

namespace
{
    using GameCore::Entity;
    using GameCore::EntityID;
    using namespace NimbleEngine;

    class TestHost : public IGameHost
    {
    public:
        void     SetFrameRateLimit(uint32_t) override {}
        uint32_t GetFrameRateLimit() const override { return 0; }
        const InputState& GetInput() const override { return Input; }
        InputState Input;
    };

    /* A Player Start, and a car to drive, as a level file would place them. */
    class WalkerLevel : public ILevel
    {
    public:
        const char* GetName() const override { return "Walker"; }
        bool OnLoad(GameSession&) override
        {
            Entity Start = GetWorld().CreateEntity("Start");
            GameCore::TransformComponent At;
            At.Position = EngineCore::Vector3(100.0, 0.0, 0.0);
            Start.AddComponent(At);
            Start.AddComponent<PlayerStartComponent>();

            Entity Car = GetWorld().CreateEntity("Car");
            GameCore::TransformComponent CarAt;
            CarAt.Position = EngineCore::Vector3(250.0, 0.0, 0.0);
            Car.AddComponent(CarAt);
            Car.AddComponent<GameCore::NetComponent>();
            AvatarHarnessComponent Harness;
            std::snprintf(Harness.InputMap, sizeof(Harness.InputMap), "%s", "Walker");
            Harness.bRequestable = 1;
            Car.AddComponent(Harness);
            return true;
        }
        IGameMode* GetGameMode() override { return Mode; }
        IGameMode* Mode = nullptr;
    };

    struct Run
    {
        TestHost               Host;
        GameCore::ScriptEngine Scripts;
        RenderCore::Renderer   Rendering;
        GameSession*           Session = nullptr;
        WalkerLevel            Level;
        uint64_t               FrameNumber = 1;

        explicit Run(IGameMode& InMode)
        {
            Level.Mode = &InMode;
            Scripts.Initialize();
            Session = new GameSession(Host, Scripts, Rendering);
            Expect(Session->LoadLevel(Level), "the Walker level loads");
        }
        ~Run()
        {
            Session->UnloadLevel();
            delete Session;
            Scripts.Shutdown();
        }

        template<typename F> void Frame(F&& InInput)
        {
            Host.Input.BeginFrame(FrameNumber);
            InInput(Host.Input);
            FrameContext Context;
            Context.DeltaSeconds = 0.05;
            Context.FrameNumber  = FrameNumber++;
            Session->Tick(Context);
        }
        void Frame() { Frame([](InputState&) {}); }

        GameCore::World& World() { return Session->GetWorld(); }
        EntityID Player() { return FindLocalPlayer(World(), 0); }
        EntityID Avatar() { return GetAvatar(World(), Player()); }
    };

    void TestCpp()
    {
        std::printf("[C++]\n");
        WalkerMode Mode;
        Run R(Mode);
        const EntityID Pawn = R.Avatar();
        Expect(Pawn.IsSet() && std::string(R.World().GetEntity(Pawn).GetName()) == "Pawn 1",
               "NetGameMode spawned the pawn prefab and possessed it");
        Expect(Pawn.IsSet() && R.World().GetEntity(Pawn).GetComponent<GameCore::TransformComponent>()->Position.GetX() == 100.0,
               "at the Player Start");

        R.Frame([](InputState& In) { In.Press(FindInputKey("D")); });
        R.Frame();
        const double X = R.World().GetEntity(Pawn).GetComponent<GameCore::TransformComponent>()->Position.GetX();
        Expect(std::fabs(X - 140.0) < 1e-3, "WalkerMove moved it 400/s for two frames: " + std::to_string(X));

        R.Frame([](InputState& In) { In.Release(FindInputKey("D")); In.Press(FindInputKey("Space")); });
        Expect(R.World().GetEntity(Pawn).GetComponent<WalkerPawnComponent>()->Jumps == 1, "Jump counted once by the server");
        R.Frame();
        Expect(R.World().GetEntity(Pawn).GetComponent<WalkerPawnComponent>()->Jumps == 1, "a held key is not pressed again");

        Expect(R.Session->GetViewTarget().IsSet()
               && R.World().GetEntity(R.Session->GetViewTarget()).HasComponent<RenderCore::CameraComponent>(),
               "the pawn's Eye camera is the view");

        const EntityID Car = R.World().FindEntityByName("Car");
        R.Frame([](InputState& In) { In.Release(FindInputKey("Space")); In.Press(FindInputKey("E")); });
        Expect(R.Avatar() == Car, "Interact near the car gets in");
        R.Frame([](InputState& In) { In.Release(FindInputKey("E")); });
        R.Frame([](InputState& In) { In.Press(FindInputKey("E")); });
        Expect(R.Avatar() == Pawn, "and again gets out, back to the pawn");
    }

    void TestLua()
    {
        std::printf("[Lua]\n");
        std::ofstream(LuaScriptPath(), std::ios::binary | std::ios::trunc) << PawnLua;
        ScriptedWalkerMode Mode("native:WalkerTest.LuaPawn");
        Run R(Mode);
        R.Frame();   // the script starts, and hears it was possessed
        const EntityID Pawn = R.Avatar();
        Expect(Pawn.IsSet() && std::string(R.World().GetEntity(Pawn).GetName()) == "Local Player 1's pawn",
               "OnPossessed reached a script that started after the possession");

        R.Frame([](InputState& In) { In.Press(FindInputKey("Space")); });
        Expect(R.World().GetEntity(R.Player()).GetComponent<PlayerStateComponent>()->Score == 1, "the script's Jump scored");

        Unpossess(R.World(), R.Player());
        Expect(std::string(R.World().GetEntity(Pawn).GetName()) == "Empty pawn", "OnUnpossessed ran");
    }

    // ==== GUIDE: script graph ===================================================
    // The same nodes the editor places; the guide lists them and their links.
    NodeGraph::NodeGuid Id(const uint64_t InNumber)
    {
        NodeGraph::NodeGuid Guid;
        Guid.Hi = 0x57616c6b65724777ull;
        Guid.Lo = InNumber;
        return Guid;
    }

    NodeGraph::Node& AddNode(NodeGraph::Graph& InGraph, const uint64_t InId, const char* InType)
    {
        NodeGraph::Node Added;
        Added.Id   = Id(InId);
        Added.Type = InType;
        InGraph.Nodes.push_back(Added);
        return InGraph.Nodes.back();
    }

    void Connect(NodeGraph::Graph& InGraph, const uint64_t InFrom, const char* InFromPin, const uint64_t InTo, const char* InToPin)
    {
        InGraph.Links.push_back({ NodeGraph::PinRef{ Id(InFrom), InFromPin }, NodeGraph::PinRef{ Id(InTo), InToPin } });
    }

    NodeGraph::Graph MakePawnGraph()
    {
        NodeGraph::Graph G;
        G.SchemaName = "Script";
        G.Name       = "WalkerPawn";
        G.Properties["TickPhase"] = "PrePhysics";

        /* Each tick: Jump pressed, on the server, scores 2 for the driver. */
        AddNode(G, 1, "Event.OnTick");
        AddNode(G, 2, "Component.AvatarHarness.Get");
        AddNode(G, 3, "Component.AvatarHarness.WasPressed").Constants["Action"] = "Jump";
        AddNode(G, 4, "Library.Net.HasAuthority");
        AddNode(G, 5, "Logic.And");
        AddNode(G, 6, "Flow.Branch");
        AddNode(G, 7, "Component.AvatarHarness.GetPossessor");
        AddNode(G, 8, "Library.Game.AddScore").Constants["Amount"] = "2";
        Connect(G, 2, "AvatarHarness", 3, "Target");
        Connect(G, 3, "Pressed", 5, "A");
        Connect(G, 4, "Authority", 5, "B");
        Connect(G, 1, "Then", 6, "Exec");
        Connect(G, 5, "Out", 6, "Condition");
        Connect(G, 2, "AvatarHarness", 7, "Target");
        Connect(G, 6, "True", 8, "Exec");
        Connect(G, 7, "Player", 8, "Player");

        /* Anyone taking any avatar: "<player> drives <avatar>". */
        AddNode(G, 10, "Library.Game.OnPossessed");
        AddNode(G, 11, "Entity.GetName");
        AddNode(G, 12, "String.Append").Constants["B"] = " drives ";
        AddNode(G, 13, "Entity.GetName");
        AddNode(G, 14, "String.Append");
        AddNode(G, 15, "Debug.Print");
        Connect(G, 10, "Player", 11, "Entity");
        Connect(G, 11, "Name", 12, "A");
        Connect(G, 10, "Avatar", 13, "Entity");
        Connect(G, 12, "Out", 14, "A");
        Connect(G, 13, "Name", 14, "B");
        Connect(G, 10, "Then", 15, "Exec");
        Connect(G, 14, "Out", 15, "Value");
        return G;
    }
    // ==== END GUIDE: script graph ===============================================

    void TestGraph()
    {
        std::printf("[script graph]\n");
        NodeGraph::RefreshSchemas();
        const NodeGraph::Schema* Script = NodeGraph::FindSchema("Script");
        Expect(Script && Script->FindNode("Component.AvatarHarness.WasPressed"), "the harness's queries are graph nodes");
        Expect(Script && Script->FindNode("Library.Game.OnPossessed"), "Game.Possessed is an event node");

        NodeGraph::Graph G = MakePawnGraph();
        NodeGraph::CompileResult Compiled;
        NodeGraph::CompileGraph(G, Compiled);
        Expect(!Compiled.HasErrors(), "the pawn graph compiles");
        if(Compiled.HasErrors()) { std::printf("%s\n", NodeGraph::FormatDiagnostics(G, Compiled).c_str()); return; }
        if(std::getenv("NIMBLE_PRINT_GUIDE_GRAPH")) { std::printf("%s\n", Compiled.Code.c_str()); }

        std::ofstream(GraphScriptPath(), std::ios::binary | std::ios::trunc) << Compiled.Code;
        ScriptedWalkerMode Mode("native:WalkerTest.GraphPawn");
        Run R(Mode);
        R.Frame();
        R.Frame([](InputState& In) { In.Press(FindInputKey("Space")); });
        Expect(R.World().GetEntity(R.Player()).GetComponent<PlayerStateComponent>()->Score == 2, "the graph's Jump scored 2");
    }
}

int main()
{
    std::printf("NimbleEngineAvatarGuideTests\n");
    TestCpp();
    TestLua();
    TestGraph();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
