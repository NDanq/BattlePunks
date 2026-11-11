#include "Public/NimbleEngine/NetHost.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/NimbleEngine/LevelTravel.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Private/NimbleEngine/NetInternal.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/LagCompensation.h"
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
/* A client shows other entities this far in the past, blended between the
 * server's updates; 0 snaps to each (NetComponent.h). */
NIMBLE_CONFIG_INT(GNetInterpolationDelayMs, "Net", "InterpolationDelayMs", 100, nullptr);
NIMBLE_CONFIG_INT(GNetSimulatedLossPercent, "Net", "SimulatedLossPercent", 0, nullptr);
/* A server's limits on each client's RPCs (NetCore/README.md, "RPC limits"). */
NIMBLE_CONFIG_INT(GNetRpcPerSecond, "Net", "RpcPerSecond", 1000, nullptr);
/* How long a lost client's place is kept, and a client tries to rejoin (NetCore/README.md, "Reconnecting"). */
NIMBLE_CONFIG_INT(GNetReconnectGraceMs, "Net", "ReconnectGraceMs", 30000, nullptr);
NIMBLE_CONFIG_INT(GNetRpcFloodPerSecond, "Net", "RpcFloodPerSecond", 1000, nullptr);
NIMBLE_CONFIG_INT(GNetMaxRpcBytes, "Net", "MaxRpcBytes", 1024, nullptr);
/* A server's budget for each client's state, KB a second; 0 none (NetCore/README.md, "Bandwidth budget"). */
NIMBLE_CONFIG_INT(GNetClientBandwidthKBps, "Net", "ClientBandwidthKBps", 256, nullptr);
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
        Desc.InterpolationDelayMs  = uint32_t(GNetInterpolationDelayMs.GetInt() > 0 ? GNetInterpolationDelayMs.GetInt() : 0);
        Desc.SimulatedLossPercent  = uint32_t(std::clamp(GNetSimulatedLossPercent.GetInt(), 0, 100));
        Desc.RpcPerSecond          = uint32_t(std::max(0, GNetRpcPerSecond.GetInt()));
        Desc.ReconnectGraceMs      = uint32_t(std::max(0, GNetReconnectGraceMs.GetInt()));
        Desc.ClientBandwidthKBps   = uint32_t(std::max(0, GNetClientBandwidthKBps.GetInt()));
        Desc.RpcFloodPerSecond     = uint32_t(std::max(0, GNetRpcFloodPerSecond.GetInt()));
        Desc.MaxRpcBytes           = uint32_t(std::max(0, GNetMaxRpcBytes.GetInt()));
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

#if WITH_NETCORE
        /* A LevelTravel on the wire (NetSession's LevelChange carries it opaque):
         * u32 field count first, so a later field can be added behind it. */
        std::vector<uint8_t> WriteTravel(const LevelTravel& InTravel)
        {
            EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
            uint32_t    Fields     = 5;
            std::string Entry      = InTravel.Entry;
            std::string Title      = InTravel.Title;
            std::string Tip        = InTravel.Tip;
            uint8_t     Transition = InTravel.Transition;
            uint32_t    Count      = uint32_t(InTravel.Values.size());
            Ar << Fields << Entry << Title << Tip << Transition << Count;
            for(const std::pair<std::string, std::string>& Each : InTravel.Values)
            {
                std::string Key = Each.first, Value = Each.second;
                Ar << Key << Value;
            }
            return Ar.HasError() ? std::vector<uint8_t>() : Ar.GetBytes();
        }

        LevelTravel ReadTravel(const std::vector<uint8_t>& InBytes)
        {
            LevelTravel Travel;
            if(InBytes.empty()) { return Travel; }
            EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
            uint32_t Fields = 0, Count = 0;
            Ar << Fields << Travel.Entry << Travel.Title << Travel.Tip << Travel.Transition << Count;
            for(uint32_t Index = 0; Index < Count && !Ar.HasError(); ++Index)
            {
                std::string Key, Value;
                Ar << Key << Value;
                if(!Ar.HasError()) { Travel.Values.emplace_back(std::move(Key), std::move(Value)); }
            }
            if(Ar.HasError())
            {
                CORE_LOG_WARN("[Net] the server's level travel did not read; following without it")
                return LevelTravel();
            }
            return Travel;
        }

        bool IsPlayerIndexFree(GameCore::World& InWorld, const uint8_t InIndex)
        {
            bool bFree = true;
            InWorld.Each<PlayerStateComponent>([&](GameCore::Entity, PlayerStateComponent& InState)
            {
                if(InState.PlayerIndex == InIndex) { bFree = false; }
            });
            return bFree;
        }
#endif
    }

    struct NetHost::Impl
    {
        GameSession* Session = nullptr;
        NetHostDesc  Desc;
#if WITH_NETCORE
        NetCore::NetSession* Net       = nullptr;
        uint32_t             BoundLoad = 0;

        /* LEVEL TRAVEL. The session's level requests already announced (a
         * server's), and each client player's PlayerState as the old level
         * left it, for the same player in the next. */
        uint32_t                                                       SeenRequests = 0;
        std::vector<std::pair<GameCore::NetClientID, PlayerStateComponent>> Carried;

        /* The session's World, once per load. A server's later loads are
         * level travel: its clients' players are made again in the new one. */
        void Rebind()
        {
            if(!Net || !Session || !Session->HasWorld()) { return; }
            if(Net->GetBoundWorld() && BoundLoad == Session->GetLoadCount()) { return; }
            const bool bTravelled = BoundLoad != 0;
            ILevel* Level = Session->GetLevel();
            Net->BindWorld(Session->GetWorld(), Level ? Level->GetName() : "");
            BoundLoad = Session->GetLoadCount();
            if(bTravelled && Net->IsServer()) { WelcomeTravelledClients(); }
        }

        /* A server, before its level goes: each client player's standing. */
        void CarryPlayerStates(GameCore::World& InWorld)
        {
            Carried.clear();
            InWorld.Each<GameCore::NetComponent>([&](GameCore::Entity InEntity, GameCore::NetComponent& InNet)
            {
                if(InNet.OwnerClient == GameCore::NET_CLIENT_NONE) { return; }
                const PlayerStateComponent* State = InEntity.GetComponent<PlayerStateComponent>();
                if(!State) { return; }
                for(const auto& Each : Carried) { if(Each.first == InNet.OwnerClient) { return; } }
                Carried.emplace_back(InNet.OwnerClient, *State);
            });
        }

        /* InClient's player entity, with InCarried's standing if it had one;
         * unset without [Net] SpawnPlayerPerClient. */
        GameCore::EntityID MakeClientPlayer(const GameCore::NetClientID InClient, const PlayerStateComponent* InCarried)
        {
            if(!Desc.bSpawnPlayerPerClient) { return GameCore::EntityID(); }
            GameCore::World& World = Session->GetWorld();

            /* Net first, so the player is already networked -- and owned --
             * when OnPlayerAdded sees it. */
            char Name[32];
            std::snprintf(Name, sizeof(Name), "Player %d", int(InClient));
            const GameCore::Entity Entity = World.CreateEntity(Name);
            GameCore::NetComponent Net;
            Net.OwnerClient = InClient;
            Entity.AddComponent(Net);
            Entity.AddComponent<PossessorComponent>();   // a client's own player is its local player 1
            PlayerStateComponent State;
            if(InCarried)
            {
                State = *InCarried;
                if(!IsPlayerIndexFree(World, State.PlayerIndex)) { State.PlayerIndex = FindFreePlayerIndex(World); }
            }
            else
            {
                std::snprintf(State.PlayerName, sizeof(State.PlayerName), "%s", Name);
                State.PlayerIndex = FindFreePlayerIndex(World);   // not the host's
            }
            Entity.AddComponent(State);
            return Entity.GetID();
        }

        /* The server is in its new level: every client it had -- the ones
         * still loading it, and those away -- is a player in it now. */
        void WelcomeTravelledClients()
        {
            std::vector<GameCore::NetClientID> Present, All;
            Net->GetClientIds(Present, false);
            Net->GetClientIds(All, true);
            GameCore::World& World = Session->GetWorld();
            for(const GameCore::NetClientID Client : All)
            {
                const PlayerStateComponent* Standing = nullptr;
                for(const auto& Each : Carried) { if(Each.first == Client) { Standing = &Each.second; } }
                const GameCore::EntityID Player = MakeClientPlayer(Client, Standing);

                ILevel*    Level = Session->GetLevel();
                IGameMode* Mode  = Level ? Level->GetGameMode() : nullptr;
                if(Mode) { Mode->OnClientTravelled(Client, Player); }
                EmitClientEvent(World, "ClientConnected", Client, Player);
                if(std::find(Present.begin(), Present.end(), Client) == Present.end())
                {
                    if(Mode) { Mode->OnClientAway(Client, Player); }
                    EmitClientEvent(World, "ClientAway", Client, Player);
                }
            }
            if(!All.empty())
            {
                CORE_LOG("[Net] " + std::to_string(All.size()) + " client(s) brought to level '"
                         + std::string(Session->GetLevel() ? Session->GetLevel()->GetName() : "") + "'")
            }
            Carried.clear();
        }

        /* A client: the server is changing level; go too. */
        static void OnLevelChange(const char* InLevelName, const std::vector<uint8_t>& InTravel, void* InUserData)
        {
            Impl& Self = *static_cast<Impl*>(InUserData);
            if(!Self.Session) { return; }
            if(!Self.Session->FollowServerLevel(InLevelName, ReadTravel(InTravel)))
            {
                CORE_LOG_ERROR(std::string("[Net] the server went to level '") + (InLevelName ? InLevelName : "")
                               + "', which this game does not have; this client cannot follow")
            }
        }

        static void OnClientConnected(const GameCore::NetClientID InClient, void* InUserData)
        {
            Impl& Self = *static_cast<Impl*>(InUserData);
            if(!Self.Session || !Self.Session->HasWorld()) { return; }

            const GameCore::EntityID Player = Self.MakeClientPlayer(InClient, nullptr);
            ILevel* Level = Self.Session->GetLevel();
            if(IGameMode* Mode = Level ? Level->GetGameMode() : nullptr) { Mode->OnClientConnected(InClient, Player); }
            EmitClientEvent(Self.Session->GetWorld(), "ClientConnected", InClient, Player);
        }

        /* A simulated avatar replays its own frames (Avatar.h); anything else
         * goes to the game's handler (NetFacade.h, SetNetCorrectionHandler),
         * looked up at each call: a mode sets it after networking has started. */
        static void OnCorrection(GameCore::World& InWorld, const GameCore::EntityID InEntity, const int32_t InAck, void*)
        {
            AvatarWorld* Avatars = AvatarWorld::Find(InWorld);
            if(Avatars && Avatars->HandleCorrection(InEntity, InAck)) { return; }
            void* UserData = nullptr;
            if(const NetCorrectionFn Handler = GetNetCorrectionHandler(UserData)) { Handler(InWorld, InEntity, InAck, UserData); }
        }

        /* InClient's player: what it owns that has a PlayerState. */
        static GameCore::EntityID FindClientPlayer(GameCore::World& InWorld, const GameCore::NetClientID InClient)
        {
            GameCore::EntityID Player;
            InWorld.Each<GameCore::NetComponent>([&](GameCore::Entity InEntity, GameCore::NetComponent& InNet)
            {
                if(!Player.IsSet() && InNet.OwnerClient == InClient && InEntity.HasComponent<PlayerStateComponent>()) { Player = InEntity.GetID(); }
            });
            return Player;
        }

        /* Lost, its place kept: everything it owns stays, its avatar idle (no input). */
        static void OnClientAway(const GameCore::NetClientID InClient, void* InUserData)
        {
            Impl& Self = *static_cast<Impl*>(InUserData);
            if(!Self.Session || !Self.Session->HasWorld()) { return; }
            GameCore::World& World = Self.Session->GetWorld();
            const GameCore::EntityID Player = FindClientPlayer(World, InClient);
            ILevel* Level = Self.Session->GetLevel();
            if(IGameMode* Mode = Level ? Level->GetGameMode() : nullptr) { Mode->OnClientAway(InClient, Player); }
            EmitClientEvent(World, "ClientAway", InClient, Player);
        }

        static void OnClientReturned(const GameCore::NetClientID InClient, void* InUserData)
        {
            Impl& Self = *static_cast<Impl*>(InUserData);
            if(!Self.Session || !Self.Session->HasWorld()) { return; }
            GameCore::World& World = Self.Session->GetWorld();
            const GameCore::EntityID Player = FindClientPlayer(World, InClient);
            ILevel* Level = Self.Session->GetLevel();
            if(IGameMode* Mode = Level ? Level->GetGameMode() : nullptr) { Mode->OnClientReturned(InClient, Player); }
            EmitClientEvent(World, "ClientReturned", InClient, Player);
        }

        static void OnClientDisconnected(const GameCore::NetClientID InClient, void* InUserData)
        {
            Impl& Self = *static_cast<Impl*>(InUserData);
            if(!Self.Session || !Self.Session->HasWorld()) { return; }

            /* What its players drove goes back to whoever owns it undriven: a
             * level's vehicle to the server, the client's own avatar still to
             * the client. Then everything the client owns; its player is the
             * one with a PlayerState. */
            GameCore::World& World = Self.Session->GetWorld();
            ReleaseClientAvatars(World, InClient);
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
        Config.InterpolationDelayMs = InDesc.InterpolationDelayMs;
        Config.SimulatedLossPercent = InDesc.SimulatedLossPercent;
        Config.RpcPerSecond         = InDesc.RpcPerSecond;
        Config.ReconnectGraceMs     = InDesc.ReconnectGraceMs;
        Config.ClientBandwidthKBps  = InDesc.ClientBandwidthKBps;
        Config.RpcFloodPerSecond    = InDesc.RpcFloodPerSecond;
        Config.MaxRpcBytes          = InDesc.MaxRpcBytes;

        P->Net = new NetCore::NetSession();
        P->Net->SetClientEvents(&Impl::OnClientConnected, &Impl::OnClientDisconnected, P);
        P->Net->SetClientAwayEvents(&Impl::OnClientAway, &Impl::OnClientReturned);   // SetClientEvents's user data
        P->Net->SetCorrectionHandler(&Impl::OnCorrection, P);
        P->Net->SetLevelChangeHandler(&Impl::OnLevelChange, P);
        P->SeenRequests = InSession.GetLevelRequestCount();   // only what is asked for from now on goes to clients
        P->Carried.clear();

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
            if(GameCore::World* Bound = P->Net->GetBoundWorld()) { ClearLagCompensation(*Bound); }
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
        if(!P->Net || !P->Session) { return; }

        /* A server asked for a level: its clients are told now, ahead of the
         * swap, so a fade-out runs on both (NetCore/README.md, "Level travel"). */
        if(P->Net->IsServer() && P->Session->GetLevelRequestCount() != P->SeenRequests)
        {
            P->SeenRequests = P->Session->GetLevelRequestCount();
            if(ILevel* Next = P->Session->GetRequestedLevel())
            {
                P->Net->AnnounceLevel(Next->GetName(), WriteTravel(P->Session->GetRequestedTravel()));
            }
        }

        /* The World is about to be freed: let go of it first -- a server
         * keeping its clients' standing for the next level. */
        if(P->Session->HasPendingLevel())
        {
            if(GameCore::World* Bound = P->Net->GetBoundWorld())
            {
                if(P->Net->IsServer()) { P->CarryPlayerStates(*Bound); }
                ClearLagCompensation(*Bound);
            }
            P->Net->UnbindWorld();
        }
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
        if(!P->Net) { return; }
        /* Where every lag-compensated entity stood this frame, for the shots that arrive later (LagCompensation.h). */
        if(P->Net->IsServer() && P->Net->GetBoundWorld()) { RecordLagCompensation(*P->Net->GetBoundWorld(), P->Net->GetTimeMs()); }
        P->Net->FlushOutgoing();
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
