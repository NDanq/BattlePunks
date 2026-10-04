#include "Public/NetCore/NetSession.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Private/NetCore/NetProtocol.h"
#include "Private/NetCore/NetReplication.h"
#include "Private/NetCore/NetSchema.h"
#include "Private/NetCore/NetTransport.h"

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/*
 * The session: handshake, the server's spawn / state / destroy stream, the
 * client's side of it, and RPC routing. NetSession.h has the contract.
 */

namespace NetCore
{
using GameCore::ComponentTypeID;
using GameCore::EntityID;
using GameCore::NetClientID;
using GameCore::NetComponent;
using GameCore::NET_CLIENT_NONE;

namespace
{
    /* Every started session, oldest first. Usually one; a listen server, or
     * the loopback test, has a server and a client in the same process. */
    std::vector<NetSession::Impl*> GStarted;

    /* How an EntityID argument of an RPC travels (ComponentRpc.h's sink). */
    enum : uint8_t
    {
        RPC_ENTITY_NONE  = 0,
        RPC_ENTITY_NETID = 1,   // a networked entity, by NetId
        RPC_ENTITY_LOCAL = 2,   // a local one, by its packed id: meaningful only in this process
    };

    uint64_t ShadowKey(const int32_t InNetId, const ComponentTypeID InType)
    {
        return (uint64_t(uint32_t(InNetId)) << 16) | uint64_t(InType);
    }

    EngineCore::Archive BeginMessage(const ENetMessage InType)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        uint8_t Type = uint8_t(InType);
        Ar << Type;
        return Ar;
    }
}

const char* GetNetRoleName(const ENetRole InRole)
{
    switch(InRole)
    {
        case ENetRole::Server: return "Server";
        case ENetRole::Client: return "Client";
        default:               return "Standalone";
    }
}

struct NetSession::Impl final : public INetEntityMap
{
    NetSession*  Self     = nullptr;
    ENetRole     Role     = ENetRole::Standalone;
    bool         bStarted = false;
    NetConfig    Config;
    NetTransport Transport;
    NetSchema    Schema;

    // --- client ---------------------------------------------------------
    NetPeerID   ServerPeer  = NET_PEER_NONE;
    bool        bWelcomed   = false;
    NetClientID LocalClient = NET_CLIENT_NONE;

    struct PendingParent
    {
        EntityID           Child;
        GameCore::EntityGuid Parent;
    };
    std::vector<PendingParent> PendingParents;

    // --- server ---------------------------------------------------------
    struct ClientInfo
    {
        NetClientID ID        = NET_CLIENT_NONE;
        bool        bWelcomed = false;
    };
    std::unordered_map<NetPeerID, ClientInfo> Clients;
    NetClientID                               NextClientID = 0;

    ClientEventFn OnConnected    = nullptr;
    ClientEventFn OnDisconnected = nullptr;
    void*         EventUserData  = nullptr;

    // --- the World ------------------------------------------------------
    GameCore::World*         BoundWorld = nullptr;
    std::string              LevelName;
    GameCore::DelegateHandle DestroyedHandle;

    std::unordered_map<int32_t, EntityID>  NetToEntity;
    std::unordered_map<uint64_t, int32_t>  EntityToNet;   // by EntityID::Pack()
    int32_t                                NextNetId = 1;

    /* The server's last-sent copy of each networked component, by
     * ShadowKey: what the next diff compares against. */
    std::unordered_map<uint64_t, std::vector<uint8_t>> Shadows;
    uint32_t                                           ShadowGeneration = 0;

    /* Set while a remote RPC's arguments decode: a "local" entity from
     * another process means nothing here. */
    bool bDecodingRemote = false;

    // === INetEntityMap ===================================================

    int32_t ToNetId(const EntityID InEntity) const override
    {
        if(!InEntity.IsSet()) { return 0; }
        const auto Found = EntityToNet.find(InEntity.Pack());
        return Found != EntityToNet.end() ? Found->second : 0;
    }

    EntityID FromNetId(const int32_t InNetId) const override
    {
        if(InNetId == 0) { return EntityID(); }
        const auto Found = NetToEntity.find(InNetId);
        return Found != NetToEntity.end() ? Found->second : EntityID();
    }

    void MapEntity(const int32_t InNetId, const EntityID InEntity)
    {
        NetToEntity[InNetId]            = InEntity;
        EntityToNet[InEntity.Pack()]    = InNetId;
    }

    void UnmapEntity(const int32_t InNetId)
    {
        const auto Found = NetToEntity.find(InNetId);
        if(Found == NetToEntity.end()) { return; }
        EntityToNet.erase(Found->second.Pack());
        NetToEntity.erase(Found);
        for(auto It = Shadows.begin(); It != Shadows.end();)
        {
            if(int32_t(uint32_t(It->first >> 16)) == InNetId) { It = Shadows.erase(It); }
            else                                             { ++It; }
        }
    }

    // === Sending =========================================================

    void SendTo(const NetPeerID InPeer, const EngineCore::Archive& Ar)
    {
        if(!Ar.HasError()) { Transport.Send(InPeer, NET_CHANNEL_GAME, Ar.GetBytes()); }
    }

    /* Every welcomed client. */
    void Broadcast(const EngineCore::Archive& Ar)
    {
        if(Ar.HasError()) { return; }
        for(const auto& Entry : Clients)
        {
            if(Entry.second.bWelcomed) { Transport.Send(Entry.first, NET_CHANNEL_GAME, Ar.GetBytes()); }
        }
    }

    NetPeerID PeerOfClient(const NetClientID InClient) const
    {
        for(const auto& Entry : Clients)
        {
            if(Entry.second.bWelcomed && Entry.second.ID == InClient) { return Entry.first; }
        }
        return NET_PEER_NONE;
    }

    bool HasWelcomedClients() const
    {
        for(const auto& Entry : Clients) { if(Entry.second.bWelcomed) { return true; } }
        return false;
    }

    // === The server's stream =============================================

    EngineCore::Archive BuildSpawn(const int32_t InNetId, const EntityID InEntity)
    {
        GameCore::Entity Handle(BoundWorld, InEntity);
        EngineCore::Archive Ar = BeginMessage(ENetMessage::Spawn);

        int32_t NetId = InNetId;
        GameCore::EntityGuid Guid = Handle.GetGuid();
        std::string Name = Handle.GetName();
        GameCore::EntityGuid ParentGuid;
        if(Handle.HasParent()) { ParentGuid = GameCore::Entity(BoundWorld, Handle.GetParent()).GetGuid(); }
        Ar << NetId << Guid.Hi << Guid.Lo << Name << ParentGuid.Hi << ParentGuid.Lo;

        std::vector<ComponentTypeID> Types;
        for(size_t Index = 0; Index < GameCore::GetComponentTypeCount(); ++Index)
        {
            const ComponentTypeID Type = ComponentTypeID(Index);
            if(GameCore::FindComponentSerializer(Type) && Handle.HasComponentByType(Type)) { Types.push_back(Type); }
        }

        uint16_t Count = uint16_t(Types.size());
        Ar << Count;
        for(const ComponentTypeID Type : Types)
        {
            std::string TypeName = GameCore::GetComponentType(Type)->Name;
            Ar << TypeName;
            WriteComponentData(Ar, Type, Handle.GetComponentByType(Type), *this);
        }
        return Ar;
    }

    /* InMask's fields of InType on InEntity, or every field with bInFull. */
    bool BuildState(const int32_t InNetId, const ComponentTypeID InType, const void* InComponent,
                    const uint32_t InMask, EngineCore::Archive& OutAr)
    {
        size_t FieldCount = 0;
        const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(InType, FieldCount);
        const uint16_t Wire = Schema.ToWire(InType);
        if(!Fields || InMask == 0 || Wire == NET_WIRE_TYPE_INVALID) { return false; }

        OutAr = BeginMessage(ENetMessage::State);
        int32_t  NetId    = InNetId;
        uint16_t WireType = Wire;
        uint32_t Mask     = InMask;
        OutAr << NetId << WireType << Mask;
        for(size_t Field = 0; Field < FieldCount; ++Field)
        {
            if(InMask & (1u << Field)) { WriteReplicatedField(OutAr, Fields[Field], InComponent, *this); }
        }
        return !OutAr.HasError();
    }

    uint32_t FullMask(const ComponentTypeID InType) const
    {
        size_t FieldCount = 0;
        GameCore::FindComponentReplication(InType, FieldCount);
        return FieldCount >= 32 ? 0xFFFFFFFFu : ((1u << FieldCount) - 1u);
    }

    std::vector<ComponentTypeID> ReplicatedTypes() const
    {
        std::vector<ComponentTypeID> Types;
        GameCore::ForEachReplicatedType([](const ComponentTypeID InType, void* InUser)
        {
            static_cast<std::vector<ComponentTypeID>*>(InUser)->push_back(InType);
        }, &Types);
        return Types;
    }

    /* Spawn, then every replicated field, to one client: a late joiner. */
    void SendSnapshot(const NetPeerID InPeer)
    {
        if(!BoundWorld) { return; }

        std::vector<int32_t> Ids;
        Ids.reserve(NetToEntity.size());
        for(const auto& Entry : NetToEntity) { Ids.push_back(Entry.first); }
        std::sort(Ids.begin(), Ids.end());   // creation order: parents before children, usually

        const std::vector<ComponentTypeID> Types = ReplicatedTypes();
        for(const int32_t NetId : Ids)
        {
            const EntityID Entity = NetToEntity[NetId];
            SendTo(InPeer, BuildSpawn(NetId, Entity));
            for(const ComponentTypeID Type : Types)
            {
                const void* Component = GameCore::Entity(BoundWorld, Entity).GetComponentByType(Type);
                EngineCore::Archive Ar = BeginMessage(ENetMessage::State);
                if(Component && BuildState(NetId, Type, Component, FullMask(Type), Ar)) { SendTo(InPeer, Ar); }
            }
        }
    }

    void ServerFlush()
    {
        if(!BoundWorld) { return; }
        Schema.Refresh();

        /* A table changed (a module reloaded): what was cached about the old
         * layout is meaningless; everything goes again in full. */
        if(ShadowGeneration != GameCore::GetNetTablesGeneration())
        {
            Shadows.clear();
            ShadowGeneration = GameCore::GetNetTablesGeneration();
        }

        /* New networked entities: an id each, then a spawn to everyone. */
        std::vector<std::pair<int32_t, EntityID>> Spawned;
        BoundWorld->Each<NetComponent>([&](GameCore::Entity InEntity, NetComponent& InNet)
        {
            if(!InNet.bReplicate || InNet.NetId != 0) { return; }
            InNet.NetId = NextNetId++;
            MapEntity(InNet.NetId, InEntity.GetID());
            Spawned.emplace_back(InNet.NetId, InEntity.GetID());
        });
        const bool bAnyone = HasWelcomedClients();
        if(bAnyone)
        {
            for(const auto& Entry : Spawned) { Broadcast(BuildSpawn(Entry.first, Entry.second)); }
        }

        /* Changed fields. A component with no shadow yet sends everything. */
        const std::vector<ComponentTypeID> Types = ReplicatedTypes();
        for(const auto& Entry : NetToEntity)
        {
            const GameCore::Entity Handle(BoundWorld, Entry.second);
            for(const ComponentTypeID Type : Types)
            {
                const void* Component = Handle.GetComponentByType(Type);
                if(!Component) { continue; }

                size_t FieldCount = 0;
                const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(Type, FieldCount);
                const uint32_t Size = GameCore::GetComponentType(Type)->SizeBytes;
                std::vector<uint8_t>& Shadow = Shadows[ShadowKey(Entry.first, Type)];

                uint32_t Mask = 0;
                if(Shadow.size() != Size)
                {
                    Mask = FullMask(Type);
                }
                else
                {
                    for(size_t Field = 0; Field < FieldCount; ++Field)
                    {
                        if((Fields[Field].Flags & GameCore::RFF_InitialOnly) == 0
                           && ReplicatedFieldDiffers(Fields[Field], Component, Shadow.data()))
                        {
                            Mask |= 1u << Field;
                        }
                    }
                }
                if(Mask == 0) { continue; }

                Shadow.assign(static_cast<const uint8_t*>(Component), static_cast<const uint8_t*>(Component) + Size);
                EngineCore::Archive Ar = BeginMessage(ENetMessage::State);
                if(bAnyone && BuildState(Entry.first, Type, Component, Mask, Ar)) { Broadcast(Ar); }
            }
        }
    }

    // === Receiving =======================================================

    void HandleConnected(const NetPeerID InPeer)
    {
        if(Role == ENetRole::Server)
        {
            Clients[InPeer] = ClientInfo();
            CORE_LOG("NetCore: " + Transport.DescribePeer(InPeer) + " connected; waiting for its hello")
            return;
        }

        /* The client reached its server: introduce itself. */
        Schema.Refresh();
        EngineCore::Archive Ar = BeginMessage(ENetMessage::Hello);
        uint32_t    Version = NET_PROTOCOL_VERSION;
        uint64_t    Hash    = Schema.GetHash();
        std::string Level   = LevelName;
        Ar << Version << Hash << Level;
        SendTo(InPeer, Ar);
        CORE_LOG("NetCore: connected to " + Transport.DescribePeer(InPeer) + "; saying hello")
    }

    void HandleDisconnected(const NetPeerID InPeer)
    {
        if(Role == ENetRole::Server)
        {
            const auto Found = Clients.find(InPeer);
            if(Found == Clients.end()) { return; }
            const ClientInfo Info = Found->second;
            Clients.erase(Found);
            if(Info.bWelcomed)
            {
                CORE_LOG("NetCore: client " + std::to_string(Info.ID) + " disconnected")
                if(OnDisconnected) { OnDisconnected(Info.ID, EventUserData); }
            }
            return;
        }

        if(InPeer == ServerPeer)
        {
            CORE_LOG_WARN(bWelcomed ? "NetCore: lost the connection to the server"
                                    : "NetCore: could not connect to the server")
            bWelcomed   = false;
            LocalClient = NET_CLIENT_NONE;
            ServerPeer  = NET_PEER_NONE;
        }
    }

    void Reject(const NetPeerID InPeer, const std::string& InReason)
    {
        CORE_LOG_WARN("NetCore: refusing " + Transport.DescribePeer(InPeer) + ": " + InReason)
        EngineCore::Archive Ar = BeginMessage(ENetMessage::Reject);
        std::string Reason = InReason;
        Ar << Reason;
        SendTo(InPeer, Ar);
        Transport.Disconnect(InPeer);
    }

    void ServerHello(const NetPeerID InPeer, EngineCore::Archive& Ar)
    {
        uint32_t    Version = 0;
        uint64_t    Hash    = 0;
        std::string Level;
        Ar << Version << Hash << Level;
        if(Ar.HasError()) { return; }

        auto Found = Clients.find(InPeer);
        if(Found == Clients.end() || Found->second.bWelcomed) { return; }

        Schema.Refresh();
        if(Version != NET_PROTOCOL_VERSION)
        {
            Reject(InPeer, "protocol version " + std::to_string(Version) + ", the server speaks "
                           + std::to_string(NET_PROTOCOL_VERSION));
            return;
        }
        if(Hash != Schema.GetHash())
        {
            Reject(InPeer, "its replicated components or RPCs differ from the server's (a different build of the game)");
            return;
        }
        if(Level != LevelName)
        {
            CORE_LOG_WARN("NetCore: " + Transport.DescribePeer(InPeer) + " is in level '" + Level
                          + "', the server in '" + LevelName + "'")
        }
        if(Config.MaxClients != 0 && GetWelcomedCount() >= Config.MaxClients)
        {
            Reject(InPeer, "the server is full");
            return;
        }

        Found->second.ID        = NextClientID++;
        Found->second.bWelcomed = true;

        EngineCore::Archive Welcome = BeginMessage(ENetMessage::Welcome);
        int32_t ID = Found->second.ID;
        Welcome << ID;
        SendTo(InPeer, Welcome);
        CORE_LOG("NetCore: " + Transport.DescribePeer(InPeer) + " is client " + std::to_string(ID))

        SendSnapshot(InPeer);
        if(OnConnected) { OnConnected(ID, EventUserData); }
    }

    size_t GetWelcomedCount() const
    {
        size_t Count = 0;
        for(const auto& Entry : Clients) { if(Entry.second.bWelcomed) { ++Count; } }
        return Count;
    }

    void ClientSpawn(EngineCore::Archive& Ar)
    {
        int32_t              NetId = 0;
        GameCore::EntityGuid Guid, ParentGuid;
        std::string          Name;
        uint16_t             Count = 0;
        Ar << NetId << Guid.Hi << Guid.Lo << Name << ParentGuid.Hi << ParentGuid.Lo << Count;
        if(Ar.HasError() || NetId == 0) { return; }
        if(!BoundWorld)
        {
            CORE_LOG_WARN("NetCore: a spawn arrived before a World was bound; dropped")
            return;
        }

        /* The level's own entity if this client has it; a new one if not. */
        EntityID Entity = FromNetId(NetId);
        if(!Entity.IsSet() || !BoundWorld->IsAlive(Entity)) { Entity = Guid.IsSet() ? BoundWorld->FindEntityByGuid(Guid) : EntityID(); }
        if(!Entity.IsSet())                                 { Entity = BoundWorld->CreateEntity(Name.c_str(), Guid).GetID(); }
        MapEntity(NetId, Entity);
        GameCore::Entity Handle(BoundWorld, Entity);

        for(uint16_t Index = 0; Index < Count && !Ar.HasError(); ++Index)
        {
            std::string TypeName;
            Ar << TypeName;
            const GameCore::ComponentTypeInfo* Info = GameCore::FindComponentType(TypeName.c_str());
            if(!Info || !GameCore::FindComponentSerializer(Info->ID))
            {
                /* A type this build lacks: skip its chunk. */
                uint16_t Version = 0;
                if(Ar.OpenChunk(EngineCore::MakeArchiveTag('D', 'A', 'T', 'A'), Version)) { Ar.CloseChunk(); }
                continue;
            }

            if(void* Live = Handle.GetComponentByType(Info->ID))
            {
                ReadComponentData(Ar, Info->ID, Live, *this);
            }
            else
            {
                std::vector<uint8_t> Bytes(Info->SizeBytes, 0);
                const GameCore::ComponentSerializerInfo* Serializer = GameCore::FindComponentSerializer(Info->ID);
                if(Serializer->DefaultsFn) { Serializer->DefaultsFn(Bytes.data()); }
                if(ReadComponentData(Ar, Info->ID, Bytes.data(), *this)) { Handle.AddComponentByType(Info->ID, Bytes.data()); }
            }
        }

        /* Its id, whatever the component data said. */
        NetComponent* Net = Handle.GetComponent<NetComponent>();
        if(!Net) { Net = Handle.AddComponent<NetComponent>(); }
        if(Net) { Net->NetId = NetId; }

        if(ParentGuid.IsSet()) { PendingParents.push_back({ Entity, ParentGuid }); }
        ResolveParents();
    }

    /* A child can arrive before its parent; link what can be linked now. */
    void ResolveParents()
    {
        for(auto It = PendingParents.begin(); It != PendingParents.end();)
        {
            const EntityID Parent = BoundWorld->FindEntityByGuid(It->Parent);
            if(!BoundWorld->IsAlive(It->Child)) { It = PendingParents.erase(It); continue; }
            if(!Parent.IsSet())                 { ++It; continue; }
            GameCore::Entity(BoundWorld, It->Child).SetParent(Parent, false);
            It = PendingParents.erase(It);
        }
    }

    void ClientDestroy(EngineCore::Archive& Ar)
    {
        int32_t NetId = 0;
        Ar << NetId;
        if(Ar.HasError() || !BoundWorld) { return; }
        const EntityID Entity = FromNetId(NetId);
        UnmapEntity(NetId);
        if(Entity.IsSet() && BoundWorld->IsAlive(Entity)) { BoundWorld->DestroyEntity(Entity); }
    }

    void ClientState(EngineCore::Archive& Ar)
    {
        int32_t  NetId    = 0;
        uint16_t WireType = 0;
        uint32_t Mask     = 0;
        Ar << NetId << WireType << Mask;
        if(Ar.HasError() || !BoundWorld) { return; }

        const EntityID        Entity = FromNetId(NetId);
        const ComponentTypeID Type   = Schema.FromWire(WireType);
        void* Component = (Entity.IsSet() && Type != GameCore::COMPONENT_TYPE_INVALID)
                        ? GameCore::Entity(BoundWorld, Entity).GetComponentByType(Type) : nullptr;
        if(!Component) { return; }   // an entity or component this client does not have (yet)

        size_t FieldCount = 0;
        const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(Type, FieldCount);
        for(size_t Field = 0; Field < FieldCount && !Ar.HasError(); ++Field)
        {
            if(Mask & (1u << Field)) { ReadReplicatedField(Ar, Fields[Field], Component, *this); }
        }
    }

    void ReceiveRpc(const NetPeerID InPeer, EngineCore::Archive& Ar)
    {
        int32_t              NetId    = 0;
        uint16_t             WireType = 0;
        uint8_t              Index    = 0;
        std::vector<uint8_t> Args;
        uint32_t             Size     = 0;
        Ar << NetId << WireType << Index << Size;
        if(Ar.HasError() || Size > Ar.GetSize() - Ar.GetCursor()) { Ar.SetError("an RPC's arguments run past the packet"); return; }
        Args.resize(Size);
        if(Size > 0) { Ar.SerializeBytes(Args.data(), Size); }
        if(Ar.HasError() || !BoundWorld) { return; }

        const EntityID        Entity = FromNetId(NetId);
        const ComponentTypeID Type   = Schema.FromWire(WireType);
        size_t RpcCount = 0;
        const GameCore::RpcEntry* Rpcs = GameCore::FindComponentRpcs(Type, RpcCount);
        if(!Entity.IsSet() || !Rpcs || Index >= RpcCount) { return; }
        const GameCore::RpcEntry& Rpc = Rpcs[Index];

        NetClientID Sender = NET_CLIENT_NONE;
        if(Role == ENetRole::Server)
        {
            const auto Found = Clients.find(InPeer);
            if(Found == Clients.end() || !Found->second.bWelcomed) { return; }
            Sender = Found->second.ID;

            /* A client may only ask for Server RPCs, and only on what it owns. */
            const NetComponent* Net = GameCore::Entity(BoundWorld, Entity).GetComponent<NetComponent>();
            if(Rpc.Target != GameCore::ERpcTarget::Server || !Net || Net->OwnerClient != Sender)
            {
                CORE_LOG_WARN("NetCore: client " + std::to_string(Sender) + " asked for " + Rpc.Name
                              + " on an entity it does not own, or that is not a Server RPC; refused")
                return;
            }
        }
        else if(Rpc.Target == GameCore::ERpcTarget::Server)
        {
            return;   // the server never sends those
        }

        bDecodingRemote = true;
        GameCore::InvokeRpc(*BoundWorld, Entity, Type, Rpc, Args, Sender, true);
        bDecodingRemote = false;
    }

    void HandleReceived(const NetPeerID InPeer, const uint8_t InChannel, const std::vector<uint8_t>& InData)
    {
        if(InChannel != NET_CHANNEL_GAME) { return; }

        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InData);
        uint8_t Type = 0;
        Ar << Type;

        if(Role == ENetRole::Server)
        {
            const auto Found = Clients.find(InPeer);
            const bool bWelcomedPeer = Found != Clients.end() && Found->second.bWelcomed;
            switch(ENetMessage(Type))
            {
                case ENetMessage::Hello: ServerHello(InPeer, Ar); break;
                case ENetMessage::Rpc:   if(bWelcomedPeer) { ReceiveRpc(InPeer, Ar); } break;
                default:                 Ar.SetError("a message a client does not send"); break;
            }
        }
        else
        {
            switch(ENetMessage(Type))
            {
                case ENetMessage::Welcome:
                {
                    int32_t ID = NET_CLIENT_NONE;
                    Ar << ID;
                    if(!Ar.HasError())
                    {
                        bWelcomed   = true;
                        LocalClient = ID;
                        CORE_LOG("NetCore: the server welcomed this process as client " + std::to_string(ID))
                    }
                    break;
                }
                case ENetMessage::Reject:
                {
                    std::string Reason;
                    Ar << Reason;
                    CORE_LOG_ERROR("NetCore: the server refused this client: " + Reason)
                    break;
                }
                case ENetMessage::Spawn:   ClientSpawn(Ar);         break;
                case ENetMessage::Destroy: ClientDestroy(Ar);       break;
                case ENetMessage::State:   ClientState(Ar);         break;
                case ENetMessage::Rpc:     ReceiveRpc(InPeer, Ar);  break;
                default:                   Ar.SetError("a message a server does not send"); break;
            }
        }

        if(Ar.HasError())
        {
            CORE_LOG_WARN("NetCore: a malformed packet from " + Transport.DescribePeer(InPeer) + "; disconnecting it")
            Transport.Disconnect(InPeer);
        }
    }

    // === The World =======================================================

    static void OnEntityDestroyed(const EntityID InEntity, void* InUserData)
    {
        Impl& Self = *static_cast<Impl*>(InUserData);
        const int32_t NetId = Self.ToNetId(InEntity);
        if(NetId == 0) { return; }

        if(Self.Role == ENetRole::Server)
        {
            EngineCore::Archive Ar = BeginMessage(ENetMessage::Destroy);
            int32_t ID = NetId;
            Ar << ID;
            Self.Broadcast(Ar);
        }
        Self.UnmapEntity(NetId);
    }

    // === RPCs: RouterSink's per-session half =============================

    void SendRpc(const NetPeerID InPeer, const int32_t InNetId, const ComponentTypeID InType,
                 const GameCore::RpcEntry& InRpc, const std::vector<uint8_t>& InArgs, const bool bInBroadcast)
    {
        const uint16_t Wire = Schema.ToWire(InType);
        if(Wire == NET_WIRE_TYPE_INVALID) { return; }

        EngineCore::Archive Ar = BeginMessage(ENetMessage::Rpc);
        int32_t  NetId    = InNetId;
        uint16_t WireType = Wire;
        uint8_t  Index    = InRpc.Index;
        uint32_t Size     = uint32_t(InArgs.size());
        Ar << NetId << WireType << Index << Size;
        if(Size > 0)
        {
            std::vector<uint8_t> Copy = InArgs;
            Ar.SerializeBytes(Copy.data(), Size);
        }
        if(bInBroadcast) { Broadcast(Ar); }
        else             { SendTo(InPeer, Ar); }
    }

    void Route(GameCore::World& InWorld, const EntityID InEntity, const ComponentTypeID InType,
               const GameCore::RpcEntry& InRpc, const std::vector<uint8_t>& InArgs)
    {
        Schema.Refresh();
        const int32_t NetId = ToNetId(InEntity);
        const NetComponent* Net = GameCore::Entity(&InWorld, InEntity).GetComponent<NetComponent>();

        if(Role == ENetRole::Server)
        {
            switch(InRpc.Target)
            {
                case GameCore::ERpcTarget::Server:
                    GameCore::InvokeRpc(InWorld, InEntity, InType, InRpc, InArgs, NET_CLIENT_NONE, false);
                    break;
                case GameCore::ERpcTarget::Client:
                {
                    const NetClientID OwnerClient = Net ? Net->OwnerClient : NET_CLIENT_NONE;
                    if(OwnerClient == NET_CLIENT_NONE)
                    {
                        GameCore::InvokeRpc(InWorld, InEntity, InType, InRpc, InArgs, NET_CLIENT_NONE, false);
                        break;
                    }
                    const NetPeerID Peer = PeerOfClient(OwnerClient);
                    if(Peer == NET_PEER_NONE || NetId == 0)
                    {
                        CORE_LOG_WARN(std::string("NetCore: Client RPC ") + InRpc.Name + ": its owner is not connected, "
                                      "or the entity has not been sent yet; dropped")
                        break;
                    }
                    SendRpc(Peer, NetId, InType, InRpc, InArgs, false);
                    break;
                }
                case GameCore::ERpcTarget::Multicast:
                    GameCore::InvokeRpc(InWorld, InEntity, InType, InRpc, InArgs, NET_CLIENT_NONE, false);
                    if(NetId != 0) { SendRpc(NET_PEER_NONE, NetId, InType, InRpc, InArgs, true); }
                    break;
            }
            return;
        }

        /* A client. */
        if(InRpc.Target != GameCore::ERpcTarget::Server)
        {
            GameCore::InvokeRpc(InWorld, InEntity, InType, InRpc, InArgs, LocalClient, false);
            return;
        }
        if(!bWelcomed || NetId == 0)
        {
            CORE_LOG_WARN(std::string("NetCore: Server RPC ") + InRpc.Name
                          + ": not connected, or the entity is not a networked one; dropped")
            return;
        }
        SendRpc(ServerPeer, NetId, InType, InRpc, InArgs, false);
    }

    void WriteEntity(const EntityID InEntity, EngineCore::Archive& Ar)
    {
        uint8_t Kind = RPC_ENTITY_NONE;
        if(!InEntity.IsSet()) { Ar << Kind; return; }

        const int32_t NetId = ToNetId(InEntity);
        if(NetId != 0)
        {
            Kind = RPC_ENTITY_NETID;
            int32_t ID = NetId;
            Ar << Kind << ID;
            return;
        }
        Kind = RPC_ENTITY_LOCAL;
        uint64_t Packed = InEntity.Pack();
        Ar << Kind << Packed;
    }

    EntityID ReadEntity(EngineCore::Archive& Ar)
    {
        uint8_t Kind = RPC_ENTITY_NONE;
        Ar << Kind;
        if(Kind == RPC_ENTITY_NETID)
        {
            int32_t ID = 0;
            Ar << ID;
            return FromNetId(ID);
        }
        if(Kind == RPC_ENTITY_LOCAL)
        {
            uint64_t Packed = 0;
            Ar << Packed;
            return bDecodingRemote ? EntityID() : EntityID::Unpack(Packed);
        }
        if(Kind != RPC_ENTITY_NONE) { Ar.SetError("an RPC entity argument of an unknown kind"); }
        return EntityID();
    }
};

// === The sink ================================================================

namespace
{
    /*
     * GameCore has one RPC sink per process; this is it while any session is
     * started. A call goes to the session whose World it was made in, and
     * runs locally when there is none -- an editor's preview World, or a
     * Standalone role.
     */
    class RouterSink final : public GameCore::IRpcSink
    {
    public:
        static NetSession::Impl* FindFor(const GameCore::World& InWorld);

        void Route(GameCore::World& InWorld, const EntityID InEntity, const ComponentTypeID InType,
                   const GameCore::RpcEntry& InRpc, const std::vector<uint8_t>& InArgs) override
        {
            if(NetSession::Impl* Session = FindFor(InWorld)) { Session->Route(InWorld, InEntity, InType, InRpc, InArgs); }
            else { GameCore::InvokeRpc(InWorld, InEntity, InType, InRpc, InArgs, NET_CLIENT_NONE, false); }
        }

        void WriteEntity(GameCore::World& InWorld, const EntityID InEntity, EngineCore::Archive& Ar) override
        {
            if(NetSession::Impl* Session = FindFor(InWorld)) { Session->WriteEntity(InEntity, Ar); }
            else { IRpcSink::WriteEntity(InWorld, InEntity, Ar); }
        }

        EntityID ReadEntity(GameCore::World& InWorld, EngineCore::Archive& Ar) override
        {
            if(NetSession::Impl* Session = FindFor(InWorld)) { return Session->ReadEntity(Ar); }
            return IRpcSink::ReadEntity(InWorld, Ar);
        }
    };

    RouterSink GRouter;
}

/* Out of the class: it needs Impl complete, and Impl is above. */
NetSession::Impl* RouterSink::FindFor(const GameCore::World& InWorld)
{
    for(NetSession::Impl* Impl : GStarted)
    {
        if(Impl->Role != ENetRole::Standalone && Impl->BoundWorld == &InWorld) { return Impl; }
    }
    return nullptr;
}

// =============================================================================

NetSession::NetSession() : P(new Impl())
{
    P->Self = this;
}

NetSession::~NetSession()
{
    UnbindWorld();   // the World must still be alive: unbind before destroying it, or destroy this first
    Stop();
    delete P;
}

bool NetSession::Start(const ENetRole InRole, const NetConfig& InConfig)
{
    Stop();
    P->Role   = InRole;
    P->Config = InConfig;

    if(InRole == ENetRole::Server)
    {
        if(!P->Transport.Listen(InConfig.Port, InConfig.MaxClients + 4, InConfig.TimeoutMs)) { P->Role = ENetRole::Standalone; return false; }
        CORE_LOG("NetCore: server listening on port " + std::to_string(P->Transport.GetPort()))
    }
    else if(InRole == ENetRole::Client)
    {
        if(!P->Transport.Connect(InConfig.ServerAddress, InConfig.Port, InConfig.TimeoutMs, P->ServerPeer))
        {
            P->Role = ENetRole::Standalone;
            return false;
        }
        CORE_LOG(std::string("NetCore: connecting to ") + InConfig.ServerAddress + ":" + std::to_string(InConfig.Port))
    }

    P->bStarted = true;
    GStarted.push_back(P);
    GameCore::SetRpcSink(&GRouter);
    return true;
}

void NetSession::Stop()
{
    if(!P->bStarted) { return; }

    P->Transport.Close();
    P->Clients.clear();
    P->ServerPeer  = NET_PEER_NONE;
    P->bWelcomed   = false;
    P->LocalClient = NET_CLIENT_NONE;
    P->bStarted    = false;
    P->Role        = ENetRole::Standalone;

    GStarted.erase(std::remove(GStarted.begin(), GStarted.end(), P), GStarted.end());
    if(GStarted.empty() && &GameCore::GetRpcSink() == &GRouter) { GameCore::SetRpcSink(nullptr); }
}

ENetRole NetSession::GetRole() const { return P->Role; }

bool NetSession::IsConnected() const
{
    if(!P->bStarted) { return false; }
    return P->Role == ENetRole::Server ? P->Transport.IsOpen() : P->bWelcomed;
}

NetClientID NetSession::GetLocalClientId() const { return P->LocalClient; }
uint16_t    NetSession::GetPort() const          { return P->Transport.GetPort(); }
size_t      NetSession::GetClientCount() const   { return P->GetWelcomedCount(); }

void NetSession::BindWorld(GameCore::World& InWorld, const char* InLevelName)
{
    UnbindWorld();
    P->BoundWorld      = &InWorld;
    P->LevelName       = InLevelName ? InLevelName : "";
    P->DestroyedHandle = InWorld.OnEntityDestroyed().Add(&Impl::OnEntityDestroyed, P);
}

void NetSession::UnbindWorld()
{
    if(!P->BoundWorld) { return; }
    P->BoundWorld->OnEntityDestroyed().Remove(P->DestroyedHandle);
    P->DestroyedHandle = GameCore::DelegateHandle();
    P->BoundWorld = nullptr;
    P->NetToEntity.clear();
    P->EntityToNet.clear();
    P->Shadows.clear();
    P->PendingParents.clear();
}

GameCore::World* NetSession::GetBoundWorld() const { return P->BoundWorld; }

void NetSession::PollIncoming()
{
    if(!P->bStarted) { return; }

    NetTransportEvent Event;
    while(P->Transport.Service(Event))
    {
        switch(Event.EventType)
        {
            case NetTransportEvent::EType::Connected:    P->HandleConnected(Event.Peer); break;
            case NetTransportEvent::EType::Disconnected: P->HandleDisconnected(Event.Peer); break;
            case NetTransportEvent::EType::Received:     P->HandleReceived(Event.Peer, Event.Channel, Event.Data); break;
        }
    }
}

void NetSession::FlushOutgoing()
{
    if(!P->bStarted) { return; }
    if(P->Role == ENetRole::Server) { P->ServerFlush(); }
    P->Transport.Flush();
}

int32_t NetSession::GetNetId(const EntityID InEntity) const { return P->ToNetId(InEntity); }

EntityID NetSession::FindEntityByNetId(const int32_t InNetId) const { return P->FromNetId(InNetId); }

void NetSession::SetClientEvents(const ClientEventFn InConnected, const ClientEventFn InDisconnected, void* InUserData)
{
    P->OnConnected    = InConnected;
    P->OnDisconnected = InDisconnected;
    P->EventUserData  = InUserData;
}

NetSession* NetSession::GetActive() { return GStarted.empty() ? nullptr : GStarted.front()->Self; }
}
