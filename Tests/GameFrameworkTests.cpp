/*
 * NimbleEngineGameFrameworkTests -- the game framework (GAMEFRAMEWORK.md):
 * the GameState's phase, the GameMode's rules, PlayerState components, the
 * "Game" script library, and the phase's save chunk. A real GameSession, with
 * a host that has no window and a Renderer that was never initialised (it
 * draws nothing, and says so once per load).
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/RenderCore/Renderer.h"

using namespace NimbleEngine;

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
    class TestHost : public IGameHost
    {
    public:
        void     SetFrameRateLimit(uint32_t) override {}
        uint32_t GetFrameRateLimit() const override { return 0; }
    };

    class TestState : public GameState
    {
    public:
        void OnReset() override { Resets++; }
        const char* GetInitialPhase() const override { return "Lobby"; }

        int Resets = 0;
    };

    /* Records every hook, in order, as one string each. */
    class TestMode : public IGameMode
    {
    public:
        bool OnLoad(GameSession& InSession) override { Session = &InSession; return true; }
        void OnTick(const FrameContext&) override {}
        void OnUnload() override { Session = nullptr; }

        bool CanEnterPhase(const char* InFrom, const char* InTo) override
        {
            /* The rule under test: nothing skips straight from Lobby to Over. */
            return !(std::strcmp(InFrom, "Lobby") == 0 && std::strcmp(InTo, "Over") == 0);
        }

        void OnPhaseChanged(const char* InPrevious, const char* InCurrent) override
        {
            Log.push_back(std::string("phase ") + InPrevious + ">" + InCurrent);

            /* A change asked for mid-announcement waits its turn. */
            if(std::strcmp(InCurrent, "Countdown") == 0 && Session)
            {
                Session->SetPhase("Playing");
                Log.push_back(std::string("after request, phase is ") + Session->GetPhase());
            }
        }

        void OnPlayerAdded(GameCore::EntityID)   override { Log.push_back("player+"); }
        void OnPlayerRemoved(GameCore::EntityID) override { Log.push_back("player-"); }
        void OnScoreChanged(GameCore::EntityID, int32_t InScore) override { Log.push_back("score " + std::to_string(InScore)); }
        void OnLivesChanged(GameCore::EntityID, int32_t InLives) override { Log.push_back("lives " + std::to_string(InLives)); }

        GameSession*             Session = nullptr;
        std::vector<std::string> Log;
    };

    class TestLevel : public ILevel
    {
    public:
        const char* GetName() const override { return "FrameworkTest"; }

        bool OnLoad(GameSession&) override
        {
            /* A player before the mode has loaded: reported once it has. */
            GameCore::Entity Player = GetWorld().CreateEntity("Player");
            PlayerStateComponent Init;
            Init.Lives = 3;
            Player.AddComponent(Init);
            return true;
        }

        void OnPrepareForLoadedGame() override
        {
            std::vector<GameCore::EntityID> Doomed;
            GetWorld().ForEachEntity([](GameCore::EntityID InEntity, void* InUserData)
            {
                static_cast<std::vector<GameCore::EntityID>*>(InUserData)->push_back(InEntity);
            }, &Doomed);
            for(const GameCore::EntityID Entity : Doomed) { GetWorld().DestroyEntity(Entity); }
        }

        const char* GetStartupScript() const override { return StartupScript; }
        IGameMode*  GetGameMode() override  { return &Mode; }
        GameState*  GetGameState() override { return &State; }

        const char* StartupScript = "";
        TestMode    Mode;
        TestState   State;
    };

    bool Has(const std::vector<std::string>& InLog, const std::string& InEntry)
    {
        for(const std::string& Each : InLog) { if(Each == InEntry) { return true; } }
        return false;
    }

    void EventRecorder(GameCore::EventID, const GameCore::EventArgs& InArgs, void* InUserData)
    {
        auto& Seen = *static_cast<std::vector<std::string>*>(InUserData);
        std::string Line;
        for(uint8_t Index = 0; Index < InArgs.Count; ++Index)
        {
            const GameCore::EventArg& Arg = InArgs.Args[Index];
            if(Index) { Line += ","; }
            if(Arg.Type == GameCore::EventArg::ET_String)      { Line += Arg.String; }
            else if(Arg.Type == GameCore::EventArg::ET_Number) { Line += std::to_string(int(Arg.Number)); }
            else if(Arg.Type == GameCore::EventArg::ET_Entity) { Line += "entity"; }
        }
        Seen.push_back(Line);
    }

    struct Fixture
    {
        TestHost               Host;
        GameCore::ScriptEngine Scripts;
        RenderCore::Renderer   Rendering;
        GameSession*           Session = nullptr;

        Fixture()  { Scripts.Initialize(); Session = new GameSession(Host, Scripts, Rendering); }
        ~Fixture() { delete Session; Scripts.Shutdown(); }

        void Tick(double InSeconds)
        {
            FrameContext Frame;
            Frame.DeltaSeconds = InSeconds;
            Session->Tick(Frame);
        }
    };

    void TestPhases()
    {
        std::printf("[phases]\n");
        Fixture F;
        TestLevel Level;

        Expect(F.Session->LoadLevel(Level), "the level loads");
        Expect(Level.State.Resets == 1, "the state was reset once");
        Expect(std::string(F.Session->GetPhase()) == "Lobby", "a load starts in the state's initial phase");
        Expect(FindGameSession(F.Session->GetWorld()) == F.Session, "the World finds its session");
        Expect(Has(Level.Mode.Log, "player+"), "a player the level made is reported once the mode has loaded");

        std::vector<std::string> Events;
        F.Session->GetWorld().GetEvents().Subscribe("PhaseChanged", &EventRecorder, &Events);

        Expect(!F.Session->SetPhase("Over"), "the mode's rules refuse Lobby -> Over");
        Expect(std::string(F.Session->GetPhase()) == "Lobby", "a refused change changes nothing");
        Expect(F.Session->SetPhase("Lobby"), "the same phase is accepted, as a no-op");
        Expect(Events.empty(), "neither a refusal nor a no-op is announced");

        F.Tick(0.25);
        F.Tick(0.25);
        Expect(Level.State.GetPhaseSeconds() > 0.49 && Level.State.GetPhaseSeconds() < 0.51, "the phase timer advances with the tick");

        Level.Mode.Log.clear();
        Expect(F.Session->SetPhase("Countdown"), "Lobby -> Countdown is accepted");
        Expect(Level.State.GetPhaseSeconds() == 0.0, "entering a phase restarts its timer");
        Expect(Level.Mode.Log.size() == 3
               && Level.Mode.Log[0] == "phase Lobby>Countdown"
               && Level.Mode.Log[1] == "after request, phase is Countdown"
               && Level.Mode.Log[2] == "phase Countdown>Playing",
               "a change asked for mid-announcement is applied after it, in order");
        Expect(Events.size() == 2 && Events[0] == "Countdown,Lobby" && Events[1] == "Playing,Countdown",
               "PhaseChanged(Phase, Previous) is emitted for each change, in order");
        Expect(F.Session->SetPhase("Over"), "Playing -> Over is allowed");

        char TooLong[GAME_PHASE_NAME_MAX + 4];
        std::memset(TooLong, 'x', sizeof(TooLong) - 1);
        TooLong[sizeof(TooLong) - 1] = '\0';
        Expect(!F.Session->SetPhase(TooLong) && !F.Session->SetPhase(""), "an empty or too-long phase name is refused");

        F.Session->UnloadLevel();
        Expect(FindGameSession(*reinterpret_cast<GameCore::World*>(&Level)) == nullptr, "nothing else finds a session");
    }

    void TestPlayers()
    {
        std::printf("[players]\n");
        Fixture F;
        TestLevel Level;
        F.Session->LoadLevel(Level);
        GameCore::World& World = F.Session->GetWorld();

        const GameCore::EntityID First = F.Session->GetPlayer(0);
        Expect(First.IsSet() && F.Session->GetPlayerCount() == 1, "the level's player is player 0");

        GameCore::Entity Second = World.CreateEntity("Second");
        PlayerStateComponent Init;
        Init.PlayerIndex = 1;
        Level.Mode.Log.clear();
        Second.AddComponent(Init);
        Expect(Level.Mode.Log.size() == 1 && Level.Mode.Log[0] == "player+", "adding a PlayerState tells the mode");
        Expect(F.Session->GetPlayer(1) == Second.GetID() && F.Session->GetPlayerCount() == 2, "GetPlayer finds by PlayerIndex");
        Expect(!F.Session->GetPlayer(7).IsSet(), "an unknown index is no player");

        std::vector<std::string> Events;
        World.GetEvents().Subscribe("ScoreChanged", &EventRecorder, &Events);
        World.GetEvents().Subscribe("LivesChanged", &EventRecorder, &Events);

        Level.Mode.Log.clear();
        Expect(F.Session->AddScore(First, 10) && F.Session->AddScore(First, 5), "AddScore on a player");
        Expect(F.Session->AddLives(First, -1), "AddLives on a player");
        Expect(World.GetEntity(First).GetComponent<PlayerStateComponent>()->Score == 15
               && World.GetEntity(First).GetComponent<PlayerStateComponent>()->Lives == 2, "the component holds the totals");
        Expect(Level.Mode.Log.size() == 3 && Level.Mode.Log[0] == "score 10" && Level.Mode.Log[1] == "score 15"
               && Level.Mode.Log[2] == "lives 2", "the mode hears each change with the new value");
        Expect(Events.size() == 3 && Events[1] == "entity,15" && Events[2] == "entity,2", "ScoreChanged / LivesChanged are emitted");

        GameCore::Entity Loose = World.CreateEntity("Loose");
        Expect(!F.Session->AddScore(Loose.GetID(), 1), "AddScore needs a PlayerState");

        Level.Mode.Log.clear();
        World.DestroyEntity(Second.GetID());
        Expect(Level.Mode.Log.size() == 1 && Level.Mode.Log[0] == "player-", "destroying a player tells the mode");

        F.Session->UnloadLevel();
    }

    void TestScripts()
    {
        std::printf("[scripts]\n");
        Fixture F;
        TestLevel Level;
        F.Session->LoadLevel(Level);

        const bool bRan = F.Scripts.RunString(R"(
            Seen = {}
            Events.On(Game.PhaseChanged, function(phase, previous) Seen[#Seen + 1] = previous .. ">" .. phase end)
            Events.On(Game.ScoreChanged, function(player, score) LastScore = score end)

            assert(Game.GetPhase() == "Lobby")
            assert(Game.IsPhase("Lobby") and not Game.IsPhase("Playing"))
            assert(Game.GetRound() == 0)
            assert(Game.RequestPhase("Over") == false, "the rules refuse")
            assert(Game.RequestPhase("Playing") == true)
            assert(Game.GetPhase() == "Playing" and Game.GetPhaseTime() == 0)

            local p = Game.GetPlayer(0)
            assert(p:IsValid() and Game.GetPlayerCount() == 1)
            assert(Game.AddScore(p, 7) == 7)
            assert(Game.AddLives(p, -1) == 2)
            local ps = p:GetComponent("PlayerState")
            assert(ps:GetScore() == 7 and ps:GetLives() == 2 and ps:GetPlayerIndex() == 0)
            assert(ps.SetScore == nil, "Score is read-only to scripts")
            assert(not Game.GetPlayer(5):IsValid())
        )", "framework-1");
        Expect(bRan, "the Game library works from Lua");
        Expect(F.Scripts.RunString("assert(#Seen == 1 and Seen[1] == 'Lobby>Playing') assert(LastScore == 7)", "framework-2"),
               "Lua hears PhaseChanged and ScoreChanged");
        Expect(F.Scripts.GetErrorCount() == 0, "no Lua errors");

        F.Session->UnloadLevel();

        /* A bare World has no session: an error, not a crash. */
        GameCore::World Bare;
        GameCore::ScriptEngine Lone;
        Lone.Initialize();
        Lone.BindWorld(&Bare);
        Expect(!Lone.RunString("Game.GetPhase()", "framework-3"), "the Game library needs a running game");
        Lone.UnbindWorld();
        Lone.Shutdown();
    }

    void TestStartupScript()
    {
        std::printf("[startup script]\n");
        std::FILE* File = std::fopen("FrameworkStartup.lua", "w");
        std::fputs("StartupRan = (StartupRan or 0) + 1\n", File);
        std::fclose(File);
        EngineCore::LoadConfigText("[Scripting]\nStartupScript = FrameworkStartup.lua\n");

        Fixture F;
        TestLevel Level;

        Level.StartupScript = "";
        F.Session->LoadLevel(Level);
        Expect(F.Scripts.RunString("assert(StartupRan == nil)", "startup-1"), "\"\" runs no startup script");
        F.Session->UnloadLevel();

        Level.StartupScript = nullptr;
        F.Session->LoadLevel(Level);
        Expect(F.Scripts.RunString("assert(StartupRan == 1)", "startup-2"), "nullptr runs [Scripting] StartupScript");
        F.Session->UnloadLevel();

        EngineCore::LoadConfigText("[Scripting]\nStartupScript = \n");
        std::remove("FrameworkStartup.lua");
    }

    void TestSave()
    {
        std::printf("[save]\n");
        Fixture F;
        TestLevel Level;
        RegisterLevel(Level);

        F.Session->LoadLevel(Level);
        F.Session->SetPhase("Playing");
        Level.State.SetRound(4);
        F.Tick(1.5);
        F.Session->AddScore(F.Session->GetPlayer(0), 42);
        Expect(SaveGameToFile(*F.Session, "FrameworkTest.nmsg"), "the game saves");

        F.Session->SetPhase("Over");
        Level.State.SetRound(9);

        Expect(LoadGameFromFile(*F.Session, "FrameworkTest.nmsg"), "the save is accepted");
        Level.Mode.Log.clear();
        F.Session->ApplyPendingLevel();

        Expect(std::string(F.Session->GetPhase()) == "Playing", "the phase is restored");
        Expect(Level.State.GetRound() == 4, "the round is restored");
        Expect(Level.State.GetPhaseSeconds() > 1.49 && Level.State.GetPhaseSeconds() < 1.51, "the time in the phase is restored");
        Expect(!Has(Level.Mode.Log, "phase Lobby>Playing"), "a restore is not a transition: no hooks");

        const GameCore::EntityID Player = F.Session->GetPlayer(0);
        const PlayerStateComponent* State = F.Session->GetWorld().GetEntity(Player).GetComponent<PlayerStateComponent>();
        Expect(State && State->Score == 42 && State->Lives == 3, "the PlayerState is restored with the World");
        Expect(F.Session->GetPlayerCount() == 1, "the restored World has its one player, not two");

        F.Session->UnloadLevel();
        UnregisterLevel(Level);
        std::remove("FrameworkTest.nmsg");
    }
}

/* The editor's Play starts the level of the open world: FindLevelByWorldFile. */
static void TestLevelByWorldFile()
{
    std::printf("[level by world file]\n");

    class FileLevel : public ILevel
    {
    public:
        FileLevel(const char* InName, const char* InFile) : Name(InName), File(InFile) {}
        const char* GetName() const override      { return Name; }
        const char* GetWorldFile() const override { return File; }
        const char* Name;
        const char* File;
    };
    FileLevel Yard("WorldFileYard", "Content/Levels/Yard.nmworld");
    FileLevel Code("WorldFileCode", nullptr);
    RegisterLevel(Yard);
    RegisterLevel(Code);

    Expect(FindLevelByWorldFile("Content/Levels/Yard.nmworld") == &Yard, "a level is found by the world it loads");
    Expect(FindLevelByWorldFile("content\\levels\\YARD.nmworld") == &Yard, "case and slashes do not matter");
    Expect(FindLevelByWorldFile("Content/Levels/Other.nmworld") == nullptr, "another world finds no level");
    Expect(FindLevelByWorldFile("") == nullptr && FindLevelByWorldFile(nullptr) == nullptr,
           "no world finds no level, not one that loads none");

    UnregisterLevel(Yard);
    UnregisterLevel(Code);
}

// === Networking, without a session ============================================
// What every build has, NetCore compiled in or out: RPCs that run locally,
// the facade's Standalone answers, the launchers' command line, and a
// headless Application -- the dedicated server's host, minus the network.

struct TestBeaconComponent
{
    GAMECORE_COMPONENT_BODY()
    int32_t Flashes = 0;
    int32_t LastPower = 0;

    void ServerFlash(const GameCore::RpcContext& InContext, int32_t InPower)
    {
        ++Flashes;
        LastPower = InPower;
        bLastRemote = InContext.bRemote;
    }
    bool bLastRemote = true;
};

GAMECORE_DEFINE_COMPONENT(TestBeaconComponent, "TestBeacon")

GAMECORE_DEFINE_COMPONENT_RPCS(TestBeaconComponent)
{
    Rpc.Server<&TestBeaconComponent::ServerFlash>("ServerFlash");
}

static void TestNetworkingStandalone()
{
    std::printf("[networking, standalone]\n");

    GameCore::World World;
    GameCore::Entity Beacon = World.CreateEntity("Beacon");
    Beacon.AddComponent<TestBeaconComponent>();

    Expect(GetNetMode() == ENetMode::Standalone && HasAuthority() && !IsClient(), "with no session, the process is standalone");
    Expect(GetLocalClientId() == GameCore::NET_CLIENT_NONE, "and has no client id");
    GameCore::NetComponent Net;
    Expect(IsLocallyOwned(Net), "and owns what no client does");
    Expect(IsNetworkingAvailable() == (WITH_NETCORE != 0), "the facade knows whether NetCore is built");

    Expect(GAMECORE_RPC(TestBeaconComponent, ServerFlash)(Beacon, 7), "an RPC is accepted");
    const TestBeaconComponent* State = Beacon.GetComponent<TestBeaconComponent>();
    Expect(State->Flashes == 1 && State->LastPower == 7 && !State->bLastRemote, "and runs here, at once");

    size_t Count = 0;
    GameCore::FindComponentReplication(GameCore::TransformComponent::StaticType().ID, Count);
    Expect(WITH_NETCORE ? Count == 3 : Count == 0, "Transform replicates exactly when NetCore is built");

    const char* const Args[] = { "Game", "-connect=10.0.0.5:9000", "-Display.RHI=0", "-listen", "stray", "-port=7000" };
    const std::vector<std::string> Overrides = ParseLaunchArgs(int(sizeof(Args) / sizeof(Args[0])), Args);
    const std::vector<std::string> Expected = { "Net.ServerAddress 10.0.0.5", "Net.Port 9000", "Display.RHI 0", "Net.Listen 1", "Net.Port 7000" };
    Expect(Overrides == Expected, "the command line becomes config overrides, in order, strays dropped");
}

static void TestHeadlessApplication()
{
    std::printf("[headless application]\n");

    class CountingMode : public IGameMode
    {
    public:
        void OnTick(const FrameContext&) override { ++Ticks; }
        int Ticks = 0;
    };

    CountingMode Mode;
    ApplicationDesc Desc;
    Desc.bHeadless = true;
    Application App(Desc);
    App.SetConfigOverrides({ "Application.MaxFrames 3", "Display.FrameRateLimit 0" });
    App.Exec(Mode);
    Expect(Mode.Ticks == 3, "a headless Application runs with no window, for MaxFrames frames");
}

int main()
{
    std::printf("NimbleEngineGameFrameworkTests\n");

    TestPhases();
    TestPlayers();
    TestScripts();
    TestStartupScript();
    TestSave();
    TestLevelByWorldFile();
    TestNetworkingStandalone();
    TestHeadlessApplication();   // last: the Application claims the main thread

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
