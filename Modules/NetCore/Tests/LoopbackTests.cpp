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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
        float               Glow     = 0.0f;   // replicated Smooth()

        void ServerMove(const RpcContext& InContext, const EngineCore::Vector3& InTo);
        void ClientPing(int32_t InValue);
        void MulticastFlash(const EngineCore::Color& InColor, const RpcString<16>& InText, EntityID InAbout);
        void ServerPoke();                                  // RateLimit(10)
        void ServerShout(const RpcString<200>& InText);     // longer than a strict MaxRpcBytes
        void ServerStamp(const RpcContext& InContext);      // records the call's view time
        void MulticastSpark(int32_t InValue);               // Unreliable
        void ServerSpark(int32_t InValue);                  // Unreliable
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
        int         Pokes           = 0;
        int         Shouts          = 0;
        uint32_t    StampView       = 0xFFFFFFFFu;
        uint32_t    StampServerTime = 0;
        bool        bStampRemote    = false;
        int         Sparks          = 0;
        int32_t     LastSpark       = 0;
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

    void PawnComponent::ServerPoke()                            { ++LogFor(Owner).Pokes; }
    void PawnComponent::ServerShout(const RpcString<200>&)      { ++LogFor(Owner).Shouts; }
    void PawnComponent::MulticastSpark(const int32_t InValue) { RpcLog& Log = LogFor(Owner); ++Log.Sparks; Log.LastSpark = InValue; }
    void PawnComponent::ServerSpark(const int32_t InValue)    { RpcLog& Log = LogFor(Owner); ++Log.Sparks; Log.LastSpark = InValue; }

    void PawnComponent::ServerStamp(const RpcContext& InContext)
    {
        RpcLog& Log = LogFor(Owner);
        Log.StampView       = InContext.ViewTimeMs;
        Log.bStampRemote    = InContext.bRemote;
        Log.StampServerTime = NetSession::GetActive() ? NetSession::GetActive()->GetTimeMs() : 0;
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
        Rep.Field("Glow",     &PawnComponent::Glow).Smooth();
    }

    GAMECORE_DEFINE_COMPONENT_RPCS(PawnComponent)
    {
        Rpc.Server<&PawnComponent::ServerMove>("ServerMove");
        Rpc.Client<&PawnComponent::ClientPing>("ClientPing");
        Rpc.Multicast<&PawnComponent::MulticastFlash>("MulticastFlash");
        Rpc.Server<&PawnComponent::ServerPoke>("ServerPoke").RateLimit(10);
        Rpc.Server<&PawnComponent::ServerShout>("ServerShout");
        Rpc.Server<&PawnComponent::ServerStamp>("ServerStamp");
        Rpc.Multicast<&PawnComponent::MulticastSpark>("MulticastSpark").Unreliable();
        Rpc.Server<&PawnComponent::ServerSpark>("ServerSpark").Unreliable();
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
        Check(Fields && Count == 5, "the replication table has its five fields");
        Check(Fields && (Fields[4].Flags & RFF_Smooth) != 0 && (Fields[1].Flags & RFF_Smooth) == 0, "Smooth() marks its field, and only its field");
        if(Fields && Count == 5)
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

namespace
{
    /* A server with tight RPC limits, and a client that owns one pawn on it. */
    struct LimitedPair
    {
        World       ServerWorld, ClientWorld;
        NetSession  Server, Client;
        Pair        Both{ &Server, &Client };
        Entity      ServerPawn;
        Entity      ClientPawn;

        bool Start(const uint32_t InMaxRpcBytes)
        {
            GServerWorld = &ServerWorld;
            NetConfig ServerConfig;
            ServerConfig.Port              = 0;
            ServerConfig.RpcPerSecond      = 50;    // a 100-call burst
            ServerConfig.RpcFloodPerSecond = 200;
            ServerConfig.MaxRpcBytes       = InMaxRpcBytes;
            if(!Server.Start(ENetRole::Server, ServerConfig)) { return false; }
            Server.BindWorld(ServerWorld, "Limits");

            NetConfig ClientConfig;
            std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
            ClientConfig.Port = Server.GetPort();
            if(!Client.Start(ENetRole::Client, ClientConfig)) { return false; }
            Client.BindWorld(ClientWorld, "Limits");
            if(!StepUntil(Both, [&] { return Client.IsConnected(); })) { return false; }

            ServerPawn = ServerWorld.CreateEntity("Limited");
            NetComponent Net;
            Net.OwnerClient = Client.GetLocalClientId();
            ServerPawn.AddComponent(Net);
            ServerPawn.AddComponent<PawnComponent>();
            EntityID There;
            if(!StepUntil(Both, [&] { There = Client.FindEntityByNetId(Server.GetNetId(ServerPawn.GetID())); return There.IsSet(); })) { return false; }
            ClientPawn = Entity(&ClientWorld, There);
            return true;
        }

        void Settle() { for(int Index = 0; Index < 100; ++Index) { Step(Both); } }

        ~LimitedPair()
        {
            Client.Stop();
            Server.UnbindWorld();
            Server.Stop();
            Client.UnbindWorld();
            GServerWorld = nullptr;
        }
    };

    /* A server takes so many Server RPCs a second from each client, and so many of one with a RateLimit. */
    void TestRpcLimits()
    {
        std::printf("[rpc limits]\n");
        {
            LimitedPair Run;
            Check(Run.Start(1024), "a server with tight RPC limits takes a client");
            GServerLog = RpcLog();

            /* Thirty pokes at once: RateLimit(10) lets a second's worth through. */
            for(int Index = 0; Index < 30; ++Index) { GAMECORE_RPC(PawnComponent, ServerPoke)(Run.ClientPawn); }
            Run.Settle();
            Check(GServerLog.Pokes >= 10 && GServerLog.Pokes <= 12, "an RPC's RateLimit drops what is over it");

            /* A second on, there is room again. */
            const auto Until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1100);
            while(std::chrono::steady_clock::now() < Until) { Step(Run.Both); }
            const int Before = GServerLog.Pokes;
            for(int Index = 0; Index < 5; ++Index) { GAMECORE_RPC(PawnComponent, ServerPoke)(Run.ClientPawn); }
            Run.Settle();
            Check(GServerLog.Pokes == Before + 5, "and refills at its rate");

            /* 150 moves at once against a 50/s budget: about its 100-call burst lands, the client stays. */
            for(int Index = 0; Index < 150; ++Index)
            {
                GAMECORE_RPC(PawnComponent, ServerMove)(Run.ClientPawn, EngineCore::Vector3(float(Index), 0.0f, 0.0f));
            }
            Run.Settle();
            Check(GServerLog.ServerMoves >= 95 && GServerLog.ServerMoves <= 115, "the overall budget drops the rest of a burst");
            Check(Run.Client.IsConnected(), "a client a little over its budget stays connected");

            /* Hundreds over it in a second: a flood, and the client is let go. */
            for(int Index = 0; Index < 800; ++Index)
            {
                GAMECORE_RPC(PawnComponent, ServerMove)(Run.ClientPawn, EngineCore::Vector3(0.0f, 0.0f, 0.0f));
            }
            Check(StepUntil(Run.Both, [&] { return !Run.Client.IsConnected() && Run.Server.GetClientCount() == 0; }),
                  "a client flooding RPCs is disconnected");
        }
        {
            LimitedPair Run;
            Check(Run.Start(64), "a server with a 64-byte MaxRpcBytes takes a client");
            GServerLog = RpcLog();
            GAMECORE_RPC(PawnComponent, ServerShout)(Run.ClientPawn, "short");
            Run.Settle();
            Check(GServerLog.Shouts == 1 && Run.Client.IsConnected(), "an RPC under MaxRpcBytes runs");
            std::string Long(150, 'x');
            GAMECORE_RPC(PawnComponent, ServerShout)(Run.ClientPawn, Long.c_str());
            Check(StepUntil(Run.Both, [&] { return !Run.Client.IsConnected(); }) && GServerLog.Shouts == 1,
                  "one over it is malformed: refused, and its sender disconnected");
        }
    }
}

namespace
{
    /* A server and a client with short timeouts, counting the server's client events. */
    struct ReconnectPair
    {
        World      ServerWorld, ClientWorld;
        NetSession Server, Client;
        Pair       Both{ &Server, &Client };
        struct Counts { int Connected = 0, Disconnected = 0, Away = 0, Returned = 0; } Seen;

        bool Start(const uint32_t InGraceMs)
        {
            NetConfig ServerConfig;
            ServerConfig.Port             = 0;
            ServerConfig.TimeoutMs        = 1000;
            ServerConfig.ReconnectGraceMs = InGraceMs;
            if(!Server.Start(ENetRole::Server, ServerConfig)) { return false; }
            Server.BindWorld(ServerWorld, "Rejoin");
            Server.SetClientEvents([](NetClientID, void* U) { ++static_cast<Counts*>(U)->Connected; },
                                   [](NetClientID, void* U) { ++static_cast<Counts*>(U)->Disconnected; }, &Seen);
            Server.SetClientAwayEvents([](NetClientID, void* U) { ++static_cast<Counts*>(U)->Away; },
                                       [](NetClientID, void* U) { ++static_cast<Counts*>(U)->Returned; });

            NetConfig ClientConfig;
            std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
            ClientConfig.Port             = Server.GetPort();
            ClientConfig.TimeoutMs        = 1000;
            ClientConfig.ReconnectGraceMs = 8000;
            if(!Client.Start(ENetRole::Client, ClientConfig)) { return false; }
            Client.BindWorld(ClientWorld, "Rejoin");
            return StepUntil(Both, [&] { return Client.IsConnected() && Seen.Connected == 1; });
        }

        /* The client stops answering -- a cable pulled -- while the server goes on. */
        void Stall(const int InMs)
        {
            const auto Until = std::chrono::steady_clock::now() + std::chrono::milliseconds(InMs);
            while(std::chrono::steady_clock::now() < Until)
            {
                Server.PollIncoming();
                Server.FlushOutgoing();
                EngineCore::Threads::SleepMs(2);
            }
        }

        EntityID OnClient(const Entity& InServer) const { return Client.FindEntityByNetId(Server.GetNetId(InServer.GetID())); }

        ~ReconnectPair()
        {
            Client.Stop();
            Server.UnbindWorld();
            Server.Stop();
            Client.UnbindWorld();
        }
    };

    /* A client's Server RPC says what server time it was showing: its render
     * time, an interpolation delay behind the server's clock. A local call says nothing. */
    void TestViewTime()
    {
        std::printf("[view time]\n");
        LimitedPair Run;
        Check(Run.Start(1024), "a server and client for view times");
        const auto Until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while(std::chrono::steady_clock::now() < Until) { Step(Run.Both); }   // the client learns the server's clock
        Check(Run.Client.GetTimeMs() != 0 && Run.Server.GetTimeMs() != 0, "both have the server's clock");
        const int32_t Skew = int32_t(Run.Server.GetTimeMs()) - int32_t(Run.Client.GetTimeMs());
        Check(Skew > -60 && Skew < 60, "the client's estimate is close, on loopback");

        GServerLog = RpcLog();
        GAMECORE_RPC(PawnComponent, ServerStamp)(Run.ClientPawn);
        Check(StepUntil(Run.Both, [&] { return GServerLog.StampView != 0xFFFFFFFFu; }), "a client's call arrives");
        const int32_t Behind = int32_t(GServerLog.StampServerTime) - int32_t(GServerLog.StampView);
        Check(GServerLog.bStampRemote && GServerLog.StampView != 0 && Behind >= 80 && Behind <= 200,
              "stamped with what it showed: about its 100 ms interpolation delay back");

        GServerLog = RpcLog();
        GAMECORE_RPC(PawnComponent, ServerStamp)(Run.ServerPawn);
        Check(!GServerLog.bStampRemote && GServerLog.StampView == 0, "a local call has no view time: now");
    }

    void TestReconnect()
    {
        std::printf("[reconnect]\n");
        {
            ReconnectPair Run;
            Check(Run.Start(6000), "a server keeping lost clients' places takes a client");
            const NetClientID Id = Run.Client.GetLocalClientId();
            const uint64_t Token = Run.Client.GetReconnectToken();
            Check(Token != 0, "the client is given a reconnect token");

            Entity Hero = Run.ServerWorld.CreateEntity("Hero");
            NetComponent HeroNet;
            HeroNet.OwnerClient = Id;
            Hero.AddComponent(HeroNet);
            Hero.AddComponent<PawnComponent>();
            Entity Doomed = Run.ServerWorld.CreateEntity("Doomed");
            Doomed.AddComponent<NetComponent>();
            Check(StepUntil(Run.Both, [&] { return Run.OnClient(Hero).IsSet() && Run.OnClient(Doomed).IsSet(); }), "both spawn on the client");
            const EntityID ClientHero = Run.OnClient(Hero);
            const EntityID ClientDoomed = Run.OnClient(Doomed);

            Run.Stall(2600);   // ENet pings a silent peer after 500 ms, and gives up on it a timeout later
            Check(Run.Seen.Away == 1 && Run.Seen.Disconnected == 0, "a client that stops answering is away, not gone");
            Check(Run.ServerWorld.IsAlive(Hero.GetID()), "and what it owns stays");

            /* While it is away the game goes on. */
            Doomed.Destroy();
            Entity Fresh = Run.ServerWorld.CreateEntity("Fresh");
            Fresh.AddComponent<NetComponent>();
            Hero.GetComponent<PawnComponent>()->Health = 33;
            Run.Stall(200);

            Check(StepUntil(Run.Both, [&] { return Run.Client.IsReconnecting(); }, 4000), "the client notices, and tries to rejoin");
            Check(StepUntil(Run.Both, [&] { return Run.Client.IsConnected() && Run.Seen.Returned == 1; }, 6000),
                  "it is taken back within the grace");
            Check(Run.Seen.Connected == 1 && Run.Seen.Disconnected == 0, "as a return, not a new client");
            Check(Run.Client.GetLocalClientId() == Id && Run.Client.GetReconnectToken() == Token, "with the same id and token");
            Check(StepUntil(Run.Both, [&]
            {
                const PawnComponent* Pawn = Entity(&Run.ClientWorld, ClientHero).GetComponent<PawnComponent>();
                return Pawn && Pawn->Health == 33 && Run.OnClient(Fresh).IsSet();
            }), "its own entity kept, brought up to date, and what was made meanwhile sent");
            Check(Run.OnClient(Hero) == ClientHero, "the very same entity, not a copy");
            Check(!Run.ClientWorld.IsAlive(ClientDoomed), "what was destroyed while it was away is gone");
            Check(Run.Server.GetClientCount() == 1, "the server counts it once");
        }
        {
            ReconnectPair Run;
            Check(Run.Start(800), "a server with a short grace takes a client");
            const NetClientID Id = Run.Client.GetLocalClientId();
            Entity Mine = Run.ServerWorld.CreateEntity("Mine");
            NetComponent MineNet;
            MineNet.OwnerClient = Id;
            Mine.AddComponent(MineNet);
            Check(StepUntil(Run.Both, [&] { return Run.OnClient(Mine).IsSet(); }), "its entity spawns");
            const EntityID OldCopy = Run.OnClient(Mine);

            Run.Stall(4000);   // timed out, then the grace runs out
            Check(Run.Seen.Away == 1 && Run.Seen.Disconnected == 1, "not back within the grace: gone");
            Check(StepUntil(Run.Both, [&] { return Run.Client.IsConnected() && Run.Seen.Connected == 2; }, 8000), "the client gets in again");
            Check(Run.Client.GetLocalClientId() != Id && Run.Seen.Returned == 0, "as a new client");
            Check(StepUntil(Run.Both, [&] { return !Run.ClientWorld.IsAlive(OldCopy); }), "what the old session spawned is cleared");
        }
        {
            ReconnectPair Run;
            Check(Run.Start(5000), "a third client joins");
            Run.Client.Stop();
            Check(StepUntil(Run.Both, [&] { return Run.Seen.Disconnected == 1; }) && Run.Seen.Away == 0,
                  "one that quits is gone at once, no place kept");
        }
    }
}

namespace
{
    /* What a server's flush costs with many entities and several clients:
     * printed, not checked (timings vary); the state is checked to arrive. */
    void TestFlushCost()
    {
        std::printf("[flush cost]\n");
        constexpr int Entities = 2000, Clients = 4, Movers = 100, Ticks = 120;

        World ServerWorld;
        NetSession Server;
        NetConfig ServerConfig;
        ServerConfig.Port = 0;
        ServerConfig.RpcPerSecond = 0;
        Check(Server.Start(ENetRole::Server, ServerConfig), "a server for the flush benchmark");
        Server.BindWorld(ServerWorld, "Bench");

        std::vector<World> ClientWorlds(Clients);
        std::vector<NetSession> ClientSessions(Clients);
        for(int Index = 0; Index < Clients; ++Index)
        {
            NetConfig Config;
            std::strcpy(Config.ServerAddress, "127.0.0.1");
            Config.Port = Server.GetPort();
            Config.InterpolationDelayMs = 0;
            ClientSessions[Index].Start(ENetRole::Client, Config);
            ClientSessions[Index].BindWorld(ClientWorlds[Index], "Bench");
        }
        const auto StepAll = [&]
        {
            Server.PollIncoming();
            for(NetSession& C : ClientSessions) { C.PollIncoming(); }
            Server.FlushOutgoing();
            for(NetSession& C : ClientSessions) { C.FlushOutgoing(); }
        };
        const auto Until = [&](auto&& InDone, const int InMax)
        {
            for(int Step = 0; Step < InMax; ++Step) { if(InDone()) { return true; } StepAll(); EngineCore::Threads::SleepMs(1); }
            return InDone();
        };
        Check(Until([&] { return Server.GetClientCount() == size_t(Clients); }, 3000), "four clients join");

        std::vector<Entity> Spawned;
        for(int Index = 0; Index < Entities; ++Index)
        {
            Entity E = ServerWorld.CreateEntity("Bench");
            E.AddComponent<NetComponent>();
            E.AddComponent<TransformComponent>();
            E.AddComponent<PawnComponent>();
            Spawned.push_back(E);
        }
        Check(Until([&]
        {
            for(NetSession& C : ClientSessions) { if(!C.FindEntityByNetId(Server.GetNetId(Spawned.back().GetID())).IsSet()) { return false; } }
            return true;
        }, 5000), "every client has all 2000");
        for(int Index = 0; Index < 300; ++Index) { StepAll(); EngineCore::Threads::SleepMs(1); }   // acks settle

        const auto Measure = [&](const int InMovers, const int InTick0)
        {
            std::vector<double> Samples;
            for(int Tick = 0; Tick < Ticks; ++Tick)
            {
                for(int Index = 0; Index < InMovers; ++Index)
                {
                    Spawned[size_t(Index)].GetComponent<TransformComponent>()->Position = EngineCore::Vector3(float(InTick0 + Tick), 0.0f, float(Index));
                }
                Server.PollIncoming();
                for(NetSession& C : ClientSessions) { C.PollIncoming(); }
                const auto Start = std::chrono::steady_clock::now();
                Server.FlushOutgoing();
                Samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count());
                for(NetSession& C : ClientSessions) { C.FlushOutgoing(); }
                EngineCore::Threads::SleepMs(1);
            }
            std::sort(Samples.begin(), Samples.end());
            /* The fastest tick is the work itself; the median adds what else the machine was doing. */
            return std::make_pair(Samples.front(), Samples[Samples.size() / 2]);
        };
        const auto Still  = Measure(0, 0);
        NetPeerStats Before, After;
        Server.GetClientStats(ClientSessions.front().GetLocalClientId(), Before);
        const auto Moving = Measure(Movers, 1000);
        Server.GetClientStats(ClientSessions.front().GetLocalClientId(), After);
        const double PerTick = double(After.BytesSent - Before.BytesSent) / Ticks;
        std::printf("state: %.0f bytes a tick to each client with %d moving -- %.1f bytes an entity whose Position changed\n",
                    PerTick, Movers, PerTick / Movers);
        std::printf("flush cost: %d entities x 3 replicated components, %d clients: all still %.3f ms (median %.3f), "
                    "%d moving %.3f ms (median %.3f), a tick\n",
                    Entities, Clients, Still.first, Still.second, Movers, Moving.first, Moving.second);

        const EntityID Last = ClientSessions.back().FindEntityByNetId(Server.GetNetId(Spawned[Movers - 1].GetID()));
        Check(Until([&]
        {
            const TransformComponent* T = Entity(&ClientWorlds.back(), Last).GetComponent<TransformComponent>();
            return T && Near(T->Position.GetX(), float(1000 + Ticks - 1));
        }, 2000), "and the movers' state still reaches every client");

        for(NetSession& C : ClientSessions) { C.Stop(); C.UnbindWorld(); }
        Server.UnbindWorld();
        Server.Stop();
    }
}

namespace
{
    /* Quantized fields (ComponentReplication.h): the rounding, and a Transform
     * arriving as exactly what QuantizeReplicatedFields makes of it. */
    void TestQuantization()
    {
        std::printf("[quantization]\n");
        namespace Q = GameCore::ReplicationQuantize;
        Check(Q::SnapStep(1.23456f, 0.01f) == Q::FromSteps(123, 0.01f) && std::fabs(Q::SnapStep(-2.005f, 0.01f) + 2.0f) < 0.0101f,
              "Precision rounds to the nearest step, either sign");
        Check(std::fabs(Q::SnapAngle(270.0f) + 90.0f) < 0.01f && std::fabs(Q::SnapAngle(-450.0f) + 90.0f) < 0.01f
              && std::fabs(Q::SnapAngle(180.0f) + 180.0f) < 0.01f, "Angles wrap into [-180, 180)");
        Check(std::fabs(Q::SnapAngle(33.3f) - 33.3f) < 360.0f / 65536.0f, "and keep 65536ths of a turn");

        size_t Count = 0;
        const ReplicatedFieldEntry* Fields = FindComponentReplication(TransformComponent::StaticType().ID, Count);
        Check(Count == 3 && Fields[0].Quantize == RQ_Step && Fields[1].Quantize == RQ_Angle && Fields[2].Quantize == RQ_Step,
              "Transform replicates Position and Scale by Precision, Rotation by Angles");

        World ServerWorld, ClientWorld;
        NetSession Server, Client;
        const Pair Both{ &Server, &Client };
        NetConfig ServerConfig;
        ServerConfig.Port = 0;
        Check(Server.Start(ENetRole::Server, ServerConfig), "a server for quantized state");
        Server.BindWorld(ServerWorld, "Quantized");
        NetConfig ClientConfig;
        std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
        ClientConfig.Port = Server.GetPort();
        ClientConfig.InterpolationDelayMs = 0;   // Transforms as they arrive
        Client.Start(ENetRole::Client, ClientConfig);
        Client.BindWorld(ClientWorld, "Quantized");
        Check(StepUntil(Both, [&] { return Client.IsConnected(); }), "a client for quantized state");

        Entity Thing = ServerWorld.CreateEntity("Quantized");
        Thing.AddComponent<NetComponent>();
        TransformComponent* T = Thing.AddComponent<TransformComponent>();
        EntityID There;
        Check(StepUntil(Both, [&] { There = Client.FindEntityByNetId(Server.GetNetId(Thing.GetID())); return There.IsSet(); }), "it spawns");

        T = Thing.GetComponent<TransformComponent>();
        T->Position = EngineCore::Vector3(1.234567f, -3210.98765f, 100000.004f);
        T->Rotation = EngineCore::Vector3(270.0f, -12.3456f, 0.0f);
        T->Scale    = EngineCore::Vector3(1.0f, 0.12345f, 2.0f);
        TransformComponent Expected = *T;
        QuantizeReplicatedFields(TransformComponent::StaticType().ID, &Expected);
        const auto Same = [](const EngineCore::Vector3& A, const EngineCore::Vector3& B)
        {
            return A.GetX() == B.GetX() && A.GetY() == B.GetY() && A.GetZ() == B.GetZ();
        };
        Check(StepUntil(Both, [&]
        {
            const TransformComponent* Got = Entity(&ClientWorld, There).GetComponent<TransformComponent>();
            return Got && Same(Got->Position, Expected.Position) && Same(Got->Rotation, Expected.Rotation) && Same(Got->Scale, Expected.Scale);
        }), "the client reads exactly the snapped values -- to the bit");
        Check(std::fabs(Expected.Position.GetX() - 1.23f) < 0.001f && std::fabs(Expected.Rotation.GetX() + 90.0f) < 0.01f,
              "which are the values to their precision");
        Check(T->Position.GetX() == 1.234567f, "the server's own value is left as it was");

        Client.Stop();
        Server.UnbindWorld();
        Server.Stop();
        Client.UnbindWorld();
    }
}

namespace
{
    /* [Net] ClientBandwidthKBps: under a tight budget, a client's own entity
     * keeps up, a higher Priority is updated more often, and everything
     * still arrives once things settle. */
    void TestBandwidthBudget()
    {
        std::printf("[bandwidth budget]\n");
        constexpr int Entities = 300;

        World ServerWorld, ClientWorld;
        NetSession Server, Client;
        const Pair Both{ &Server, &Client };
        NetConfig ServerConfig;
        ServerConfig.Port = 0;
        ServerConfig.RpcPerSecond = 0;
        ServerConfig.ClientBandwidthKBps = 4;
        Check(Server.Start(ENetRole::Server, ServerConfig), "a server with a 4 KB/s budget per client");
        Server.BindWorld(ServerWorld, "Budget");
        NetConfig ClientConfig;
        std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
        ClientConfig.Port = Server.GetPort();
        ClientConfig.InterpolationDelayMs = 0;
        Client.Start(ENetRole::Client, ClientConfig);
        Client.BindWorld(ClientWorld, "Budget");
        Check(StepUntil(Both, [&] { return Client.IsConnected(); }), "a client joins it");

        /* The client's own at the origin; High and Low the same distance off; a crowd further out. */
        const auto Make = [&](const char* InName, const float InX, const float InPriority, const NetClientID InOwner)
        {
            Entity E = ServerWorld.CreateEntity(InName);
            NetComponent Net;
            Net.Priority    = InPriority;
            Net.OwnerClient = InOwner;
            E.AddComponent(Net);
            TransformComponent T;
            T.Position = EngineCore::Vector3(InX, 0.0f, 0.0f);
            E.AddComponent(T);
            E.AddComponent<PawnComponent>();
            return E;
        };
        Entity Own  = Make("Own", 0.0f, 1.0f, Client.GetLocalClientId());
        Entity High = Make("High", 1000.0f, 8.0f, NET_CLIENT_NONE);
        Entity Low  = Make("Low", -1000.0f, 1.0f, NET_CLIENT_NONE);
        std::vector<Entity> Crowd;
        for(int Index = 0; Index < Entities; ++Index) { Crowd.push_back(Make("Crowd", 3000.0f + float(Index), 1.0f, NET_CLIENT_NONE)); }
        const auto OnClient = [&](const Entity& InServer) -> const PawnComponent*
        {
            const EntityID There = Client.FindEntityByNetId(Server.GetNetId(InServer.GetID()));
            return There.IsSet() ? Entity(&ClientWorld, There).GetComponent<PawnComponent>() : nullptr;
        };
        Check(StepUntil(Both, [&] { return OnClient(Crowd.back()) != nullptr; }, 5000), "everything spawns (spawns are not budgeted)");

        /* Every entity changes every step, for two seconds. */
        int Steps = 0, OwnSeen = 0, HighSeen = 0, LowSeen = 0;
        int32_t LastOwn = -1, LastHigh = -1, LastLow = -1;
        uint32_t MostDeferred = 0;
        const auto Until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while(std::chrono::steady_clock::now() < Until)
        {
            ++Steps;
            for(Entity* E : { &Own, &High, &Low }) { E->GetComponent<PawnComponent>()->Health = 1000 + Steps; }
            for(Entity& E : Crowd) { E.GetComponent<PawnComponent>()->Health = 1000 + Steps; }
            Step(Both);
            const PawnComponent* O = OnClient(Own);
            const PawnComponent* H = OnClient(High);
            const PawnComponent* L = OnClient(Low);
            if(O && O->Health != LastOwn)  { LastOwn = O->Health; ++OwnSeen; }
            if(H && H->Health != LastHigh) { LastHigh = H->Health; ++HighSeen; }
            if(L && L->Health != LastLow)  { LastLow = L->Health; ++LowSeen; }
            NetPeerStats Stats;
            if(Server.GetClientStats(Client.GetLocalClientId(), Stats)) { MostDeferred = std::max(MostDeferred, Stats.StateDeferred); }
        }
        std::printf("budget: %d steps; the client saw its own entity change %d times, Priority 8 %d, Priority 1 %d; "
                    "up to %u entities held back a tick\n", Steps, OwnSeen, HighSeen, LowSeen, MostDeferred);
        Check(MostDeferred > 0, "the budget holds entities back when they do not all fit");
        Check(OwnSeen > Steps / 2, "the client's own entity keeps up regardless");
        Check(HighSeen > LowSeen * 2, "a higher Priority is updated more often");
        Check(LowSeen > 0, "but a lower one still gets its turn");

        /* Still now: every last value arrives. */
        const int32_t Final = 1000 + Steps;
        Check(StepUntil(Both, [&]
        {
            for(const Entity& E : Crowd) { const PawnComponent* P = OnClient(E); if(!P || P->Health != Final) { return false; } }
            return OnClient(Low) && OnClient(Low)->Health == Final;
        }, 20000), "once things settle, everything held back arrives");

        Client.Stop();
        Server.UnbindWorld();
        Server.Stop();
        Client.UnbindWorld();
    }
}

namespace
{
    /* RPCs declared Unreliable: sent once on their own channel. They arrive on
     * a clean link; under total loss they never do, while reliable ones still do. */
    void TestUnreliableRpcs()
    {
        std::printf("[unreliable rpcs]\n");
        size_t Count = 0;
        const RpcEntry* Rpcs = FindComponentRpcs(PawnComponent::StaticType().ID, Count);
        bool bFlagged = false;
        for(size_t Index = 0; Index < Count; ++Index) { if(std::strcmp(Rpcs[Index].Name, "MulticastSpark") == 0) { bFlagged = Rpcs[Index].bUnreliable != 0; } }
        Check(bFlagged, "Unreliable() marks the RPC's entry");

        for(const uint32_t Loss : { 0u, 100u })
        {
            World ServerWorld, ClientWorld;
            NetSession Server, Client;
            const Pair Both{ &Server, &Client };
            GServerWorld = &ServerWorld;
            NetConfig ServerConfig;
            ServerConfig.Port = 0;
            ServerConfig.SimulatedLossPercent = Loss;
            Server.Start(ENetRole::Server, ServerConfig);
            Server.BindWorld(ServerWorld, "Unreliable");
            NetConfig ClientConfig;
            std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
            ClientConfig.Port = Server.GetPort();
            Client.Start(ENetRole::Client, ClientConfig);
            Client.BindWorld(ClientWorld, "Unreliable");
            Check(StepUntil(Both, [&] { return Client.IsConnected(); }), Loss ? "a server losing every unreliable packet takes a client"
                                                                            : "a server and client for unreliable RPCs");
            Entity Pawn = ServerWorld.CreateEntity("Sparky");
            NetComponent Net;
            Net.OwnerClient = Client.GetLocalClientId();
            Pawn.AddComponent(Net);
            Pawn.AddComponent<PawnComponent>();
            EntityID There;
            Check(StepUntil(Both, [&] { There = Client.FindEntityByNetId(Server.GetNetId(Pawn.GetID())); return There.IsSet(); }),
                  "its pawn spawns (spawns are reliable)");
            GServerLog = RpcLog();
            GClientLog = RpcLog();

            GAMECORE_RPC(PawnComponent, MulticastSpark)(Pawn, 7);
            GAMECORE_RPC(PawnComponent, ClientPing)(Pawn, 9);   // reliable, alongside
            for(int Index = 0; Index < 200; ++Index) { Step(Both); }
            if(Loss == 0)
            {
                Check(GServerLog.Sparks == 1 && GClientLog.Sparks == 1 && GClientLog.LastSpark == 7,
                      "an unreliable Multicast runs on the server and reaches the client");
                GAMECORE_RPC(PawnComponent, ServerSpark)(Entity(&ClientWorld, There), 11);
                Check(StepUntil(Both, [&] { return GServerLog.Sparks == 2 && GServerLog.LastSpark == 11; }),
                      "an unreliable Server RPC reaches the server");
            }
            else
            {
                Check(GServerLog.Sparks == 1 && GClientLog.Sparks == 0, "lost on the way, an unreliable one never arrives");
                Check(GClientLog.Pings == 1 && GClientLog.LastPing == 9, "while a reliable one sent with it does");
            }

            Client.Stop();
            Server.UnbindWorld();
            Server.Stop();
            Client.UnbindWorld();
            GServerWorld = nullptr;
        }
    }
}

namespace
{
    /* A Smooth() field: a client blends it between the server's updates, as a
     * Transform, so a step from 0 to 100 passes through what lies between --
     * while a plain field jumps. */
    void TestFieldSmoothing()
    {
        std::printf("[field smoothing]\n");
        World ServerWorld, ClientWorld;
        NetSession Server, Client;
        const Pair Both{ &Server, &Client };
        NetConfig ServerConfig;
        ServerConfig.Port = 0;
        Server.Start(ENetRole::Server, ServerConfig);
        Server.BindWorld(ServerWorld, "Smooth");
        NetConfig ClientConfig;
        std::strcpy(ClientConfig.ServerAddress, "127.0.0.1");
        ClientConfig.Port = Server.GetPort();
        ClientConfig.InterpolationDelayMs = 200;
        Client.Start(ENetRole::Client, ClientConfig);
        Client.BindWorld(ClientWorld, "Smooth");
        Check(StepUntil(Both, [&] { return Client.IsConnected(); }), "a client showing others 200 ms back");

        Entity Lamp = ServerWorld.CreateEntity("Lamp");
        Lamp.AddComponent<NetComponent>();
        Lamp.AddComponent<PawnComponent>();
        EntityID There;
        Check(StepUntil(Both, [&] { There = Client.FindEntityByNetId(Server.GetNetId(Lamp.GetID())); return There.IsSet(); }), "it spawns");
        const auto Pawn = [&] { return Entity(&ClientWorld, There).GetComponent<PawnComponent>(); };
        for(int Index = 0; Index < 300; ++Index) { Step(Both); }   // settled at Glow 0

        /* From 0 to 100 over half a second, in ten steps; Health jumps once. */
        bool bBetween = false, bHealthBetween = false;
        float Highest = 0.0f;
        const auto Start = std::chrono::steady_clock::now();
        Lamp.GetComponent<PawnComponent>()->Health = 500;
        for(int Leg = 1; Leg <= 10; ++Leg)
        {
            Lamp.GetComponent<PawnComponent>()->Glow = 10.0f * float(Leg);
            while(std::chrono::steady_clock::now() < Start + std::chrono::milliseconds(50 * Leg))
            {
                Step(Both);
                const float Glow = Pawn()->Glow;
                if(std::fabs(Glow - std::round(Glow / 10.0f) * 10.0f) > 0.5f) { bBetween = true; }
                Highest = std::max(Highest, Glow);
                if(Pawn()->Health != 100 && Pawn()->Health != 500) { bHealthBetween = true; }
            }
        }
        Check(bBetween, "a smoothed field shows values between the server's updates");
        Check(!bHealthBetween, "a plain one only ever shows what was sent");
        Check(StepUntil(Both, [&] { return Near(Pawn()->Glow, 100.0f); }, 3000), "and the smoothed one settles on the last value");
        Check(Highest <= 100.0f + 0.01f, "never past it");

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
    TestRpcLimits();
    TestReconnect();
    TestViewTime();
    TestQuantization();
    TestBandwidthBudget();
    TestUnreliableRpcs();
    TestFieldSmoothing();
    TestFlushCost();
    TestStructureChanges(0);
    TestStructureChanges(50);

    if(GFailures == 0) { std::printf("NetCore loopback: all passed\n"); }
    else               { std::fprintf(stderr, "NetCore loopback: %d failure(s)\n", GFailures); }
    return GFailures == 0 ? 0 : 1;
}
