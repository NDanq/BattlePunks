/*
 * NimbleEngineGameFrameworkTests -- the game framework (GAMEFRAMEWORK.md):
 * the GameState's phase, the GameMode's rules, PlayerState components, the
 * "Game" script library, and the phase's save chunk. A real GameSession, with
 * a host that has no window and a Renderer that was never initialised (it
 * draws nothing, and says so once per load).
 */

#include <cmath>
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
#include "Public/NimbleEngine/GameConsole.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/LagCompensation.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/NetFacade.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/NimbleEngine/WorldList.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/UI/UISystem.h"

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
        Fixture F;
        TestLevel Level;

        Level.StartupScript = "";
        F.Session->LoadLevel(Level);
        Expect(F.Scripts.RunString("assert(StartupRan == nil)", "startup-1"), "\"\" runs no startup script");
        F.Session->UnloadLevel();

        Level.StartupScript = nullptr;
        F.Session->LoadLevel(Level);
        Expect(F.Scripts.RunString("assert(StartupRan == nil)", "startup-2"), "nullptr runs none either: no project-wide script");
        F.Session->UnloadLevel();

        Level.StartupScript = "FrameworkStartup.lua";
        F.Session->LoadLevel(Level);
        Expect(F.Scripts.RunString("assert(StartupRan == 1)", "startup-3"), "the level's own script runs");
        F.Session->UnloadLevel();

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

/* Worlds come from the config's [World_*] list; modes are registered by name. */
static void TestWorldList()
{
    std::printf("[world list]\n");

    class ListMode : public IGameMode
    {
    public:
        void OnTick(const FrameContext&) override {}
    };
    RegisterGameMode<ListMode>("ListMode");
    Expect(HasGameMode("listmode"), "a registered mode is found by name, any case");

    EngineCore::LoadConfigText("[World_ListYard]\nFile = Content/Levels/ListYard.nmworld\nGameMode = ListMode\n"
                               "StartupScript = Scripts/Yard.lua\nCook = false\n"
                               "[World_ListBare]\nFile = Content/Levels/ListBare.nmworld\n"
                               "[World_ListNoFile]\nGameMode = ListMode\n");
    const std::vector<WorldEntry> Worlds = ReadWorldList();
    const WorldEntry* Yard = nullptr;
    const WorldEntry* Bare = nullptr;
    bool bNoFile = false;
    for(const WorldEntry& Each : Worlds)
    {
        if(Each.Name == "ListYard") { Yard = &Each; }
        if(Each.Name == "ListBare") { Bare = &Each; }
        bNoFile |= Each.Name == "ListNoFile";
    }
    Expect(Yard && Yard->File == "Content/Levels/ListYard.nmworld" && Yard->GameMode == "ListMode",
           "a world's section gives its name, file and mode");
    Expect(Yard && Yard->StartupScript == "Scripts/Yard.lua" && !Yard->bCook, "a world's own startup script, and Cook=false read");
    Expect(Bare && Bare->StartupScript.empty() && Bare->bCook, "absent keys default: no script, cooked");
    Expect(!bNoFile, "a world with no File is left out");
    Expect(Yard && Bare && Yard < Bare, "the list keeps file order");

    RegisterWorldLevels();
    ILevel* YardLevel = FindLevelByName("ListYard");
    Expect(YardLevel != nullptr, "each listed world is registered as a level");
    Expect(FindLevelByWorldFile("content/levels/listyard.nmworld") == YardLevel, "and found by its world file");
    Expect(YardLevel && YardLevel->GetStartupScript() && std::string(YardLevel->GetStartupScript()) == "Scripts/Yard.lua",
           "the level runs the world's startup script");
    Expect(FindLevelByName("ListNoFile") == nullptr, "a world with no file is no level");

    UnregisterWorldLevels();
    Expect(FindLevelByName("ListYard") == nullptr, "unregistering drops the world levels");
    UnregisterGameMode("ListMode");
    Expect(!HasGameMode("ListMode"), "an unregistered mode is gone");
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

/* A level with a falling ball, a joint and a networked entity: GameSession
 * attaches physics to the World when JoltPhysics is built, and either way
 * says which of those components this build cannot run. */
class PhysicsLevel : public ILevel
{
public:
    const char* GetName() const override { return "PhysicsTest"; }
    const char* GetStartupScript() const override { return ""; }

    bool OnLoad(GameSession&) override
    {
        GameCore::Entity Ball = GetWorld().CreateEntity("Ball");
        GameCore::TransformComponent At;
        At.Position = EngineCore::Vector3(0.0, 500.0, 0.0);
        Ball.AddComponent(At);
        GameCore::BoundsComponent Box;
        Box.Local = GameCore::AABB(EngineCore::Vector3(-10.0, -10.0, -10.0), EngineCore::Vector3(10.0, 10.0, 10.0));
        Ball.AddComponent(Box);
        Ball.AddComponent<GameCore::RigidBodyComponent>();
        BallId = Ball.GetID();

        /* Apart from the ball: a joint on it would pin it to the world. */
        GameCore::Entity Marker = GetWorld().CreateEntity("Marker");
        Marker.AddComponent<GameCore::PhysicsConstraintComponent>();
        Marker.AddComponent<GameCore::NetComponent>();
        return true;
    }

    GameCore::EntityID BallId;
};

static void TestPhysicsInSession()
{
    std::printf("[physics in a session]\n");
    Fixture F;
    PhysicsLevel Level;
    Expect(F.Session->LoadLevel(Level), "a level with physics components loads");
    Expect(IsWorldSimulated(F.Session->GetWorld()) == (WITH_JOLTPHYSICS != 0), "the session simulates its World exactly when JoltPhysics is built");

    for(int Index = 0; Index < 30; ++Index) { F.Tick(1.0 / 60.0); }
    const double Y = double(F.Session->GetWorld().GetEntity(Level.BallId).GetComponent<GameCore::TransformComponent>()->Position.GetY());
    Expect(WITH_JOLTPHYSICS ? Y < 450.0 : Y == 500.0, "a Rigid Body falls with physics, and stays put (inert) without it");

    /* Which components are inactive is this build's to say. */
    const bool bExpectWarning = !WITH_JOLTPHYSICS || !WITH_NETCORE;
    Expect(WarnInactiveModuleComponents(F.Session->GetWorld(), "PhysicsTest") == bExpectWarning,
           "the inactive-module warning fires exactly when a module these components need is compiled out");
    Expect(IsInactiveModuleComponent(GameCore::RigidBodyComponent::StaticType().ID) == !WITH_JOLTPHYSICS
           && IsInactiveModuleComponent(GameCore::PhysicsConstraintComponent::StaticType().ID) == !WITH_JOLTPHYSICS
           && IsInactiveModuleComponent(GameCore::VehicleComponent::StaticType().ID) == !WITH_JOLTPHYSICS
           && IsInactiveModuleComponent(GameCore::NetComponent::StaticType().ID) == !WITH_NETCORE
           && !IsInactiveModuleComponent(GameCore::TransformComponent::StaticType().ID),
           "and knows which component needs which module");

    F.Session->UnloadLevel();
    Expect(!F.Session->HasWorld(), "and unloads, physics and all");
}

static void TestNetScripting()
{
    std::printf("[networking from Lua, standalone]\n");

    GameCore::ScriptEngine Scripts;
    Expect(Scripts.Initialize(), "a script engine starts");
    GameCore::World World;
    Scripts.BindWorld(&World);
    GameCore::Entity Beacon = World.CreateEntity("Beacon");
    Beacon.AddComponent<TestBeaconComponent>();

    Expect(Scripts.RunString(R"(
        assert(Net.HasAuthority() and Net.IsStandalone() and not Net.IsClient() and not Net.IsServer())
        assert(Net.GetLocalClientId() == -1 and not Net.IsConnected() and not Net.IsDedicatedServer())
        local beacon = World.FindEntity("Beacon")
        assert(not Net.IsNetworked(beacon) and Net.GetOwner(beacon) == -1 and Net.IsLocallyOwned(beacon))
        local b = beacon:GetComponent("TestBeacon")
        assert(b:ServerFlash(3) == true)                              -- a component's RPC, by name
        assert(Net.Call(beacon, "TestBeacon", "ServerFlash", 9) == true)  -- the same, through the library
        assert(not pcall(function() b:ServerFlash("x") end))           -- a mistyped argument raises
        assert(not pcall(function() b:ServerFlash(1, 2) end))          -- so does one too many
        assert(not pcall(function() Net.Call(beacon, "TestBeacon", "Nope") end))
    )", "net-script"), "the Net library and component RPCs answer from Lua");

    const TestBeaconComponent* State = Beacon.GetComponent<TestBeaconComponent>();
    Expect(State->Flashes == 2 && State->LastPower == 9 && !State->bLastRemote, "Lua's RPC calls ran here, standalone");

    Scripts.UnbindWorld();
    Scripts.Shutdown();
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

/* For the UI test: gameplay's own binding of the left button. */
NIMBLE_DEFINE_INPUT_MAP(UITest)
{
    Map.Action("Fire", "Mouse.Left");
    Map.Action("Jump", "Space");
}

namespace
{
    /* A host whose devices the test drives. */
    class InputHost : public IGameHost
    {
    public:
        void     SetFrameRateLimit(uint32_t) override {}
        uint32_t GetFrameRateLimit() const override { return 0; }
        const InputState& GetInput() const override { return Input; }
        void SetTextInputActive(const bool bInActive) override { bTextInput = bInActive; }

        InputState Input;
        bool       bTextInput = false;
    };

    /* A widget description, by type and properties as text. */
    UI::UIWidgetDesc Widget(const char* InType, const int32_t InParent, std::vector<UI::UIPropertyText> InProperties)
    {
        UI::UIWidgetDesc Desc;
        Desc.Type       = InType;
        Desc.Parent     = InParent;
        Desc.Properties = std::move(InProperties);
        return Desc;
    }

    void WorldTicks(GameCore::World&, float, void* InUserData) { ++*static_cast<int*>(InUserData); }

    void TestSessionUI()
    {
        std::printf("[session UI]\n");
        InputHost              Host;
        GameCore::ScriptEngine Scripts;
        RenderCore::Renderer   Rendering;   // not running: nothing is drawn, the UI still works
        Scripts.Initialize();
        auto* Session = new GameSession(Host, Scripts, Rendering);
        TestLevel Level;
        Expect(Session->LoadLevel(Level), "the level loads");

        int Ticks = 0;
        Session->GetWorld().RegisterSystem("Test.Ticks", GameCore::TP_PostUpdate, &WorldTicks, &Ticks);
        GameCore::Entity Player = Session->GetWorld().CreateEntity("Shooter");
        InputComponent Controls;
        std::snprintf(Controls.Map, sizeof(Controls.Map), "%s", "UITest");
        Player.AddComponent(Controls);

        uint64_t FrameNumber = 1;
        const auto Frame = [&](const auto& InInput)
        {
            Host.Input.BeginFrame(FrameNumber);
            InInput(Host.Input);
            FrameContext Context;
            Context.DeltaSeconds   = 0.016;
            Context.FrameNumber    = FrameNumber++;
            Context.ViewportPixels = EngineCore::Size(1920, 1080);
            Session->Tick(Context);
        };
        const auto Nothing = [](InputState&) {};
        const InputKey Left = FindInputKey("Mouse.Left"), Escape = FindInputKey("Escape"), Space = FindInputKey("Space");

        /* A pause menu: a button that pauses (to see it run) and Back that resumes. */
        auto Menu = std::make_shared<UI::UIDocumentAsset>();
        Menu->Settings.InputMode = UI::EUIInputMode::UIOnly;
        Menu->Settings.OnBack    = "Native:Game.Resume";
        Menu->Widgets.push_back(Widget("Canvas", -1, {}));
        Menu->Widgets.push_back(Widget("Button", 0, { { "Name", "Pause" }, { "Position", "100,100" }, { "Size", "200,100" },
                                                      { "OnClick", "Native:Game.Pause" } }));
        Expect(Session->GetUI().OpenAsset(Menu, "Test/Pause.nmui") != UI::UI_DOCUMENT_INVALID, "a menu opens in the session's UI");
        Frame(Nothing);   // laid out and painted

        Frame([&](InputState& In) { In.MoveMouse(200, 150, 0, 0); In.Press(Left); });
        const InputComponent* Seen = Player.GetComponent<InputComponent>();
        Expect(!Session->GetInput().IsDown(Left) && Seen && !Seen->IsDown("Fire"),
               "a press on the menu's button does not reach the game");
        Frame([&](InputState& In) { In.Release(Left); });
        Expect(Session->IsPaused(), "the click ran Native:Game.Pause");
        Expect(Seen && !Seen->WasPressed("Fire") && !Seen->WasReleased("Fire"), "nor did the click's release reach it");

        const int TicksWhenPaused = Ticks;
        Frame(Nothing);
        Frame(Nothing);
        Expect(Ticks == TicksWhenPaused, "paused, the World does not tick");

        Frame([&](InputState& In) { In.Press(Escape); });
        Expect(!Session->IsPaused(), "Escape (the UI map's Back) ran the menu's OnBack: resumed");
        Expect(!Session->GetInput().WasPressed(Escape), "the Escape the menu took is not the game's");
        Frame([&](InputState& In) { In.Release(Escape); });
        Expect(Ticks > TicksWhenPaused, "resumed, the World ticks again");

        /* While a UIOnly menu is open the keyboard is the menu's. */
        Frame([&](InputState& In) { In.Press(Space); });
        Expect(Seen && !Seen->WasPressed("Jump"), "a UIOnly document takes the keyboard from the game");
        Frame([&](InputState& In) { In.Release(Space); });

        /* A HUD: clicks outside its widgets are the game's. */
        Session->GetUI().Close("Test/Pause.nmui");
        auto Hud = std::make_shared<UI::UIDocumentAsset>();
        Hud->Widgets.push_back(Widget("Canvas", -1, {}));
        Hud->Widgets.push_back(Widget("Border", 0, { { "Name", "Panel" }, { "Size", "300,80" } }));
        Hud->Widgets.push_back(Widget("Text", 0, { { "Name", "Lives" }, { "Position", "0,100" }, { "Text", "Lives: {Bind:Player0.Lives}" } }));
        Hud->Widgets.push_back(Widget("Button", 0, { { "Name", "Close" }, { "Position", "500,0" }, { "Size", "100,50" },
                                                     { "OnClick", "Native:UI.Close" } }));
        Session->GetUI().OpenAsset(Hud, "Test/HUD.nmui");
        Frame(Nothing);
        Frame([&](InputState& In) { In.MoveMouse(1000, 800, 0, 0); In.Press(Left); });
        Frame(Nothing);   // the World resolves last frame's
        Expect(Seen && Seen->IsDown("Fire"), "outside a HUD's widgets the game fires");
        Frame([&](InputState& In) { In.Release(Left); In.MoveMouse(100, 40, 0, 0); In.Press(Left); });
        Frame(Nothing);
        Expect(Seen && !Seen->IsDown("Fire"), "over its panel, the HUD takes the click");
        Frame([&](InputState& In) { In.Release(Left); });

        const UI::UIValue* Lives = Session->GetUI().GetValue("Player0.Lives");
        Expect(Lives && Lives->AsNumber() == 3.0, "the session publishes Player0.Lives");
        UI::UIDocumentInstance* HudDoc = Session->GetUI().Find("Test/HUD.nmui");
        Expect(HudDoc && HudDoc->GetProperty(*HudDoc->FindWidget("Lives"), "Text").AsString() == "Lives: 3", "and a HUD binds it");
        const UI::UIValue* Phase = Session->GetUI().GetValue("Game.Phase");
        Expect(Phase && Phase->AsString() == "Lobby", "and Game.Phase");

        /* A button whose action closes its own document. */
        Frame([&](InputState& In) { In.MoveMouse(550, 25, 0, 0); In.Press(Left); });
        Frame([&](InputState& In) { In.Release(Left); });
        Expect(!Session->GetUI().IsOpen("Test/HUD.nmui"), "Native:UI.Close from its own button closes it, safely");

        /* A level change closes the level's documents. */
        Session->GetUI().OpenAsset(Hud, "Test/HUD.nmui");
        Session->UnloadLevel();
        Expect(Session->GetUI().GetDocumentCount() == 0, "unloading the level closes its documents");

        delete Session;
        Scripts.Shutdown();
    }
}

/* The console's: a var to set and a command to run. */
NIMBLE_CONFIG_INT(GConsoleTestValue, "ConsoleTest", "Value", 1, nullptr);

namespace
{
    std::vector<std::string> GConsoleTestArgs;
    int                      GConsoleTestRuns = 0;
}

NIMBLE_CONSOLE_COMMAND(consoletest, "[words]", "The tests' command")
{
    GConsoleTestArgs = Console.Args;
    ++GConsoleTestRuns;
    Console.Print("consoletest ran");
}

namespace
{
    bool OutputHas(const GameConsole& InConsole, const std::string& InLine)
    {
        for(const std::string& Each : InConsole.GetOutput())
        {
            if(Each == InLine) return true;
        }
        return false;
    }

    void TestConsole()
    {
        std::printf("[console]\n");
        InputHost              Host;
        GameCore::ScriptEngine Scripts;
        RenderCore::Renderer   Rendering;
        Scripts.Initialize();
        auto* Session = new GameSession(Host, Scripts, Rendering);
        TestLevel Level;
        Expect(Session->LoadLevel(Level), "the level loads");

        GameCore::Entity Player = Session->GetWorld().CreateEntity("Typist");
        InputComponent Controls;
        std::snprintf(Controls.Map, sizeof(Controls.Map), "%s", "UITest");
        Player.AddComponent(Controls);

        uint64_t FrameNumber = 1;
        const auto Frame = [&](const auto& InInput)
        {
            Host.Input.BeginFrame(FrameNumber);
            InInput(Host.Input);
            FrameContext Context;
            Context.DeltaSeconds   = 0.016;
            Context.FrameNumber    = FrameNumber++;
            Context.ViewportPixels = EngineCore::Size(1920, 1080);
            Session->Tick(Context);
        };
        const auto Nothing = [](InputState&) {};
        const InputKey Grave = FindInputKey("Grave"), Return = FindInputKey("Return"), Space = FindInputKey("Space"),
                       Escape = FindInputKey("Escape"), Backspace = FindInputKey("Backspace"), Up = FindInputKey("Up"),
                       Tab = FindInputKey("Tab"), Left = FindInputKey("Left");
        GameConsole& Console = Session->GetConsole();

        Expect(FindConsoleCommand("consoletest") && FindConsoleCommand("help") && FindConsoleCommand("open"),
               "the macro registers commands, the engine's among them");
        const std::vector<std::string> Words = SplitConsoleLine("set  \"two words\" \"\" x");
        Expect(Words.size() == 4 && Words[1] == "two words" && Words[2].empty(), "a line splits on spaces; quotes group");

        Frame([&](InputState& In) { In.Press(Grave); });
        Expect(Console.IsOpen(), "the tilde key opens it");
        Expect(Host.bTextInput, "with the window's text input on");
        Expect(Session->GetUI().IsOpen("Engine/Console"), "and its overlay drawn");
        Expect(!Session->GetInput().WasPressed(Grave), "the game does not see the toggle");

        /* Typing: the game sees none of it. */
        const InputComponent* Seen = Player.GetComponent<InputComponent>();
        Frame([&](InputState& In) { In.Release(Grave); In.AppendText("consoletest a \"b c\""); In.Press(Space); });
        Frame(Nothing);   // the World resolves last frame's
        Expect(Console.GetInputLine() == "consoletest a \"b c\"", "typed text goes into the line");
        Expect(Seen && !Seen->IsDown("Jump"), "keys pressed while it is open are not the game's");
        Frame([&](InputState& In) { In.Release(Space); In.Press(Return); });
        Expect(GConsoleTestRuns == 1 && GConsoleTestArgs.size() == 2 && GConsoleTestArgs[1] == "b c",
               "Return runs a registered command with its words");
        Expect(OutputHas(Console, "> consoletest a \"b c\"") && OutputHas(Console, "consoletest ran"),
               "the line is echoed, then what the command printed");
        Expect(Console.GetInputLine().empty(), "and the line empties");
        Frame([&](InputState& In) { In.Release(Return); });

        /* Editing. */
        Frame([&](InputState& In) { In.AppendText("abcd"); });
        Frame([&](InputState& In) { In.Press(Backspace); In.Repeat(Backspace); });
        Expect(Console.GetInputLine() == "ab", "Backspace, and its repeat, delete");
        Frame([&](InputState& In) { In.Release(Backspace); In.Press(Left); });
        Frame([&](InputState& In) { In.Release(Left); In.AppendText("X"); });
        Expect(Console.GetInputLine() == "aXb", "text goes in at the cursor");
        const UI::UIValue* Prompt = Session->GetUI().GetValue("Console.Prompt");
        Expect(Prompt && Prompt->AsString() == "> aX|b", "the prompt shows the line and the cursor");

        Frame([&](InputState& In) { In.Press(Up); });
        Expect(Console.GetInputLine() == "consoletest a \"b c\"", "Up recalls the last line");
        Frame([&](InputState& In) { In.Release(Up); });

        /* Completion. */
        for(int Each = 0; Each < 32; ++Each) { Frame([&](InputState& In) { In.Press(Backspace); }); Frame([&](InputState& In) { In.Release(Backspace); }); }
        Frame([&](InputState& In) { In.AppendText("consolet"); });
        Frame([&](InputState& In) { In.Press(Tab); });
        Expect(Console.GetInputLine() == "consoletest", "Tab completes as far as the choices agree");
        Expect(OutputHas(Console, "consoletest  ConsoleTest.Value"), "and lists them, commands first");
        Frame([&](InputState& In) { In.Release(Tab); In.AppendText(" "); });
        Expect(Console.GetInputLine() == "consoletest ", "typed on after it");

        /* Config vars, and what nothing handles. */
        Expect(Console.Execute("ConsoleTest.Value 7") && GConsoleTestValue.GetInt() == 7, "a line that is no command sets a config var");
        Expect(!Console.Execute("nosuchthing"), "a line nothing handles is refused");
        Expect(!Console.GetOutput().empty() && Console.GetOutput().back().find("Unknown command") != std::string::npos,
               "and says why");

        /* The log. */
        CORE_LOG("console test: from the log")
        Frame(Nothing);
        Expect(OutputHas(Console, "console test: from the log"), "the log shows in it");

        /* Escape closes it, and is not the game's or the UI's. */
        Frame([&](InputState& In) { In.Press(Escape); });
        Expect(!Console.IsOpen() && !Host.bTextInput, "Escape closes it and turns text input off");
        Expect(!Session->GetUI().IsOpen("Engine/Console"), "its overlay goes");
        Expect(!Session->GetInput().WasPressed(Escape), "the Escape is not the game's");
        Frame([&](InputState& In) { In.Release(Escape); });

        /* Closed, the keyboard is the game's again. */
        Frame([&](InputState& In) { In.Press(Space); });
        Frame(Nothing);
        Expect(Seen && Seen->IsDown("Jump"), "closed, keys reach the game");
        Frame([&](InputState& In) { In.Release(Space); });

        /* The level swap keeps it open. */
        Frame([&](InputState& In) { In.Press(Grave); });
        Session->UnloadLevel();
        Expect(Console.IsOpen() && Session->GetUI().IsOpen("Engine/Console"), "a level change does not close it");

        delete Session;
        Expect(!Host.bTextInput, "the session going turns text input off");
        Scripts.Shutdown();
        GConsoleTestValue.ResetToDefault();
    }
}

/* A target that ran across a ray's path: hit where it stood at the time
 * asked for, missed where it is now, blended between its kept positions. */
static void TestLagCompensation()
{
    std::printf("[lag compensation]\n");
    using EngineCore::Vector3;

    GameCore::World World;
    GameCore::Entity Runner = World.CreateEntity("Runner");
    Runner.AddComponent<GameCore::TransformComponent>();
    LagCompensatedComponent Shape;
    Shape.Radius = 30.0f;
    Shape.Bottom = 0.0f;
    Shape.Top    = 180.0f;
    Runner.AddComponent(Shape);
    GameCore::Entity Shooter = World.CreateEntity("Shooter");
    LagCompensatedComponent ShooterShape;
    Shooter.AddComponent(ShooterShape);

    /* At 1000 ms it stood on the ray's line; by 1100 it is 5 m along. */
    auto At = [&](const float InX) { Runner.GetComponent<GameCore::TransformComponent>()->Position = Vector3(InX, 0.0f, 0.0f); };
    At(0.0f);
    RecordLagCompensation(World, 1000);
    At(500.0f);
    RecordLagCompensation(World, 1100);

    const Vector3 Origin(0.0f, 100.0f, -1000.0f), Along(0.0f, 0.0f, 1.0f);   // through the capsule's straight middle
    LagCompensatedHit Hit;
    Expect(LagCompensatedRaycastAt(World, 1000, Shooter.GetID(), Origin, Along, 5000.0f, Hit) && Hit.bCompensated
           && Hit.Entity == Runner.GetID(), "a ray at the time it stood there hits it");
    Expect(std::fabs(Hit.Distance - 970.0f) < 1.0f && std::fabs(Hit.Height - 100.0f) < 1.0f,
           "at its capsule's surface, and says how high above its feet");
    Expect(!LagCompensatedRaycastAt(World, 1100, Shooter.GetID(), Origin, Along, 5000.0f, Hit), "where it is now, it is missed");
    Expect(!LagCompensatedRaycastAt(World, 0, Shooter.GetID(), Origin, Along, 5000.0f, Hit), "time 0 is now");
    Expect(!LagCompensatedRaycastAt(World, 1050, Shooter.GetID(), Origin, Along, 5000.0f, Hit), "halfway, it was 2.5 m off: missed");
    Expect(LagCompensatedRaycastAt(World, 1005, Shooter.GetID(), Origin, Along, 5000.0f, Hit), "5 ms in, 25 cm off: inside its radius");
    Expect(LagCompensatedRaycastAt(World, 400, Shooter.GetID(), Origin, Along, 5000.0f, Hit), "older than it was kept: its oldest");

    Vector3 Then;
    Expect(GetLagCompensatedPosition(World, Runner.GetID(), 1050, Then) && std::fabs(Then.GetX() - 250.0f) < 0.5f,
           "its position at a time between two samples is blended");
    Expect(!GetLagCompensatedPosition(World, World.CreateEntity("Plain").GetID(), 1050, Then), "an entity without the component has none");

    /* The shooter's own capsule, on the ray's start, is skipped; another's is not. */
    Shooter.AddComponent<GameCore::TransformComponent>()->Position = Vector3(0.0f, 0.0f, -1000.0f);
    RecordLagCompensation(World, 1100);
    Expect(LagCompensatedRaycastAt(World, 1000, Shooter.GetID(), Origin, Along, 5000.0f, Hit) && Hit.Entity == Runner.GetID(),
           "the shooter's own capsule is skipped");
    Expect(LagCompensatedRaycastAt(World, 1000, GameCore::EntityID(), Origin, Along, 5000.0f, Hit) && Hit.Entity == Shooter.GetID(),
           "but not anyone else's");

    /* A local call -- the host's own player -- tests now. */
    GameCore::RpcContext Local;
    Local.OwningWorld = &World;
    Local.Entity      = Shooter.GetID();
    Local.ViewTimeMs  = 1000;   // ignored: not remote
    Expect(!LagCompensatedRaycast(World, Local, Origin, Along, 5000.0f, Hit), "a local call tests where things are now");
    GameCore::RpcContext Remote = Local;
    Remote.bRemote = true;
    Expect(LagCompensatedRaycast(World, Remote, Origin, Along, 5000.0f, Hit) && Hit.Entity == Runner.GetID(),
           "a client's call tests where it saw them");

    Runner.GetComponent<LagCompensatedComponent>()->bEnabled = 0;
    RecordLagCompensation(World, 1200);
    Expect(!LagCompensatedRaycastAt(World, 1000, Shooter.GetID(), Origin, Along, 5000.0f, Hit), "a disabled capsule is not hit");
    ClearLagCompensation(World);
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
    TestWorldList();
    TestSessionUI();
    TestConsole();
    TestNetworkingStandalone();
    TestLagCompensation();
    TestNetScripting();
    TestPhysicsInSession();
    TestHeadlessApplication();   // last: the Application claims the main thread

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
