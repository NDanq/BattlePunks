#include "Public/NetCore/NetSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <random>
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

    /*
     * THE SERVER'S WORD, whole. Each replicated component as the server last
     * stated it (by ShadowKey), from the spawn on, each batch applied over
     * it. A batch carries only the fields that changed, so the live
     * component of an entity this client PREDICTS holds its own newer values
     * in the rest -- and a correction replayed from that mix would apply the
     * newer input twice. Instead the whole of the server's state is copied
     * in before the replay (ClientState), so it starts from exactly where
     * the server was.
     */
    std::unordered_map<uint64_t, std::vector<uint8_t>> ServerValues;

    /* RECONNECTING. The token the server gave; whether this client is trying
     * to rejoin, since when, and when it tries next. */
    uint64_t Token          = 0;
    bool     bReconnecting  = false;
    uint32_t LostAtMs       = 0;
    uint32_t NextAttemptMs  = 0;

    /* A welcome came before in this bound World: a later one is a rejoin.
     * The entities a spawn created here (by EntityID::Pack()), which a new
     * session must remove; and, until the snapshot is complete, the NetIds
     * held from before that it has not sent again -- gone on the server. */
    bool                         bHadSession       = false;
    bool                         bAwaitingSnapshot = false;
    std::unordered_set<int32_t>  Unconfirmed;
    std::unordered_set<uint64_t> CreatedHere;

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

    /* RPC LIMITS: a token bucket per client for all its Server RPCs, and one
     * per RPC that declares a RateLimit. A call takes a token; without one
     * it is dropped. Tokens come back at the rate, up to the bucket's size. */
    struct Bucket
    {
        double Tokens = -1.0;    // < 0: full, first use
        double LastMs = 0.0;

        bool Take(const double InPerSecond, const double InCapacity, const double InNowMs)
        {
            if(Tokens < 0.0) { Tokens = InCapacity; LastMs = InNowMs; }
            Tokens = std::min(InCapacity, Tokens + (InNowMs - LastMs) * InPerSecond / 1000.0);
            LastMs = InNowMs;
            if(Tokens < 1.0) { return false; }
            Tokens -= 1.0;
            return true;
        }
    };

    struct ClientInfo
    {
        NetClientID ID        = NET_CLIENT_NONE;
        bool        bWelcomed = false;

        Bucket                                 RpcBudget;
        std::unordered_map<uint32_t, Bucket>   RpcLimits;     // by wire type << 8 | rpc index
        uint32_t                               DroppedThisSecond = 0;
        uint32_t                               DroppedSecond     = 0;   // NowMs() / 1000 the count is for
        uint64_t                               DroppedTotal      = 0;
        bool                                   bKicked           = false;
        uint64_t                               Token             = 0;   // what rejoins as this client
        uint32_t                               RefusedLogSecond  = 0xFFFFFFFFu;

        uint32_t                               NextBatch = 1;
        std::unordered_map<uint64_t, Baseline> Baselines;   // by ShadowKey
        /* The keys whose Baseline has fields unacknowledged: with this tick's
         * changes, all a client in step needs looked at. A client is in step
         * after its first full pass (new, back, or after a table reload). */
        std::unordered_set<uint64_t>           UnackedKeys;
        bool                                   bNeedsFullPass = true;

        /* THE BANDWIDTH BUDGET ([Net] ClientBandwidthKBps): bytes it may be
         * sent now, refilled with time; what was held back for want of it
         * (looked at again next tick); and, by NetId, the ticks an entity has
         * been waiting -- its priority grows with them, so nothing starves. */
        double                                 BudgetBytes   = 0.0;
        uint32_t                               BudgetAtMs    = 0;
        bool                                   bBudgetStarted = false;
        std::unordered_set<uint64_t>           DeferredKeys;
        std::unordered_map<int32_t, uint32_t>  Waiting;
        uint32_t                               LastDeferred  = 0;
        EntityID                               ViewEntity;   // its own entity with a Transform: where it looks from
        std::deque<SentBatch>                  InFlight;    // oldest first
    };
    std::unordered_map<NetPeerID, ClientInfo> Clients;
    NetClientID                               NextClientID = 0;

    /* Clients whose connection was lost, their places kept until ExpiresMs. */
    struct AwayClient
    {
        NetClientID ID        = NET_CLIENT_NONE;
        uint64_t    Token     = 0;
        uint32_t    ExpiresMs = 0;
    };
    std::vector<AwayClient> Away;

    ClientEventFn OnConnected    = nullptr;
    ClientEventFn OnDisconnected = nullptr;
    ClientEventFn OnAway         = nullptr;
    ClientEventFn OnReturned     = nullptr;
    void*         EventUserData  = nullptr;

    // --- the World ------------------------------------------------------
    GameCore::World*         BoundWorld = nullptr;
    std::string              LevelName;
    GameCore::DelegateHandle DestroyedHandle;

    GameCore::DelegateHandle ComponentAddedHandle;
    GameCore::DelegateHandle ComponentRemovedHandle;

    std::unordered_map<int32_t, EntityID>  NetToEntity;
    std::unordered_map<uint64_t, int32_t>  EntityToNet;   // by EntityID::Pack()
    int32_t                                NextNetId = 1;

    /*
     * WHAT CLIENTS WERE TOLD a networked entity is: its serialized
     * components, sorted, and its parent. The server keeps it as of the last
     * flush and sends AddComponents / RemoveComponents / Reparent when the
     * entity drifts from it; state goes only for components listed here. A
     * client keeps the components the server named, to tell state that is
     * ahead of its AddComponents from state for one removed here.
     */
    struct Structure
    {
        std::vector<ComponentTypeID> Types;
        GameCore::EntityGuid         Parent;
        /* The server's: the entity, and each of Types as it was at the last
         * flush (empty until first seen, or for a type that does not
         * replicate) -- what tells this tick's changes (BuildTickItems). */
        EntityID                          Entity;
        std::vector<std::vector<uint8_t>> Shadows;
    };
    std::unordered_map<int32_t, Structure> Structures;   // by NetId

    /* The server: entities whose components changed since the last flush (by EntityID::Pack()). */
    std::unordered_set<uint64_t> StructureDirty;

    /* The replication tables' generation the baselines were made under. */
    uint32_t BaselineGeneration = 0;

    /*
     * WHAT CHANGED THIS TICK, found once for every client: each replicated
     * component against its shadow from the last flush (Structure::Shadows).
     * One that matches has not changed, so a client that has acknowledged
     * all of it needs nothing; a client in step looks only at this tick's
     * changes and its own unacknowledged components (SendBatches). Only a
     * changed component is diffed field by field against a client's
     * baseline. (A change to a field that does not replicate makes it look
     * changed; the diff then finds nothing.)
     */

    /* Each type's replication table, by ComponentTypeID: looked up once per
     * table generation, not once a component a tick. */
    std::vector<const GameCore::ReplicatedFieldEntry*> RepFields;
    std::vector<size_t>                                RepCounts;
    uint32_t                                           RepGeneration = 0xFFFFFFFFu;

    void RefreshRepTables()
    {
        const size_t TypeCount = GameCore::GetComponentTypeCount();
        if(RepGeneration == GameCore::GetNetTablesGeneration() && RepFields.size() == TypeCount) { return; }
        RepFields.assign(TypeCount, nullptr);
        RepCounts.assign(TypeCount, 0);
        for(size_t Type = 0; Type < TypeCount; ++Type) { RepFields[Type] = GameCore::FindComponentReplication(ComponentTypeID(Type), RepCounts[Type]); }
        RepGeneration = GameCore::GetNetTablesGeneration();
    }

    struct TickItem
    {
        int32_t                               NetId      = 0;
        uint64_t                              Key        = 0;
        ComponentTypeID                       Type       = GameCore::COMPONENT_TYPE_INVALID;
        const void*                           Component  = nullptr;
        uint32_t                              Size       = 0;
        const GameCore::ReplicatedFieldEntry* Fields     = nullptr;
        size_t                                FieldCount = 0;
        bool                                  bChanged   = false;
    };
    std::vector<TickItem>        TickItems;        // this flush's, entity by entity
    std::vector<size_t>          TickEntities;     // where each entity's items start in TickItems
    std::vector<size_t>          TickChanged;      // the items that changed, in order
    std::unordered_set<uint64_t> TickChangedKeys;  // and their keys

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

    /* A Smooth() field (ComponentReplication.h) of a remote entity over time,
     * as the server sent it; Interpolate() writes the component. By
     * ShadowKey << 5 | the field's index. */
    struct FieldSample
    {
        uint32_t TimeMs = 0;
        float    Value[3] = {};
    };
    struct FieldSmoothing
    {
        std::vector<FieldSample> Samples;
        uint8_t                  Components = 1;   // 1 a Float, 3 a Vector3
        bool                     bAngle     = false;
    };
    std::unordered_map<uint64_t, FieldSmoothing> FieldSmooths;

    static uint64_t FieldKey(const int32_t InNetId, const ComponentTypeID InType, const size_t InField)
    {
        return (ShadowKey(InNetId, InType) << 5) | uint64_t(InField & 31u);
    }

    /* Every smoothing of InNetId's InType component: it went, or the entity did. */
    void ForgetFieldSmooths(const int32_t InNetId, const ComponentTypeID InType)
    {
        for(size_t Field = 0; Field < GameCore::REPLICATED_FIELDS_PER_TYPE_MAX; ++Field) { FieldSmooths.erase(FieldKey(InNetId, InType, Field)); }
    }

    /* The value of InField in InBytes (a component's), as up to three floats. */
    static uint8_t ReadFloats(const GameCore::ReplicatedFieldEntry& InField, const uint8_t* InBytes, float OutValue[3])
    {
        if(InField.FieldType == GameCore::EScriptFieldType::Float)
        {
            std::memcpy(&OutValue[0], InBytes + InField.Offset, sizeof(float));
            return 1;
        }
        EngineCore::Vector3 Vector;
        std::memcpy(&Vector, InBytes + InField.Offset, sizeof(Vector));
        OutValue[0] = float(Vector.GetX());
        OutValue[1] = float(Vector.GetY());
        OutValue[2] = float(Vector.GetZ());
        return 3;
    }

    static void WriteFloats(const GameCore::ReplicatedFieldEntry& InField, void* InComponent, const float InValue[3])
    {
        char* At = static_cast<char*>(InComponent) + InField.Offset;
        if(InField.FieldType == GameCore::EScriptFieldType::Float) { std::memcpy(At, &InValue[0], sizeof(float)); return; }
        const EngineCore::Vector3 Vector(InValue[0], InValue[1], InValue[2]);
        std::memcpy(At, &Vector, sizeof(Vector));
    }

    /* A new value for a smoothed field, at the batch's server time -- with the
     * same care as a Transform's (PushSample) for a field that sat still. */
    void PushFieldSample(const uint64_t InKey, const GameCore::ReplicatedFieldEntry& InField, const float InValue[3])
    {
        FieldSmoothing& Smooth = FieldSmooths[InKey];
        Smooth.Components = InField.FieldType == GameCore::EScriptFieldType::Float ? 1 : 3;
        Smooth.bAngle     = InField.Quantize == GameCore::RQ_Angle;

        FieldSample Sample;
        Sample.TimeMs = LastServerTimeMs;
        std::memcpy(Sample.Value, InValue, sizeof(Sample.Value));
        std::vector<FieldSample>& Samples = Smooth.Samples;
        if(!Samples.empty() && PrevServerTimeMs != 0 && Samples.back().TimeMs < PrevServerTimeMs && PrevServerTimeMs < Sample.TimeMs)
        {
            FieldSample Held = Samples.back();
            Held.TimeMs = PrevServerTimeMs;
            Samples.push_back(Held);
        }
        if(!Samples.empty() && Samples.back().TimeMs >= Sample.TimeMs) { Samples.back() = Sample; }
        else                                                           { Samples.push_back(Sample); }
        while(Samples.size() > 2 && Samples.front().TimeMs + 1000 < Sample.TimeMs) { Samples.erase(Samples.begin()); }
    }

    /* Every smoothed field, at the render time: between its samples, else held at the nearest. */
    void InterpolateFields(const double InRenderMs)
    {
        for(auto It = FieldSmooths.begin(); It != FieldSmooths.end();)
        {
            const uint64_t        Shadow = It->first >> 5;
            const size_t          Index  = size_t(It->first & 31u);
            const int32_t         NetId  = int32_t(uint32_t(Shadow >> 16));
            const ComponentTypeID Type   = ComponentTypeID(Shadow & 0xFFFFu);
            const EntityID        Entity = FromNetId(NetId);
            size_t FieldCount = 0;
            const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(Type, FieldCount);
            void* Component = Entity.IsSet() && Fields && Index < FieldCount ? GameCore::Entity(BoundWorld, Entity).GetComponentByType(Type) : nullptr;
            const std::vector<FieldSample>& Samples = It->second.Samples;
            if(!Component || Samples.empty()) { It = FieldSmooths.erase(It); continue; }

            /* Became this client's to predict: the server's latest, and no more blending. */
            if(IsPredictedHere(Entity))
            {
                WriteFloats(Fields[Index], Component, Samples.back().Value);
                It = FieldSmooths.erase(It);
                continue;
            }

            size_t After = 0;
            while(After < Samples.size() && double(Samples[After].TimeMs) <= InRenderMs) { ++After; }
            float Value[3] = {};
            if(After == 0 || After == Samples.size())
            {
                std::memcpy(Value, (After == 0 ? Samples.front() : Samples.back()).Value, sizeof(Value));
            }
            else
            {
                const FieldSample& A = Samples[After - 1];
                const FieldSample& B = Samples[After];
                const double Alpha = (InRenderMs - double(A.TimeMs)) / double(B.TimeMs - A.TimeMs);
                for(uint8_t C = 0; C < It->second.Components; ++C)
                {
                    Value[C] = It->second.bAngle ? float(LerpAngle(A.Value[C], B.Value[C], Alpha))
                                                 : float(double(A.Value[C]) + (double(B.Value[C]) - double(A.Value[C])) * Alpha);
                }
            }
            WriteFloats(Fields[Index], Component, Value);
            ++It;
        }
    }

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
        /* Only its own keys: one per component clients were told of (state goes for no other). */
        if(const auto Known = Structures.find(InNetId); Known != Structures.end())
        {
            for(const ComponentTypeID Type : Known->second.Types)
            {
                const uint64_t Key = ShadowKey(InNetId, Type);
                ServerValues.erase(Key);
                ForgetFieldSmooths(InNetId, Type);
                for(auto& Client : Clients)
                {
                    Client.second.Baselines.erase(Key);
                    Client.second.UnackedKeys.erase(Key);
                    Client.second.DeferredKeys.erase(Key);
                }
            }
        }
        Interpolations.erase(InNetId);
        Structures.erase(InNetId);
        for(auto& Client : Clients) { Client.second.Waiting.erase(InNetId); }
    }

    /* Every client's baseline for one component, gone with it. */
    void DropBaselines(const int32_t InNetId, const ComponentTypeID InType)
    {
        const uint64_t Key = ShadowKey(InNetId, InType);
        for(auto& Client : Clients) { Client.second.Baselines.erase(Key); Client.second.UnackedKeys.erase(Key); Client.second.DeferredKeys.erase(Key); }
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
        InterpolateFields(RenderMs);

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

    /* Every replicated field of every component of InEntity, as the server
     * last stated it: the whole state a replay starts from. */
    void RestoreServerValues(const int32_t InNetId, const EntityID InEntity)
    {
        const auto Known = Structures.find(InNetId);
        if(Known == Structures.end()) { return; }
        const GameCore::Entity Handle(BoundWorld, InEntity);
        for(const ComponentTypeID Type : Known->second.Types)
        {
            const auto Server = ServerValues.find(ShadowKey(InNetId, Type));
            void* Component = Handle.GetComponentByType(Type);
            size_t FieldCount = 0;
            const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(Type, FieldCount);
            if(Server == ServerValues.end() || !Component || !Fields) { continue; }
            for(size_t Field = 0; Field < FieldCount; ++Field)
            {
                if(Fields[Field].Offset + Fields[Field].Size > Server->second.size()) { continue; }
                std::memcpy(static_cast<char*>(Component) + Fields[Field].Offset, Server->second.data() + Fields[Field].Offset, Fields[Field].Size);
            }
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
        if(!BoundWorld) { return; }
        for(const int32_t NetId : Ids)
        {
            const EntityID Entity = FromNetId(NetId);
            if(!Entity.IsSet() || !BoundWorld->IsAlive(Entity)) { continue; }
            RestoreServerValues(NetId, Entity);   // with or without a handler: it is the server's state
            const NetComponent* Net = GameCore::Entity(BoundWorld, Entity).GetComponent<NetComponent>();
            if(Net && OnCorrection) { OnCorrection(*BoundWorld, Entity, Net->InputAck, CorrectionUserData); }
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

    /* What a spawn carries: InEntity's serialized components, in type order. */
    std::vector<ComponentTypeID> SerializedTypes(const GameCore::Entity& InHandle) const
    {
        std::vector<ComponentTypeID> Types;
        for(size_t Index = 0; Index < GameCore::GetComponentTypeCount(); ++Index)
        {
            const ComponentTypeID Type = ComponentTypeID(Index);
            if(GameCore::FindComponentSerializer(Type) && InHandle.HasComponentByType(Type)) { Types.push_back(Type); }
        }
        return Types;
    }

    GameCore::EntityGuid ParentGuidOf(const GameCore::Entity& InHandle) const
    {
        return InHandle.HasParent() ? GameCore::Entity(BoundWorld, InHandle.GetParent()).GetGuid() : GameCore::EntityGuid();
    }

    /* u16 count, then each component's type name and serialized data. */
    void WriteComponents(EngineCore::Archive& Ar, const GameCore::Entity& InHandle, const std::vector<ComponentTypeID>& InTypes)
    {
        uint16_t Count = uint16_t(InTypes.size());
        Ar << Count;
        for(const ComponentTypeID Type : InTypes)
        {
            std::string TypeName = GameCore::GetComponentType(Type)->Name;
            Ar << TypeName;
            WriteComponentData(Ar, Type, InHandle.GetComponentByType(Type), *this);
        }
    }

    EngineCore::Archive BuildSpawn(const int32_t InNetId, const EntityID InEntity)
    {
        GameCore::Entity Handle(BoundWorld, InEntity);
        EngineCore::Archive Ar = BeginMessage(ENetMessage::Spawn);

        int32_t NetId = InNetId;
        GameCore::EntityGuid Guid = Handle.GetGuid();
        std::string Name = Handle.GetName();
        GameCore::EntityGuid ParentGuid = ParentGuidOf(Handle);
        Ar << NetId << Guid.Hi << Guid.Lo << Name << ParentGuid.Hi << ParentGuid.Lo;
        WriteComponents(Ar, Handle, SerializedTypes(Handle));
        return Ar;
    }

    /*
     * Networked entities that changed shape since the last flush: components
     * gained or lost, a new parent, or no longer replicated (bReplicate off,
     * or the NetComponent removed -- clients destroy it, and turned back on
     * it spawns again under a new NetId). Reliable, so in order with spawns.
     */
    void SendStructureChanges()
    {
        std::vector<int32_t> Unreplicated;
        for(const auto& Entry : NetToEntity)
        {
            const int32_t          NetId = Entry.first;
            const GameCore::Entity Handle(BoundWorld, Entry.second);
            const NetComponent*    Net = Handle.GetComponent<NetComponent>();
            if(!Net || !Net->bReplicate)
            {
                Unreplicated.push_back(NetId);
                continue;
            }

            Structure& Known = Structures[NetId];
            if(StructureDirty.count(Entry.second.Pack()) != 0)
            {
                std::vector<ComponentTypeID> Types = SerializedTypes(Handle);
                std::vector<ComponentTypeID> Added, Removed;
                std::set_difference(Types.begin(), Types.end(), Known.Types.begin(), Known.Types.end(), std::back_inserter(Added));
                std::set_difference(Known.Types.begin(), Known.Types.end(), Types.begin(), Types.end(), std::back_inserter(Removed));

                if(!Added.empty())
                {
                    EngineCore::Archive Ar = BeginMessage(ENetMessage::AddComponents);
                    int32_t ID = NetId;
                    Ar << ID;
                    WriteComponents(Ar, Handle, Added);
                    Broadcast(Ar);
                }
                if(!Removed.empty())
                {
                    EngineCore::Archive Ar = BeginMessage(ENetMessage::RemoveComponents);
                    int32_t  ID    = NetId;
                    uint16_t Count = uint16_t(Removed.size());
                    Ar << ID << Count;
                    for(const ComponentTypeID Type : Removed)
                    {
                        std::string TypeName = GameCore::GetComponentType(Type)->Name;
                        Ar << TypeName;
                        DropBaselines(NetId, Type);
                    }
                    Broadcast(Ar);
                }
                std::vector<std::vector<uint8_t>> Shadows(Types.size());
                for(size_t Index = 0; Index < Types.size(); ++Index)
                {
                    const auto Old = std::find(Known.Types.begin(), Known.Types.end(), Types[Index]);
                    if(Old != Known.Types.end() && size_t(Old - Known.Types.begin()) < Known.Shadows.size())
                    {
                        Shadows[Index] = std::move(Known.Shadows[size_t(Old - Known.Types.begin())]);
                    }
                }
                Known.Types   = std::move(Types);
                Known.Shadows = std::move(Shadows);
            }

            const GameCore::EntityGuid Parent = ParentGuidOf(Handle);
            if(Parent != Known.Parent)
            {
                EngineCore::Archive Ar = BeginMessage(ENetMessage::Reparent);
                int32_t ID = NetId;
                GameCore::EntityGuid Guid = Parent;
                Ar << ID << Guid.Hi << Guid.Lo;
                Broadcast(Ar);
                Known.Parent = Parent;
            }
        }
        StructureDirty.clear();

        for(const int32_t NetId : Unreplicated)
        {
            EngineCore::Archive Ar = BeginMessage(ENetMessage::Destroy);
            int32_t ID = NetId;
            Ar << ID;
            Broadcast(Ar);

            const EntityID Entity = FromNetId(NetId);
            if(NetComponent* Net = GameCore::Entity(BoundWorld, Entity).GetComponent<NetComponent>()) { Net->NetId = 0; }
            UnmapEntity(NetId);
        }
    }

    /* One state entry: InMask's fields of InType on InComponent. */
    bool WriteStateEntry(EngineCore::Archive& Ar, const int32_t InNetId, const ComponentTypeID InType,
                         const void* InComponent, const uint32_t InMask)
    {
        size_t FieldCount = 0;
        const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(InType, FieldCount);
        const uint16_t Wire = Schema.ToWire(InType);
        if(!Fields || InMask == 0 || Wire == NET_WIRE_TYPE_INVALID) { return false; }

        /* Variable-length: a NetId under 16384 is two bytes, a wire type and a mask of the first 7 fields one each. */
        WriteVarUInt(Ar, uint32_t(InNetId));
        WriteVarUInt(Ar, Wire);
        WriteVarUInt(Ar, InMask);
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

    /* This flush's replicated components, entity by entity, each marked
     * changed or not against its shadow -- once, for every client. Only
     * what clients were told an entity has: state for anything else would
     * wait on a spawn that never comes. */
    void BuildTickItems()
    {
        RefreshRepTables();
        TickItems.clear();
        TickEntities.clear();
        TickChanged.clear();
        TickChangedKeys.clear();
        for(auto& Entry : Structures)
        {
            Structure& Known = Entry.second;
            if(!Known.Entity.IsSet()) { continue; }
            if(Known.Shadows.size() != Known.Types.size()) { Known.Shadows.resize(Known.Types.size()); }
            const GameCore::Entity Handle(BoundWorld, Known.Entity);
            const size_t Start = TickItems.size();
            for(size_t Index = 0; Index < Known.Types.size(); ++Index)
            {
                const ComponentTypeID Type = Known.Types[Index];
                if(size_t(Type) >= RepFields.size() || !RepFields[Type] || RepCounts[Type] == 0) { continue; }
                const void* Component = Handle.GetComponentByType(Type);
                if(!Component) { continue; }

                TickItem Item;
                Item.NetId      = Entry.first;
                Item.Key        = ShadowKey(Entry.first, Type);
                Item.Type       = Type;
                Item.Component  = Component;
                Item.Size       = GameCore::GetComponentType(Type)->SizeBytes;
                Item.Fields     = RepFields[Type];
                Item.FieldCount = RepCounts[Type];

                std::vector<uint8_t>& Shadow = Known.Shadows[Index];
                const uint8_t* Bytes = static_cast<const uint8_t*>(Component);
                Item.bChanged = Shadow.size() != Item.Size || std::memcmp(Shadow.data(), Bytes, Item.Size) != 0;
                if(Item.bChanged)
                {
                    Shadow.assign(Bytes, Bytes + Item.Size);
                    TickChanged.push_back(TickItems.size());
                    TickChangedKeys.insert(Item.Key);
                }
                TickItems.push_back(Item);
            }
            if(TickItems.size() > Start) { TickEntities.push_back(Start); }
        }
    }

    /* Tokens for InClient's state: the rate times the time since, up to a
     * quarter second's worth (and never under one packet), so a quiet spell
     * cannot bank a flood. */
    void RefillBudget(ClientInfo& InClient)
    {
        const double Rate     = double(Config.ClientBandwidthKBps) * 1024.0;
        const double Capacity = std::max(double(NET_STATE_PACKET_BYTES), Rate * 0.25);
        const uint32_t Now = NowMs();
        if(!InClient.bBudgetStarted)
        {
            InClient.BudgetBytes    = Capacity;
            InClient.bBudgetStarted = true;
        }
        else
        {
            InClient.BudgetBytes = std::min(Capacity, InClient.BudgetBytes + Rate * double(Now - InClient.BudgetAtMs) / 1000.0);
        }
        InClient.BudgetAtMs = Now;
    }

    /* Where InClient looks from: its own entity with a Transform (its
     * avatar), found again only when the last one is gone. Origin if none. */
    EngineCore::Vector3 ViewPointOf(ClientInfo& InClient)
    {
        const auto Usable = [&](const EntityID InEntity)
        {
            if(!InEntity.IsSet() || !BoundWorld->IsAlive(InEntity)) { return false; }
            const GameCore::Entity Handle(BoundWorld, InEntity);
            const NetComponent* Net = Handle.GetComponent<NetComponent>();
            return Net && Net->OwnerClient == InClient.ID && Handle.HasComponent<GameCore::TransformComponent>();
        };
        if(!Usable(InClient.ViewEntity))
        {
            InClient.ViewEntity = EntityID();
            for(const auto& Entry : NetToEntity)
            {
                if(Usable(Entry.second)) { InClient.ViewEntity = Entry.second; break; }
            }
        }
        if(!InClient.ViewEntity.IsSet()) { return EngineCore::Vector3(); }
        return GameCore::Entity(BoundWorld, InClient.ViewEntity).GetWorldPose().Position;
    }

    /* How much an entity's changes want to go to InClient this tick: its
     * Net component's Priority, times the ticks it has waited plus one, times
     * nearness to where the client looks from (1 at its feet, a half at 20 m). */
    double SendScore(const ClientInfo& InClient, const int32_t InNetId, const GameCore::Entity& InHandle, const NetComponent* InNet,
                     const EngineCore::Vector3& InView) const
    {
        const double Priority = InNet ? std::max(0.01, double(InNet->Priority)) : 1.0;
        const auto   Waited   = InClient.Waiting.find(InNetId);
        const double Age      = 1.0 + (Waited != InClient.Waiting.end() ? double(Waited->second) : 0.0);
        double Near = 1.0;
        if(InHandle.HasComponent<GameCore::TransformComponent>())
        {
            const EngineCore::Vector3 Delta = InHandle.GetWorldPose().Position - InView;
            Near = 1.0 / (1.0 + double(Delta.GetLength()) / 2000.0);
        }
        return Priority * Age * Near;
    }

    /* This tick's state for one client, against what it has acknowledged:
     * one batch or more (split between entities, never within one, so an
     * entity's fields always arrive together), and at least one, entries or
     * not, so the client's clock keeps time. A client in step pays for what
     * changed and what it has not acknowledged, not for the whole World. */
    void SendBatches(const NetPeerID InPeer, ClientInfo& InClient, const uint32_t InNowMs)
    {
        struct Item
        {
            const TickItem* Tick;
            uint32_t        Mask;      // what is sent
            uint32_t        Changed;   // of those, what changed since last sent
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

        /* What to look at: everything, on a client's first pass; else this
         * tick's changes, this client's unacknowledged components, and what
         * the budget held back last time -- the rest it has, as they are. */
        std::vector<const TickItem*> Candidates;
        std::deque<TickItem>         Resolved;   // not changed this tick: found here, held stable
        if(InClient.bNeedsFullPass)
        {
            Candidates.reserve(TickItems.size());
            for(const TickItem& Tick : TickItems) { Candidates.push_back(&Tick); }
            InClient.bNeedsFullPass = false;
            InClient.DeferredKeys.clear();
        }
        else
        {
            for(const size_t Index : TickChanged) { Candidates.push_back(&TickItems[Index]); }
            /* A held-back one is diffed again in full against the baseline: it
             * may not have changed since, but the client still lacks it. */
            const auto Resolve = [&](std::unordered_set<uint64_t>& InKeys, const bool bInForceDiff, const std::unordered_set<uint64_t>* InSkip)
            {
                for(auto It = InKeys.begin(); It != InKeys.end();)
                {
                    const uint64_t Key = *It;
                    if(TickChangedKeys.count(Key) != 0 || (InSkip && InSkip->count(Key) != 0)) { ++It; continue; }
                    const int32_t         NetId  = int32_t(uint32_t(Key >> 16));
                    const ComponentTypeID Type   = ComponentTypeID(Key & 0xFFFFu);
                    const EntityID        Entity = FromNetId(NetId);
                    const bool  bReplicates = size_t(Type) < RepFields.size() && RepFields[Type] != nullptr;
                    const GameCore::ReplicatedFieldEntry* Fields = bReplicates ? RepFields[Type] : nullptr;
                    const size_t FieldCount = bReplicates ? RepCounts[Type] : 0;
                    const void* Component = Entity.IsSet() && Fields ? GameCore::Entity(BoundWorld, Entity).GetComponentByType(Type) : nullptr;
                    if(!Component) { It = InKeys.erase(It); continue; }   // gone: nothing left to send
                    TickItem Tick;
                    Tick.NetId      = NetId;
                    Tick.Key        = Key;
                    Tick.Type       = Type;
                    Tick.Component  = Component;
                    Tick.Size       = GameCore::GetComponentType(Type)->SizeBytes;
                    Tick.Fields     = Fields;
                    Tick.FieldCount = FieldCount;
                    Tick.bChanged   = bInForceDiff;
                    Resolved.push_back(Tick);
                    Candidates.push_back(&Resolved.back());
                    ++It;
                }
            };
            Resolve(InClient.UnackedKeys, false, nullptr);
            Resolve(InClient.DeferredKeys, true, &InClient.UnackedKeys);
            std::stable_sort(Candidates.begin(), Candidates.end(),
                             [](const TickItem* A, const TickItem* B) { return A->NetId < B->NetId; });
        }

        /* Each entity's entry, ready to go, and how much it wants to. */
        struct Group
        {
            int32_t             NetId = 0;
            std::vector<Item>   Items;
            EngineCore::Archive Bytes = EngineCore::Archive::ForWriting();
            bool                bOwn  = false;   // the client's own: never held back
            double              Score = 0.0;
        };
        std::vector<Group> Groups;
        const bool bBudget = Config.ClientBandwidthKBps != 0;
        const EngineCore::Vector3 View = bBudget ? ViewPointOf(InClient) : EngineCore::Vector3();
        for(size_t First = 0; First < Candidates.size();)
        {
            size_t Last = First + 1;
            while(Last < Candidates.size() && Candidates[Last]->NetId == Candidates[First]->NetId) { ++Last; }
            Group Next;
            Next.NetId = Candidates[First]->NetId;
            for(size_t Index = First; Index < Last; ++Index)
            {
                const TickItem& Tick  = *Candidates[Index];
                const auto      Found = InClient.Baselines.find(Tick.Key);
                const bool      bKnown = Found != InClient.Baselines.end() && Found->second.Sent.size() == Tick.Size;
                const uint32_t  Unacked = bKnown ? Found->second.Unacked : 0u;
                if(bKnown && !Tick.bChanged && Unacked == 0) { InClient.DeferredKeys.erase(Tick.Key); continue; }   // the client has it all

                uint32_t Changed = 0;
                if(!bKnown)
                {
                    Changed = FullMask(Tick.Type);   // new to this client: everything
                }
                else if(Tick.bChanged)
                {
                    for(size_t Field = 0; Field < Tick.FieldCount; ++Field)
                    {
                        if((Tick.Fields[Field].Flags & GameCore::RFF_InitialOnly) == 0
                           && ReplicatedFieldDiffers(Tick.Fields[Field], Tick.Component, Found->second.Sent.data()))
                        {
                            Changed |= 1u << Field;
                        }
                    }
                }
                const uint32_t Mask = Changed | Unacked;
                if(Mask != 0 && WriteStateEntry(Next.Bytes, Tick.NetId, Tick.Type, Tick.Component, Mask))
                {
                    Next.Items.push_back({ &Tick, Mask, Changed });
                }
                else
                {
                    InClient.DeferredKeys.erase(Tick.Key);   // nothing to send after all
                }
            }
            First = Last;
            if(Next.Items.empty()) { continue; }
            if(bBudget)
            {
                const EntityID Entity = FromNetId(Next.NetId);
                const GameCore::Entity Handle(BoundWorld, Entity);
                const NetComponent* Net = Handle.GetComponent<NetComponent>();
                Next.bOwn  = Net && Net->OwnerClient == InClient.ID;
                Next.Score = SendScore(InClient, Next.NetId, Handle, Net, View);
            }
            Groups.push_back(std::move(Next));
        }

        /* Under a budget: the client's own first, then the eager, the long
         * waiting and the near; what does not fit waits for the next tick. */
        if(bBudget)
        {
            std::stable_sort(Groups.begin(), Groups.end(), [](const Group& A, const Group& B)
            {
                return A.bOwn != B.bOwn ? A.bOwn : A.Score > B.Score;
            });
            RefillBudget(InClient);
        }

        uint32_t Deferred = 0;
        for(Group& Each : Groups)
        {
            const size_t Size = Each.Bytes.GetSize();
            if(bBudget && !Each.bOwn && InClient.BudgetBytes < double(Size))
            {
                /* Held back: kept to look at next tick, and a tick older for it. */
                for(const Item& Held : Each.Items) { InClient.DeferredKeys.insert(Held.Tick->Key); }
                ++InClient.Waiting[Each.NetId];
                ++Deferred;
                continue;
            }
            if(bBudget) { InClient.BudgetBytes -= double(Size); }
            InClient.Waiting.erase(Each.NetId);

            /* Full: this entity starts the next batch. */
            if(Count > 0 && Body.size() + Size > NET_STATE_PACKET_BYTES) { Emit(); }

            Body.insert(Body.end(), Each.Bytes.GetBytes().begin(), Each.Bytes.GetBytes().end());
            for(const Item& Sent : Each.Items)
            {
                InClient.DeferredKeys.erase(Sent.Tick->Key);
                Baseline& Base = InClient.Baselines[Sent.Tick->Key];
                const uint8_t* Bytes = static_cast<const uint8_t*>(Sent.Tick->Component);
                Base.Sent.assign(Bytes, Bytes + Sent.Tick->Size);
                for(uint32_t Field = 0; Field < 32; ++Field)
                {
                    if(Sent.Changed & (1u << Field)) { Base.ValueSeq[Field] = Record.Seq; }
                }
                Base.Unacked |= Sent.Mask;
                InClient.UnackedKeys.insert(Sent.Tick->Key);
                Record.Entries.emplace_back(Sent.Tick->Key, Sent.Mask);
                ++Count;
            }
        }
        InClient.LastDeferred = Deferred;
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
                    if(Base.Unacked == 0) { InClient.UnackedKeys.erase(Entry.first); }
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
                Client.second.UnackedKeys.clear();
                Client.second.DeferredKeys.clear();
                Client.second.Waiting.clear();
                Client.second.bNeedsFullPass = true;
            }
            for(auto& Entry : Structures) { for(std::vector<uint8_t>& Shadow : Entry.second.Shadows) { Shadow.clear(); } }
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
            Structure& Known = Structures[InNet.NetId];
            Known.Types  = SerializedTypes(InEntity);
            Known.Parent = ParentGuidOf(InEntity);
            Known.Entity = InEntity.GetID();
            Known.Shadows.assign(Known.Types.size(), std::vector<uint8_t>());
        });
        for(const auto& Entry : Spawned) { Broadcast(BuildSpawn(Entry.first, Entry.second)); }

        /* Then what changed on the ones already sent -- kept current with no one to tell, too. */
        SendStructureChanges();
        if(!HasWelcomedClients()) { return; }

        /* Then each client's state, unreliably. */
        BuildTickItems();
        const uint32_t Now = NowMs();
        for(auto& Client : Clients)
        {
            if(Client.second.bWelcomed) { SendBatches(Client.first, Client.second, Now); }
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
        uint64_t    Rejoin  = Token;
        Ar << Version << Hash << Level << Rejoin;
        SendTo(InPeer, Ar);
        CORE_LOG("NetCore: connected to " + Transport.DescribePeer(InPeer) + (Token ? "; asking to rejoin" : "; saying hello"))
    }

    void HandleDisconnected(const NetPeerID InPeer, const uint32_t InReason)
    {
        if(Role == ENetRole::Server)
        {
            const auto Found = Clients.find(InPeer);
            if(Found == Clients.end()) { return; }
            const ClientInfo Info = Found->second;
            Clients.erase(Found);
            if(!Info.bWelcomed) { return; }

            /* Lost, not quit or kicked: keep its place for a while. */
            if(Config.ReconnectGraceMs > 0 && InReason == NET_DISCONNECT_LOST && !Info.bKicked)
            {
                Away.push_back({ Info.ID, Info.Token, NowMs() + Config.ReconnectGraceMs });
                char Grace[32];
                std::snprintf(Grace, sizeof(Grace), "%.1f s", double(Config.ReconnectGraceMs) / 1000.0);
                CORE_LOG_WARN("NetCore: lost client " + std::to_string(Info.ID) + "; keeping its place for " + Grace)
                if(OnAway) { OnAway(Info.ID, EventUserData); }
                return;
            }
            CORE_LOG("NetCore: client " + std::to_string(Info.ID) + (InReason == NET_DISCONNECT_QUIT ? " left" : " disconnected"))
            if(OnDisconnected) { OnDisconnected(Info.ID, EventUserData); }
            return;
        }

        if(InPeer != ServerPeer) { return; }
        const bool bWasWelcomed = bWelcomed;
        bWelcomed  = false;
        ServerPeer = NET_PEER_NONE;

        const bool bMayReturn = InReason == NET_DISCONNECT_LOST && Config.ReconnectGraceMs > 0 && Token != 0;
        if(bWasWelcomed && bMayReturn)
        {
            /* Its entities, and its id, stay while it tries: its own still read as its own. */
            bReconnecting = true;
            LostAtMs      = NowMs();
            NextAttemptMs = LostAtMs;
            char Grace[32];
            std::snprintf(Grace, sizeof(Grace), "%.1f s", double(Config.ReconnectGraceMs) / 1000.0);
            CORE_LOG_WARN(std::string("NetCore: lost the connection to the server; trying to rejoin for ") + Grace)
            return;
        }
        if(bReconnecting && bMayReturn)
        {
            NextAttemptMs = NowMs() + 1000;   // that attempt failed; another in a second
            return;
        }
        if(bReconnecting)
        {
            CORE_LOG_WARN("NetCore: the server would not take this client back")
            bReconnecting = false;
        }
        else
        {
            CORE_LOG_WARN(InReason == NET_DISCONNECT_CLOSED ? "NetCore: the server shut down"
                          : bWasWelcomed                     ? "NetCore: lost the connection to the server"
                                                             : "NetCore: could not connect to the server")
        }
        LocalClient = NET_CLIENT_NONE;
    }

    /* The server's clock: its own, or a client's estimate of it. 0 for a
     * client before its first batch. */
    uint32_t GetTimeMs() const
    {
        if(Role == ENetRole::Server) { return NowMs(); }
        if(!bHaveServerTime) { return 0; }
        const double Server = double(NowMs()) - ServerOffsetMs;
        return Server > 1.0 ? uint32_t(Server) : 1u;
    }

    /* What an RPC this process sends says it was showing: a client's render
     * time on the server's clock (Interpolate's), the server's own now. */
    uint32_t GetViewTimeMs() const
    {
        if(Role == ENetRole::Server) { return NowMs(); }
        if(!bHaveServerTime) { return 0; }
        const double Render = double(NowMs()) - ServerOffsetMs - double(Config.InterpolationDelayMs);
        return Render > 1.0 ? uint32_t(Render) : 1u;
    }

    /* A server: places kept past their grace are given up. */
    void ExpireAway()
    {
        const uint32_t Now = NowMs();
        for(size_t Index = 0; Index < Away.size();)
        {
            if(int32_t(Now - Away[Index].ExpiresMs) < 0) { ++Index; continue; }
            const NetClientID ID = Away[Index].ID;
            Away.erase(Away.begin() + std::ptrdiff_t(Index));
            CORE_LOG("NetCore: client " + std::to_string(ID) + " did not come back; gone")
            if(OnDisconnected) { OnDisconnected(ID, EventUserData); }
        }
    }

    /* A client: the next attempt to rejoin, or giving up once the grace is over. */
    void TickReconnect()
    {
        if(!bReconnecting) { return; }
        const uint32_t Now = NowMs();
        if(Now - LostAtMs > Config.ReconnectGraceMs)
        {
            CORE_LOG_WARN("NetCore: could not rejoin the server in time; giving up")
            if(ServerPeer != NET_PEER_NONE) { Transport.Close(NET_DISCONNECT_QUIT); }
            ServerPeer    = NET_PEER_NONE;
            bReconnecting = false;
            LocalClient   = NET_CLIENT_NONE;
            return;
        }
        if(ServerPeer != NET_PEER_NONE || int32_t(Now - NextAttemptMs) < 0) { return; }
        NextAttemptMs = Now + 1000;
        if(!Transport.Connect(Config.ServerAddress, Config.Port, Config.TimeoutMs, ServerPeer)) { ServerPeer = NET_PEER_NONE; }
    }

    static uint64_t NewToken()
    {
        static std::mt19937_64 Random(uint64_t(std::random_device{}()) ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
        uint64_t Token = 0;
        while(Token == 0) { Token = Random(); }
        return Token;
    }

    /* A client welcomed as a new one into a World an earlier session left
     * entities in: what that session spawned goes, the level's own stay
     * unbound, ready to be matched again. */
    void ForgetSession()
    {
        std::vector<std::pair<int32_t, EntityID>> Mapped(NetToEntity.begin(), NetToEntity.end());
        for(const auto& Entry : Mapped)
        {
            if(!BoundWorld->IsAlive(Entry.second)) { continue; }
            if(CreatedHere.count(Entry.second.Pack()) != 0) { BoundWorld->DestroyEntity(Entry.second); continue; }
            if(NetComponent* Net = GameCore::Entity(BoundWorld, Entry.second).GetComponent<NetComponent>()) { Net->NetId = 0; }
        }
        NetToEntity.clear();
        EntityToNet.clear();
        Structures.clear();
        Interpolations.clear();
        PendingParents.clear();
        CreatedHere.clear();
        ServerValues.clear();
        FieldSmooths.clear();
    }

    /* SnapshotDone: whatever the snapshot did not send again is gone on the server. */
    void ClientSnapshotDone()
    {
        if(!bAwaitingSnapshot || !BoundWorld) { return; }
        bAwaitingSnapshot = false;
        const std::vector<int32_t> Gone(Unconfirmed.begin(), Unconfirmed.end());
        Unconfirmed.clear();
        for(const int32_t NetId : Gone)
        {
            const EntityID Entity = FromNetId(NetId);
            UnmapEntity(NetId);
            CreatedHere.erase(Entity.Pack());
            if(Entity.IsSet() && BoundWorld->IsAlive(Entity)) { BoundWorld->DestroyEntity(Entity); }
        }
        if(!Gone.empty()) { CORE_LOG("NetCore: rejoined; " + std::to_string(Gone.size()) + " entities gone while away removed") }
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
        uint64_t Rejoin = 0;
        Ar << Rejoin;
        if(Ar.HasError()) { return; }
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
        /* A token the server knows: that client, back -- from its kept place,
         * or over a connection the server has not yet noticed is dead. */
        NetClientID Returning = NET_CLIENT_NONE;
        if(Rejoin != 0)
        {
            const auto Kept = std::find_if(Away.begin(), Away.end(), [Rejoin](const AwayClient& InAway) { return InAway.Token == Rejoin; });
            if(Kept != Away.end())
            {
                Returning = Kept->ID;
                Away.erase(Kept);
            }
            else
            {
                for(auto It = Clients.begin(); It != Clients.end(); ++It)
                {
                    if(It->first == InPeer || !It->second.bWelcomed || It->second.Token != Rejoin) { continue; }
                    Returning = It->second.ID;
                    const NetPeerID Stale = It->first;
                    Clients.erase(It);   // its Disconnected, later, finds nothing
                    Transport.Disconnect(Stale, NET_DISCONNECT_REJECTED);
                    break;
                }
                Found = Clients.find(InPeer);
            }
        }
        if(Returning == NET_CLIENT_NONE && Config.MaxClients != 0 && GetWelcomedCount() + Away.size() >= Config.MaxClients)
        {
            Reject(InPeer, "the server is full");
            return;
        }

        const bool bReturning   = Returning != NET_CLIENT_NONE;
        Found->second.ID        = bReturning ? Returning : NextClientID++;
        Found->second.Token     = bReturning ? Rejoin : NewToken();
        Found->second.bWelcomed = true;

        EngineCore::Archive Welcome = BeginMessage(ENetMessage::Welcome);
        int32_t  ID    = Found->second.ID;
        uint64_t Given = Found->second.Token;
        uint8_t  bBack = bReturning ? 1 : 0;
        Welcome << ID << Given << bBack;
        SendTo(InPeer, Welcome);
        CORE_LOG("NetCore: " + Transport.DescribePeer(InPeer) + " is client " + std::to_string(ID) + (bReturning ? ", back" : ""))

        SendSnapshot(InPeer);
        SendTo(InPeer, BeginMessage(ENetMessage::SnapshotDone));
        if(bReturning) { if(OnReturned) { OnReturned(ID, EventUserData); } }
        else if(OnConnected) { OnConnected(ID, EventUserData); }
    }

    size_t GetWelcomedCount() const
    {
        size_t Count = 0;
        for(const auto& Entry : Clients) { if(Entry.second.bWelcomed) { ++Count; } }
        return Count;
    }

    /* Welcomed: as a new client, or back as the one this token named. A
     * session before this one in the same World leaves entities: a new
     * client drops what it spawned; one back keeps everything until the
     * snapshot says what is still there. */
    void ClientWelcome(const int32_t InId, const uint64_t InToken, const bool bInReturning)
    {
        if(bHadSession && BoundWorld)
        {
            if(!bInReturning) { ForgetSession(); }
            ResetBatches();
            Interpolations.clear();
            FieldSmooths.clear();
            PendingParents.clear();
            bHaveServerTime = false;
            Unconfirmed.clear();
            for(const auto& Entry : NetToEntity) { Unconfirmed.insert(Entry.first); }
            bAwaitingSnapshot = true;
        }
        bWelcomed     = true;
        bReconnecting = false;
        LocalClient   = InId;
        Token         = InToken;
        bHadSession   = BoundWorld != nullptr;
        CORE_LOG(std::string("NetCore: the server ") + (bInReturning ? "took this process back" : "welcomed this process")
                 + " as client " + std::to_string(InId))
    }

    /* InEntity's replicated components as they are now -- what the server just sent -- into ServerValues. */
    void RememberServerValues(const int32_t InNetId, const EntityID InEntity)
    {
        const auto Known = Structures.find(InNetId);
        if(Known == Structures.end() || !BoundWorld->IsAlive(InEntity)) { return; }
        const GameCore::Entity Handle(BoundWorld, InEntity);
        for(const ComponentTypeID Type : Known->second.Types)
        {
            size_t FieldCount = 0;
            if(!GameCore::FindComponentReplication(Type, FieldCount) || FieldCount == 0) { continue; }
            const void* Component = Handle.GetComponentByType(Type);
            if(!Component) { continue; }
            const uint8_t* Bytes = static_cast<const uint8_t*>(Component);
            ServerValues[ShadowKey(InNetId, Type)].assign(Bytes, Bytes + GameCore::GetComponentType(Type)->SizeBytes);
        }
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
        if(!Entity.IsSet())
        {
            Entity = BoundWorld->CreateEntity(Name.c_str(), Guid).GetID();
            CreatedHere.insert(Entity.Pack());
        }
        MapEntity(NetId, Entity);
        Unconfirmed.erase(NetId);
        GameCore::Entity Handle(BoundWorld, Entity);
        Structures[NetId] = Structure();
        ReadComponents(Ar, Handle, NetId, Count);

        /* Its id, whatever the component data said. */
        NetComponent* Net = Handle.GetComponent<NetComponent>();
        if(!Net) { Net = Handle.AddComponent<NetComponent>(); }
        if(Net) { Net->NetId = NetId; }

        if(ParentGuid.IsSet()) { PendingParents.push_back({ Entity, ParentGuid }); }
        ResolveParents();
        RememberServerValues(NetId, Entity);

        /* Where it appeared is the first of its Transform's samples. */
        if(IsInterpolated(Entity))
        {
            Interpolation& Smooth = Interpolations[NetId];
            Smooth.Samples.clear();
            Smooth.Target = *Handle.GetComponent<GameCore::TransformComponent>();
            PushSample(Smooth);
        }
    }

    /* InCount components, written by WriteComponents after its count: each
     * read over the entity's own, or added. Every type named is recorded as
     * told, read or not, so state for it is never waited on. */
    void ReadComponents(EngineCore::Archive& Ar, const GameCore::Entity& InHandle, const int32_t InNetId, const uint16_t InCount)
    {
        std::vector<ComponentTypeID>& Told = Structures[InNetId].Types;
        for(uint16_t Index = 0; Index < InCount && !Ar.HasError(); ++Index)
        {
            std::string TypeName;
            Ar << TypeName;
            const GameCore::ComponentTypeInfo* Info = GameCore::FindComponentType(TypeName.c_str());
            if(Info && std::find(Told.begin(), Told.end(), Info->ID) == Told.end()) { Told.push_back(Info->ID); }
            if(!Info || !GameCore::FindComponentSerializer(Info->ID))
            {
                /* A type this build lacks: skip its chunk. */
                uint16_t Version = 0;
                if(Ar.OpenChunk(EngineCore::MakeArchiveTag('D', 'A', 'T', 'A'), Version)) { Ar.CloseChunk(); }
                continue;
            }

            if(void* Live = InHandle.GetComponentByType(Info->ID))
            {
                ReadComponentData(Ar, Info->ID, Live, *this);
            }
            else
            {
                std::vector<uint8_t> Bytes(Info->SizeBytes, 0);
                const GameCore::ComponentSerializerInfo* Serializer = GameCore::FindComponentSerializer(Info->ID);
                if(Serializer->DefaultsFn) { Serializer->DefaultsFn(Bytes.data()); }
                if(ReadComponentData(Ar, Info->ID, Bytes.data(), *this)) { InHandle.AddComponentByType(Info->ID, Bytes.data()); }
            }
        }
    }

    /* Components a sent entity gained. One already destroyed here needs nothing. */
    void ClientAddComponents(EngineCore::Archive& Ar)
    {
        int32_t  NetId = 0;
        uint16_t Count = 0;
        Ar << NetId << Count;
        if(Ar.HasError() || !BoundWorld) { return; }
        const EntityID Entity = FromNetId(NetId);
        if(!Entity.IsSet() || !BoundWorld->IsAlive(Entity)) { return; }
        ReadComponents(Ar, GameCore::Entity(BoundWorld, Entity), NetId, Count);
        RememberServerValues(NetId, Entity);
    }

    void ClientRemoveComponents(EngineCore::Archive& Ar)
    {
        int32_t  NetId = 0;
        uint16_t Count = 0;
        Ar << NetId << Count;
        if(Ar.HasError() || !BoundWorld) { return; }
        const EntityID         Entity = FromNetId(NetId);
        const GameCore::Entity Handle(BoundWorld, Entity);
        for(uint16_t Index = 0; Index < Count && !Ar.HasError(); ++Index)
        {
            std::string TypeName;
            Ar << TypeName;
            const GameCore::ComponentTypeInfo* Info = GameCore::FindComponentType(TypeName.c_str());
            if(!Info || !Entity.IsSet()) { continue; }

            const auto Found = Structures.find(NetId);
            if(Found != Structures.end())
            {
                std::vector<ComponentTypeID>& Told = Found->second.Types;
                Told.erase(std::remove(Told.begin(), Told.end(), Info->ID), Told.end());
            }
            ServerValues.erase(ShadowKey(NetId, Info->ID));
            ForgetFieldSmooths(NetId, Info->ID);
            if(BoundWorld->IsAlive(Entity)) { Handle.RemoveComponentByType(Info->ID); }
        }
    }

    void ClientReparent(EngineCore::Archive& Ar)
    {
        int32_t              NetId = 0;
        GameCore::EntityGuid ParentGuid;
        Ar << NetId << ParentGuid.Hi << ParentGuid.Lo;
        if(Ar.HasError() || !BoundWorld) { return; }
        const EntityID Entity = FromNetId(NetId);
        if(!Entity.IsSet() || !BoundWorld->IsAlive(Entity)) { return; }

        /* Replaces a link still waiting on its parent. */
        PendingParents.erase(std::remove_if(PendingParents.begin(), PendingParents.end(),
                                            [&](const PendingParent& InPending) { return InPending.Child == Entity; }),
                             PendingParents.end());

        /* The Transform is local, and the server's own follows: no keeping the world one. */
        if(ParentGuid.IsSet())
        {
            PendingParents.push_back({ Entity, ParentGuid });
            ResolveParents();
        }
        else
        {
            GameCore::Entity(BoundWorld, Entity).ClearParent(false);
        }

        /* Samples under the old parent mean something else under the new one. */
        const auto Smooth = Interpolations.find(NetId);
        if(Smooth != Interpolations.end()) { Smooth->second.Samples.clear(); }
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
        Unconfirmed.erase(NetId);
        CreatedHere.erase(Entity.Pack());
        DestroyedIds.insert(NetId);
        if(Entity.IsSet() && BoundWorld->IsAlive(Entity)) { BoundWorld->DestroyEntity(Entity); }
    }

    /* One entry of a StateBatch. Always read through, so the next one
     * parses; false if it is for an entity whose spawn has not arrived yet
     * (the batch is then not acknowledged, and the server sends it again). */
    bool ClientState(EngineCore::Archive& Ar)
    {
        const uint64_t NetId64 = ReadVarUInt(Ar);
        const uint64_t Wire64  = ReadVarUInt(Ar);
        const uint64_t Mask64  = ReadVarUInt(Ar);
        if(Ar.HasError()) { return true; }
        if(NetId64 > 0x7FFFFFFFu || Wire64 > 0xFFFFu || Mask64 > 0xFFFFFFFFu) { Ar.SetError("a state entry's header is out of range"); return true; }
        const int32_t  NetId    = int32_t(NetId64);
        const uint16_t WireType = uint16_t(Wire64);
        const uint32_t Mask     = uint32_t(Mask64);

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
            /* Not here: read it past. Whether that is done with is decided
             * below, once it is read. */
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

        /* Otherwise into the server's whole word first (ServerValues), then onto the component. */
        std::vector<uint8_t>* Server = nullptr;
        if(Component && !Smooth)
        {
            const size_t Size = GameCore::GetComponentType(Type)->SizeBytes;
            Server = &ServerValues[ShadowKey(NetId, Type)];
            if(Server->size() != Size) { Server->assign(static_cast<const uint8_t*>(Component), static_cast<const uint8_t*>(Component) + Size); }
            Into = Server->data();
        }

        for(size_t Field = 0; Field < FieldCount && !Ar.HasError(); ++Field)
        {
            if(Mask & (1u << Field)) { ReadReplicatedField(Ar, Fields[Field], Into, *this); }
        }
        if(Ar.HasError()) { return true; }

        /* What changed goes onto the component -- unless this client
         * predicts it: then all of the server's state goes on together, every
         * component, just before the replay (FireCorrections). */
        if(Server && !IsPredictedHere(Entity))
        {
            const bool bSmoothing = Config.InterpolationDelayMs > 0;
            for(size_t Field = 0; Field < FieldCount; ++Field)
            {
                if((Mask & (1u << Field)) == 0) { continue; }
                /* A Smooth() field goes into its samples; Interpolate() writes it. Its first is written at once. */
                if(bSmoothing && (Fields[Field].Flags & GameCore::RFF_Smooth) != 0)
                {
                    const uint64_t Key = FieldKey(NetId, Type, Field);
                    const bool bFirst = FieldSmooths.find(Key) == FieldSmooths.end();
                    float Value[3] = {};
                    ReadFloats(Fields[Field], Server->data(), Value);
                    PushFieldSample(Key, Fields[Field], Value);
                    if(!bFirst) { continue; }
                }
                std::memcpy(static_cast<char*>(Component) + Fields[Field].Offset, Server->data() + Fields[Field].Offset, Fields[Field].Size);
            }
        }
        if(!Component)
        {
            if(!Entity.IsSet()) { return DestroyedIds.count(NetId) != 0; }

            /* Told of and gone: removed here, done with. Not told of: its
             * AddComponents is still on the way, so wait for it. */
            const auto Known = Structures.find(NetId);
            return Known != Structures.end()
                && std::find(Known->second.Types.begin(), Known->second.Types.end(), Type) != Known->second.Types.end();
        }

        if(Smooth) { PushSample(*Smooth); }

        /* The server's word on something this client predicts -- any of its
         * components, since each is now the server's state, not the
         * prediction: the game replays from there once the poll is done. */
        if(IsPredictedHere(Entity))
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
        uint32_t             ViewTime = 0;
        std::vector<uint8_t> Args;
        uint32_t             Size     = 0;
        Ar << NetId << WireType << Index << ViewTime << Size;
        if(Ar.HasError() || Size > Ar.GetSize() - Ar.GetCursor()) { Ar.SetError("an RPC's arguments run past the packet"); return; }
        if(Role == ENetRole::Server && Config.MaxRpcBytes != 0 && Size > Config.MaxRpcBytes)
        {
            Ar.SetError("an RPC's arguments are longer than [Net] MaxRpcBytes");
            return;
        }

        /* Every call a client makes costs it a token, whatever it names. */
        ClientInfo* From = nullptr;
        if(Role == ENetRole::Server)
        {
            const auto Found = Clients.find(InPeer);
            if(Found == Clients.end() || !Found->second.bWelcomed || Found->second.bKicked) { return; }
            From = &Found->second;
            if(Config.RpcPerSecond != 0)
            {
                const double Rate = double(Config.RpcPerSecond);
                if(!TakeRpcToken(InPeer, *From, From->RpcBudget, Rate, Rate * 2.0, nullptr)) { return; }
            }
        }
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
            Sender = From->ID;
            if(Rpc.MaxPerSecond > 0.0f)
            {
                const double Rate = double(Rpc.MaxPerSecond);
                Bucket& Limit = From->RpcLimits[(uint32_t(WireType) << 8) | Rpc.Index];
                if(!TakeRpcToken(InPeer, *From, Limit, Rate, std::max(1.0, Rate), Rpc.Name))
                {
                    return;
                }
            }

            /* A client may only ask for Server RPCs, and only on what it owns. */
            const NetComponent* Net = GameCore::Entity(BoundWorld, Entity).GetComponent<NetComponent>();
            if(Rpc.Target != GameCore::ERpcTarget::Server || !Net || Net->OwnerClient != Sender)
            {
                /* Once a second at most: a client could otherwise fill the log within its budget. */
                const uint32_t Second = NowMs() / 1000u;
                if(Second != From->RefusedLogSecond)
                {
                    From->RefusedLogSecond = Second;
                    CORE_LOG_WARN("NetCore: client " + std::to_string(Sender) + " asked for " + Rpc.Name
                                  + " on an entity it does not own, or that is not a Server RPC; refused")
                }
                return;
            }
        }
        else if(Rpc.Target == GameCore::ERpcTarget::Server)
        {
            return;   // the server never sends those
        }

        /* A client's view of the past can be no later than now, nor older than a second. */
        if(Role == ENetRole::Server && ViewTime != 0)
        {
            const uint32_t Now = NowMs();
            if(int32_t(ViewTime - Now) > 0)   { ViewTime = Now; }
            if(Now - ViewTime > 1000u)        { ViewTime = Now - 1000u; }
        }
        bDecodingRemote = true;
        GameCore::InvokeRpc(*BoundWorld, Entity, Type, Rpc, Args, Sender, true, Role == ENetRole::Server ? ViewTime : 0u);
        bDecodingRemote = false;
    }

    /* One of a client's Server RPCs against a bucket: its overall budget
     * (InRpc null), or InRpc's own RateLimit. False drops the call; a
     * client that drops more than RpcFloodPerSecond in a second is
     * disconnected. */
    bool TakeRpcToken(const NetPeerID InPeer, ClientInfo& InClient, Bucket& InBucket, const double InPerSecond,
                      const double InCapacity, const char* InRpcName)
    {
        const double Now = double(NowMs());
        if(InBucket.Take(InPerSecond, InCapacity, Now)) { return true; }

        const uint32_t Second = uint32_t(Now / 1000.0);
        if(Second != InClient.DroppedSecond)
        {
            if(InClient.DroppedThisSecond > 1)
            {
                CORE_LOG_WARN("NetCore: client " + std::to_string(InClient.ID) + ": "
                              + std::to_string(InClient.DroppedThisSecond) + " RPCs dropped over its limits in a second")
            }
            InClient.DroppedSecond     = Second;
            InClient.DroppedThisSecond = 0;
        }
        if(++InClient.DroppedThisSecond == 1)
        {
            CORE_LOG_WARN("NetCore: client " + std::to_string(InClient.ID) + ": "
                          + (InRpcName ? std::string(InRpcName) + " over its RateLimit" : std::string("over [Net] RpcPerSecond"))
                          + "; dropping its RPCs")
        }
        ++InClient.DroppedTotal;
        if(Config.RpcFloodPerSecond != 0 && InClient.DroppedThisSecond > Config.RpcFloodPerSecond && !InClient.bKicked)
        {
            InClient.bKicked = true;
            Reject(InPeer, "it sent RPCs far over its limit ([Net] RpcFloodPerSecond)");
        }
        return false;
    }

    void HandleReceived(const NetPeerID InPeer, const uint8_t InChannel, const std::vector<uint8_t>& InData)
    {
        if(InChannel != NET_CHANNEL_GAME && InChannel != NET_CHANNEL_STATE && InChannel != NET_CHANNEL_UNRELIABLE) { return; }

        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InData);
        uint8_t Type = 0;
        Ar << Type;

        /* State and its acks go unreliably on their channel; an Unreliable RPC on its own; everything else
         * reliably on the game one. */
        const bool bStateMessage = ENetMessage(Type) == ENetMessage::StateBatch || ENetMessage(Type) == ENetMessage::StateAck;
        const bool bRightChannel = InChannel == NET_CHANNEL_STATE      ? bStateMessage
                                 : InChannel == NET_CHANNEL_UNRELIABLE ? ENetMessage(Type) == ENetMessage::Rpc
                                                                       : !bStateMessage;
        if(!bRightChannel) { Ar.SetError("a message on the wrong channel"); }

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
                    int32_t  ID    = NET_CLIENT_NONE;
                    uint64_t Given = 0;
                    uint8_t  bBack = 0;
                    Ar << ID << Given << bBack;
                    if(!Ar.HasError()) { ClientWelcome(ID, Given, bBack != 0); }
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
                case ENetMessage::AddComponents:    ClientAddComponents(Ar);    break;
                case ENetMessage::RemoveComponents: ClientRemoveComponents(Ar); break;
                case ENetMessage::Reparent:         ClientReparent(Ar);         break;
                case ENetMessage::SnapshotDone:     ClientSnapshotDone();       break;
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

    /* The server's cue to look at a networked entity's components in the next flush. */
    static void OnComponentChanged(const EntityID InEntity, const ComponentTypeID, void* InUserData)
    {
        Impl& Self = *static_cast<Impl*>(InUserData);
        if(Self.Role == ENetRole::Server) { Self.StructureDirty.insert(InEntity.Pack()); }
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
        uint32_t ViewTime = GetViewTimeMs();
        uint32_t Size     = uint32_t(InArgs.size());
        Ar << NetId << WireType << Index << ViewTime << Size;
        if(Size > 0)
        {
            std::vector<uint8_t> Copy = InArgs;
            Ar.SerializeBytes(Copy.data(), Size);
        }
        if(Ar.HasError()) { return; }
        if(InRpc.bUnreliable)
        {
            /* Once, on its own channel, lost as state is under SimulatedLossPercent. */
            const auto Send = [&](const NetPeerID InTo)
            {
                if(!SimulateLoss()) { Transport.Send(InTo, NET_CHANNEL_UNRELIABLE, Ar.GetBytes(), false, true); }
            };
            if(!bInBroadcast) { Send(InPeer); return; }
            for(const auto& Entry : Clients) { if(Entry.second.bWelcomed) { Send(Entry.first); } }
            return;
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
        if(!bWelcomed && bReconnecting) { return; }   // its input, while it rejoins: nowhere to go
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
    P->Token  = InRole == ENetRole::Client ? InConfig.ReconnectToken : 0;

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

    P->Transport.Close(P->Role == ENetRole::Server ? NET_DISCONNECT_CLOSED : NET_DISCONNECT_QUIT);
    P->Clients.clear();
    P->Away.clear();
    P->bReconnecting = false;
    P->Token         = 0;
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
bool        NetSession::IsReconnecting() const   { return P->bReconnecting; }
uint32_t    NetSession::GetTimeMs() const        { return P->GetTimeMs(); }
uint64_t    NetSession::GetReconnectToken() const { return P->Token; }
uint16_t    NetSession::GetPort() const          { return P->Transport.GetPort(); }
size_t      NetSession::GetClientCount() const   { return P->GetWelcomedCount(); }

bool NetSession::GetServerStats(NetPeerStats& OutStats) const
{
    return P->Role == ENetRole::Client && P->ServerPeer != NET_PEER_NONE && P->Transport.GetPeerStats(P->ServerPeer, OutStats);
}

bool NetSession::GetClientStats(const NetClientID InClient, NetPeerStats& OutStats) const
{
    if(P->Role != ENetRole::Server) { return false; }
    const NetPeerID Peer = P->PeerOfClient(InClient);
    if(Peer == NET_PEER_NONE || !P->Transport.GetPeerStats(Peer, OutStats)) { return false; }
    const auto Found = P->Clients.find(Peer);
    if(Found != P->Clients.end()) { OutStats.StateDeferred = Found->second.LastDeferred; }
    return true;
}

void NetSession::BindWorld(GameCore::World& InWorld, const char* InLevelName)
{
    UnbindWorld();
    P->BoundWorld      = &InWorld;
    P->LevelName       = InLevelName ? InLevelName : "";
    P->DestroyedHandle        = InWorld.OnEntityDestroyed().Add(&Impl::OnEntityDestroyed, P);
    P->ComponentAddedHandle   = InWorld.OnComponentAdded().Add(&Impl::OnComponentChanged, P);
    P->ComponentRemovedHandle = InWorld.OnComponentRemoved().Add(&Impl::OnComponentChanged, P);
}

void NetSession::UnbindWorld()
{
    if(!P->BoundWorld) { return; }
    P->BoundWorld->OnEntityDestroyed().Remove(P->DestroyedHandle);
    P->BoundWorld->OnComponentAdded().Remove(P->ComponentAddedHandle);
    P->BoundWorld->OnComponentRemoved().Remove(P->ComponentRemovedHandle);
    P->DestroyedHandle        = GameCore::DelegateHandle();
    P->ComponentAddedHandle   = GameCore::DelegateHandle();
    P->ComponentRemovedHandle = GameCore::DelegateHandle();
    P->BoundWorld = nullptr;
    P->NetToEntity.clear();
    P->EntityToNet.clear();
    P->Structures.clear();
    P->StructureDirty.clear();
    P->TickItems.clear();
    P->TickEntities.clear();
    for(auto& Client : P->Clients)
    {
        Client.second.Baselines.clear();
        Client.second.InFlight.clear();
        Client.second.UnackedKeys.clear();
        Client.second.DeferredKeys.clear();
        Client.second.Waiting.clear();
        Client.second.bNeedsFullPass = true;
    }
    P->ResetBatches();
    P->PendingParents.clear();
    P->Interpolations.clear();
    P->Corrected.clear();
    P->bHadSession       = false;
    P->bAwaitingSnapshot = false;
    P->ServerValues.clear();
    P->FieldSmooths.clear();
    P->Unconfirmed.clear();
    P->CreatedHere.clear();
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
            case NetTransportEvent::EType::Disconnected: P->HandleDisconnected(Event.Peer, Event.Reason); break;
            case NetTransportEvent::EType::Received:     P->HandleReceived(Event.Peer, Event.Channel, Event.Data); break;
        }
    }

    if(P->Role == ENetRole::Server)      { P->ExpireAway(); }
    else if(P->Role == ENetRole::Client) { P->TickReconnect(); }

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

void NetSession::SetClientAwayEvents(const ClientEventFn InAway, const ClientEventFn InReturned)
{
    P->OnAway     = InAway;
    P->OnReturned = InReturned;
}

void NetSession::SetCorrectionHandler(const CorrectionFn InFn, void* InUserData)
{
    P->OnCorrection       = InFn;
    P->CorrectionUserData = InUserData;
}

NetSession* NetSession::GetActive() { return GStarted.empty() ? nullptr : GStarted.front()->Self; }
}
