#include "Public/NetCore/NetSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
#include "Public/GameCore/Components/TransformComponent.h"
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

    /* State batches: the newest seen (anything older is ignored), and which
     * were applied, for the next StateAck. */
    uint32_t HighestBatch = 0;
    uint32_t AckLatest    = 0;
    uint32_t AckBits      = 0;
    bool     bAckDue      = false;

    /* Destroyed here: state still in flight for one is not missing a spawn. */
    std::unordered_set<int32_t> DestroyedIds;

    // --- server ---------------------------------------------------------

    /*
     * ACKNOWLEDGED BASELINES. Per client and networked component: the values
     * last sent, and the fields whose current value the client has not yet
     * acknowledged. Every batch carries, for each component it mentions,
     * every such field -- so whatever batches were lost, the newest one to
     * arrive brings the client to the server's state as of that batch, and a
     * field is sent each tick from when it changes until a batch carrying
     * that value is acknowledged.
     */
    struct Baseline
    {
        std::vector<uint8_t> Sent;              // the component's bytes as last sent
        uint32_t             Unacked = 0;       // fields the client may not have
        uint32_t             ValueSeq[32] = {}; // per field: the batch its current value was first sent in
    };

    /* What a batch carried: acknowledged, those fields' values are the client's. */
    struct SentBatch
    {
        uint32_t                                   Seq = 0;
        std::vector<std::pair<uint64_t, uint32_t>> Entries;   // ShadowKey, field mask
    };

    struct ClientInfo
    {
        NetClientID ID        = NET_CLIENT_NONE;
        bool        bWelcomed = false;

        uint32_t                               NextBatch = 1;
        std::unordered_map<uint64_t, Baseline> Baselines;   // by ShadowKey
        std::deque<SentBatch>                  InFlight;    // oldest first
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

    /* The replication tables' generation the baselines were made under. */
    uint32_t BaselineGeneration = 0;

    /* SimulatedLossPercent's dice. */
    uint32_t LossRandom = 0x9E3779B9u;

    /* Set while a remote RPC's arguments decode: a "local" entity from
     * another process means nothing here. */
    bool bDecodingRemote = false;

    // --- time ------------------------------------------------------------
    /* Each process's own clock, from when it started: the server stamps its
     * flushes with it (Frame), a client keeps the offset to it. */
    std::chrono::steady_clock::time_point Epoch = std::chrono::steady_clock::now();

    uint32_t NowMs() const
    {
        return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Epoch).count());
    }

    // --- a client's interpolation ------------------------------------------
    /* The server's clock as this client estimates it: local - offset. */
    bool     bHaveServerTime  = false;
    double   ServerOffsetMs   = 0.0;
    uint32_t LastServerTimeMs = 0;   // the last Frame's: the time of the State that follows it
    uint32_t PrevServerTimeMs = 0;   // the Frame before that

    struct TransformSample
    {
        uint32_t            TimeMs = 0;
        EngineCore::Vector3 Position, Rotation, Scale;
    };

    /* A remote entity's Transform as the server sent it, over time. Target
     * is the latest, as a whole component, so a State with only some fields
     * builds on it. */
    struct Interpolation
    {
        std::vector<TransformSample>  Samples;
        GameCore::TransformComponent  Target;
    };
    std::unordered_map<int32_t, Interpolation> Interpolations;   // by NetId

    /* Owned, predicted entities whose server state arrived this poll. */
    std::vector<int32_t> Corrected;
    CorrectionFn         OnCorrection        = nullptr;
    void*                CorrectionUserData  = nullptr;

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
        for(auto& Client : Clients)
        {
            std::unordered_map<uint64_t, Baseline>& Baselines = Client.second.Baselines;
            for(auto It = Baselines.begin(); It != Baselines.end();)
            {
                if(int32_t(uint32_t(It->first >> 16)) == InNetId) { It = Baselines.erase(It); }
                else                                             { ++It; }
            }
        }
        Interpolations.erase(InNetId);
    }

    // === A client's smoothing and prediction ==============================

    /* This client owns InEntity and moves it itself (NetComponent::bPredicted). */
    bool IsPredictedHere(const EntityID InEntity) const
    {
        const NetComponent* Net = GameCore::Entity(BoundWorld, InEntity).GetComponent<NetComponent>();
        return Net && Net->bPredicted && LocalClient != NET_CLIENT_NONE && Net->OwnerClient == LocalClient;
    }

    /* Shown in the past and blended: every networked Transform on a client,
     * but one it predicts. */
    bool IsInterpolated(const EntityID InEntity) const
    {
        return Role == ENetRole::Client && Config.InterpolationDelayMs > 0 && !IsPredictedHere(InEntity)
            && GameCore::Entity(BoundWorld, InEntity).HasComponent<GameCore::TransformComponent>();
    }

    /* The server's Transform at LastServerTimeMs, from InTarget. */
    void PushSample(Interpolation& InOut)
    {
        TransformSample Sample;
        Sample.TimeMs   = LastServerTimeMs;
        Sample.Position = InOut.Target.Position;
        Sample.Rotation = InOut.Target.Rotation;
        Sample.Scale    = InOut.Target.Scale;

        /* State is sent only when it changes, so an entity that sat still has
         * no sample since it stopped: it was still where the last one says
         * at the server's previous frame, and the move began after that. */
        if(!InOut.Samples.empty() && PrevServerTimeMs != 0 && InOut.Samples.back().TimeMs < PrevServerTimeMs
           && PrevServerTimeMs < Sample.TimeMs)
        {
            TransformSample Held = InOut.Samples.back();
            Held.TimeMs = PrevServerTimeMs;
            InOut.Samples.push_back(Held);
        }

        if(!InOut.Samples.empty() && InOut.Samples.back().TimeMs >= Sample.TimeMs) { InOut.Samples.back() = Sample; }
        else                                                                   { InOut.Samples.push_back(Sample); }

        /* A second of history is ample for any sane delay. */
        while(InOut.Samples.size() > 2 && InOut.Samples.front().TimeMs + 1000 < Sample.TimeMs)
        {
            InOut.Samples.erase(InOut.Samples.begin());
        }
    }

    /* A batch's time: the server's clock as near as arrival tells. The
     * offset is smoothed, so arrival jitter does not shake what is shown; a
     * jump (a stall on either side) is taken at once. A tick's batches share
     * one time. */
    void ReceiveServerTime(const uint32_t ServerMs)
    {
        if(ServerMs != LastServerTimeMs)
        {
            PrevServerTimeMs = LastServerTimeMs;
            LastServerTimeMs = ServerMs;
        }

        const double Sample = double(NowMs()) - double(ServerMs);
        if(!bHaveServerTime || std::fabs(Sample - ServerOffsetMs) > 1000.0)
        {
            ServerOffsetMs  = Sample;
            bHaveServerTime = true;
        }
        else
        {
            ServerOffsetMs += (Sample - ServerOffsetMs) * 0.1;
        }
    }

    static double LerpAngle(const double InFrom, const double InTo, const double InAlpha)
    {
        double Delta = std::fmod(InTo - InFrom, 360.0);
        if(Delta > 180.0)  { Delta -= 360.0; }
        if(Delta < -180.0) { Delta += 360.0; }
        return InFrom + Delta * InAlpha;
    }

    static EngineCore::Vector3 Lerp(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, const double InAlpha)
    {
        return EngineCore::Vector3(InA.GetX() + (InB.GetX() - InA.GetX()) * InAlpha,
                                   InA.GetY() + (InB.GetY() - InA.GetY()) * InAlpha,
                                   InA.GetZ() + (InB.GetZ() - InA.GetZ()) * InAlpha);
    }

    /* Every interpolated Transform, at the server's time minus the delay:
     * blended between the samples either side, or held at the newest when
     * the server has gone quiet (no guessing ahead). */
    void Interpolate()
    {
        if(Role != ENetRole::Client || !BoundWorld || !bHaveServerTime) { return; }
        const double RenderMs = double(NowMs()) - ServerOffsetMs - double(Config.InterpolationDelayMs);

        for(auto It = Interpolations.begin(); It != Interpolations.end();)
        {
            const EntityID Entity = FromNetId(It->first);
            GameCore::TransformComponent* Transform =
                Entity.IsSet() ? GameCore::Entity(BoundWorld, Entity).GetComponent<GameCore::TransformComponent>() : nullptr;
            if(!Transform) { It = Interpolations.erase(It); continue; }

            /* Became this client's to predict (its owner arrived after its
             * Transform): the server's latest, then the game's. */
            if(IsPredictedHere(Entity))
            {
                Transform->Position = It->second.Target.Position;
                Transform->Rotation = It->second.Target.Rotation;
                Transform->Scale    = It->second.Target.Scale;
                It = Interpolations.erase(It);
                continue;
            }

            const std::vector<TransformSample>& Samples = It->second.Samples;
            if(Samples.empty()) { ++It; continue; }

            size_t After = 0;
            while(After < Samples.size() && double(Samples[After].TimeMs) <= RenderMs) { ++After; }
            if(After == 0 || After == Samples.size())
            {
                const TransformSample& Held = After == 0 ? Samples.front() : Samples.back();
                Transform->Position = Held.Position;
                Transform->Rotation = Held.Rotation;
                Transform->Scale    = Held.Scale;
            }
            else
            {
                const TransformSample& A = Samples[After - 1];
                const TransformSample& B = Samples[After];
                const double Alpha = (RenderMs - double(A.TimeMs)) / double(B.TimeMs - A.TimeMs);
                Transform->Position = Lerp(A.Position, B.Position, Alpha);
                Transform->Rotation = EngineCore::Vector3(LerpAngle(A.Rotation.GetX(), B.Rotation.GetX(), Alpha),
                                                          LerpAngle(A.Rotation.GetY(), B.Rotation.GetY(), Alpha),
                                                          LerpAngle(A.Rotation.GetZ(), B.Rotation.GetZ(), Alpha));
                Transform->Scale    = Lerp(A.Scale, B.Scale, Alpha);
            }
            ++It;
        }
    }

    /* After the poll's state is written: the game reconciles what it predicts. */
    void FireCorrections()
    {
        if(Corrected.empty()) { return; }
        std::vector<int32_t> Ids;
        Ids.swap(Corrected);
        std::sort(Ids.begin(), Ids.end());
        Ids.erase(std::unique(Ids.begin(), Ids.end()), Ids.end());
        if(!OnCorrection || !BoundWorld) { return; }
        for(const int32_t NetId : Ids)
        {
            const EntityID Entity = FromNetId(NetId);
            const NetComponent* Net = Entity.IsSet() ? GameCore::Entity(BoundWorld, Entity).GetComponent<NetComponent>() : nullptr;
            if(Net && BoundWorld->IsAlive(Entity)) { OnCorrection(*BoundWorld, Entity, Net->InputAck, CorrectionUserData); }
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

    /* One state entry: InMask's fields of InType on InComponent. */
    bool WriteStateEntry(EngineCore::Archive& Ar, const int32_t InNetId, const ComponentTypeID InType,
                         const void* InComponent, const uint32_t InMask)
    {
        size_t FieldCount = 0;
        const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(InType, FieldCount);
        const uint16_t Wire = Schema.ToWire(InType);
        if(!Fields || InMask == 0 || Wire == NET_WIRE_TYPE_INVALID) { return false; }

        int32_t  NetId    = InNetId;
        uint16_t WireType = Wire;
        uint32_t Mask     = InMask;
        Ar << NetId << WireType << Mask;
        for(size_t Field = 0; Field < FieldCount; ++Field)
        {
            if(InMask & (1u << Field)) { WriteReplicatedField(Ar, Fields[Field], InComponent, *this); }
        }
        return !Ar.HasError();
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

    /* Testing: true for SimulatedLossPercent of calls. */
    bool SimulateLoss()
    {
        if(Config.SimulatedLossPercent == 0) { return false; }
        LossRandom ^= LossRandom << 13;
        LossRandom ^= LossRandom >> 17;
        LossRandom ^= LossRandom << 5;
        return LossRandom % 100u < Config.SimulatedLossPercent;
    }

    /* Every networked entity, to a client that just joined. Its state follows
     * in the next batches: a new client has no baselines, so they carry
     * every field. */
    void SendSnapshot(const NetPeerID InPeer)
    {
        if(!BoundWorld) { return; }

        std::vector<int32_t> Ids;
        Ids.reserve(NetToEntity.size());
        for(const auto& Entry : NetToEntity) { Ids.push_back(Entry.first); }
        std::sort(Ids.begin(), Ids.end());   // creation order: parents before children, usually

        for(const int32_t NetId : Ids) { SendTo(InPeer, BuildSpawn(NetId, NetToEntity[NetId])); }
    }

    /* This tick's state for one client, against what it has acknowledged:
     * one batch or more (split between entities, never within one, so an
     * entity's fields always arrive together), and at least one, entries or
     * not, so the client's clock keeps time. */
    void SendBatches(const NetPeerID InPeer, ClientInfo& InClient, const std::vector<ComponentTypeID>& InTypes,
                     const uint32_t InNowMs)
    {
        struct Item
        {
            uint64_t    Key;
            const void* Component;
            uint32_t    Size;
            uint32_t    Mask;      // what is sent
            uint32_t    Changed;   // of those, what changed since last sent
        };

        std::vector<uint8_t> Body;   // the open batch's entries
        uint16_t             Count = 0;
        SentBatch            Record;
        Record.Seq = InClient.NextBatch;

        const auto Emit = [&]
        {
            EngineCore::Archive Ar = BeginMessage(ENetMessage::StateBatch);
            uint32_t Seq  = Record.Seq;
            uint32_t Time = InNowMs;
            uint16_t N    = Count;
            Ar << Seq << Time << N;
            if(!Body.empty()) { Ar.SerializeBytes(Body.data(), Body.size()); }
            if(!Ar.HasError() && !SimulateLoss()) { Transport.Send(InPeer, NET_CHANNEL_STATE, Ar.GetBytes(), false); }

            InClient.InFlight.push_back(std::move(Record));
            while(InClient.InFlight.size() > 1024) { InClient.InFlight.pop_front(); }   // never answered: its fields stay unacked
            InClient.NextBatch++;

            Record = SentBatch();
            Record.Seq = InClient.NextBatch;
            Body.clear();
            Count = 0;
        };

        std::vector<Item> Items;
        for(const auto& Entry : NetToEntity)
        {
            const GameCore::Entity Handle(BoundWorld, Entry.second);
            Items.clear();
            EngineCore::Archive EntityAr = EngineCore::Archive::ForWriting();

            for(const ComponentTypeID Type : InTypes)
            {
                const void* Component = Handle.GetComponentByType(Type);
                if(!Component) { continue; }

                size_t FieldCount = 0;
                const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(Type, FieldCount);
                const uint32_t Size  = GameCore::GetComponentType(Type)->SizeBytes;
                const uint64_t Key   = ShadowKey(Entry.first, Type);
                const auto     Found = InClient.Baselines.find(Key);

                uint32_t Changed = 0;
                if(Found == InClient.Baselines.end() || Found->second.Sent.size() != Size)
                {
                    Changed = FullMask(Type);   // new to this client: everything
                }
                else
                {
                    for(size_t Field = 0; Field < FieldCount; ++Field)
                    {
                        if((Fields[Field].Flags & GameCore::RFF_InitialOnly) == 0
                           && ReplicatedFieldDiffers(Fields[Field], Component, Found->second.Sent.data()))
                        {
                            Changed |= 1u << Field;
                        }
                    }
                }
                const uint32_t Unacked = Found != InClient.Baselines.end() ? Found->second.Unacked : 0u;
                const uint32_t Mask    = Changed | Unacked;
                if(Mask == 0) { continue; }

                if(WriteStateEntry(EntityAr, Entry.first, Type, Component, Mask))
                {
                    Items.push_back({ Key, Component, Size, Mask, Changed });
                }
            }
            if(Items.empty()) { continue; }

            /* Full: this entity starts the next batch. */
            if(Count > 0 && Body.size() + EntityAr.GetSize() > NET_STATE_PACKET_BYTES) { Emit(); }

            Body.insert(Body.end(), EntityAr.GetBytes().begin(), EntityAr.GetBytes().end());
            for(const Item& Sent : Items)
            {
                Baseline& Base = InClient.Baselines[Sent.Key];
                Base.Sent.assign(static_cast<const uint8_t*>(Sent.Component), static_cast<const uint8_t*>(Sent.Component) + Sent.Size);
                for(uint32_t Field = 0; Field < 32; ++Field)
                {
                    if(Sent.Changed & (1u << Field)) { Base.ValueSeq[Field] = Record.Seq; }
                }
                Base.Unacked |= Sent.Mask;
                Record.Entries.emplace_back(Sent.Key, Sent.Mask);
                ++Count;
            }
        }
        Emit();
    }

    /* A client's StateAck: the batches it applied. Their fields are the
     * client's now -- each one whose value has not changed since that batch
     * stops being sent. A batch the ack passes over unapplied needs nothing:
     * what it carried is still unacked, so it is already in the next one. */
    void ReceiveStateAck(ClientInfo& InClient, EngineCore::Archive& Ar)
    {
        uint32_t Latest = 0, Bits = 0;
        Ar << Latest << Bits;
        if(Ar.HasError()) { return; }

        while(!InClient.InFlight.empty() && InClient.InFlight.front().Seq <= Latest)
        {
            const SentBatch& Batch  = InClient.InFlight.front();
            const uint32_t   Behind = Latest - Batch.Seq;
            const bool bApplied = Behind == 0 || (Behind <= 32 && ((Bits >> (Behind - 1)) & 1u) != 0);
            if(bApplied)
            {
                for(const auto& Entry : Batch.Entries)
                {
                    const auto Found = InClient.Baselines.find(Entry.first);
                    if(Found == InClient.Baselines.end()) { continue; }
                    Baseline& Base = Found->second;
                    for(uint32_t Field = 0; Field < 32; ++Field)
                    {
                        if((Entry.second & (1u << Field)) && Base.ValueSeq[Field] <= Batch.Seq) { Base.Unacked &= ~(1u << Field); }
                    }
                }
            }
            InClient.InFlight.pop_front();
        }
    }

    void ServerFlush()
    {
        if(!BoundWorld) { return; }
        Schema.Refresh();

        /* A table changed (a module reloaded): what was known about the old
         * layout is meaningless; everything goes again in full. */
        if(BaselineGeneration != GameCore::GetNetTablesGeneration())
        {
            for(auto& Client : Clients)
            {
                Client.second.Baselines.clear();
                Client.second.InFlight.clear();
            }
            BaselineGeneration = GameCore::GetNetTablesGeneration();
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
        if(!HasWelcomedClients()) { return; }
        for(const auto& Entry : Spawned) { Broadcast(BuildSpawn(Entry.first, Entry.second)); }

        /* Then each client's state, unreliably. */
        const std::vector<ComponentTypeID> Types = ReplicatedTypes();
        const uint32_t Now = NowMs();
        for(auto& Client : Clients)
        {
            if(Client.second.bWelcomed) { SendBatches(Client.first, Client.second, Types, Now); }
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

        /* Where it appeared is the first of its Transform's samples. */
        if(IsInterpolated(Entity))
        {
            Interpolation& Smooth = Interpolations[NetId];
            Smooth.Samples.clear();
            Smooth.Target = *Handle.GetComponent<GameCore::TransformComponent>();
            PushSample(Smooth);
        }
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
        DestroyedIds.insert(NetId);
        if(Entity.IsSet() && BoundWorld->IsAlive(Entity)) { BoundWorld->DestroyEntity(Entity); }
    }

    /* One entry of a StateBatch. Always read through, so the next one
     * parses; false if it is for an entity whose spawn has not arrived yet
     * (the batch is then not acknowledged, and the server sends it again). */
    bool ClientState(EngineCore::Archive& Ar)
    {
        int32_t  NetId    = 0;
        uint16_t WireType = 0;
        uint32_t Mask     = 0;
        Ar << NetId << WireType << Mask;
        if(Ar.HasError()) { return true; }

        const ComponentTypeID Type = Schema.FromWire(WireType);
        size_t FieldCount = 0;
        const GameCore::ReplicatedFieldEntry* Fields =
            Type != GameCore::COMPONENT_TYPE_INVALID ? GameCore::FindComponentReplication(Type, FieldCount) : nullptr;
        if(!Fields)
        {
            Ar.SetError("state for a type that does not replicate");
            return true;
        }

        const EntityID Entity    = FromNetId(NetId);
        void*          Component = Entity.IsSet() ? GameCore::Entity(BoundWorld, Entity).GetComponentByType(Type) : nullptr;

        /* A Transform shown in the past: the server's goes into its samples,
         * and Interpolate() writes the component. */
        const bool bTransform = Type == GameCore::TransformComponent::StaticType().ID;
        void* Into = Component;
        Interpolation* Smooth = nullptr;
        std::vector<uint8_t> Scratch;
        if(!Component)
        {
            /* Not here: read it past. A component the entity lacks, or an
             * entity already destroyed, is done with; one not spawned yet is
             * not. */
            Scratch.assign(GameCore::GetComponentType(Type)->SizeBytes, 0);
            Into = Scratch.data();
        }
        else if(bTransform && IsInterpolated(Entity))
        {
            const auto Found = Interpolations.find(NetId);
            Smooth = &Interpolations[NetId];
            if(Found == Interpolations.end()) { Smooth->Target = *static_cast<GameCore::TransformComponent*>(Component); }
            Into = &Smooth->Target;
        }

        for(size_t Field = 0; Field < FieldCount && !Ar.HasError(); ++Field)
        {
            if(Mask & (1u << Field)) { ReadReplicatedField(Ar, Fields[Field], Into, *this); }
        }
        if(Ar.HasError()) { return true; }
        if(!Component) { return Entity.IsSet() || DestroyedIds.count(NetId) != 0; }

        if(Smooth) { PushSample(*Smooth); }

        /* The server's word on something this client predicts: the game
         * reconciles once the poll is done. */
        if((bTransform || Type == NetComponent::StaticType().ID) && IsPredictedHere(Entity))
        {
            Corrected.push_back(NetId);
        }
        return true;
    }

    /* A StateBatch: newer than any before it, or ignored -- applying an
     * older one would put older values over newer. Applied whole, it is
     * acknowledged in the next flush. */
    void ReceiveStateBatch(EngineCore::Archive& Ar)
    {
        uint32_t Seq = 0, ServerMs = 0;
        uint16_t Count = 0;
        Ar << Seq << ServerMs << Count;
        if(Ar.HasError() || !BoundWorld || !bWelcomed || Seq <= HighestBatch) { return; }
        HighestBatch = Seq;
        ReceiveServerTime(ServerMs);

        bool bComplete = true;
        for(uint16_t Index = 0; Index < Count && !Ar.HasError(); ++Index)
        {
            if(!ClientState(Ar)) { bComplete = false; }
        }
        if(Ar.HasError() || !bComplete) { return; }

        /* Newest applied, and the 32 before it as bits. */
        const uint32_t Ahead = AckLatest == 0 ? 64u : Seq - AckLatest;
        const uint64_t Bits  = Ahead >= 64 ? 0u : ((uint64_t(AckBits) << Ahead) | (uint64_t(1) << (Ahead - 1)));
        AckBits   = uint32_t(Bits);
        AckLatest = Seq;
        bAckDue   = true;
    }

    /* A client's batch bookkeeping, for a new level or session. */
    void ResetBatches()
    {
        HighestBatch = 0;
        AckLatest    = 0;
        AckBits      = 0;
        bAckDue      = false;
        DestroyedIds.clear();
    }

    void SendStateAck()
    {
        if(!bAckDue || ServerPeer == NET_PEER_NONE) { return; }
        bAckDue = false;
        EngineCore::Archive Ar = BeginMessage(ENetMessage::StateAck);
        uint32_t Latest = AckLatest, Bits = AckBits;
        Ar << Latest << Bits;
        if(!Ar.HasError() && !SimulateLoss()) { Transport.Send(ServerPeer, NET_CHANNEL_STATE, Ar.GetBytes(), false); }
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
        if(InChannel != NET_CHANNEL_GAME && InChannel != NET_CHANNEL_STATE) { return; }

        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InData);
        uint8_t Type = 0;
        Ar << Type;

        /* State and its acks go unreliably on their channel; everything else reliably on the game one. */
        const bool bStateMessage = ENetMessage(Type) == ENetMessage::StateBatch || ENetMessage(Type) == ENetMessage::StateAck;
        if(bStateMessage != (InChannel == NET_CHANNEL_STATE)) { Ar.SetError("a message on the wrong channel"); }

        if(Role == ENetRole::Server)
        {
            const auto Found = Clients.find(InPeer);
            const bool bWelcomedPeer = Found != Clients.end() && Found->second.bWelcomed;
            switch(ENetMessage(Type))
            {
                case ENetMessage::Hello:    if(!Ar.HasError()) { ServerHello(InPeer, Ar); } break;
                case ENetMessage::StateAck: if(bWelcomedPeer && !Ar.HasError()) { ReceiveStateAck(Found->second, Ar); } break;
                case ENetMessage::Rpc:      if(bWelcomedPeer && !Ar.HasError()) { ReceiveRpc(InPeer, Ar); } break;
                default:                    Ar.SetError("a message a client does not send"); break;
            }
        }
        else if(!Ar.HasError())
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
                case ENetMessage::StateBatch: ReceiveStateBatch(Ar); break;
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
    P->bHaveServerTime  = false;
    P->LastServerTimeMs = 0;
    P->PrevServerTimeMs = 0;
    P->ResetBatches();
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
    for(auto& Client : P->Clients)
    {
        Client.second.Baselines.clear();
        Client.second.InFlight.clear();
    }
    P->ResetBatches();
    P->PendingParents.clear();
    P->Interpolations.clear();
    P->Corrected.clear();
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

    /* A client: other entities where they were a moment ago, smoothly; then
     * what it predicts, reconciled with what just arrived. */
    P->Interpolate();
    P->FireCorrections();
}

void NetSession::FlushOutgoing()
{
    if(!P->bStarted) { return; }
    if(P->Role == ENetRole::Server)      { P->ServerFlush(); }
    else if(P->Role == ENetRole::Client) { P->SendStateAck(); }
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

void NetSession::SetCorrectionHandler(const CorrectionFn InFn, void* InUserData)
{
    P->OnCorrection       = InFn;
    P->CorrectionUserData = InUserData;
}

NetSession* NetSession::GetActive() { return GStarted.empty() ? nullptr : GStarted.front()->Self; }
}
