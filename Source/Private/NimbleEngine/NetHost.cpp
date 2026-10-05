#include "Public/NimbleEngine/NetHost.h"

#include <cstdio>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Private/NimbleEngine/NetInternal.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/PlayerState.h"

#if WITH_NETCORE
#include "Public/NetCore/NetSession.h"
#endif

/* NetHost.h has the contract. */

#if WITH_NETCORE
/* A client: the server to join. Empty plays standalone. -connect sets it. */
NIMBLE_CONFIG_STRING(GNetServerAddress, "Net", "ServerAddress", "", nullptr);
/* A server listens here; a client connects here. */
NIMBLE_CONFIG_INT(GNetPort, "Net", "Port", 7777, nullptr);
/* A windowed game that hosts: a listen server (-listen). */
NIMBLE_CONFIG_BOOL(GNetListen, "Net", "Listen", false, nullptr);
NIMBLE_CONFIG_INT(GNetMaxClients, "Net", "MaxClients", 16, nullptr);
NIMBLE_CONFIG_INT(GNetTimeoutMs, "Net", "TimeoutMs", 30000, nullptr);
/* A headless server's frame rate; also how often it sends state. */
NIMBLE_CONFIG_INT(GNetServerTickRate, "Net", "ServerTickRate", 30, nullptr);
/* The host makes a player for each client that joins (IGameMode::OnClientConnected). */
NIMBLE_CONFIG_BOOL(GNetSpawnPlayerPerClient, "Net", "SpawnPlayerPerClient", true, nullptr);
#endif

namespace NimbleEngine
{
    NetHostDesc NetHostDesc::FromConfig(const ENetMode InMode)
    {
        NetHostDesc Desc;
        Desc.Mode = InMode;
#if WITH_NETCORE
        if(Desc.Mode == ENetMode::Standalone && GNetListen.GetBool())             { Desc.Mode = ENetMode::Server; }
        if(Desc.Mode == ENetMode::Standalone && GNetServerAddress.GetString()[0]) { Desc.Mode = ENetMode::Client; }
        if(GNetServerAddress.GetString()[0]) { Desc.ServerAddress = GNetServerAddress.GetString(); }
        Desc.Port                  = uint16_t(GNetPort.GetInt());
        Desc.MaxClients            = uint32_t(GNetMaxClients.GetInt() > 0 ? GNetMaxClients.GetInt() : 1);
        Desc.TimeoutMs             = uint32_t(GNetTimeoutMs.GetInt() > 0 ? GNetTimeoutMs.GetInt() : 30000);
        Desc.bSpawnPlayerPerClient = GNetSpawnPlayerPerClient.GetBool();
#endif
        return Desc;
    }

    uint32_t GetConfiguredServerTickRate()
    {
#if WITH_NETCORE
        return GNetServerTickRate.GetInt() > 0 ? uint32_t(GNetServerTickRate.GetInt()) : 30u;
#else
        return 30u;
#endif
    }

    namespace
    {
        /* The Net script library's events (NetScript.cpp): (Client, Player). */
        void EmitClientEvent(GameCore::World& InWorld, const char* InName, const GameCore::NetClientID InClient,
                             const GameCore::EntityID InPlayer)
        {
            GameCore::EventArgs Args;
            Args.Args[0] = GameCore::EventArg::Make(double(InClient));
            Args.Args[1] = GameCore::EventArg::Make(InPlayer);
            Args.Count   = 2;
            InWorld.GetEvents().Emit(InName, Args);
        }
    }

    struct NetHost::Impl
    {
        GameSession* Session = nullptr;
        NetHostDesc  Desc;
#if WITH_NETCORE
        NetCore::NetSession* Net       = nullptr;
        uint32_t             BoundLoad = 0;

        /* The session's World, once per load. */
        void Rebind()
        {
            if(!Net || !Session || !Session->HasWorld()) { return; }
            if(Net->GetBoundWorld() && BoundLoad == Session->GetLoadCount()) { return; }
            ILevel* Level = Session->GetLevel();
            Net->BindWorld(Session->GetWorld(), Level ? Level->GetName() : "");
            BoundLoad = Session->GetLoadCount();
        }

        static void OnClientConnected(const GameCore::NetClientID InClient, void* InUserData)
        {
            Impl& Self = *static_cast<Impl*>(InUserData);
            if(!Self.Session || !Self.Session->HasWorld()) { return; }

            GameCore::EntityID Player;
            if(Self.Desc.bSpawnPlayerPerClient)
            {
                /* Net first, so the player is already networked -- and owned --
                 * when OnPlayerAdded sees it. */
                char Name[32];
                std::snprintf(Name, sizeof(Name), "Player %d", int(InClient));
                const GameCore::Entity Entity = Self.Session->GetWorld().CreateEntity(Name);
                GameCore::NetComponent Net;
                Net.OwnerClient = InClient;
                Entity.AddComponent(Net);
                PlayerStateComponent State;
                std::snprintf(State.PlayerName, sizeof(State.PlayerName), "%s", Name);
                State.PlayerIndex = uint8_t(InClient & 0xFF);
                Entity.AddComponent(State);
                Player = Entity.GetID();
            }

            ILevel* Level = Self.Session->GetLevel();
            if(IGameMode* Mode = Level ? Level->GetGameMode() : nullptr) { Mode->OnClientConnected(InClient, Player); }
            EmitClientEvent(Self.Session->GetWorld(), "ClientConnected", InClient, Player);
        }

        static void OnClientDisconnected(const GameCore::NetClientID InClient, void* InUserData)
        {
            Impl& Self = *static_cast<Impl*>(InUserData);
            if(!Self.Session || !Self.Session->HasWorld()) { return; }

            /* Everything the client owned; its player is the one with a PlayerState. */
            GameCore::World& World = Self.Session->GetWorld();
            std::vector<GameCore::EntityID> Owned;
            GameCore::EntityID Player;
            World.Each<GameCore::NetComponent>([&](GameCore::Entity InEntity, GameCore::NetComponent& InNet)
            {
                if(InNet.OwnerClient != InClient) { return; }
                Owned.push_back(InEntity.GetID());
                if(!Player.IsSet() && InEntity.HasComponent<PlayerStateComponent>()) { Player = InEntity.GetID(); }
            });

            ILevel* Level = Self.Session->GetLevel();
            if(IGameMode* Mode = Level ? Level->GetGameMode() : nullptr) { Mode->OnClientDisconnected(InClient, Player); }
            EmitClientEvent(World, "ClientDisconnected", InClient, Player);

            for(const GameCore::EntityID Entity : Owned) { World.DestroyEntity(Entity); }
        }
#endif
    };

    NetHost::NetHost() : P(new Impl()) {}

    NetHost::~NetHost()
    {
        Stop();
        delete P;
    }

    bool NetHost::Start(GameSession& InSession, const NetHostDesc& InDesc)
    {
        Stop();
        P->Session = &InSession;
        P->Desc    = InDesc;
        SetDedicatedServerFlag(InDesc.Mode == ENetMode::Server && InDesc.bDedicated);
        if(InDesc.Mode == ENetMode::Standalone) { return true; }

#if WITH_NETCORE
        NetCore::NetConfig Config;
        std::snprintf(Config.ServerAddress, sizeof(Config.ServerAddress), "%s", InDesc.ServerAddress.c_str());
        Config.Port       = InDesc.Port;
        Config.MaxClients = InDesc.MaxClients;
        Config.TimeoutMs  = InDesc.TimeoutMs;

        P->Net = new NetCore::NetSession();
        P->Net->SetClientEvents(&Impl::OnClientConnected, &Impl::OnClientDisconnected, P);

        /* Bound before starting, so a client's handshake names its level. */
        P->Rebind();
        const NetCore::ENetRole Role = InDesc.Mode == ENetMode::Server ? NetCore::ENetRole::Server : NetCore::ENetRole::Client;
        if(!P->Net->Start(Role, Config))
        {
            CORE_LOG_ERROR(std::string("Networking failed to start as ") + NetCore::GetNetRoleName(Role) + "; running standalone")
            Stop();
            return false;
        }
        return true;
#else
        CORE_LOG_WARN("This build has networking compiled out (NIMBLE_WITH_NETCORE=OFF); running standalone")
        P->Desc.Mode = ENetMode::Standalone;
        return false;
#endif
    }

    void NetHost::Stop()
    {
#if WITH_NETCORE
        if(P->Net)
        {
            P->Net->FlushOutgoing();
            P->Net->UnbindWorld();
            P->Net->Stop();
            delete P->Net;
            P->Net = nullptr;
        }
        P->BoundLoad = 0;
#endif
        P->Session = nullptr;
        SetDedicatedServerFlag(false);
    }

    void NetHost::BeginFrame()
    {
#if WITH_NETCORE
        /* The World is about to be freed: let go of it first. */
        if(P->Net && P->Session && P->Session->HasPendingLevel()) { P->Net->UnbindWorld(); }
#endif
    }

    void NetHost::Poll()
    {
#if WITH_NETCORE
        if(!P->Net) { return; }
        P->Rebind();
        P->Net->PollIncoming();
#endif
    }

    void NetHost::Flush()
    {
#if WITH_NETCORE
        if(P->Net) { P->Net->FlushOutgoing(); }
#endif
    }

    bool NetHost::IsActive() const
    {
#if WITH_NETCORE
        return P->Net != nullptr;
#else
        return false;
#endif
    }

    ENetMode NetHost::GetMode() const { return IsActive() ? P->Desc.Mode : ENetMode::Standalone; }

    uint16_t NetHost::GetPort() const
    {
#if WITH_NETCORE
        return P->Net ? P->Net->GetPort() : 0;
#else
        return 0;
#endif
    }
}
