/*
 * NetCore over the loopback interface: a server and a client NetSession in
 * one process, each with its own World, stepped together. Covers the
 * handshake, spawning (a new entity, and one the client's level already has),
 * field replication, entity references, Server / Client / Multicast RPCs, the
 * ownership check, a late joiner's snapshot, destruction and disconnects,
 * and an entity changing shape after its spawn.
 *
 * Real sockets on 127.0.0.1, on a port the OS picks, so it runs beside
 * anything else. Every wait is bounded.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreThread.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptEngine.h"
#include "Public/GameCore/World.h"
#include "Public/NetCore/NetEditorLink.h"
#include "Public/NetCore/NetSession.h"

using namespace GameCore;
using namespace NetCore;

namespace
{
    int GFailures = 0;

    void Check(const bool bInCondition, const char* InWhat)
    {
        if(!bInCondition)
        {
            ++GFailures;
            std::fprintf(stderr, "FAILED: %s\n", InWhat);
        }
        else
        {
            std::printf("ok: %s\n", InWhat);
        }
    }

    // === A test component ====================================================

    struct PawnComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 Position;
        int32_t             Health   = 100;
        EntityID            Target;
        char                Label[16] = "pawn";
        int32_t             Secret   = 7;   // never replicated

        void ServerMove(const RpcContext& InContext, const EngineCore::Vector3& InTo);
        void ClientPing(int32_t InValue);
        void MulticastFlash(const EngineCore::Color& InColor, const RpcString<16>& InText, EntityID InAbout);
    };

    struct RpcLog
    {
        int         ServerMoves     = 0;
        NetClientID LastMoveSender  = NET_CLIENT_NONE;
        int         Pings           = 0;
        int32_t     LastPing        = 0;
        int         Flashes         = 0;
        std::string LastFlashText;
        EntityID    LastFlashAbout;
        uint8_t     LastFlashRed    = 0;
    };
    RpcLog GServerLog;
    RpcLog GClientLog;
    World* GServerWorld = nullptr;

    RpcLog& LogFor(const ComponentOwner& InOwner) { return InOwner.OwningWorld == GServerWorld ? GServerLog : GClientLog; }

    void PawnComponent::ServerMove(const RpcContext& InContext, const EngineCore::Vector3& InTo)
    {
        Position = InTo;
        RpcLog& Log = LogFor(Owner);
        ++Log.ServerMoves;
        Log.LastMoveSender = InContext.SenderClient;
    }

    void PawnComponent::ClientPing(const int32_t InValue)
    {
        RpcLog& Log = LogFor(Owner);
        ++Log.Pings;
        Log.LastPing = InValue;
    }

    void PawnComponent::MulticastFlash(const EngineCore::Color& InColor, const RpcString<16>& InText, const EntityID InAbout)
    {
        RpcLog& Log = LogFor(Owner);
        ++Log.Flashes;
        Log.LastFlashText  = InText.c_str();
        Log.LastFlashAbout = InAbout;
        Log.LastFlashRed   = InColor.R;
    }

    GAMECORE_DEFINE_COMPONENT(PawnComponent, "TestPawn")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(PawnComponent, 1)
    {
        Ar.Field("Position", C.Position);
        Ar.Field("Health",   C.Health);
        Ar.Entity("Target",  C.Target);
        Ar.Field("Label",    C.Label);
    }

    GAMECORE_DEFINE_COMPONENT_REPLICATION(PawnComponent)
    {
        Rep.Field("Position", &PawnComponent::Position);
        Rep.Field("Health",   &PawnComponent::Health);
        Rep.Entity("Target",  &PawnComponent::Target);
        Rep.Field("Label",    &PawnComponent::Label, RFF_InitialOnly);
    }

    GAMECORE_DEFINE_COMPONENT_RPCS(PawnComponent)
    {
        Rpc.Server<&PawnComponent::ServerMove>("ServerMove");
        Rpc.Client<&PawnComponent::ClientPing>("ClientPing");
        Rpc.Multicast<&PawnComponent::MulticastFlash>("MulticastFlash");
    }

    // === Stepping ============================================================

    struct Pair
    {
        NetSession* Server;
        NetSession* Client;
    };

    void Step(const Pair& InPair)
    {
        InPair.Server->PollIncoming();
        InPair.Client->PollIncoming();
        InPair.Server->FlushOutgoing();
        InPair.Client->FlushOutgoing();
        EngineCore::Threads::SleepMs(1);
    }

    template<typename F>
    bool StepUntil(const Pair& InPair, F&& InDone, const int InMaxSteps = 3000)
    {
        for(int Index = 0; Index < InMaxSteps; ++Index)
        {
            if(InDone()) { return true; }
            Step(InPair);
        }
        return InDone();
    }

    bool Near(const float A, const float B) { return A - B < 0.001f && B - A < 0.001f; }

    void TestRpcLocalWithoutSession()
    {
        World Local;
        Entity Pawn = Local.CreateEntity("LocalPawn");
        Pawn.AddComponent<PawnComponent>();

        /* GameCore's own sink: a Server RPC simply runs. */
        const bool bCalled = GAMECORE_RPC(PawnComponent, ServerMove)(Pawn, EngineCore::Vector3(1.0f, 2.0f, 3.0f));
        Check(bCalled && Near(Pawn.GetComponent<PawnComponent>()->Position.GetY(), 2.0f),
              "without a session, an RPC runs locally");

        /* A signature mismatch is refused, not misread. */
        const bool bWrong = CallRpc<&PawnComponent::ClientPing>(Pawn, "ServerMove", 5);
        Check(!bWrong, "an RPC called with another RPC's name and arguments is refused");
    }

    void TestReplicationTable()
    {
        size_t Count = 0;
        const ReplicatedFieldEntry* Fields = FindComponentReplication(PawnComponent::StaticType().ID, Count);
        Check(Fields && Count == 4, "the replication table has its four fields");
        if(Fields && Count == 4)
        {
            Check(Fields[0].Offset == offsetof(PawnComponent, Position) && Fields[1].Offset == offsetof(PawnComponent, Health),
                  "replicated field offsets match offsetof");
            Check(Fields[2].Kind == EReplicatedKind::Entity, "an EntityID field replicates as an entity");
            Check((Fields[3].Flags & RFF_InitialOnly) != 0, "RFF_InitialOnly is recorded");
        }
    }
}

namespace
{
    /* STUB(NetCore-Editor): one editor hosts, two join; what one announces
     * reaches the others, relayed by the host. */
    void TestEditorLink()
    {
        struct Heard { int Count = 0; std::string User, Text; };
        const NetEditorLink::CommandFn Listen = [](const char* InUser, const char* InText, void* InHeard)
        {
            Heard& H = *static_cast<Heard*>(InHeard);
            ++H.Count;
            H.User = InUser;
            H.Text = InText;
        };

        NetEditorLink Host, Alice, Bob;
        Heard HostHeard, AliceHeard, BobHeard;
        Host.SetUserName("host");   Host.SetCommandHandler(Listen, &HostHeard);
        Alice.SetUserName("alice"); Alice.SetCommandHandler(Listen, &AliceHeard);
        Bob.SetUserName("bob");     Bob.SetCommandHandler(Listen, &BobHeard);

        Check(Host.Host(0), "an editor hosts");
        Check(Alice.Join("127.0.0.1", Host.GetPort()) && Bob.Join("127.0.0.1", Host.GetPort()), "two editors join it");

        const auto PollAll = [&]
        {
            Host.Poll(); Alice.Poll(); Bob.Poll();
            EngineCore::Threads::SleepMs(1);
        };
        for(int Index = 0; Index < 3000 && Host.GetPeerCount() < 2; ++Index) { PollAll(); }
        for(int Index = 0; Index < 50; ++Index) { PollAll(); }   // the hellos
        Check(Host.GetPeerCount() == 2, "the host sees both");

        Alice.SendCommand("Move Crate");
        for(int Index = 0; Index < 3000 && (HostHeard.Count == 0 || BobHeard.Count == 0); ++Index) { PollAll(); }
        Check(HostHeard.Count == 1 && HostHeard.User == "alice" && HostHeard.Text == "Move Crate", "the host hears an edit");
        Check(BobHeard.Count == 1 && BobHeard.Text == "Move Crate", "and relays it to the other editor");
        Check(AliceHeard.Count == 0, "but not back to its author");

        Host.SendCommand("Delete Lamp");
        for(int Index = 0; Index < 3000 && (AliceHeard.Count == 0 || BobHeard.Count < 2); ++Index) { PollAll(); }
        Check(AliceHeard.Text == "Delete Lamp" && BobHeard.Text == "Delete Lamp", "the host's own edits reach everyone");
    }
}

namespace
{
    /* State is unreliable: half of every batch and every ack is dropped here
     * (SimulatedLossPercent), yet the client must end up exactly where the
     * server is -- the acknowledged baselines resend whatever it may lack. */
    void TestLossyState()
    {
        World ServerWorld;
        World ClientWorld;
        NetSession Server;
        NetSession Client;
        const Pair Both{ &Server, &Client };

        NetConfig ServerConfig;
        ServerConfig.Port                 = 0;
        ServerConfig.SimulatedLossPercent = 50;
        Check(Server.Start(ENetRole::Server, ServerConfig), "a lossy server listens");
        Server.BindWorld(ServerWorld, "Lossy");

        NetConfig ClientConfig;
        std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
        ClientConfig.Port                 = Server.GetPort();
        ClientConfig.SimulatedLossPercent = 50;
        Check(Client.Start(ENetRole::Client, ClientConfig), "a lossy client connects");
        Client.BindWorld(ClientWorld, "Lossy");
        Check(StepUntil(Both, [&] { return Client.IsConnected(); }), "the handshake is reliable, so it is unaffected");

        Entity ServerPawn = ServerWorld.CreateEntity("LossyPawn");
        ServerPawn.AddComponent<NetComponent>();
        ServerPawn.AddComponent<PawnComponent>();
        ServerPawn.AddComponent<TransformComponent>();

        EntityID ClientPawn;
        Check(StepUntil(Both, [&]
        {
            ClientPawn = Client.FindEntityByNetId(ServerPawn.GetComponent<NetComponent>()->NetId);
            return ClientPawn.IsSet();
        }), "an entity spawns over it");
        Entity ClientHandle(&ClientWorld, ClientPawn);

        /* Changing every step, then still: the last values arrive. */
        for(int Index = 1; Index <= 200; ++Index)
        {
            ServerPawn.GetComponent<PawnComponent>()->Health = Index;
            ServerPawn.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(float(Index), 0.0f, 0.0f);
            Step(Both);
        }
        Check(StepUntil(Both, [&]
        {
            return ClientHandle.GetComponent<PawnComponent>()->Health == 200
                && Near(ClientHandle.GetComponent<TransformComponent>()->Position.GetX(), 200.0f);
        }), "under 50% loss each way, the latest values arrive");

        /* Changed and changed straight back, again and again: whatever was
         * lost, the client settles on the server's value. */
        bool bAllSettled = true;
        for(int Round = 0; Round < 20; ++Round)
        {
            ServerPawn.GetComponent<PawnComponent>()->Health = 1000 + Round;
            Step(Both);
            Step(Both);
            ServerPawn.GetComponent<PawnComponent>()->Health = 200;
            if(!StepUntil(Both, [&] { return ClientHandle.GetComponent<PawnComponent>()->Health == 200; }, 2000)) { bAllSettled = false; }
            for(int Index = 0; Index < 30; ++Index) { Step(Both); }
            if(ClientHandle.GetComponent<PawnComponent>()->Health != 200) { bAllSettled = false; }
        }
        Check(bAllSettled, "a value changed and changed back ends right, every time");

        /* State for a new entity, changed right after its spawn went out. */
        Entity Late = ServerWorld.CreateEntity("LossyLate");
        Late.AddComponent<NetComponent>();
        Late.AddComponent<PawnComponent>();
        Step(Both);   // its spawn goes, reliably
        Late.GetComponent<PawnComponent>()->Health = 79;
        Check(StepUntil(Both, [&]
        {
            const EntityID There = Client.FindEntityByNetId(Late.GetComponent<NetComponent>()->NetId);
            const PawnComponent* Pawn = There.IsSet() ? Entity(&ClientWorld, There).GetComponent<PawnComponent>() : nullptr;
            return Pawn && Pawn->Health == 79;
        }), "and a change made just after a spawn reaches it");

        Client.Stop();
        Server.UnbindWorld();
        Server.Stop();
        Client.UnbindWorld();
    }
}

namespace
{
    /* An entity changing shape after its spawn: components gained and lost,
     * a new parent, and replication turned off and on. Run clean, then with
     * state packets lost each way, where a batch can outrun the
     * AddComponents it depends on. */
    void TestStructureChanges(const uint32_t InLossPercent)
    {
        const std::string Run = InLossPercent == 0 ? "" : " (" + std::to_string(InLossPercent) + "% loss)";
        auto Say = [&](const char* InWhat) { return std::string(InWhat) + Run; };

        World ServerWorld;
        World ClientWorld;
        NetSession Server;
        NetSession Client;
        const Pair Both{ &Server, &Client };

        NetConfig ServerConfig;
        ServerConfig.Port                 = 0;
        ServerConfig.SimulatedLossPercent = InLossPercent;
        Check(Server.Start(ENetRole::Server, ServerConfig), Say("a server listens").c_str());
        Server.BindWorld(ServerWorld, "Shapes");

        NetConfig ClientConfig;
        std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
        ClientConfig.Port                 = Server.GetPort();
        ClientConfig.SimulatedLossPercent = InLossPercent;
        ClientConfig.InterpolationDelayMs = 0;   // Transforms as they arrive, to compare them
        Check(Client.Start(ENetRole::Client, ClientConfig), Say("a client connects").c_str());
        Client.BindWorld(ClientWorld, "Shapes");
        Check(StepUntil(Both, [&] { return Client.IsConnected(); }), Say("it is welcomed").c_str());

        /* The client's copy of a server entity, as of now; unset if it has none. */
        auto ClientOf = [&](const Entity& InServer) -> Entity
        {
            const NetComponent* Net = InServer.GetComponent<NetComponent>();
            const EntityID There = Net && Net->NetId != 0 ? Client.FindEntityByNetId(Net->NetId) : EntityID();
            return Entity(&ClientWorld, There.IsSet() && ClientWorld.IsAlive(There) ? There : EntityID());
        };
        auto HealthThere = [&](const Entity& InServer) -> int32_t
        {
            const Entity There = ClientOf(InServer);
            const PawnComponent* Pawn = There.GetID().IsSet() ? There.GetComponent<PawnComponent>() : nullptr;
            return Pawn ? Pawn->Health : -1;
        };
        auto XThere = [&](const Entity& InServer) -> float
        {
            const Entity There = ClientOf(InServer);
            const TransformComponent* Transform = There.GetID().IsSet() ? There.GetComponent<TransformComponent>() : nullptr;
            return Transform ? Transform->Position.GetX() : -1.0f;
        };

        Entity Shape = ServerWorld.CreateEntity("Shape");
        Shape.AddComponent<NetComponent>();
        Shape.AddComponent<TransformComponent>();
        Entity Anchor = ServerWorld.CreateEntity("Anchor");
        Anchor.AddComponent<NetComponent>();
        Anchor.AddComponent<TransformComponent>();
        Check(StepUntil(Both, [&] { return ClientOf(Shape).GetID().IsSet() && ClientOf(Anchor).GetID().IsSet(); }),
              Say("both entities spawn").c_str());
        Check(!ClientOf(Shape).HasComponent<PawnComponent>(), Say("without the component it does not have yet").c_str());

        // --- a component gained after the spawn -----------------------------------
        PawnComponent Gained;
        Gained.Health = 42;
        Shape.AddComponent(Gained);
        Check(StepUntil(Both, [&] { return HealthThere(Shape) == 42; }), Say("a component added after the spawn reaches the client").c_str());
        Shape.GetComponent<PawnComponent>()->Health = 43;
        Check(StepUntil(Both, [&] { return HealthThere(Shape) == 43; }), Say("and its later changes follow").c_str());

        /* Added and changed in the same frame as its state goes out. */
        Entity Second = ServerWorld.CreateEntity("Second");
        Second.AddComponent<NetComponent>();
        Check(StepUntil(Both, [&] { return ClientOf(Second).GetID().IsSet(); }), Say("a bare entity spawns").c_str());
        Second.AddComponent<PawnComponent>();
        Step(Both);
        Second.GetComponent<PawnComponent>()->Health = 5;
        Check(StepUntil(Both, [&] { return HealthThere(Second) == 5; }), Say("a change right after the add lands").c_str());

        // --- a component lost -------------------------------------------------------
        Shape.RemoveComponent<PawnComponent>();
        Check(StepUntil(Both, [&] { return ClientOf(Shape).GetID().IsSet() && !ClientOf(Shape).HasComponent<PawnComponent>(); }),
              Say("a removed component goes on the client too").c_str());
        Shape.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(12.0f, 0.0f, 0.0f);
        Check(StepUntil(Both, [&] { return Near(XThere(Shape), 12.0f); }), Say("the entity's other state still flows").c_str());

        /* Gained back: everything again, not a diff against the old one. */
        PawnComponent Back;
        Back.Health = 7;
        Shape.AddComponent(Back);
        Check(StepUntil(Both, [&] { return HealthThere(Shape) == 7; }), Say("re-added, it arrives whole").c_str());

        // --- a new parent ----------------------------------------------------------
        Shape.SetParent(Anchor.GetID(), false);
        Check(StepUntil(Both, [&] { return ClientOf(Shape).GetParent() == ClientOf(Anchor).GetID(); }),
              Say("a new parent reaches the client").c_str());
        Shape.ClearParent(false);
        Check(StepUntil(Both, [&] { return ClientOf(Shape).GetID().IsSet() && !ClientOf(Shape).HasParent(); }),
              Say("and so does losing it").c_str());

        // --- replication turned off and on -------------------------------------------
        const EntityID Before = ClientOf(Shape).GetID();
        Shape.GetComponent<NetComponent>()->bReplicate = 0;
        Check(StepUntil(Both, [&] { return !ClientWorld.IsAlive(Before); }), Say("bReplicate off destroys it on the client").c_str());
        Check(Shape.GetComponent<NetComponent>()->NetId == 0, Say("and the server forgets its NetId").c_str());

        Shape.GetComponent<NetComponent>()->bReplicate = 1;
        Check(StepUntil(Both, [&] { return HealthThere(Shape) == 7; }), Say("turned back on, it spawns again, whole").c_str());

        Second.RemoveComponent<NetComponent>();
        const EntityID SecondThere = Client.FindEntityByNetId(Server.GetNetId(Second.GetID()));
        Check(StepUntil(Both, [&] { return !ClientWorld.IsAlive(SecondThere); }),
              Say("removing the NetComponent destroys it on the client").c_str());

        /* Nothing left waiting: a plain change still arrives. */
        Shape.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(30.0f, 0.0f, 0.0f);
        Check(StepUntil(Both, [&] { return Near(XThere(Shape), 30.0f); }), Say("state still flows at the end").c_str());

        Client.Stop();
        Server.UnbindWorld();
        Server.Stop();
        Client.UnbindWorld();
    }
}

int main()
{
    TestEditorLink();
    TestReplicationTable();
    TestRpcLocalWithoutSession();
    GServerLog = RpcLog();
    GClientLog = RpcLog();

    World ServerWorld;
    World ClientWorld;
    GServerWorld = &ServerWorld;

    /* An entity both "levels" have, as two processes loading the same file
     * would: the same GUID on both sides. */
    const EntityGuid SharedGuid = EntityGuid::Generate();
    Entity ServerDoor = ServerWorld.CreateEntity("Door", SharedGuid);
    ServerDoor.AddComponent<NetComponent>();
    PawnComponent DoorPawn;
    DoorPawn.Health = 55;
    ServerDoor.AddComponent(DoorPawn);
    Entity ClientDoor = ClientWorld.CreateEntity("Door", SharedGuid);
    ClientDoor.AddComponent<NetComponent>();
    ClientDoor.AddComponent<PawnComponent>();

    NetSession Server;
    NetSession Client;
    const Pair Both{ &Server, &Client };

    NetConfig ServerConfig;
    ServerConfig.Port = 0;   // any free port
    Check(Server.Start(ENetRole::Server, ServerConfig), "the server listens");
    Server.BindWorld(ServerWorld, "Loopback");

    int Connected = 0, Disconnected = 0;
    NetClientID ConnectedId = NET_CLIENT_NONE;
    struct Events { int* Connected; int* Disconnected; NetClientID* Id; } EventState{ &Connected, &Disconnected, &ConnectedId };
    Server.SetClientEvents(
        [](const NetClientID InClient, void* InUser) { Events& E = *static_cast<Events*>(InUser); ++*E.Connected; *E.Id = InClient; },
        [](const NetClientID, void* InUser) { ++*static_cast<Events*>(InUser)->Disconnected; },
        &EventState);

    NetConfig ClientConfig;
    std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
    ClientConfig.Port = Server.GetPort();
    Check(ClientConfig.Port != 0, "the server reports the port it got");
    Check(Client.Start(ENetRole::Client, ClientConfig), "the client starts connecting");
    Client.BindWorld(ClientWorld, "Loopback");

    // --- handshake -----------------------------------------------------------
    Check(StepUntil(Both, [&] { return Client.IsConnected() && Connected == 1; }), "the server welcomes the client");
    Check(Client.GetLocalClientId() == ConnectedId && ConnectedId != NET_CLIENT_NONE, "the client knows its id");
    Check(Server.GetClientCount() == 1, "the server counts one client");

    // --- connection stats ----------------------------------------------------------
    {
        /* Long enough for a whole second of rates. */
        const auto Until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1300);
        while(std::chrono::steady_clock::now() < Until) { Step(Both); }
        NetPeerStats ToServer, ToClient, Nobody;
        Check(Client.GetServerStats(ToServer) && ToServer.BytesSent > 0 && ToServer.BytesReceived > 0 && ToServer.PacketsReceived > 0,
              "a client reports its traffic with the server");
        Check(Server.GetClientStats(ConnectedId, ToClient) && ToClient.BytesSent > 0 && ToClient.PacketsReceived > 0,
              "the server reports its traffic with that client");
        Check(ToServer.RoundTripMs < 500 && ToClient.RoundTripMs < 500, "both report a round trip, a loopback's");
        Check(ToClient.SendBytesPerSecond > 0.0f && ToServer.ReceiveBytesPerSecond > 0.0f,
              "and the last second's rates: a batch every tick, and its acks");
        Check(!Server.GetClientStats(ConnectedId + 99, Nobody) && !Server.GetServerStats(Nobody) && !Client.GetClientStats(0, Nobody),
              "no stats for a client that is not there, or from the wrong end");
    }

    // --- the level's own entity is matched by GUID, not duplicated --------------
    Check(StepUntil(Both, [&] { return ClientDoor.GetComponent<NetComponent>()->NetId != 0; }), "the level entity is bound by GUID");
    Check(ClientWorld.GetEntityCount() == 1, "no duplicate was created for it");
    Check(ClientDoor.GetComponent<PawnComponent>()->Health == 55, "its components came from the server");

    // --- a long frame does not drop the connection -------------------------------
    {
        /* The client sends something reliable, then stops servicing for longer
         * than ENet's 5s minimum timeout -- a first frame compiling its
         * pipelines -- while the server keeps going. ENet checks timeouts
         * before it reads the socket, so with a timeout of exactly 5s the ack
         * waiting there is not seen in time and the client drops. (The RPC
         * itself is refused: the door is not the client's.) */
        GAMECORE_RPC(PawnComponent, ServerMove)(ClientDoor, EngineCore::Vector3(0.0f, 0.0f, 0.0f));
        Client.FlushOutgoing();
        const auto Until = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
        while(std::chrono::steady_clock::now() < Until)
        {
            Server.PollIncoming();
            Server.FlushOutgoing();
            EngineCore::Threads::SleepMs(5);
        }
        for(int Index = 0; Index < 200; ++Index) { Step(Both); }
        Check(Client.IsConnected() && Server.GetClientCount() == 1, "a client that stalls for 6 seconds stays connected");
    }

    // --- a new entity is spawned, owned by the client ---------------------------
    Entity ServerPawn = ServerWorld.CreateEntity("Hero");
    NetComponent HeroNet;
    HeroNet.OwnerClient = ConnectedId;
    ServerPawn.AddComponent(HeroNet);
    PawnComponent HeroPawn;
    HeroPawn.Health = 80;
    HeroPawn.Secret = 99;
    std::strcpy(HeroPawn.Label, "hero");
    HeroPawn.Target = ServerDoor.GetID();
    ServerPawn.AddComponent(HeroPawn);
    ServerPawn.AddComponent<TransformComponent>();

    EntityID ClientHero;
    Check(StepUntil(Both, [&]
    {
        ClientHero = Client.FindEntityByNetId(ServerPawn.GetComponent<NetComponent>()->NetId);
        return ServerPawn.GetComponent<NetComponent>()->NetId != 0 && ClientHero.IsSet();
    }), "a new networked entity spawns on the client");

    Entity ClientHeroHandle(&ClientWorld, ClientHero);
    const PawnComponent* ClientHeroPawn = ClientHeroHandle.GetComponent<PawnComponent>();
    Check(ClientHeroPawn != nullptr, "with its components");
    Check(ClientHeroHandle.HasComponent<TransformComponent>(), "every serialized component, not only replicated ones");
    Check(std::strcmp(ClientHeroHandle.GetName(), "Hero") == 0, "and its name");
    Check(ClientHeroHandle.GetGuid() == ServerPawn.GetGuid(), "and its GUID");
    if(ClientHeroPawn)
    {
        Check(ClientHeroPawn->Health == 80, "a replicated int arrives");
        Check(std::strcmp(ClientHeroPawn->Label, "hero") == 0, "an initial-only string arrives");
        Check(ClientHeroPawn->Secret == 7, "an unreplicated, unserialized field stays the client's own");
        Check(ClientHeroPawn->Target == ClientDoor.GetID(), "an entity reference resolves to the client's entity");
    }
    const NetComponent* ClientHeroNet = ClientHeroHandle.GetComponent<NetComponent>();
    Check(ClientHeroNet && ClientHeroNet->OwnerClient == ConnectedId, "the owner replicates");
    if(GFailures > 0)
    {
        /* What follows uses the hero; stop rather than dereference nothing. */
        std::fprintf(stderr, "NetCore loopback: stopping early, %d failure(s)\n", GFailures);
        return 1;
    }

    // --- changes replicate; initial-only ones do not ----------------------------
    {
        PawnComponent* Hero = ServerPawn.GetComponent<PawnComponent>();
        Hero->Position = EngineCore::Vector3(4.0f, 5.0f, 6.0f);
        Hero->Health   = 42;
        std::strcpy(Hero->Label, "renamed");
    }
    Check(StepUntil(Both, [&]
    {
        const PawnComponent* Pawn = ClientHeroHandle.GetComponent<PawnComponent>();
        return Pawn && Pawn->Health == 42 && Near(Pawn->Position.GetZ(), 6.0f);
    }), "changed fields replicate");
    for(int Index = 0; Index < 20; ++Index) { Step(Both); }
    Check(std::strcmp(ClientHeroHandle.GetComponent<PawnComponent>()->Label, "hero") == 0, "an initial-only field does not");

    // --- RPCs ------------------------------------------------------------------
    GAMECORE_RPC(PawnComponent, ServerMove)(ClientHeroHandle, EngineCore::Vector3(7.0f, 8.0f, 9.0f));
    Check(StepUntil(Both, [&] { return GServerLog.ServerMoves == 1; }), "a client's Server RPC runs on the server");
    Check(GServerLog.LastMoveSender == ConnectedId, "and the server knows who sent it");
    Check(GClientLog.ServerMoves == 0, "and not on the client");
    Check(StepUntil(Both, [&] { return Near(ClientHeroHandle.GetComponent<PawnComponent>()->Position.GetX(), 7.0f); }),
          "its effect replicates back");

    /* The door is the server's: the client may not drive it. */
    GAMECORE_RPC(PawnComponent, ServerMove)(ClientDoor, EngineCore::Vector3(1.0f, 1.0f, 1.0f));
    for(int Index = 0; Index < 50; ++Index) { Step(Both); }
    Check(GServerLog.ServerMoves == 1, "a Server RPC on an entity the client does not own is refused");

    GAMECORE_RPC(PawnComponent, ClientPing)(ServerPawn, 1234);
    Check(StepUntil(Both, [&] { return GClientLog.Pings == 1; }), "a Client RPC reaches the owning client");
    Check(GClientLog.LastPing == 1234 && GServerLog.Pings == 0, "with its argument, and only there");

    GAMECORE_RPC(PawnComponent, ClientPing)(ServerDoor, 5);
    Check(GServerLog.Pings == 1, "a Client RPC on a server-owned entity runs on the server");

    GAMECORE_RPC(PawnComponent, MulticastFlash)(ServerPawn, EngineCore::Color(200, 0, 0), "boom", ServerDoor.GetID());
    Check(GServerLog.Flashes == 1, "a Multicast runs on the server at once");
    Check(StepUntil(Both, [&] { return GClientLog.Flashes == 1; }), "and on the client");
    Check(GClientLog.LastFlashText == "boom" && GClientLog.LastFlashRed == 200, "with its arguments");
    Check(GClientLog.LastFlashAbout == ClientDoor.GetID(), "and an entity argument mapped to the client's entity");

    // --- RPCs from Lua ----------------------------------------------------------
    {
        /* Each end's own script engine on its own World, as each process has. */
        GameCore::ScriptEngine ServerScripts, ClientScripts;
        Check(ServerScripts.Initialize() && ClientScripts.Initialize(), "two script engines start");
        ServerScripts.BindWorld(&ServerWorld);
        ClientScripts.BindWorld(&ClientWorld);

        const int MovesBefore = GServerLog.ServerMoves;
        Check(ClientScripts.RunString(R"(
            local hero = World.FindEntity("Hero")
            assert(hero:GetComponent("TestPawn"):ServerMove(11, 12, 13) == true)
        )", "loopback-client"), "a client's Lua calls a Server RPC on its component");
        Check(StepUntil(Both, [&] { return GServerLog.ServerMoves == MovesBefore + 1; }), "it runs on the server");
        Check(GServerLog.LastMoveSender == ConnectedId && Near(ServerPawn.GetComponent<PawnComponent>()->Position.GetY(), 12.0f),
              "from that client, with its Vector3");

        const int FlashesBefore = GClientLog.Flashes;
        Check(ServerScripts.RunString(R"(
            local hero = World.FindEntity("Hero")
            local door = World.FindEntity("Door")
            assert(hero:GetComponent("TestPawn"):MulticastFlash(10, 20, 30, 255, "from lua", door) == true)
        )", "loopback-server"), "the server's Lua sends a Multicast with a Color, a string and an entity");
        Check(StepUntil(Both, [&] { return GClientLog.Flashes == FlashesBefore + 1; }), "the client receives it");
        Check(GClientLog.LastFlashText == "from lua" && GClientLog.LastFlashRed == 10 && GClientLog.LastFlashAbout == ClientDoor.GetID(),
              "with every argument, the entity mapped to the client's");

        /* The door is the server's: the client's Lua is refused like C++ is. */
        const int RefusedBefore = GServerLog.ServerMoves;
        Check(ClientScripts.RunString(R"(
            World.FindEntity("Door"):GetComponent("TestPawn"):ServerMove(0, 0, 0)
        )", "loopback-refused"), "a client's Lua may ask about the door");
        for(int Index = 0; Index < 50; ++Index) { Step(Both); }
        Check(GServerLog.ServerMoves == RefusedBefore, "but the server refuses an RPC on what the client does not own");

        ServerScripts.UnbindWorld();
        ClientScripts.UnbindWorld();
        ServerScripts.Shutdown();
        ClientScripts.Shutdown();
    }

    // --- a late joiner -------------------------------------------------------------
    {
        /* (A schema mismatch cannot be staged in one process -- both ends
         * read the same registries -- so the refusal path is exercised by
         * running two builds against each other, not here.) */
        World      StrangerWorld;   // first, so it outlives the session bound to it
        NetSession Stranger;
        NetConfig  StrangerConfig = ClientConfig;
        Check(Stranger.Start(ENetRole::Client, StrangerConfig), "a second client starts connecting");
        Stranger.BindWorld(StrangerWorld, "Elsewhere");   // a level mismatch is only a warning
        const Pair Three{ &Server, &Stranger };
        Check(StepUntil(Three, [&] { return Stranger.IsConnected(); }), "a second client with the same build is welcomed");
        Check(StepUntil(Three, [&] { return StrangerWorld.GetEntityCount() == 2; }), "and gets a snapshot of every networked entity");
        Stranger.Stop();
        Check(StepUntil(Both, [&] { return Disconnected == 1; }), "its disconnect reaches the server");
    }

    // --- smoothing: a remote Transform is shown in the past, blended ------------
    {
        Entity ServerMover = ServerWorld.CreateEntity("Mover");
        ServerMover.AddComponent<NetComponent>();
        ServerMover.AddComponent<TransformComponent>();

        EntityID ClientMover;
        Check(StepUntil(Both, [&]
        {
            ClientMover = Client.FindEntityByNetId(ServerMover.GetComponent<NetComponent>()->NetId);
            return ClientMover.IsSet();
        }), "a server-owned entity with a Transform spawns");
        Entity ClientMoverHandle(&ClientWorld, ClientMover);

        /* Still for a while, then 0 to 100 over 200 ms. The client is 100 ms
         * behind, so it has not started moving when the server is a quarter
         * of the way there. */
        using Clock = std::chrono::steady_clock;
        const auto StepFor = [&](const int InMs)
        {
            const auto Until = Clock::now() + std::chrono::milliseconds(InMs);
            while(Clock::now() < Until) { Step(Both); }
        };
        StepFor(300);

        float Early = -1.0f;
        bool bSawBetween = false;
        const auto Start = Clock::now();
        while(Clock::now() < Start + std::chrono::milliseconds(700))
        {
            const double Ms = std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
            ServerMover.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(float(std::min(Ms / 2.0, 100.0)), 0.0f, 0.0f);
            Step(Both);
            const float X = ClientMoverHandle.GetComponent<TransformComponent>()->Position.GetX();
            if(Ms >= 50.0 && Early < 0.0f) { Early = X; }
            if(X > 1.0f && X < 99.0f)     { bSawBetween = true; }
        }
        Check(Early >= 0.0f && Early < 1.0f, "the client shows the move late, not at once");
        Check(bSawBetween, "and blends through the positions between");
        Check(Near(ClientMoverHandle.GetComponent<TransformComponent>()->Position.GetX(), 100.0f),
              "and arrives where the server put it");

        ServerMover.Destroy();
        Check(StepUntil(Both, [&] { return !ClientWorld.IsAlive(ClientMover); }), "the mover goes");
    }

    // --- prediction: the owner's predicted entity is corrected, not blended ------
    {
        /* The client predicts its hero: its own choice, on its own copy. */
        ClientHeroHandle.GetComponent<NetComponent>()->bPredicted = 1;

        struct Corrections { int Count = 0; int32_t LastAck = 0; float XAtCall = 0.0f; } Seen;
        Client.SetCorrectionHandler([](World& InWorld, const EntityID InEntity, const int32_t InAck, void* InSeen)
        {
            Corrections& S = *static_cast<Corrections*>(InSeen);
            ++S.Count;
            S.LastAck = InAck;
            S.XAtCall = Entity(&InWorld, InEntity).GetComponent<TransformComponent>()->Position.GetX();
        }, &Seen);
        for(int Index = 0; Index < 5; ++Index) { Step(Both); }   // hands the predicted entity its latest Transform
        Seen = Corrections();

        /* The server applied the client's move 5, which put it at X = 50. */
        ServerPawn.GetComponent<NetComponent>()->InputAck = 5;
        ServerPawn.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(50.0f, 0.0f, 0.0f);
        Check(StepUntil(Both, [&] { return Seen.Count > 0 && Seen.LastAck == 5; }), "the correction handler runs with the server's input ack");
        Check(Near(Seen.XAtCall, 50.0f), "with the server's Transform already written, not delayed");

        /* The server sends it each tick until the client acknowledges it,
         * then stops. */
        for(int Index = 0; Index < 100; ++Index) { Step(Both); }
        Seen = Corrections();
        for(int Index = 0; Index < 50; ++Index) { Step(Both); }
        Check(Seen.Count == 0, "once acknowledged, unchanged state is not sent again");

        /* The game replays on top; the session leaves that alone. */
        ClientHeroHandle.GetComponent<TransformComponent>()->Position = EngineCore::Vector3(60.0f, 0.0f, 0.0f);
        for(int Index = 0; Index < 50; ++Index) { Step(Both); }
        Check(Near(ClientHeroHandle.GetComponent<TransformComponent>()->Position.GetX(), 60.0f),
              "a predicted Transform is not interpolated over");

        Client.SetCorrectionHandler(nullptr, nullptr);
    }

    // --- destruction ------------------------------------------------------------
    ServerPawn.Destroy();
    Check(StepUntil(Both, [&] { return !ClientWorld.IsAlive(ClientHero); }), "a destroyed entity goes on the client too");

    // --- disconnect ---------------------------------------------------------------
    Client.Stop();
    Check(StepUntil(Both, [&] { return Disconnected == 2; }), "the server sees the client leave");
    Check(Server.GetClientCount() == 0, "and forgets it");

    Server.UnbindWorld();
    Server.Stop();
    const int MovesBefore = GServerLog.ServerMoves;
    GAMECORE_RPC(PawnComponent, ServerMove)(ServerDoor, EngineCore::Vector3(0.0f, 0.0f, 0.0f));
    Check(GServerLog.ServerMoves == MovesBefore + 1, "stopping the last session restores local RPCs");

    TestLossyState();
    TestStructureChanges(0);
    TestStructureChanges(50);

    if(GFailures == 0) { std::printf("NetCore loopback: all passed\n"); }
    else               { std::fprintf(stderr, "NetCore loopback: %d failure(s)\n", GFailures); }
    return GFailures == 0 ? 0 : 1;
}
