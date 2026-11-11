#include "Public/GameCore/Replay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreLog.h"
#include "Public/GameCore/World.h"

/* State replays (Replay.h): the recorder's diff, the clip's file chunk, and
 * the player's rebuild-from-keyframe plus forward deltas. */

namespace GameCore
{
namespace
{
    constexpr uint32_t TAG_REPLAY = EngineCore::MakeArchiveTag('R','P','L','Y');
    constexpr uint32_t TAG_DATA   = EngineCore::MakeArchiveTag('D','A','T','A');

    constexpr uint16_t REPLAY_CHUNK_VERSION = 1;

    constexpr uint16_t NO_TYPE = 0xFFFF;

    /* Entity references inside a component, as GUIDs: written from the
     * recording World, read back through the player's bindings (or the
     * target World's own GUIDs for an entity the clip never recorded). */
    class GuidRemapper final : public IEntityRemapper
    {
    public:
        GuidRemapper(const World& InWorld, const std::unordered_map<EntityGuid, EntityID, EntityGuidHash>* InBound)
            : TheWorld(InWorld), Bound(InBound) {}

        void RemapEntity(EngineCore::Archive& Ar, EntityID& InOut) override
        {
            EntityGuid Guid;
            if(Ar.IsSaving() && InOut.IsSet() && TheWorld.IsAlive(InOut))
            {
                Guid = const_cast<World&>(TheWorld).GetEntity(InOut).GetGuid();
            }
            Ar << Guid.Hi << Guid.Lo;
            if(!Ar.IsLoading() || Ar.HasError()) { return; }
            InOut = Resolve(Guid);
        }

        EntityID Resolve(const EntityGuid& InGuid) const
        {
            if(!InGuid.IsSet()) { return InvalidEntityID(); }
            if(Bound)
            {
                const auto Found = Bound->find(InGuid);
                if(Found != Bound->end() && TheWorld.IsAlive(Found->second)) { return Found->second; }
            }
            return TheWorld.FindEntityByGuid(InGuid);
        }

    private:
        const World& TheWorld;
        const std::unordered_map<EntityGuid, EntityID, EntityGuidHash>* Bound;
    };

    std::vector<uint8_t> EncodeComponent(const World& InWorld, const ComponentSerializerInfo& InSerializer, void* InComponent)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        uint16_t Version = InSerializer.Version;
        Ar.OpenChunk(TAG_DATA, Version);
        GuidRemapper Remap(InWorld, nullptr);
        BinaryComponentArchive ComponentAr(Ar, Remap);
        InSerializer.Fn(ComponentAr, InComponent);
        Ar.CloseChunk();
        return Ar.GetBytes();
    }

    /* Into live storage: only the serialized fields are written. */
    bool DecodeComponent(GuidRemapper& InRemap, const ComponentSerializerInfo& InSerializer,
                         const std::vector<uint8_t>& InBytes, void* OutComponent)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
        uint16_t Version = 0;
        if(!Ar.OpenChunk(TAG_DATA, Version)) { return false; }
        BinaryComponentArchive ComponentAr(Ar, InRemap);
        InSerializer.Fn(ComponentAr, OutComponent);
        Ar.CloseChunk();
        return !Ar.HasError();
    }

    /* Name, parent GUID, authored mobility. */
    std::vector<uint8_t> EncodeHeader(World& InWorld, const EntityID InEntity)
    {
        const Entity Handle = InWorld.GetEntity(InEntity);
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        std::string Name(Handle.GetName());
        EntityGuid  Parent;
        const EntityID ParentId = Handle.GetParent();
        if(ParentId.IsSet() && InWorld.IsAlive(ParentId)) { Parent = InWorld.GetEntity(ParentId).GetGuid(); }
        uint8_t Mobility = uint8_t(Handle.GetAuthoredMobility());
        Ar << Name << Parent.Hi << Parent.Lo << Mobility;
        return Ar.GetBytes();
    }

    struct DecodedHeader
    {
        std::string Name;
        EntityGuid  Parent;
        EMobility   Mobility = EMobility::Dynamic;
    };

    bool DecodeHeader(const std::vector<uint8_t>& InBytes, DecodedHeader& Out)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading(InBytes);
        uint8_t Mobility = 0;
        Ar << Out.Name << Out.Parent.Hi << Out.Parent.Lo << Mobility;
        Out.Mobility = (Mobility <= uint8_t(EMobility::Stationary)) ? EMobility(Mobility) : EMobility::Dynamic;
        return !Ar.HasError();
    }

    // --- the clip's file helpers --------------------------------------------

    size_t Remaining(const EngineCore::Archive& Ar) { return Ar.GetSize() - std::min(Ar.GetSize(), Ar.GetCursor()); }

    /* A count read back that could not fit in what is left fails the archive. */
    bool CountFits(EngineCore::Archive& Ar, const uint64_t InCount, const size_t InMinBytesEach)
    {
        if(!Ar.IsLoading() || Ar.HasError()) { return !Ar.HasError(); }
        if(InCount > Remaining(Ar) / std::max<size_t>(InMinBytesEach, 1))
        {
            Ar.SetError("a replay count runs past the end of the clip");
            return false;
        }
        return true;
    }

    void SerializeBlob(EngineCore::Archive& Ar, std::vector<uint8_t>& InOut)
    {
        uint32_t Size = uint32_t(InOut.size());
        Ar << Size;
        if(!CountFits(Ar, Size, 1)) { return; }
        if(Ar.IsLoading()) { InOut.resize(Size); }
        if(Size > 0) { Ar.SerializeBytes(InOut.data(), Size); }
    }

    float WrapDegrees(float InDegrees)
    {
        float Wrapped = std::fmod(InDegrees + 180.0f, 360.0f);
        if(Wrapped < 0.0f) { Wrapped += 360.0f; }
        return Wrapped - 180.0f;
    }

    EngineCore::Vector3 LerpVector(const EngineCore::Vector3& A, const EngineCore::Vector3& B, const float T)
    {
        return EngineCore::Vector3(float(A.GetX()) + (float(B.GetX()) - float(A.GetX())) * T,
                                   float(A.GetY()) + (float(B.GetY()) - float(A.GetY())) * T,
                                   float(A.GetZ()) + (float(B.GetZ()) - float(A.GetZ())) * T);
    }

    /* Each Euler axis the short way round. */
    EngineCore::Vector3 LerpAngles(const EngineCore::Vector3& A, const EngineCore::Vector3& B, const float T)
    {
        return EngineCore::Vector3(float(A.GetX()) + WrapDegrees(float(B.GetX()) - float(A.GetX())) * T,
                                   float(A.GetY()) + WrapDegrees(float(B.GetY()) - float(A.GetY())) * T,
                                   float(A.GetZ()) + WrapDegrees(float(B.GetZ()) - float(A.GetZ())) * T);
    }
}

// ============================================================================
// ReplayClip
// ============================================================================

void ReplayClip::Clear()
{
    LevelPath.clear();
    TypeNames.clear();
    Frames.clear();
}

double ReplayClip::GetStartTime() const { return Frames.empty() ? 0.0 : Frames.front().Time; }
double ReplayClip::GetEndTime() const   { return Frames.empty() ? 0.0 : Frames.back().Time; }

size_t ReplayClip::FindFrame(const double InTime) const
{
    if(Frames.empty()) { return NO_FRAME; }
    const auto After = std::upper_bound(Frames.begin(), Frames.end(), InTime,
                                        [](const double T, const ReplayFrame& F) { return T < F.Time; });
    return (After == Frames.begin()) ? 0 : size_t(After - Frames.begin()) - 1;
}

size_t ReplayClip::FindKeyframe(size_t InIndex) const
{
    if(Frames.empty()) { return NO_FRAME; }
    InIndex = std::min(InIndex, Frames.size() - 1);
    while(InIndex > 0 && !Frames[InIndex].bKeyframe) { --InIndex; }
    return InIndex;
}

uint16_t ReplayClip::InternType(const char* InName)
{
    for(size_t Index = 0; Index < TypeNames.size(); ++Index)
    {
        if(TypeNames[Index] == InName) { return uint16_t(Index); }
    }
    TypeNames.emplace_back(InName);
    return uint16_t(TypeNames.size() - 1);
}

size_t ReplayClip::GetPayloadBytes() const
{
    size_t Total = 0;
    for(const ReplayFrame& Frame : Frames)
    {
        Total += Frame.Destroyed.size() * sizeof(EntityGuid);
        for(const ReplayEntityState& State : Frame.Entities)
        {
            Total += sizeof(EntityGuid) + State.Header.size() + State.Removed.size() * sizeof(uint16_t);
            for(const ReplayComponentState& Component : State.Components) { Total += Component.Bytes.size(); }
        }
    }
    return Total;
}

bool ReplayClip::Serialize(EngineCore::Archive& Ar)
{
    if(Ar.IsLoading()) { Clear(); }

    uint16_t Version = REPLAY_CHUNK_VERSION;
    if(!Ar.OpenChunk(TAG_REPLAY, Version))
    {
        if(Ar.IsLoading()) { Ar.SetError("not a replay clip (no RPLY chunk)"); }
        return false;
    }

    Ar << LevelPath;

    uint32_t TypeCount = uint32_t(TypeNames.size());
    Ar << TypeCount;
    if(CountFits(Ar, TypeCount, 4))
    {
        if(Ar.IsLoading()) { TypeNames.resize(TypeCount); }
        for(std::string& Name : TypeNames) { Ar << Name; }
    }

    uint32_t FrameCount = uint32_t(Frames.size());
    Ar << FrameCount;
    if(CountFits(Ar, FrameCount, 13))
    {
        if(Ar.IsLoading()) { Frames.resize(FrameCount); }
        for(ReplayFrame& Frame : Frames)
        {
            if(Ar.HasError()) { break; }
            uint8_t bKey = Frame.bKeyframe ? 1 : 0;
            Ar << Frame.Time << bKey;
            Frame.bKeyframe = bKey != 0;

            uint32_t EntityCount = uint32_t(Frame.Entities.size());
            Ar << EntityCount;
            if(!CountFits(Ar, EntityCount, 21)) { break; }
            if(Ar.IsLoading()) { Frame.Entities.resize(EntityCount); }
            for(ReplayEntityState& State : Frame.Entities)
            {
                Ar << State.Guid.Hi << State.Guid.Lo << State.Flags;
                if(State.Flags & REF_Header) { SerializeBlob(Ar, State.Header); }

                uint16_t ComponentCount = uint16_t(State.Components.size());
                Ar << ComponentCount;
                if(!CountFits(Ar, ComponentCount, 6)) { break; }
                if(Ar.IsLoading()) { State.Components.resize(ComponentCount); }
                for(ReplayComponentState& Component : State.Components)
                {
                    Ar << Component.Type;
                    SerializeBlob(Ar, Component.Bytes);
                    if(Ar.IsLoading() && !Ar.HasError() && Component.Type >= TypeNames.size())
                    {
                        Ar.SetError("a replay component names a type the clip does not list");
                    }
                }

                uint16_t RemovedCount = uint16_t(State.Removed.size());
                Ar << RemovedCount;
                if(!CountFits(Ar, RemovedCount, 2)) { break; }
                if(Ar.IsLoading()) { State.Removed.resize(RemovedCount); }
                for(uint16_t& Type : State.Removed) { Ar << Type; }
                if(Ar.HasError()) { break; }
            }

            uint32_t DestroyedCount = uint32_t(Frame.Destroyed.size());
            Ar << DestroyedCount;
            if(!CountFits(Ar, DestroyedCount, 16)) { break; }
            if(Ar.IsLoading()) { Frame.Destroyed.resize(DestroyedCount); }
            for(EntityGuid& Guid : Frame.Destroyed) { Ar << Guid.Hi << Guid.Lo; }
        }
    }

    Ar.CloseChunk();

    if(Ar.IsLoading() && !Ar.HasError() && !Frames.empty() && !Frames.front().bKeyframe)
    {
        Ar.SetError("a replay clip must begin with a keyframe");
    }
    if(Ar.IsLoading() && Ar.HasError()) { Clear(); }
    return !Ar.HasError();
}

bool SaveReplayClip(const std::string& InPath, ReplayClip& InClip)
{
    EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
    if(!InClip.Serialize(Ar)) { return false; }
    return EngineCore::SaveArchiveToFile(InPath, Ar);
}

bool LoadReplayClip(const std::string& InPath, ReplayClip& OutClip)
{
    EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
    if(!EngineCore::LoadArchiveFromFile(InPath, Ar)) { return false; }
    return OutClip.Serialize(Ar);
}

// ============================================================================
// ReplayRecorder
// ============================================================================

ReplayRecorder::ReplayRecorder(ReplayClip& OutClip, const ReplayRecordSettings& InSettings)
    : Clip(OutClip), Settings(InSettings), Excluded(COMPONENT_TYPE_MAX, 0)
{
    Clip.Clear();
}

ReplayRecorder::~ReplayRecorder()
{
    Stop();
}

void ReplayRecorder::SetEntityFilter(const ReplayEntityFilterFn InFn, void* InUserData)
{
    Filter         = InFn;
    FilterUserData = InUserData;
}

void ReplayRecorder::ExcludeComponentType(const ComponentTypeID InType)
{
    if(InType < COMPONENT_TYPE_MAX) { Excluded[InType] = 1; }
}

void ReplayRecorder::RecordOnly(const std::vector<std::string>& InTypeNames)
{
    if(InTypeNames.empty()) { return; }
    for(size_t Id = 0; Id < COMPONENT_TYPE_MAX; ++Id)
    {
        const ComponentTypeInfo* Info = GetComponentType(ComponentTypeID(Id));
        if(Info && std::find(InTypeNames.begin(), InTypeNames.end(), Info->Name) == InTypeNames.end())
        {
            ExcludeComponentType(ComponentTypeID(Id));
        }
    }
}

void ReplayRecorder::Capture(World& InWorld, const double InTime)
{
    /* The entities to record, in slot order. */
    struct CollectContext
    {
        ReplayRecorder*        Self;
        World*                 TheWorld;
        std::vector<EntityID>* Out;
    };
    std::vector<EntityID> Candidates;
    CollectContext Collect{ this, &InWorld, &Candidates };
    InWorld.ForEachEntity([](const EntityID InEntity, void* InUserData)
    {
        CollectContext& C = *static_cast<CollectContext*>(InUserData);
        if(!C.Self->Settings.bRecordStatic && C.TheWorld->GetEntity(InEntity).IsStatic()) { return; }
        if(C.Self->Filter && !C.Self->Filter(*C.TheWorld, InEntity, C.Self->FilterUserData)) { return; }
        C.Out->push_back(InEntity);
    }, &Collect);

    std::vector<EntityGuid>                                         Order;
    std::unordered_map<EntityGuid, EntitySnapshot, EntityGuidHash>  Current;
    std::unordered_map<uint64_t, EntitySnapshot*>                   BySlot;
    Order.reserve(Candidates.size());
    Current.reserve(Candidates.size());
    BySlot.reserve(Candidates.size());

    for(const EntityID Id : Candidates)
    {
        const EntityGuid Guid = InWorld.GetEntity(Id).GetGuid();
        if(!Guid.IsSet() || Current.count(Guid) != 0) { continue; }
        EntitySnapshot& Snapshot = Current[Guid];
        Snapshot.Header = EncodeHeader(InWorld, Id);
        Order.push_back(Guid);
        BySlot[Id.Pack()] = &Snapshot;
    }

    /* Type id order, then pool order: the same World gives the same frame. */
    for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
    {
        if(Excluded[Type]) { continue; }
        ComponentPool* Pool = InWorld.FindPool(Type);
        if(!Pool || Pool->GetCount() == 0) { continue; }
        const ComponentSerializerInfo* Serializer = FindComponentSerializer(Type);
        const ComponentTypeInfo*       Info       = GetComponentType(Type);
        if(!Serializer || !Serializer->Fn || !Info) { continue; }

        uint16_t ClipType = NO_TYPE;
        char*    Dense    = static_cast<char*>(Pool->GetDenseData());
        for(size_t Index = 0; Index < Pool->GetCount(); ++Index)
        {
            const auto Found = BySlot.find(Pool->GetEntityAt(Index).Pack());
            if(Found == BySlot.end()) { continue; }
            if(ClipType == NO_TYPE) { ClipType = Clip.InternType(Info->Name); }
            Found->second->Components[ClipType] = EncodeComponent(InWorld, *Serializer, Dense + Index * Info->SizeBytes);
            Found->second->Order.push_back(ClipType);
        }
    }

    const bool bKeyframe = Clip.Frames.empty() || (InTime - LastKeyframeTime) >= double(Settings.KeyframeInterval);

    ReplayFrame Frame;
    Frame.Time      = InTime;
    Frame.bKeyframe = bKeyframe;

    for(const EntityGuid& Guid : Order)
    {
        const EntitySnapshot& Snapshot = Current[Guid];
        const auto Before = bKeyframe ? Previous.end() : Previous.find(Guid);

        ReplayEntityState State;
        State.Guid = Guid;

        if(Before == Previous.end())
        {
            State.Flags  = REF_Header;
            State.Header = Snapshot.Header;
            for(const uint16_t Type : Snapshot.Order)
            {
                State.Components.push_back(ReplayComponentState{ Type, Snapshot.Components.at(Type) });
            }
            Frame.Entities.push_back(std::move(State));
            continue;
        }

        const EntitySnapshot& Old = Before->second;
        if(Snapshot.Header != Old.Header)
        {
            State.Flags  = REF_Header;
            State.Header = Snapshot.Header;
        }
        for(const uint16_t Type : Snapshot.Order)
        {
            const std::vector<uint8_t>& Bytes = Snapshot.Components.at(Type);
            const auto OldBytes = Old.Components.find(Type);
            if(OldBytes == Old.Components.end() || OldBytes->second != Bytes)
            {
                State.Components.push_back(ReplayComponentState{ Type, Bytes });
            }
        }
        for(const uint16_t Type : Old.Order)
        {
            if(Snapshot.Components.count(Type) == 0) { State.Removed.push_back(Type); }
        }
        if(State.Flags != REF_None || !State.Components.empty() || !State.Removed.empty())
        {
            Frame.Entities.push_back(std::move(State));
        }
    }

    if(!bKeyframe)
    {
        for(const EntityGuid& Guid : PreviousOrder)
        {
            if(Current.count(Guid) == 0) { Frame.Destroyed.push_back(Guid); }
        }
    }

    Clip.Frames.push_back(std::move(Frame));
    if(bKeyframe) { LastKeyframeTime = InTime; }

    Previous      = std::move(Current);
    PreviousOrder = std::move(Order);

    TrimToMaxDuration();
}

void ReplayRecorder::TrimToMaxDuration()
{
    if(Settings.MaxDuration <= 0.0f || Clip.Frames.size() < 2) { return; }
    const double Cutoff = Clip.Frames.back().Time - double(Settings.MaxDuration);

    /* The latest keyframe at or before the cutoff becomes the first frame. */
    size_t Drop = 0;
    for(size_t Index = 1; Index < Clip.Frames.size() && Clip.Frames[Index].Time <= Cutoff; ++Index)
    {
        if(Clip.Frames[Index].bKeyframe) { Drop = Index; }
    }
    if(Drop > 0)
    {
        Clip.Frames.erase(Clip.Frames.begin(), Clip.Frames.begin() + std::ptrdiff_t(Drop));
    }
}

void ReplayRecorder::Start(World& InWorld)
{
    Stop();
    Attached    = &InWorld;
    SinceSample = 0.0;
    Capture(InWorld, Clock);
    InWorld.RegisterSystem("ReplayRecorder", TP_PostUpdate, &ReplayRecorder::TickSystem, this);
}

void ReplayRecorder::Stop()
{
    if(!Attached) { return; }
    Attached->UnregisterSystem(&ReplayRecorder::TickSystem, this);
    Attached = nullptr;
}

void ReplayRecorder::TickSystem(World& InWorld, const float InDeltaSeconds, void* InUserData)
{
    ReplayRecorder& Self = *static_cast<ReplayRecorder*>(InUserData);
    Self.Clock       += double(InDeltaSeconds);
    Self.SinceSample += double(InDeltaSeconds);

    if(Self.Settings.SampleRate > 0.0f)
    {
        const double Interval = 1.0 / double(Self.Settings.SampleRate);
        if(Self.SinceSample + 1e-6 < Interval) { return; }
        /* No catch-up burst after a long frame: one sample, then on. */
        Self.SinceSample -= Interval;
        if(Self.SinceSample >= Interval) { Self.SinceSample = 0.0; }
    }
    Self.Capture(InWorld, Self.Clock);
}

// ============================================================================
// ReplayPlayer
// ============================================================================

ReplayPlayer::ReplayPlayer(World& InWorld, const ReplayClip& InClip)
    : TargetWorld(InWorld), Clip(InClip)
{
    const ComponentTypeID TransformId = TransformComponent::StaticType().ID;
    Types.resize(Clip.GetTypeCount(), COMPONENT_TYPE_INVALID);
    for(size_t Index = 0; Index < Clip.GetTypeCount(); ++Index)
    {
        const ComponentTypeInfo*       Info       = FindComponentType(Clip.GetTypeName(Index));
        const ComponentSerializerInfo* Serializer = Info ? FindComponentSerializer(Info->ID) : nullptr;
        if(!Info || !Serializer || !Serializer->Fn)
        {
            GAMECORE_LOG_WARN(std::string("ReplayPlayer: the clip's '") + Clip.GetTypeName(Index) + "' components will not play -- "
                              + (Info ? "no serializer registered" : "type not registered in this build"))
            continue;
        }
        Types[Index] = Info->ID;
        if(Info->ID == TransformId) { TransformType = uint16_t(Index); }
    }
    Time = Clip.GetStartTime();
}

ReplayPlayer::~ReplayPlayer() = default;

bool ReplayPlayer::IsAtEnd() const
{
    if(Clip.IsEmpty()) { return true; }
    return (Rate >= 0.0f) ? Time >= Clip.GetEndTime() : Time <= Clip.GetStartTime();
}

EntityID ReplayPlayer::FindEntity(const EntityGuid& InGuid) const
{
    const auto Found = Bound.find(InGuid);
    return (Found != Bound.end() && TargetWorld.IsAlive(Found->second)) ? Found->second : InvalidEntityID();
}

EntityID ReplayPlayer::Resolve(const EntityGuid& InGuid) const
{
    return GuidRemapper(TargetWorld, &Bound).Resolve(InGuid);
}

void ReplayPlayer::Advance(const float InDeltaSeconds)
{
    if(!bPlaying || Clip.IsEmpty()) { return; }

    const double Start    = Clip.GetStartTime();
    const double End      = Clip.GetEndTime();
    const double Duration = End - Start;
    double Next = Time + double(InDeltaSeconds) * double(Rate);

    if(Next >= End && Rate > 0.0f)
    {
        if(bLoop && Duration > 0.0) { Next = Start + std::fmod(Next - Start, Duration); }
        else                        { Next = End; bPlaying = false; }
    }
    else if(Next <= Start && Rate < 0.0f)
    {
        if(bLoop && Duration > 0.0) { Next = End - std::fmod(Start - Next, Duration); }
        else                        { Next = Start; bPlaying = false; }
    }
    Seek(Next);
}

void ReplayPlayer::Seek(double InTime)
{
    if(Clip.IsEmpty()) { return; }
    InTime = std::clamp(InTime, Clip.GetStartTime(), Clip.GetEndTime());
    Time   = InTime;

    const size_t Frame    = Clip.FindFrame(InTime);
    const size_t Keyframe = Clip.FindKeyframe(Frame);

    /* What the last seek blended holds a value between frames: write it again. */
    for(const EntityGuid& Guid : Interpolated) { MarkDirty(Guid, false, TransformType); }
    Interpolated.clear();

    /* Forward one delta at a time, unless a keyframe since is closer. */
    const size_t From = (Current == ReplayClip::NO_FRAME || Frame < Current || Keyframe > Current) ? Keyframe : Current + 1;
    for(size_t Index = From; Index <= Frame; ++Index) { ApplyFrameToState(Index); }
    Current = Frame;

    if(bHold)
    {
        for(const auto& [Guid, Entry] : State) { DirtySet[Guid].bAll = true; }
    }

    FlushToWorld();
    if(bInterpolate) { Interpolate(Frame, InTime); }
}

void ReplayPlayer::MarkDirty(const EntityGuid& InGuid, const bool bInHeader, const uint16_t InType)
{
    Dirty& D = DirtySet[InGuid];
    if(bInHeader) { D.bHeader = true; }
    if(InType != NO_TYPE && std::find(D.Types.begin(), D.Types.end(), InType) == D.Types.end())
    {
        D.Types.push_back(InType);
    }
}

void ReplayPlayer::ApplyFrameToState(const size_t InIndex)
{
    const ReplayFrame& Frame = Clip.GetFrame(InIndex);

    if(Frame.bKeyframe)
    {
        /* Replaces the state; only what differs from it is marked. */
        std::unordered_map<EntityGuid, EntityState, EntityGuidHash> Next;
        Next.reserve(Frame.Entities.size());
        for(const ReplayEntityState& Entry : Frame.Entities)
        {
            EntityState& NewState = Next[Entry.Guid];
            NewState.Header = Entry.Header;
            for(const ReplayComponentState& Component : Entry.Components) { NewState.Components[Component.Type] = Component.Bytes; }

            const auto Old = State.find(Entry.Guid);
            if(Old == State.end())
            {
                DirtySet[Entry.Guid].bAll = true;
                continue;
            }
            if(Old->second.Header != NewState.Header) { MarkDirty(Entry.Guid, true, NO_TYPE); }
            for(const auto& [Type, Bytes] : NewState.Components)
            {
                const auto OldBytes = Old->second.Components.find(Type);
                if(OldBytes == Old->second.Components.end() || OldBytes->second != Bytes) { MarkDirty(Entry.Guid, false, Type); }
            }
            for(const auto& [Type, Bytes] : Old->second.Components)
            {
                if(NewState.Components.count(Type) == 0) { MarkDirty(Entry.Guid, false, Type); }
            }
        }
        for(const auto& [Guid, Old] : State)
        {
            if(Next.count(Guid) == 0) { Removed.push_back(Guid); }
        }
        State = std::move(Next);
        return;
    }

    for(const EntityGuid& Guid : Frame.Destroyed)
    {
        if(State.erase(Guid) != 0) { Removed.push_back(Guid); }
    }

    for(const ReplayEntityState& Entry : Frame.Entities)
    {
        auto Found = State.find(Entry.Guid);
        if(Found == State.end())
        {
            EntityState& NewState = State[Entry.Guid];
            NewState.Header = Entry.Header;
            for(const ReplayComponentState& Component : Entry.Components) { NewState.Components[Component.Type] = Component.Bytes; }
            DirtySet[Entry.Guid].bAll = true;
            continue;
        }
        EntityState& Existing = Found->second;
        if(Entry.Flags & REF_Header)
        {
            Existing.Header = Entry.Header;
            MarkDirty(Entry.Guid, true, NO_TYPE);
        }
        for(const ReplayComponentState& Component : Entry.Components)
        {
            Existing.Components[Component.Type] = Component.Bytes;
            MarkDirty(Entry.Guid, false, Component.Type);
        }
        for(const uint16_t Type : Entry.Removed)
        {
            Existing.Components.erase(Type);
            MarkDirty(Entry.Guid, false, Type);
        }
    }
}

void ReplayPlayer::ApplyComponent(const EntityID InEntity, const uint16_t InType, const std::vector<uint8_t>& InBytes)
{
    if(InType >= Types.size() || Types[InType] == COMPONENT_TYPE_INVALID) { return; }
    const ComponentTypeID          Type       = Types[InType];
    const ComponentSerializerInfo* Serializer = FindComponentSerializer(Type);
    const ComponentTypeInfo*       Info       = GetComponentType(Type);
    if(!Serializer || !Serializer->Fn || !Info) { return; }

    const Entity Handle = TargetWorld.GetEntity(InEntity);
    void* Component = Handle.GetComponentByType(Type);
    if(!Component)
    {
        std::vector<uint8_t> Defaults(Info->SizeBytes, 0);
        if(Serializer->DefaultsFn) { Serializer->DefaultsFn(Defaults.data()); }
        Component = Handle.AddComponentByType(Type, Defaults.data());
        if(!Component) { return; }
    }

    GuidRemapper Remap(TargetWorld, &Bound);
    if(!DecodeComponent(Remap, *Serializer, InBytes, Component))
    {
        GAMECORE_LOG_WARN(std::string("ReplayPlayer: a '") + Info->Name + "' state did not read back cleanly")
    }
}

void ReplayPlayer::FlushToWorld()
{
    /* 1. Bind every dirty entity, creating the ones the world lacks, before
     *    any parent or entity reference is resolved. */
    for(auto& [Guid, D] : DirtySet)
    {
        const auto Entry = State.find(Guid);
        if(Entry == State.end()) { continue; }

        const auto Known = Bound.find(Guid);
        if(Known != Bound.end() && TargetWorld.IsAlive(Known->second)) { continue; }

        /* First touch of this entity (or it died under us): sync it whole. */
        D.bAll = true;
        EntityID Id = TargetWorld.FindEntityByGuid(Guid);
        if(!Id.IsSet() || !TargetWorld.IsAlive(Id))
        {
            DecodedHeader Header;
            DecodeHeader(Entry->second.Header, Header);
            Id = TargetWorld.CreateEntity(Header.Name.c_str(), Guid).GetID();
            Spawned.insert(Guid);
        }
        Bound[Guid] = Id;
    }

    /* 2. Names, parents, mobility. */
    for(const auto& [Guid, D] : DirtySet)
    {
        if(!D.bAll && !D.bHeader) { continue; }
        const auto Entry = State.find(Guid);
        if(Entry == State.end()) { continue; }

        DecodedHeader Header;
        if(!DecodeHeader(Entry->second.Header, Header)) { continue; }
        const Entity Handle = TargetWorld.GetEntity(Bound[Guid]);
        if(std::strcmp(Handle.GetName(), Header.Name.c_str()) != 0) { Handle.SetName(Header.Name.c_str()); }
        const EntityID Parent = Resolve(Header.Parent);
        if(Handle.GetParent() != Parent) { Handle.SetParent(Parent, false); }
        if(Handle.GetAuthoredMobility() != Header.Mobility) { Handle.SetMobility(Header.Mobility); }
    }

    /* 3. Components: set what the state holds, remove what it no longer does. */
    for(const auto& [Guid, D] : DirtySet)
    {
        const auto Entry = State.find(Guid);
        if(Entry == State.end()) { continue; }
        const EntityID Id     = Bound[Guid];
        const Entity   Handle = TargetWorld.GetEntity(Id);

        auto Sync = [&](const uint16_t InType)
        {
            if(InType >= Types.size() || Types[InType] == COMPONENT_TYPE_INVALID) { return; }
            const auto Bytes = Entry->second.Components.find(InType);
            if(Bytes != Entry->second.Components.end()) { ApplyComponent(Id, InType, Bytes->second); }
            else if(Handle.HasComponentByType(Types[InType])) { Handle.RemoveComponentByType(Types[InType]); }
        };

        if(D.bAll) { for(size_t Type = 0; Type < Types.size(); ++Type) { Sync(uint16_t(Type)); } }
        else       { for(const uint16_t Type : D.Types) { Sync(Type); } }
    }
    DirtySet.clear();

    /* 4. Destroys last, so a child reparented away in this frame survives
     *    its old parent. One that came back (destroyed, then recreated with
     *    the same GUID, in one seek) is still in the state and stays. */
    for(const EntityGuid& Guid : Removed)
    {
        if(State.count(Guid) != 0) { continue; }
        const auto Known = Bound.find(Guid);
        if(Known == Bound.end()) { continue; }
        if(TargetWorld.IsAlive(Known->second)) { TargetWorld.DestroyEntity(Known->second); }
        Bound.erase(Known);
        Spawned.erase(Guid);
    }
    Removed.clear();
}

void ReplayPlayer::Interpolate(const size_t InFrame, const double InTime)
{
    if(TransformType == NO_TYPE || InFrame + 1 >= Clip.GetFrameCount()) { return; }
    const ReplayFrame& From = Clip.GetFrame(InFrame);
    const ReplayFrame& To   = Clip.GetFrame(InFrame + 1);
    if(To.Time <= From.Time || InTime <= From.Time) { return; }
    const float Alpha = float(std::clamp((InTime - From.Time) / (To.Time - From.Time), 0.0, 1.0));

    const ComponentSerializerInfo* Serializer = FindComponentSerializer(TransformComponent::StaticType().ID);
    if(!Serializer || !Serializer->Fn) { return; }
    GuidRemapper Remap(TargetWorld, &Bound);

    for(const ReplayEntityState& Entry : To.Entities)
    {
        const auto Target = std::find_if(Entry.Components.begin(), Entry.Components.end(),
                                         [this](const ReplayComponentState& C) { return C.Type == TransformType; });
        if(Target == Entry.Components.end()) { continue; }

        const auto Have = State.find(Entry.Guid);
        if(Have == State.end()) { continue; }
        /* Never blend across a reparent: the two are in different spaces. */
        if((Entry.Flags & REF_Header) && Entry.Header != Have->second.Header) { continue; }
        const auto Source = Have->second.Components.find(TransformType);
        if(Source == Have->second.Components.end() || Source->second == Target->Bytes) { continue; }

        const EntityID Id = FindEntity(Entry.Guid);
        if(!Id.IsSet()) { continue; }
        TransformComponent* Live = TargetWorld.GetEntity(Id).GetComponent<TransformComponent>();
        if(!Live) { continue; }

        TransformComponent A{};
        TransformComponent B{};
        if(!DecodeComponent(Remap, *Serializer, Source->second, &A) || !DecodeComponent(Remap, *Serializer, Target->Bytes, &B)) { continue; }

        Live->Position = LerpVector(A.Position, B.Position, Alpha);
        Live->Rotation = LerpAngles(A.Rotation, B.Rotation, Alpha);
        Live->Scale    = LerpVector(A.Scale, B.Scale, Alpha);
        Interpolated.push_back(Entry.Guid);
    }
}

void ReplayPlayer::DestroySpawnedEntities()
{
    for(const EntityGuid& Guid : Spawned)
    {
        const auto Known = Bound.find(Guid);
        if(Known != Bound.end() && TargetWorld.IsAlive(Known->second)) { TargetWorld.DestroyEntity(Known->second); }
    }
    Spawned.clear();
    Bound.clear();
    State.clear();
    DirtySet.clear();
    Removed.clear();
    Interpolated.clear();
    Current = ReplayClip::NO_FRAME;
}
}
