/*
 * NimbleEngineGameModeGuideTests -- Engine/Docs/Guides/GameModeGuide.md's
 * examples, built and run, so the guide stays true: Coin Rush's state and mode
 * in C++, a mode built on NetGameMode, the rules driven from Lua (a startup
 * script and a coin prefab's script), and from a script graph.
 *
 * Between the "GUIDE" markers is the guide's code; the rest drives it.
 *
 * The guide's graph screenshot: NIMBLE_GUIDE_GRAPH_OUT=<file.script.nmgraph> saves the graph
 * from here; the editor's NimbleEditorDetailsTests then draws it (NIMBLE_GRAPH_SNAPSHOT_FILE,
 * NIMBLE_GRAPH_SNAPSHOT=<png>; QT_QPA_FONTDIR=C:/Windows/Fonts for text offscreen).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Public/NodeGraph/Schema.h"
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

static std::string TempPath(const char* InName) { return (std::filesystem::temp_directory_path() / InName).generic_string(); }

// ==== GUIDE: C++ state and mode =================================================

// --- The state: the game's progress. The engine keeps the phase in it. ----------
class CoinRushState : public NimbleEngine::GameState
{
public:
    /* Back to starting values on every level (re)start, before OnStart. */
    void OnReset() override
    {
        CoinsToWin  = 3;
        SecondsLeft = 30.0;
    }

    int32_t CoinsToWin  = 3;
    double  SecondsLeft = 30.0;
};

// --- The mode: the rules. -------------------------------------------------------
class CoinRushMode : public NimbleEngine::IGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        Session = &InSession;     // the one handle a mode keeps; it outlives the mode
        return true;
    }

    void OnStart() override
    {
        Session->SetPhase("Playing");
    }

    /* Once per frame, while the game is not paused. The FrameContext carries
     * this frame's numbers; nothing here reaches for a clock or a window. */
    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        if(!State.IsPhase("Playing")) { return; }

        State.SecondsLeft -= InFrame.DeltaSeconds;
        if(State.SecondsLeft <= 0.0) { Session->SetPhase("GameOver"); }
    }

    void OnUnload() override
    {
        Session = nullptr;
    }

    NimbleEngine::GameState* GetGameState() override { return &State; }

    // --- Rules: asked before, told after ---------------------------------------

    /* A finished round only goes back to the start. */
    bool CanEnterPhase(const char* InFrom, const char* InTo) override
    {
        const bool bFinished = std::strcmp(InFrom, "Won") == 0 || std::strcmp(InFrom, "GameOver") == 0;
        return !bFinished || std::strcmp(InTo, "WaitingToStart") == 0;
    }

    void OnPhaseChanged(const char* InPrevious, const char* InCurrent) override
    {
        CORE_LOG(std::string("[CoinRush] ") + InPrevious + " -> " + InCurrent)
        Phases.push_back(std::string(InPrevious) + ">" + InCurrent);
    }

    void OnScoreChanged(GameCore::EntityID /*InPlayer*/, int32_t InScore) override
    {
        if(InScore >= State.CoinsToWin) { Session->SetPhase("Won"); }
    }

    void OnLivesChanged(GameCore::EntityID /*InPlayer*/, int32_t InLives) override
    {
        if(InLives <= 0) { Session->SetPhase("GameOver"); }
    }

    NimbleEngine::GameSession* Session = nullptr;
    CoinRushState              State;
    std::vector<std::string>   Phases;   // for the test
};

// ==== END GUIDE: C++ state and mode ============================================

// ==== GUIDE: registration =======================================================
//
//   class CoinRushModule : public NimbleEngine::IGameModule
//   {
//   public:
//       const char* GetName() const override          { return "CoinRushGame"; }
//       const char* GetVersionString() const override { return "0.1"; }
//
//       bool OnStartup(const EngineCore::ModuleReloadState* /*InPrev*/) override
//       {
//           NimbleEngine::RegisterGameMode<CoinRushMode>("CoinRush");
//           return true;
//       }
//
//       void OnShutdown(EngineCore::ModuleReloadState& /*OutState*/) override
//       {
//           NimbleEngine::UnregisterGameMode("CoinRush");
//       }
//   };
//
//   NIMBLE_IMPLEMENT_GAME_MODULE(CoinRushModule, "CoinRushGame")
//
// ==== END GUIDE: registration ===================================================

// ==== GUIDE: NetGameMode ========================================================

// --- A pawn for each player: a native prefab (or author Content/Prefabs/Runner.nmprefab)
static void BuildRunner(GameCore::PrefabBuilder& B)
{
    GameCore::PrefabBuilder::Node Root = B.Root("Runner");
    Root.Add<GameCore::TransformComponent>();
    Root.Add<GameCore::NetComponent>();
    Root.Add<NimbleEngine::AvatarHarnessComponent>();
}
GAMECORE_DEFINE_NATIVE_PREFAB(CoinRushRunner, "CoinRush.Runner", &BuildRunner)

/* The same rules, for a game with avatars: NetGameMode gives each player a
 * Runner at a Player Start, and Coin Rush's rules ride on top. */
class CoinRushNetMode : public NimbleEngine::NetGameMode
{
public:
    bool OnLoad(NimbleEngine::GameSession& InSession) override
    {
        return NetGameMode::OnLoad(InSession);          // keeps Session, resets the join order
    }

    void OnStart() override
    {
        NetGameMode::OnStart();                         // local players get their Runners
        Session->SetPhase("Playing");
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override   // NetGameMode leaves this one to you
    {
        (void)InFrame;
    }

    NimbleEngine::GameState* GetGameState() override { return &State; }   // NetGameMode has no state of its own

    void OnScoreChanged(GameCore::EntityID /*InPlayer*/, int32_t InScore) override
    {
        if(InScore >= State.CoinsToWin) { Session->SetPhase("Won"); }
    }

protected:
    const char* GetAvatarPrefab(GameCore::EntityID /*InPlayer*/) override { return "native:CoinRush.Runner"; }

    void OnAvatarSpawned(GameCore::EntityID /*InPlayer*/, GameCore::EntityID InAvatar, uint32_t InOrdinal) override
    {
        Session->GetWorld().GetEntity(InAvatar).SetName(("Runner " + std::to_string(InOrdinal + 1)).c_str());
    }

private:
    CoinRushState State;
};

// ==== END GUIDE: NetGameMode ====================================================

// ==== GUIDE: Lua ================================================================
// Scripts/CoinRush.lua -- the world's StartupScript: runs once the mode has loaded.
static const char* const StartupLua = R"LUA(-- Places the coins, and watches the rules.
for i = 1, 4 do
    World.SpawnPrefab("native:CoinRush.Coin", i * 100, 0, 0)
end

Events.On(Game.PhaseChanged, function(phase, previous)
    print("Coin Rush: " .. previous .. " -> " .. phase)
end)

Events.On(Game.ScoreChanged, function(player, score)
    print(player:GetName() .. " has " .. math.floor(score))
end)
)LUA";

// Scripts/Coin.lua -- the coin prefab's Script: one table; each coin gets its own `self`.
static const char* const CoinLua = R"LUA(return {
    TickPhase = TickPhase.PostPhysics,

    OnStart = function(self)
        self.Transform = self.Entity:GetComponent("Transform")
    end,

    OnTick = function(self, dt)
        -- Coins are for picking up while the game is on.
        if not Game.IsPhase("Playing") then return end

        local player = Game.GetPlayer(0)
        if not player:IsValid() then return end

        local px, py = player:GetComponent("Transform"):GetPosition()
        local x, y   = self.Transform:GetPosition()
        local dx, dy = px - x, py - y

        if dx * dx + dy * dy < 50 * 50 then
            Game.AddScore(player, 1)      -- through the rules: the mode hears of it
            self.Entity:Destroy()
        end
    end,
}
)LUA";
// ==== END GUIDE: Lua ============================================================

static void BuildCoin(GameCore::PrefabBuilder& B)
{
    static const std::string Path = TempPath("NimbleCoinRushCoin.lua");
    B.Root("Coin").Add<GameCore::TransformComponent>();
    B.Script(Path.c_str());
}
GAMECORE_DEFINE_NATIVE_PREFAB(CoinRushCoin, "CoinRush.Coin", &BuildCoin)

static void BuildDirector(GameCore::PrefabBuilder& B)
{
    static const std::string Path = TempPath("NimbleCoinRushDirector.graph.lua");
    B.Root("Director").Add<GameCore::TransformComponent>();
    B.Script(Path.c_str());
}
GAMECORE_DEFINE_NATIVE_PREFAB(CoinRushDirector, "CoinRush.Director", &BuildDirector)

// ==== GUIDE: script graph =======================================================
namespace
{
    NodeGraph::NodeGuid Id(const uint64_t InNumber)
    {
        NodeGraph::NodeGuid Guid;
        Guid.Hi = 0x436f696e52757368ull;
        Guid.Lo = InNumber;
        return Guid;
    }

    NodeGraph::Node& AddNode(NodeGraph::Graph& InGraph, const uint64_t InId, const char* InType, const float InX = 0.0f, const float InY = 0.0f)
    {
        NodeGraph::Node Added;
        Added.Id   = Id(InId);
        Added.Type = InType;
        Added.X    = InX;
        Added.Y    = InY;
        InGraph.Nodes.push_back(Added);
        return InGraph.Nodes.back();
    }

    void Connect(NodeGraph::Graph& InGraph, const uint64_t InFrom, const char* InFromPin, const uint64_t InTo, const char* InToPin)
    {
        InGraph.Links.push_back({ NodeGraph::PinRef{ Id(InFrom), InFromPin }, NodeGraph::PinRef{ Id(InTo), InToPin } });
    }

    NodeGraph::Graph MakeDirectorGraph()
    {
        NodeGraph::Graph G;
        G.SchemaName = "Script";
        G.Name       = "CoinRushDirector";

        /* A bonus life when the score reaches 2. */
        AddNode(G, 1, "Library.Game.OnScoreChanged", 0, 0);
        AddNode(G, 2, "Compare.Equal", 300, 280).Constants["B"] = "2";
        AddNode(G, 3, "Flow.Branch", 560, 0);
        AddNode(G, 4, "Library.Game.AddLives", 820, 240).Constants["Amount"] = "1";
        Connect(G, 1, "Score", 2, "A");
        Connect(G, 1, "Then", 3, "Exec");
        Connect(G, 2, "Out", 3, "Condition");
        Connect(G, 3, "True", 4, "Exec");
        Connect(G, 1, "Player", 4, "Player");

        /* Say where the game is. */
        AddNode(G, 10, "Library.Game.OnPhaseChanged", 0, 560);
        AddNode(G, 11, "String.Append", 300, 720).Constants["A"] = "Coin Rush phase: ";
        AddNode(G, 12, "Debug.Print", 620, 560);
        Connect(G, 10, "Phase", 11, "B");
        Connect(G, 10, "Then", 12, "Exec");
        Connect(G, 11, "Out", 12, "Value");
        return G;
    }
}
// ==== END GUIDE: script graph ===================================================

static const char* const DirectorStartupLua = R"LUA(World.SpawnPrefab("native:CoinRush.Director"))LUA";

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

    /* What a [World_CoinRush] makes: the world's entities, its mode, its startup script. */
    template<class ModeType>
    class CoinRushLevel : public ILevel
    {
    public:
        explicit CoinRushLevel(bool bInPlayer = true, std::string InScript = std::string(), bool bInStart = false)
            : bPlayer(bInPlayer), bStart(bInStart), Script(std::move(InScript)) {}

        const char* GetName() const override { return "CoinRush"; }

        bool OnLoad(GameSession&) override
        {
            if(bPlayer)
            {
                Entity Player = GetWorld().CreateEntity("Player");
                GameCore::TransformComponent At;
                Player.AddComponent(At);
                PlayerStateComponent State;
                State.Lives = 3;
                Player.AddComponent(State);
            }
            if(bStart)
            {
                /* Placed by the level file in a real game. */
                Entity Start = GetWorld().CreateEntity("Start");
                Start.AddComponent<GameCore::TransformComponent>();
            }
            return true;
        }

        const char* GetStartupScript() const override { return Script.empty() ? nullptr : Script.c_str(); }
        IGameMode*  GetGameMode() override  { return &Mode; }
        GameState*  GetGameState() override { return Mode.GetGameState(); }

        bool        bPlayer;
        bool        bStart;
        std::string Script;
        ModeType    Mode;
    };

    template<class ModeType>
    struct Run
    {
        TestHost               Host;
        GameCore::ScriptEngine Scripts;
        RenderCore::Renderer   Rendering;
        GameSession*           Session = nullptr;
        CoinRushLevel<ModeType> Level;

        explicit Run(bool bInPlayer = true, std::string InScript = std::string(), bool bInStart = false)
            : Level(bInPlayer, std::move(InScript), bInStart)
        {
            Scripts.Initialize();
            Session = new GameSession(Host, Scripts, Rendering);
            Expect(Session->LoadLevel(Level), "the Coin Rush level loads");
        }
        ~Run()
        {
            Session->UnloadLevel();
            delete Session;
            Scripts.Shutdown();
        }

        void Frame(double InSeconds = 0.05)
        {
            FrameContext Context;
            Context.DeltaSeconds = InSeconds;
            Context.FrameNumber  = Frame_++;
            Session->Tick(Context);
        }

        GameCore::World& World() { return Session->GetWorld(); }
        Entity Player() { return World().GetEntity(Session->GetPlayer(0)); }
        PlayerStateComponent& State() { return *Player().GetComponent<PlayerStateComponent>(); }
        uint64_t Frame_ = 1;
    };

    void TestCpp()
    {
        std::printf("[C++]\n");
        {
            Run<CoinRushMode> R;
            Expect(std::string(R.Session->GetPhase()) == "Playing", "OnStart began the game");
            Expect(R.Level.Mode.State.CoinsToWin == 3, "the state was reset before OnStart");

            R.Session->AddScore(R.Player().GetID(), 1);
            R.Session->AddScore(R.Player().GetID(), 1);
            Expect(R.Session->GetPhase() == std::string("Playing"), "two coins is not enough");
            R.Session->AddScore(R.Player().GetID(), 1);
            Expect(R.Session->GetPhase() == std::string("Won"), "the third coin won (OnScoreChanged -> SetPhase)");
            Expect(!R.Session->SetPhase("Playing"), "CanEnterPhase: a finished round cannot resume");
            Expect(R.Session->GetPhase() == std::string("Won"), "a refused change changes nothing");
            Expect(R.Session->SetPhase("WaitingToStart") && R.Session->SetPhase("Playing"), "it can start over");
        }
        {
            Run<CoinRushMode> R;
            R.Session->AddLives(R.Player().GetID(), -3);
            Expect(R.Session->GetPhase() == std::string("GameOver"), "no lives left: GameOver (OnLivesChanged)");
        }
        {
            Run<CoinRushMode> R;
            R.Frame(10.0);
            R.Frame(10.0);
            Expect(R.Session->GetPhase() == std::string("Playing"), "20 of 30 seconds, still playing");
            R.Frame(10.0);
            Expect(R.Session->GetPhase() == std::string("GameOver"), "the FrameContext's DeltaSeconds ran the clock out");

            R.Session->SetPaused(true);
            Expect(R.Session->SetPhase("WaitingToStart"), "restart");
            R.Session->SetPhase("Playing");
            R.Frame(100.0);
            Expect(R.Session->GetPhase() == std::string("Playing"), "paused: OnTick is not called, the clock holds");
        }
    }

    void TestNet()
    {
        std::printf("[NetGameMode]\n");
        Run<CoinRushNetMode> R(false, std::string(), true);
        Expect(std::string(R.Session->GetPhase()) == "Playing", "the derived OnStart began the game");
        const EntityID Runner = R.World().FindEntityByName("Runner 1");
        Expect(Runner.IsSet(), "NetGameMode spawned a Runner for the local player; OnAvatarSpawned named it");
        Expect(GetAvatar(R.World(), FindLocalPlayer(R.World(), 0)) == Runner, "and the player possesses it");
    }

    void TestLua()
    {
        std::printf("[Lua]\n");
        std::ofstream(TempPath("NimbleCoinRushCoin.lua"), std::ios::binary | std::ios::trunc) << CoinLua;
        const std::string Startup = TempPath("NimbleCoinRushStartup.lua");
        std::ofstream(Startup, std::ios::binary | std::ios::trunc) << StartupLua;

        Run<CoinRushMode> R(true, Startup);
        R.Frame();
        Expect(R.World().FindEntityByName("Coin").IsSet(), "the startup script placed coins");

        auto CoinsLeft = [&R]()
        {
            int Count = 0;
            R.World().Each<GameCore::TransformComponent>([&Count](Entity InEntity, GameCore::TransformComponent&)
            {
                if(std::string(InEntity.GetName()) == "Coin") { ++Count; }
            });
            return Count;
        };
        Expect(CoinsLeft() == 4, "four coins");

        const auto MoveTo = [&R](double InX)
        {
            R.Player().GetComponent<GameCore::TransformComponent>()->Position = EngineCore::Vector3(InX, 0.0, 0.0);
            R.Frame();
        };
        MoveTo(100.0);
        Expect(R.State().Score == 1 && CoinsLeft() == 3, "walking onto a coin scored through Game.AddScore and removed it");
        MoveTo(200.0);
        MoveTo(300.0);
        Expect(R.State().Score == 3, "three coins");
        Expect(std::string(R.Session->GetPhase()) == "Won", "and the C++ mode, hearing Lua's scores, ended the round");
        MoveTo(400.0);
        Expect(R.State().Score == 3 && CoinsLeft() == 1, "no coin is taken once the round is won");
    }

    void TestGraph()
    {
        std::printf("[script graph]\n");
        NodeGraph::RefreshSchemas();
        const NodeGraph::Schema* Script = NodeGraph::FindSchema("Script");
        Expect(Script && Script->FindNode("Library.Game.OnScoreChanged") && Script->FindNode("Library.Game.OnPhaseChanged")
               && Script->FindNode("Library.Game.AddLives"), "the Game library's events and calls are graph nodes");

        NodeGraph::Graph G = MakeDirectorGraph();
        NodeGraph::CompileResult Compiled;
        NodeGraph::CompileGraph(G, Compiled);
        Expect(!Compiled.HasErrors(), "the director graph compiles");
        if(Compiled.HasErrors()) { std::printf("%s\n", NodeGraph::FormatDiagnostics(G, Compiled).c_str()); return; }
        if(std::getenv("NIMBLE_PRINT_GUIDE_GRAPH")) { std::printf("%s\n", Compiled.Code.c_str()); }

        /* NIMBLE_GUIDE_GRAPH_OUT=<file.script.nmgraph>: the graph as the editor opens it, for the guide's screenshots. */
        if(const char* Out = std::getenv("NIMBLE_GUIDE_GRAPH_OUT"))
        {
            std::string Error;
            Expect(NodeGraph::SaveGraphFile(Out, G, Error), "the graph saves: " + Error);
        }
        std::ofstream(TempPath("NimbleCoinRushDirector.graph.lua"), std::ios::binary | std::ios::trunc) << Compiled.Code;
        const std::string Startup = TempPath("NimbleCoinRushDirectorStartup.lua");
        std::ofstream(Startup, std::ios::binary | std::ios::trunc) << DirectorStartupLua;

        Run<CoinRushMode> R(true, Startup);
        R.Frame();
        Expect(R.World().FindEntityByName("Director").IsSet(), "the director prefab is in the level");
        Expect(R.State().Lives == 3, "3 lives to begin with");
        R.Session->AddScore(R.Player().GetID(), 1);
        Expect(R.State().Lives == 3, "one coin: no bonus");
        R.Session->AddScore(R.Player().GetID(), 1);
        Expect(R.State().Lives == 4, "two coins: the graph's bonus life");
        R.Session->AddScore(R.Player().GetID(), 1);
        Expect(R.State().Lives == 4, "three: only at exactly two");
    }
}

int main()
{
    std::printf("NimbleEngineGameModeGuideTests\n");
    TestCpp();
    TestNet();
    TestLua();
    TestGraph();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
