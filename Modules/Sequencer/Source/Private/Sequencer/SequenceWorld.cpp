#include "Public/Sequencer/SequenceWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/Sequencer/SequencePlayerComponent.h"

/* SequenceInstance (evaluating an asset against a World) and SequenceWorld
 * (the players that drive instances through time). README.md has the rules. */
namespace Sequencer
{
    namespace
    {
        /* Bytes a field of InType occupies; 0 for a type a track cannot write. */
        uint32_t FieldSize(const GameCore::EScriptFieldType InType)
        {
            switch(InType)
            {
                case GameCore::EScriptFieldType::Float:  return sizeof(float);
                case GameCore::EScriptFieldType::Double: return sizeof(double);
                case GameCore::EScriptFieldType::Int32:  return sizeof(int32_t);
                case GameCore::EScriptFieldType::UInt8:
                case GameCore::EScriptFieldType::Flag:   return sizeof(uint8_t);
                case GameCore::EScriptFieldType::Bool:   return sizeof(bool);
                default:                                 return 0;
            }
        }

        float ReadField(const uint8_t* InAt, const GameCore::EScriptFieldType InType)
        {
            switch(InType)
            {
                case GameCore::EScriptFieldType::Float:  { float V;   std::memcpy(&V, InAt, sizeof(V)); return V; }
                case GameCore::EScriptFieldType::Double: { double V;  std::memcpy(&V, InAt, sizeof(V)); return float(V); }
                case GameCore::EScriptFieldType::Int32:  { int32_t V; std::memcpy(&V, InAt, sizeof(V)); return float(V); }
                case GameCore::EScriptFieldType::UInt8:
                case GameCore::EScriptFieldType::Flag:   return float(*InAt);
                case GameCore::EScriptFieldType::Bool:   { bool V;    std::memcpy(&V, InAt, sizeof(V)); return V ? 1.0f : 0.0f; }
                default:                                 return 0.0f;
            }
        }

        void WriteFieldValue(uint8_t* InAt, const GameCore::EScriptFieldType InType, const float InValue)
        {
            switch(InType)
            {
                case GameCore::EScriptFieldType::Float:  { const float V = InValue;                      std::memcpy(InAt, &V, sizeof(V)); break; }
                case GameCore::EScriptFieldType::Double: { const double V = double(InValue);            std::memcpy(InAt, &V, sizeof(V)); break; }
                case GameCore::EScriptFieldType::Int32:  { const int32_t V = int32_t(std::lround(InValue)); std::memcpy(InAt, &V, sizeof(V)); break; }
                case GameCore::EScriptFieldType::UInt8:  *InAt = uint8_t(std::clamp(std::lround(InValue), 0l, 255l)); break;
                case GameCore::EScriptFieldType::Flag:   *InAt = InValue != 0.0f ? 1 : 0; break;
                case GameCore::EScriptFieldType::Bool:   { const bool V = InValue != 0.0f;              std::memcpy(InAt, &V, sizeof(V)); break; }
                default: break;
            }
        }

        bool IsNumberField(const GameCore::EScriptFieldType InType)
        {
            return InType == GameCore::EScriptFieldType::Float || InType == GameCore::EScriptFieldType::Double
                || InType == GameCore::EScriptFieldType::Int32 || InType == GameCore::EScriptFieldType::UInt8;
        }

        bool IsFlagField(const GameCore::EScriptFieldType InType)
        {
            return InType == GameCore::EScriptFieldType::Flag || InType == GameCore::EScriptFieldType::Bool
                || InType == GameCore::EScriptFieldType::UInt8;
        }

        float GetAxis(const EngineCore::Vector3& InVector, const size_t InAxis)
        {
            return InAxis == 0 ? float(InVector.GetX()) : InAxis == 1 ? float(InVector.GetY()) : float(InVector.GetZ());
        }

        void SetAxis(EngineCore::Vector3& InOutVector, const size_t InAxis, const float InValue)
        {
            if(InAxis == 0)      { InOutVector.SetX(InValue); }
            else if(InAxis == 1) { InOutVector.SetY(InValue); }
            else                 { InOutVector.SetZ(InValue); }
        }

        constexpr uint32_t TRANSFORM_OFFSET = 0xFFFFFFFFu;
    }

    // === SequenceInstance ==================================================

    SequenceInstance::SequenceInstance(GameCore::World& InWorld, std::shared_ptr<const SequenceAsset> InAsset, std::string InPath)
        : World(InWorld), Asset(std::move(InAsset)), Path(std::move(InPath))
    {
        if(!Asset)
        {
            Asset = std::make_shared<SequenceAsset>();
        }
        Bindings.resize(Asset->Bindings.size());
    }

    SequenceInstance::~SequenceInstance()
    {
        ReleaseSpawned();
    }

    bool SequenceInstance::SetBindingOverride(const std::string& InId, const GameCore::EntityID InEntity)
    {
        const int32_t Index = Asset->FindBinding(InId);
        if(Index < 0)
        {
            return false;
        }
        BindingState& State = Bindings[size_t(Index)];
        State.Override  = InEntity;
        State.bResolved = false;
        State.bWarned   = false;
        return true;
    }

    GameCore::EntityID SequenceInstance::ResolveBinding(const size_t InIndex)
    {
        if(InIndex >= Bindings.size())
        {
            return GameCore::EntityID();
        }
        BindingState&          State   = Bindings[InIndex];
        const SequenceBinding& Binding = Asset->Bindings[InIndex];

        /* Resolved and still there (or resolved to nothing): as it was. */
        if(State.bResolved && (!State.Entity.IsSet() || World.IsAlive(State.Entity)))
        {
            return State.Entity;
        }

        State.bResolved = true;
        State.Entity    = GameCore::EntityID();
        if(State.Override.IsSet() && World.IsAlive(State.Override))
        {
            State.Entity = State.Override;
        }
        else if(Binding.Kind == ESequenceBindingKind::Possess)
        {
            if(Binding.Guid.IsSet())
            {
                State.Entity = World.FindEntityByGuid(Binding.Guid);
            }
            if(!State.Entity.IsSet() && !Binding.Name.empty())
            {
                State.Entity = World.FindEntityByName(Binding.Name.c_str());
            }
        }
        else if(!Binding.Prefab.empty())
        {
            if(State.Spawned.IsSet() && World.IsInstanceAlive(State.Spawned))
            {
                State.Entity = World.GetInstance(State.Spawned).GetRoot().GetID();
            }
            else
            {
                const char* Name = Binding.Name.empty() ? Binding.Id.c_str() : Binding.Name.c_str();
                const GameCore::PrefabInstance Spawned = World.SpawnPrefab(Binding.Prefab.c_str(), GameCore::TransformPose(), Name);
                if(Spawned.IsValid())
                {
                    State.Spawned = Spawned.GetID();
                    State.Entity  = Spawned.GetRoot().GetID();
                }
            }
        }

        if(!State.Entity.IsSet() && !State.bWarned)
        {
            State.bWarned = true;
            CORE_LOG_WARN("[Sequencer] '" + Path + "': binding '" + Binding.Id + "' "
                          + (Binding.Kind == ESequenceBindingKind::Spawn ? "could not spawn '" + Binding.Prefab + "'"
                                                                         : std::string("matches no entity in the level"))
                          + "; its tracks do nothing")
        }
        BindTracks(State, Binding);
        return State.Entity;
    }

    void SequenceInstance::BindTracks(BindingState& InOutState, const SequenceBinding& InBinding)
    {
        InOutState.Tracks.clear();
        InOutState.Tracks.reserve(InBinding.Tracks.size());
        for(const SequenceTrack& Track : InBinding.Tracks)
        {
            BoundTrack Bound;
            Bound.Track = &Track;
            if(Track.Type == ESequenceTrackType::Transform)
            {
                Bound.bUsable = true;
            }
            else if(Track.Type == ESequenceTrackType::Float || Track.Type == ESequenceTrackType::Bool)
            {
                const GameCore::ComponentTypeInfo* Info = GameCore::FindComponentType(Track.Component.c_str());
                size_t Count = 0;
                const GameCore::ScriptBindingEntry* Entries = Info ? GameCore::FindComponentScript(Info->ID, Count) : nullptr;
                for(size_t Index = 0; Entries && Index < Count; ++Index)
                {
                    const GameCore::ScriptBindingEntry& Entry = Entries[Index];
                    if(Entry.bMethod || Entry.bReadOnly || Track.Field != Entry.Name)
                    {
                        continue;
                    }
                    const bool bFits = Track.Type == ESequenceTrackType::Float ? IsNumberField(Entry.FieldType) : IsFlagField(Entry.FieldType);
                    if(bFits && FieldSize(Entry.FieldType) > 0)
                    {
                        Bound.Type      = Info->ID;
                        Bound.Offset    = Entry.Offset;
                        Bound.FieldType = uint8_t(Entry.FieldType);
                        Bound.bUsable   = true;
                    }
                    break;
                }
                if(!Bound.bUsable && InOutState.Entity.IsSet())
                {
                    CORE_LOG_WARN("[Sequencer] '" + Path + "': binding '" + InBinding.Id + "': " + TrackTypeName(Track.Type)
                                  + " track " + Track.Component + "." + Track.Field
                                  + " is not a writable " + (Track.Type == ESequenceTrackType::Float ? "number" : "flag")
                                  + " field that component binds for scripts; the track does nothing")
                }
            }
            InOutState.Tracks.push_back(Bound);
        }
    }

    void SequenceInstance::Evaluate(double InFrame)
    {
        InFrame = std::clamp(InFrame, 0.0, double(std::max(Asset->Length, 0)));
        for(size_t Index = 0; Index < Bindings.size(); ++Index)
        {
            const GameCore::EntityID Entity = ResolveBinding(Index);
            if(!Entity.IsSet() || !World.IsAlive(Entity))
            {
                continue;
            }
            for(const BoundTrack& Bound : Bindings[Index].Tracks)
            {
                if(!Bound.bUsable) { continue; }
                switch(Bound.Track->Type)
                {
                    case ESequenceTrackType::Transform: WriteTransform(Entity, *Bound.Track, InFrame); break;
                    case ESequenceTrackType::Float:
                    case ESequenceTrackType::Bool:      WriteField(Entity, Bound, InFrame); break;
                    case ESequenceTrackType::Event:
                    case ESequenceTrackType::Count:     break;
                }
            }
        }
    }

    void SequenceInstance::WriteTransform(const GameCore::EntityID InEntity, const SequenceTrack& InTrack, const double InFrame)
    {
        GameCore::TransformComponent* Transform = World.GetEntity(InEntity).GetComponent<GameCore::TransformComponent>();
        if(!Transform) { return; }

        bool bAny = false;
        for(const SequenceChannel& Channel : InTrack.Channels) { bAny = bAny || !Channel.IsEmpty(); }
        if(!bAny) { return; }

        GameCore::TransformPose Pose = Transform->GetPose();
        SaveTransform(InEntity, Pose);

        EngineCore::Vector3* Parts[3] = { &Pose.Position, &Pose.Rotation, &Pose.Scale };
        for(size_t Channel = 0; Channel < InTrack.Channels.size() && Channel < TC_Count; ++Channel)
        {
            const SequenceChannel& Keys = InTrack.Channels[Channel];
            if(Keys.IsEmpty()) { continue; }
            EngineCore::Vector3& Part = *Parts[Channel / 3];
            SetAxis(Part, Channel % 3, Keys.Evaluate(InFrame, GetAxis(Part, Channel % 3)));
        }
        Transform->SetPose(Pose);
    }

    void SequenceInstance::WriteField(const GameCore::EntityID InEntity, const BoundTrack& InBound, const double InFrame)
    {
        if(InBound.Track->Channels.empty() || InBound.Track->Channels[0].IsEmpty()) { return; }
        auto* Component = static_cast<uint8_t*>(World.GetEntity(InEntity).GetComponentByType(InBound.Type));
        if(!Component) { return; }

        const auto     Type = GameCore::EScriptFieldType(InBound.FieldType);
        uint8_t*       At   = Component + InBound.Offset;
        Save(InEntity, InBound.Type, InBound.Offset, FieldSize(Type), At);
        WriteFieldValue(At, Type, InBound.Track->Channels[0].Evaluate(InFrame, ReadField(At, Type)));
    }

    void SequenceInstance::Save(const GameCore::EntityID InEntity, const GameCore::ComponentTypeID InType, const uint32_t InOffset,
                                const uint32_t InSize, const void* InNow)
    {
        const SavedKey Key{ InEntity.Pack(), InType, InOffset };
        if(SavedIndex.count(Key) || InSize > sizeof(SavedValue::Bytes)) { return; }
        SavedValue Value;
        Value.Entity = InEntity;
        Value.Type   = InType;
        Value.Offset = InOffset;
        Value.Size   = InSize;
        std::memcpy(Value.Bytes, InNow, InSize);
        SavedIndex.emplace(Key, Saved.size());
        Saved.push_back(Value);
    }

    void SequenceInstance::SaveTransform(const GameCore::EntityID InEntity, const GameCore::TransformPose& InNow)
    {
        const SavedKey Key{ InEntity.Pack(), GameCore::TransformComponent::StaticType().ID, TRANSFORM_OFFSET };
        if(SavedIndex.count(Key)) { return; }
        SavedValue Value;
        Value.Entity = InEntity;
        Value.Type   = GameCore::TransformComponent::StaticType().ID;
        Value.Offset = TRANSFORM_OFFSET;
        Value.Pose   = InNow;
        SavedIndex.emplace(Key, Saved.size());
        Saved.push_back(Value);
    }

    void SequenceInstance::RestoreState()
    {
        /* Newest first, so where two saves overlap the oldest value wins. */
        for(auto It = Saved.rbegin(); It != Saved.rend(); ++It)
        {
            const SavedValue& Value = *It;
            if(!World.IsAlive(Value.Entity)) { continue; }
            const GameCore::Entity Entity = World.GetEntity(Value.Entity);
            if(Value.Offset == TRANSFORM_OFFSET)
            {
                if(GameCore::TransformComponent* Transform = Entity.GetComponent<GameCore::TransformComponent>())
                {
                    Transform->SetPose(Value.Pose);
                }
            }
            else if(auto* Component = static_cast<uint8_t*>(Entity.GetComponentByType(Value.Type)))
            {
                std::memcpy(Component + Value.Offset, Value.Bytes, Value.Size);
            }
        }
        Saved.clear();
        SavedIndex.clear();
    }

    void SequenceInstance::DiscardState()
    {
        Saved.clear();
        SavedIndex.clear();
    }

    void SequenceInstance::ReleaseSpawned()
    {
        for(BindingState& State : Bindings)
        {
            if(!State.Spawned.IsSet()) { continue; }
            if(World.IsInstanceAlive(State.Spawned))
            {
                World.DestroyPrefabInstance(State.Spawned);
            }
            State.Spawned   = GameCore::PrefabInstanceID();
            State.Entity    = GameCore::EntityID();
            State.bResolved = false;
        }
    }

    void SequenceInstance::FireEvents(const double InFrom, const double InTo, const bool bInIncludeFrom, const bool bInSkipOnly)
    {
        if(InTo < InFrom) { return; }

        struct Pending
        {
            int32_t             Frame = 0;
            std::string         Name;
            GameCore::EventArgs Args;
        };
        std::vector<Pending> Fire;

        const auto Collect = [&](const SequenceTrack& InTrack)
        {
            if(InTrack.Type != ESequenceTrackType::Event) { return; }
            for(const SequenceEventKey& Key : InTrack.Events)
            {
                const double At = double(Key.Frame);
                const bool bIn  = (At > InFrom || (bInIncludeFrom && At >= InFrom)) && At <= InTo;
                if(!bIn || Key.Event.empty() || (bInSkipOnly && !Key.bFireOnSkip)) { continue; }

                Pending Made;
                Made.Frame = Key.Frame;
                Made.Name  = Key.Event;
                for(const SequenceEventArg& Arg : Key.Args)
                {
                    if(Made.Args.Count >= SEQUENCE_EVENT_ARGS_MAX) { break; }
                    GameCore::EventArg& Out = Made.Args.Args[Made.Args.Count++];
                    switch(Arg.Type)
                    {
                        case SequenceEventArg::EType::Number:  Out = GameCore::EventArg::Make(Arg.Number); break;
                        case SequenceEventArg::EType::Bool:    Out = GameCore::EventArg::Make(Arg.bValue); break;
                        case SequenceEventArg::EType::String:  Out = GameCore::EventArg::Make(Arg.Text.c_str()); break;
                        case SequenceEventArg::EType::Binding:
                        {
                            const int32_t Index = Asset->FindBinding(Arg.Text);
                            Out = GameCore::EventArg::Make(Index >= 0 ? ResolveBinding(size_t(Index)) : GameCore::EntityID());
                            break;
                        }
                    }
                }
                Fire.push_back(std::move(Made));
            }
        };
        for(const SequenceBinding& Binding : Asset->Bindings)
        {
            for(const SequenceTrack& Track : Binding.Tracks) { Collect(Track); }
        }
        for(const SequenceTrack& Track : Asset->Tracks) { Collect(Track); }
        if(Fire.empty()) { return; }

        std::stable_sort(Fire.begin(), Fire.end(), [](const Pending& L, const Pending& R) { return L.Frame < R.Frame; });

        /* A handler may stop or restart the player that owns this instance,
         * destroying it: from here only locals are touched. */
        GameCore::EventBus& Bus = World.GetEvents();
        for(const Pending& Event : Fire)
        {
            Bus.Emit(Event.Name.c_str(), Event.Args);
        }
    }

    // === SequenceWorld =====================================================

    namespace
    {
        std::unordered_map<const GameCore::World*, SequenceWorld*>& Systems()
        {
            static std::unordered_map<const GameCore::World*, SequenceWorld*> Map;
            return Map;
        }
    }

    SequenceWorld* SequenceWorld::Attach(GameCore::World& InWorld)
    {
        SequenceWorld*& System = Systems()[&InWorld];
        if(!System)
        {
            System = new SequenceWorld(InWorld);
            InWorld.RegisterSystem("Sequencer.Players", GameCore::TP_PostPhysics, &SequenceWorld::TickSystem, System);
        }
        ++System->Attachments;
        return System;
    }

    void SequenceWorld::Detach(GameCore::World& InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(&InWorld);
        if(Found == Map.end()) { return; }
        SequenceWorld* System = Found->second;
        if(--System->Attachments > 0) { return; }
        InWorld.UnregisterSystem(&SequenceWorld::TickSystem, System);
        Map.erase(Found);
        delete System;
    }

    SequenceWorld* SequenceWorld::Find(const GameCore::World* InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(InWorld);
        return Found != Map.end() ? Found->second : nullptr;
    }

    SequenceWorld::SequenceWorld(GameCore::World& InWorld) : World(InWorld) {}

    /* The world is going away: spawned entities go with it, nothing restored. */
    SequenceWorld::~SequenceWorld() = default;

    void SequenceWorld::TickSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        (void)InWorld;
        static_cast<SequenceWorld*>(InUserData)->Tick(InDeltaSeconds);
    }

    SequenceWorld::Player* SequenceWorld::FindPlayer(const GameCore::EntityID InPlayer)
    {
        const auto Found = Players.find(InPlayer.Pack());
        return Found != Players.end() ? &Found->second : nullptr;
    }

    const SequenceWorld::Player* SequenceWorld::FindPlayer(const GameCore::EntityID InPlayer) const
    {
        const auto Found = Players.find(InPlayer.Pack());
        return Found != Players.end() ? &Found->second : nullptr;
    }

    SequenceWorld::Player* SequenceWorld::GetPlayer(const GameCore::EntityID InPlayer)
    {
        if(!World.IsAlive(InPlayer) || !World.GetEntity(InPlayer).HasComponent<SequencePlayerComponent>())
        {
            return nullptr;
        }
        return &Players[InPlayer.Pack()];
    }

    bool SequenceWorld::Prepare(const GameCore::EntityID InPlayer, Player& InOutPlayer)
    {
        const SequencePlayerComponent* Component = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>();
        const std::string Path(Component->Sequence, strnlen(Component->Sequence, SEQUENCE_PATH_MAX));

        /* The same file, as the cache has it now (the editor may have saved). */
        std::shared_ptr<const SequenceAsset> Asset = SequenceAssetCache::Find(Path);
        if(InOutPlayer.Instance && InOutPlayer.Instance->GetPath() == Path && Asset.get() == &InOutPlayer.Instance->GetAsset())
        {
            return true;
        }
        if(!Asset)
        {
            return false;
        }
        Release(InOutPlayer);
        InOutPlayer.Instance = std::make_unique<SequenceInstance>(World, std::move(Asset), Path);
        for(const auto& Override : InOutPlayer.Overrides)
        {
            InOutPlayer.Instance->SetBindingOverride(Override.first, Override.second);
        }
        return true;
    }

    void SequenceWorld::Release(Player& InOutPlayer)
    {
        if(!InOutPlayer.Instance) { return; }
        if(InOutPlayer.bRestoreState) { InOutPlayer.Instance->RestoreState(); }
        else                          { InOutPlayer.Instance->DiscardState(); }
        InOutPlayer.Instance->ReleaseSpawned();
    }

    void SequenceWorld::Emit(const char* InEvent, const GameCore::EntityID InPlayer, const Player& InPlayerState)
    {
        GameCore::EventArgs Args;
        Args.Args[0] = GameCore::EventArg::Make(InPlayer);
        Args.Args[1] = GameCore::EventArg::Make(InPlayerState.Instance ? InPlayerState.Instance->GetPath().c_str() : "");
        Args.Count   = 2;
        World.GetEvents().Emit(InEvent, Args);
    }

    GameCore::EntityID SequenceWorld::PlaySequence(const char* InPath, const bool bInLoop)
    {
        if(!InPath || !SequenceAssetCache::Find(InPath))
        {
            return GameCore::EntityID();
        }
        const std::string Name = std::string("Sequence ") + InPath;
        GameCore::Entity Entity = World.CreateEntity(Name.c_str());

        SequencePlayerComponent Init;
        std::snprintf(Init.Sequence, sizeof(Init.Sequence), "%s", InPath);
        Init.bAutoPlay            = 0;
        Init.bLoop                = bInLoop ? 1 : 0;
        Init.bDestroyWhenFinished = 1;
        Entity.AddComponent(Init);

        if(!Play(Entity.GetID()))
        {
            World.DestroyEntity(Entity.GetID());
            return GameCore::EntityID();
        }
        return Entity.GetID();
    }

    bool SequenceWorld::Play(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        P->bAutoPlayed = true;
        if(P->State == ESequencePlayState::Paused && P->Instance)
        {
            return Resume(InPlayer);
        }
        /* Restarting: what the last run changed goes first, as it asked. */
        Release(*P);
        if(!Prepare(InPlayer, *P)) { return false; }

        P->bRestoreState = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>()->bRestoreState != 0;
        P->State         = ESequencePlayState::Playing;
        P->Frame         = 0.0;
        P->bIncludeFrame = true;
        P->bFresh        = true;
        Emit("SequenceStarted", InPlayer, *P);
        return true;
    }

    bool SequenceWorld::Pause(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        if(P->State == ESequencePlayState::Playing) { P->State = ESequencePlayState::Paused; }
        return true;
    }

    bool SequenceWorld::Resume(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        if(P->State == ESequencePlayState::Paused && P->Instance) { P->State = ESequencePlayState::Playing; }
        return true;
    }

    bool SequenceWorld::Stop(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        const bool bWasStopped = P->State == ESequencePlayState::Stopped;
        Release(*P);
        P->State         = ESequencePlayState::Stopped;
        P->Frame         = 0.0;
        P->bIncludeFrame = false;
        if(!bWasStopped)
        {
            Emit("SequenceStopped", InPlayer, *P);
        }
        return true;
    }

    bool SequenceWorld::Seek(const GameCore::EntityID InPlayer, const double InSeconds)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P) { return false; }
        if(!P->Instance)
        {
            if(!Prepare(InPlayer, *P)) { return false; }
            P->bRestoreState = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>()->bRestoreState != 0;
        }
        const SequenceAsset& Asset = P->Instance->GetAsset();
        P->Frame         = std::clamp(Asset.SecondsToFrames(InSeconds), 0.0, double(Asset.Length));
        P->bIncludeFrame = false;
        P->bAutoPlayed   = true;
        if(P->State == ESequencePlayState::Stopped || P->State == ESequencePlayState::Finished)
        {
            P->State = ESequencePlayState::Paused;
        }
        P->Instance->Evaluate(P->Frame);
        return true;
    }

    bool SequenceWorld::Skip(const GameCore::EntityID InPlayer)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P || !P->Instance) { return false; }
        if(P->State != ESequencePlayState::Playing && P->State != ESequencePlayState::Paused) { return false; }

        SequenceInstance* Instance = P->Instance.get();
        const double End = double(Instance->GetAsset().Length);
        const double From = P->Frame;
        const bool   bIncl = P->bIncludeFrame;
        P->Frame         = End;
        P->bIncludeFrame = false;
        Instance->Evaluate(End);
        Instance->FireEvents(From, End, bIncl, true);

        /* A handler may have restarted or stopped it. */
        P = FindPlayer(InPlayer);
        if(P && P->Instance.get() == Instance && P->State != ESequencePlayState::Stopped)
        {
            Finish(InPlayer, *P);
        }
        return true;
    }

    bool SequenceWorld::Bind(const GameCore::EntityID InPlayer, const char* InBindingId, const GameCore::EntityID InEntity)
    {
        Player* P = GetPlayer(InPlayer);
        if(!P || !InBindingId || !InBindingId[0]) { return false; }
        if(InEntity.IsSet()) { P->Overrides[InBindingId] = InEntity; }
        else                 { P->Overrides.erase(InBindingId); }
        if(P->Instance)
        {
            return P->Instance->SetBindingOverride(InBindingId, InEntity);
        }
        return true;
    }

    ESequencePlayState SequenceWorld::GetState(const GameCore::EntityID InPlayer) const
    {
        const Player* P = FindPlayer(InPlayer);
        return P ? P->State : ESequencePlayState::Stopped;
    }

    double SequenceWorld::GetTime(const GameCore::EntityID InPlayer) const
    {
        const Player* P = FindPlayer(InPlayer);
        return P && P->Instance ? P->Instance->GetAsset().FramesToSeconds(P->Frame) : 0.0;
    }

    double SequenceWorld::GetDuration(const GameCore::EntityID InPlayer) const
    {
        const Player* P = FindPlayer(InPlayer);
        return P && P->Instance ? P->Instance->GetAsset().GetDurationSeconds() : 0.0;
    }

    void SequenceWorld::Finish(const GameCore::EntityID InPlayer, Player& InOutPlayer)
    {
        InOutPlayer.State = ESequencePlayState::Finished;
        Release(InOutPlayer);

        const SequencePlayerComponent* Component = World.IsAlive(InPlayer)
            ? World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>() : nullptr;
        const bool bDestroy = Component && Component->bDestroyWhenFinished;

        Emit("SequenceFinished", InPlayer, InOutPlayer);
        if(bDestroy && World.IsAlive(InPlayer))
        {
            World.DestroyEntity(InPlayer);
        }
    }

    void SequenceWorld::Advance(const GameCore::EntityID InPlayer, Player& InOutPlayer, const float InDeltaSeconds)
    {
        const SequencePlayerComponent* Component = World.GetEntity(InPlayer).GetComponent<SequencePlayerComponent>();
        SequenceInstance*    Instance = InOutPlayer.Instance.get();
        const SequenceAsset& Asset    = Instance->GetAsset();
        const double         Length   = double(Asset.Length);
        const double         Rate     = std::max(0.0, double(Component->PlayRate));

        const double From  = InOutPlayer.Frame;
        const bool   bIncl = InOutPlayer.bIncludeFrame;
        double       To    = From + double(InDeltaSeconds) * double(Asset.FrameRate) * Rate;
        InOutPlayer.bIncludeFrame = false;

        bool bEnded = false;
        if(To >= Length)
        {
            if(Component->bLoop && Length > 0.0)
            {
                /* The rest of this pass, then the start of the next. */
                const double Wrapped = std::fmod(To, Length);
                Instance->Evaluate(Wrapped);
                InOutPlayer.Frame = Wrapped;
                Instance->FireEvents(From, Length, bIncl);
                Player* P = FindPlayer(InPlayer);
                if(P && P->Instance.get() == Instance && P->State == ESequencePlayState::Playing)
                {
                    Instance->FireEvents(0.0, Wrapped, true);
                }
                return;
            }
            To     = Length;
            bEnded = true;
        }

        InOutPlayer.Frame = To;
        Instance->Evaluate(To);
        Instance->FireEvents(From, To, bIncl);

        if(bEnded)
        {
            Player* P = FindPlayer(InPlayer);
            if(P && P->Instance.get() == Instance && P->State == ESequencePlayState::Playing)
            {
                Finish(InPlayer, *P);
            }
        }
    }

    void SequenceWorld::Tick(const float InDeltaSeconds)
    {
        /* Players whose entity or component went: released, forgotten. */
        for(auto It = Players.begin(); It != Players.end();)
        {
            const GameCore::EntityID Entity = GameCore::EntityID::Unpack(It->first);
            if(World.IsAlive(Entity) && World.GetEntity(Entity).HasComponent<SequencePlayerComponent>())
            {
                ++It;
                continue;
            }
            Release(It->second);
            It = Players.erase(It);
        }

        /* Gathered first: a sequence may spawn, which may add players. */
        std::vector<GameCore::EntityID> Ids;
        World.Each<SequencePlayerComponent>([&Ids](const GameCore::Entity& InEntity, SequencePlayerComponent&)
        {
            Ids.push_back(InEntity.GetID());
        });

        for(const GameCore::EntityID Id : Ids)
        {
            Player* P = GetPlayer(Id);
            if(!P) { continue; }
            if(!P->bAutoPlayed)
            {
                P->bAutoPlayed = true;
                const SequencePlayerComponent* Component = World.GetEntity(Id).GetComponent<SequencePlayerComponent>();
                if(Component->bAutoPlay && P->State == ESequencePlayState::Stopped)
                {
                    Play(Id);
                }
            }
            if(P->State == ESequencePlayState::Playing && P->Instance)
            {
                /* A sequence's first tick shows the frame it starts on: no time passes. */
                const float Delta = P->bFresh ? 0.0f : InDeltaSeconds;
                P->bFresh = false;
                Advance(Id, *P, Delta);
            }
        }
    }
}
