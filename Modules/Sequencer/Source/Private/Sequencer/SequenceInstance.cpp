#include "Public/Sequencer/SequenceInstance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/Components/AudioSourceComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/PostProcessStack.h"

/* SequenceInstance: one asset applied to one World. README.md has the rules. */
namespace Sequencer
{
    namespace
    {
        constexpr uint32_t TRANSFORM_OFFSET = 0xFFFFFFFFu;
        constexpr int32_t  MAX_DEPTH        = 8;

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
                case GameCore::EScriptFieldType::Float:  { const float V = InValue;                         std::memcpy(InAt, &V, sizeof(V)); break; }
                case GameCore::EScriptFieldType::Double: { const double V = double(InValue);               std::memcpy(InAt, &V, sizeof(V)); break; }
                case GameCore::EScriptFieldType::Int32:  { const int32_t V = int32_t(std::lround(InValue)); std::memcpy(InAt, &V, sizeof(V)); break; }
                case GameCore::EScriptFieldType::UInt8:  *InAt = uint8_t(std::clamp(std::lround(InValue), 0l, 255l)); break;
                case GameCore::EScriptFieldType::Flag:   *InAt = InValue != 0.0f ? 1 : 0; break;
                case GameCore::EScriptFieldType::Bool:   { const bool V = InValue != 0.0f;                 std::memcpy(InAt, &V, sizeof(V)); break; }
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

        bool EndsWith(const std::string& InText, const char* InSuffix)
        {
            const size_t Length = std::strlen(InSuffix);
            return InText.size() >= Length && InText.compare(InText.size() - Length, Length, InSuffix) == 0;
        }

        // --- Rotation blending, through quaternions ------------------------------

        struct Quat { double W = 1.0, X = 0.0, Y = 0.0, Z = 0.0; };

        Quat ToQuat(const GameCore::Rotation3& InR)
        {
            const auto& M = InR.M;
            Quat Q;
            const double Trace = M[0][0] + M[1][1] + M[2][2];
            if(Trace > 0.0)
            {
                const double S = std::sqrt(Trace + 1.0) * 2.0;
                Q.W = 0.25 * S;
                Q.X = (M[2][1] - M[1][2]) / S;
                Q.Y = (M[0][2] - M[2][0]) / S;
                Q.Z = (M[1][0] - M[0][1]) / S;
            }
            else if(M[0][0] > M[1][1] && M[0][0] > M[2][2])
            {
                const double S = std::sqrt(1.0 + M[0][0] - M[1][1] - M[2][2]) * 2.0;
                Q.W = (M[2][1] - M[1][2]) / S;
                Q.X = 0.25 * S;
                Q.Y = (M[0][1] + M[1][0]) / S;
                Q.Z = (M[0][2] + M[2][0]) / S;
            }
            else if(M[1][1] > M[2][2])
            {
                const double S = std::sqrt(1.0 + M[1][1] - M[0][0] - M[2][2]) * 2.0;
                Q.W = (M[0][2] - M[2][0]) / S;
                Q.X = (M[0][1] + M[1][0]) / S;
                Q.Y = 0.25 * S;
                Q.Z = (M[1][2] + M[2][1]) / S;
            }
            else
            {
                const double S = std::sqrt(1.0 + M[2][2] - M[0][0] - M[1][1]) * 2.0;
                Q.W = (M[1][0] - M[0][1]) / S;
                Q.X = (M[0][2] + M[2][0]) / S;
                Q.Y = (M[1][2] + M[2][1]) / S;
                Q.Z = 0.25 * S;
            }
            return Q;
        }

        GameCore::Rotation3 FromQuat(const Quat& InQ)
        {
            const double W = InQ.W, X = InQ.X, Y = InQ.Y, Z = InQ.Z;
            GameCore::Rotation3 R;
            R.M[0][0] = 1 - 2 * (Y * Y + Z * Z); R.M[0][1] = 2 * (X * Y - Z * W);     R.M[0][2] = 2 * (X * Z + Y * W);
            R.M[1][0] = 2 * (X * Y + Z * W);     R.M[1][1] = 1 - 2 * (X * X + Z * Z); R.M[1][2] = 2 * (Y * Z - X * W);
            R.M[2][0] = 2 * (X * Z - Y * W);     R.M[2][1] = 2 * (Y * Z + X * W);     R.M[2][2] = 1 - 2 * (X * X + Y * Y);
            return R;
        }

        Quat Slerp(const Quat& InA, Quat InB, const double InT)
        {
            double Dot = InA.W * InB.W + InA.X * InB.X + InA.Y * InB.Y + InA.Z * InB.Z;
            if(Dot < 0.0) { InB = { -InB.W, -InB.X, -InB.Y, -InB.Z }; Dot = -Dot; }
            double WA = 1.0 - InT, WB = InT;
            if(Dot < 0.9995)
            {
                const double Theta = std::acos(std::clamp(Dot, -1.0, 1.0));
                const double Sin   = std::sin(Theta);
                WA = std::sin((1.0 - InT) * Theta) / Sin;
                WB = std::sin(InT * Theta) / Sin;
            }
            Quat Q{ WA * InA.W + WB * InB.W, WA * InA.X + WB * InB.X, WA * InA.Y + WB * InB.Y, WA * InA.Z + WB * InB.Z };
            const double Length = std::sqrt(Q.W * Q.W + Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z);
            if(Length > 0.0) { Q.W /= Length; Q.X /= Length; Q.Y /= Length; Q.Z /= Length; }
            return Q;
        }

        GameCore::TransformPose BlendPoses(const GameCore::TransformPose& InA, const GameCore::TransformPose& InB, const float InT)
        {
            using namespace GameCore::TransformMath;
            GameCore::TransformPose Out;
            Out.Position = InA.Position + (InB.Position - InA.Position) * InT;
            Out.Scale    = InA.Scale + (InB.Scale - InA.Scale) * InT;
            const Quat Q = Slerp(ToQuat(EulerToRotation(InA.Rotation)), ToQuat(EulerToRotation(InB.Rotation)), double(InT));
            Out.Rotation = RotationToEuler(FromQuat(Q), InB.Rotation);
            return Out;
        }

        float SmoothStep(const float InT)
        {
            const float T = std::clamp(InT, 0.0f, 1.0f);
            return T * T * (3.0f - 2.0f * T);
        }
    }

    // === Post process ========================================================

    void SequencePostProcess::Append(const SequencePostProcess& InOther)
    {
        Stacks.insert(Stacks.end(), InOther.Stacks.begin(), InOther.Stacks.end());
        for(const Setting& Each : InOther.Settings)
        {
            auto Found = std::find_if(Settings.begin(), Settings.end(), [&](const Setting& InMine) { return InMine.Name == Each.Name; });
            if(Found != Settings.end()) { Found->Value = Each.Value; }
            else                        { Settings.push_back(Each); }
        }
    }

    bool SequencePostProcess::operator==(const SequencePostProcess& InOther) const
    {
        if(Stacks.size() != InOther.Stacks.size() || Settings.size() != InOther.Settings.size()) { return false; }
        for(size_t Index = 0; Index < Stacks.size(); ++Index)
        {
            if(Stacks[Index].Path != InOther.Stacks[Index].Path || Stacks[Index].Weight != InOther.Stacks[Index].Weight) { return false; }
        }
        for(size_t Index = 0; Index < Settings.size(); ++Index)
        {
            if(Settings[Index].Name != InOther.Settings[Index].Name || Settings[Index].Value != InOther.Settings[Index].Value) { return false; }
        }
        return true;
    }

    void PushPostProcess(const GameCore::World& InWorld, const SequencePostProcess& InPost)
    {
        RenderCore::PostProcessOverride Override;
        for(const SequencePostProcess::Stack& Each : InPost.Stacks)
        {
            Override.Stacks.push_back({ Each.Path, Each.Weight });
        }
        for(const SequencePostProcess::Setting& Each : InPost.Settings)
        {
            RenderCore::EPostProcessParam Param;
            if(RenderCore::FindPostProcessParam(Each.Name.c_str(), Param)) { Override.Params.push_back({ Param, Each.Value }); }
        }
        RenderCore::SetPostProcessOverride(&InWorld, Override);
    }

    // === Lifetime ============================================================

    SequenceInstance::SequenceInstance(GameCore::World& InWorld, std::shared_ptr<const SequenceAsset> InAsset, std::string InPath,
                                       const SequenceInstanceOptions& InOptions)
        : World(InWorld), Asset(std::move(InAsset)), Path(std::move(InPath)), Options(InOptions), Alive(std::make_shared<bool>(true))
    {
        if(!Asset)
        {
            Asset = std::make_shared<SequenceAsset>();
        }
        Bindings.resize(Asset->Bindings.size());
        MasterTracks.resize(Asset->Tracks.size());
        for(size_t Index = 0; Index < Asset->Tracks.size(); ++Index)
        {
            MasterTracks[Index].Track   = &Asset->Tracks[Index];
            MasterTracks[Index].bUsable = true;
        }
    }

    SequenceInstance::~SequenceInstance()
    {
        *Alive = false;
        Release();
    }

    template<typename F>
    void SequenceInstance::ForEachTrack(F&& InFn)
    {
        for(BindingState& State : Bindings)
        {
            for(BoundTrack& Bound : State.Tracks) { InFn(State.Entity, Bound); }
        }
        for(BoundTrack& Bound : MasterTracks) { InFn(GameCore::EntityID(), Bound); }
    }

    // === Bindings ============================================================

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

        /* What the old entity had of ours goes before the new one is taken. */
        for(BoundTrack& Bound : State.Tracks)
        {
            if(Bound.bControlling) { HandBackAnimation(State.Entity, Bound); }
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
        else if(!Binding.Prefab.empty() && Options.bSpawn)
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

        if(!State.Entity.IsSet() && !State.bWarned && (Binding.Kind == ESequenceBindingKind::Possess || Options.bSpawn))
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
        /* Sounds and sub-sequences outlive a rebind only as far as Release
         * of the old ones. */
        for(BoundTrack& Old : InOutState.Tracks)
        {
            for(SoundVoice& Voice : Old.Voices)
            {
                if(Voice.Helper.IsSet() && Sounds().IsAlive(Voice.Helper)) { Sounds().DestroyEntity(Voice.Helper); }
            }
        }
        InOutState.Tracks.clear();
        InOutState.Tracks.reserve(InBinding.Tracks.size());
        for(const SequenceTrack& Track : InBinding.Tracks)
        {
            BoundTrack Bound;
            Bound.Track = &Track;
            if(Track.Type == ESequenceTrackType::Float || Track.Type == ESequenceTrackType::Bool)
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
            else
            {
                Bound.bUsable = true;
            }
            InOutState.Tracks.push_back(std::move(Bound));
        }
    }

    // === Evaluating ==========================================================

    void SequenceInstance::Evaluate(double InFrame)
    {
        InFrame = std::clamp(InFrame, 0.0, double(std::max(Asset->Length, 0)));
        Fade = -1.0f;
        Shot = SequenceShot();
        PostProcess = SequencePostProcess();
        BusGains.clear();
        Subtitle = SequenceSubtitle();

        for(size_t Index = 0; Index < Bindings.size(); ++Index)
        {
            const GameCore::EntityID Entity = ResolveBinding(Index);
            const bool bThere = Entity.IsSet() && World.IsAlive(Entity);
            for(BoundTrack& Bound : Bindings[Index].Tracks)
            {
                if(!Bound.bUsable || Bound.Track->bMuted) { continue; }
                if(bThere || Bound.Track->Type == ESequenceTrackType::SubSequence)
                {
                    EvaluateTrack(Entity, Bound, InFrame);
                }
            }
        }
        for(BoundTrack& Bound : MasterTracks)
        {
            if(!Bound.Track->bMuted) { EvaluateTrack(GameCore::EntityID(), Bound, InFrame); }
        }

        /* The camera entity shows the shot; none, and it goes. */
        if(Options.bCameraEntity)
        {
            if(Shot.bValid)
            {
                if(!CameraEntity.IsSet() || !World.IsAlive(CameraEntity))
                {
                    GameCore::Entity Made = World.CreateEntity("Sequence Camera");
                    Made.AddComponent(GameCore::TransformComponent());
                    RenderCore::CameraComponent Camera;
                    Camera.Projection = RenderCore::CP_Perspective;
                    Made.AddComponent(Camera);
                    CameraEntity = Made.GetID();
                }
                GameCore::Entity Camera = World.GetEntity(CameraEntity);
                Camera.SetWorldPose(Shot.Pose);
                if(RenderCore::CameraComponent* Component = Camera.GetComponent<RenderCore::CameraComponent>())
                {
                    Component->FovY  = Shot.FovY;
                    Component->ZNear = Shot.ZNear;
                    Component->ZFar  = Shot.ZFar;
                }
            }
            else if(CameraEntity.IsSet())
            {
                if(World.IsAlive(CameraEntity)) { World.DestroyEntity(CameraEntity); }
                CameraEntity = GameCore::EntityID();
            }
        }
    }

    void SequenceInstance::EvaluateTrack(const GameCore::EntityID InEntity, BoundTrack& InOutBound, const double InFrame)
    {
        const SequenceTrack& Track = *InOutBound.Track;
        switch(Track.Type)
        {
            case ESequenceTrackType::Transform: WriteTransform(InEntity, Track, InFrame); break;
            case ESequenceTrackType::Float:
            case ESequenceTrackType::Bool:      WriteField(InEntity, InOutBound, InFrame); break;
            case ESequenceTrackType::Animation: WriteAnimation(InEntity, InOutBound, InFrame); break;
            case ESequenceTrackType::CameraCut: if(!Shot.bValid) { EvaluateShot(Track, InFrame); } break;
            case ESequenceTrackType::Fade:
                if(Fade < 0.0f && !Track.Channels.empty() && !Track.Channels[0].IsEmpty())
                {
                    Fade = std::clamp(Track.Channels[0].Evaluate(InFrame, 0.0f), 0.0f, 1.0f);
                }
                break;
            case ESequenceTrackType::SubSequence: EvaluateSubs(InOutBound, InFrame); break;
            case ESequenceTrackType::PostProcess:
                /* Each section under the playhead, at its weight times its fades. */
                for(const SequenceSection& Section : Track.Sections)
                {
                    if(!Section.Contains(InFrame) || Section.Asset.empty()) { continue; }
                    float Envelope = 1.0f;
                    if(Section.BlendIn > 0)  { Envelope = std::min(Envelope, float((InFrame - double(Section.Start)) / double(Section.BlendIn))); }
                    if(Section.BlendOut > 0) { Envelope = std::min(Envelope, float((double(Section.End) - InFrame) / double(Section.BlendOut))); }
                    const float Weight = std::clamp(Section.Volume * std::clamp(Envelope, 0.0f, 1.0f), 0.0f, 1.0f);
                    if(Weight > 0.0f) { PostProcess.Stacks.push_back({ Section.Asset, Weight }); }
                }
                break;
            case ESequenceTrackType::BusVolume:
                if(!Track.Channels.empty() && !Track.Channels[0].IsEmpty() && FindSequenceBus(Track.Field.c_str()) >= 0)
                {
                    MultiplyGain(Track.Field, std::max(0.0f, Track.Channels[0].Evaluate(InFrame, 1.0f)));
                }
                break;
            case ESequenceTrackType::Subtitle:
                /* The latest line under the playhead. */
                for(const SequenceSection& Section : Track.Sections)
                {
                    if(Section.Contains(InFrame) && !Section.Asset.empty())
                    {
                        Subtitle.Speaker = Section.Speaker;
                        Subtitle.Text    = Section.Asset;
                    }
                }
                break;
            case ESequenceTrackType::PostProcessSetting:
                if(!Track.Channels.empty() && !Track.Channels[0].IsEmpty() && !Track.Field.empty())
                {
                    SequencePostProcess Setting;
                    Setting.Settings.push_back({ Track.Field, Track.Channels[0].Evaluate(InFrame, 0.0f) });
                    PostProcess.Append(Setting);
                }
                break;
            case ESequenceTrackType::Event:
            case ESequenceTrackType::Audio:
            case ESequenceTrackType::Count:     break;
        }
    }

    void SequenceInstance::WriteTransform(const GameCore::EntityID InEntity, const SequenceTrack& InTrack, const double InFrame)
    {
        GameCore::TransformComponent* Transform = World.GetEntity(InEntity).GetComponent<GameCore::TransformComponent>();
        if(!Transform) { return; }

        bool bAny = false;
        for(const SequenceChannel& Channel : InTrack.Channels) { bAny = bAny || !Channel.IsEmpty(); }
        if(!bAny) { return; }

        HoldRigidBody(InEntity);
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

    void SequenceInstance::HoldRigidBody(const GameCore::EntityID InEntity)
    {
        GameCore::RigidBodyComponent* Body = World.GetEntity(InEntity).GetComponent<GameCore::RigidBodyComponent>();
        if(!Body || Body->Motion != GameCore::RBM_Dynamic) { return; }
        Save(InEntity, GameCore::RigidBodyComponent::StaticType().ID, uint32_t(offsetof(GameCore::RigidBodyComponent, Motion)),
             sizeof(Body->Motion), &Body->Motion, true);
        Body->Motion = GameCore::RBM_Kinematic;
    }

    void SequenceInstance::WriteField(const GameCore::EntityID InEntity, const BoundTrack& InBound, const double InFrame)
    {
        if(InBound.Track->Channels.empty() || InBound.Track->Channels[0].IsEmpty()) { return; }
        auto* Component = static_cast<uint8_t*>(World.GetEntity(InEntity).GetComponentByType(InBound.Type));
        if(!Component) { return; }

        const auto Type = GameCore::EScriptFieldType(InBound.FieldType);
        uint8_t*   At   = Component + InBound.Offset;
        Save(InEntity, InBound.Type, InBound.Offset, FieldSize(Type), At);
        WriteFieldValue(At, Type, InBound.Track->Channels[0].Evaluate(InFrame, ReadField(At, Type)));
    }

    // --- Animation ---------------------------------------------------------------

    void SequenceInstance::WriteAnimation(const GameCore::EntityID InEntity, BoundTrack& InOutBound, const double InFrame)
    {
        const SequenceTrack& Track = *InOutBound.Track;
        InOutBound.Clips.resize(Track.Sections.size());

        /* The earliest section under the playhead is the base; the latest,
         * while it blends in, the overlay. */
        int32_t Base = -1, Over = -1;
        for(size_t Index = 0; Index < Track.Sections.size(); ++Index)
        {
            if(!Track.Sections[Index].Contains(InFrame)) { continue; }
            if(Base < 0) { Base = int32_t(Index); }
            Over = int32_t(Index);
        }
        if(Base < 0)
        {
            if(InOutBound.bControlling) { HandBackAnimation(InEntity, InOutBound); }
            return;
        }

        RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(&World);
        RenderCore::AnimationComponent* Component = World.GetEntity(InEntity).GetComponent<RenderCore::AnimationComponent>();
        if(!Animation || !Component) { return; }
        RenderCore::AnimInstance* Instance = Animation->FindInstance(InEntity);
        if(!Instance)
        {
            Animation->Sync();   // made this frame: the system has not seen it yet
            Instance = Animation->FindInstance(InEntity);
            if(!Instance) { return; }
        }

        const auto ClipOf = [&](const int32_t InSection) -> const std::shared_ptr<const RenderCore::AnimClipData>&
        {
            std::shared_ptr<const RenderCore::AnimClipData>& Clip = InOutBound.Clips[size_t(InSection)];
            if(!Clip) { Clip = RenderCore::AnimClipAssetCache::Find(Track.Sections[size_t(InSection)].Asset); }
            return Clip;
        };
        const auto TimeIn = [&](const int32_t InSection, const RenderCore::AnimClipData& InClip) -> float
        {
            const SequenceSection& Section = Track.Sections[size_t(InSection)];
            const double Seconds  = Section.AssetSeconds(InFrame, Asset->FrameRate);
            const double Duration = double(InClip.GetDuration());
            if(Duration <= 0.0) { return 0.0f; }
            return float(Section.bLoop ? std::fmod(Seconds, Duration) : std::min(Seconds, Duration));
        };

        float Weight = 1.0f;
        if(Over != Base)
        {
            const SequenceSection& Section = Track.Sections[size_t(Over)];
            Weight = Section.BlendIn > 0 ? std::clamp(float((InFrame - double(Section.Start)) / double(Section.BlendIn)), 0.0f, 1.0f) : 1.0f;
            if(Weight >= 1.0f) { Base = Over; }
        }

        const std::shared_ptr<const RenderCore::AnimClipData>& BaseClip = ClipOf(Base);
        if(!BaseClip) { return; }

        if(!InOutBound.bControlling)
        {
            InOutBound.bControlling = true;
            InOutBound.SavedPaused  = Component->bPaused;
        }
        /* Paused, the system's own advance moves nothing: the time is ours. */
        Component->bPaused = 1;

        if(Instance->GetClip() != BaseClip || Instance->GetState())
        {
            Instance->PlayClip(BaseClip);
            Instance->SetLoopOverride(1);
        }
        if(Over != Base && Weight < 1.0f && ClipOf(Over))
        {
            Instance->SetOverlayClip(ClipOf(Over), TimeIn(Over, *ClipOf(Over)), Weight);
        }
        else
        {
            Instance->SetOverlayClip(nullptr, 0.0f, 0.0f);
        }
        Instance->SetTime(TimeIn(Base, *BaseClip));   // evaluates
    }

    void SequenceInstance::HandBackAnimation(const GameCore::EntityID InEntity, BoundTrack& InOutBound)
    {
        InOutBound.bControlling = false;
        if(!InEntity.IsSet() || !World.IsAlive(InEntity)) { return; }
        RenderCore::AnimationComponent* Component = World.GetEntity(InEntity).GetComponent<RenderCore::AnimationComponent>();
        RenderCore::AnimationSystem*    Animation = RenderCore::AnimationSystem::Find(&World);
        if(!Component) { return; }
        Component->bPaused = InOutBound.SavedPaused;

        RenderCore::AnimInstance* Instance = Animation ? Animation->FindInstance(InEntity) : nullptr;
        if(!Instance) { return; }
        Instance->SetOverlayClip(nullptr, 0.0f, 0.0f);
        Instance->SetLoopOverride(-1);

        /* What its component plays, from the start, as the system would. */
        const std::string Own(Component->Animation, strnlen(Component->Animation, RenderCore::ANIMATION_PATH_MAX));
        if(EndsWith(Own, RenderCore::ANIM_STATE_EXTENSION))
        {
            Instance->SetState(RenderCore::AnimStateAssetCache::Find(Own));
        }
        else
        {
            Instance->PlayClip(Own.empty() ? nullptr : RenderCore::AnimClipAssetCache::Find(Own));
        }
    }

    // --- Camera ------------------------------------------------------------------

    void SequenceInstance::EvaluateShot(const SequenceTrack& InTrack, const double InFrame)
    {
        /* The latest section under the playhead; the one before it, if it
         * blends in from one. */
        int32_t Current = -1;
        for(size_t Index = 0; Index < InTrack.Sections.size(); ++Index)
        {
            if(InTrack.Sections[Index].Contains(InFrame)) { Current = int32_t(Index); }
        }
        /* The last frame shows the last shot. */
        if(Current < 0 && !InTrack.Sections.empty() && InFrame >= double(Asset->Length) && InTrack.Sections.back().End >= Asset->Length)
        {
            Current = int32_t(InTrack.Sections.size()) - 1;
        }
        if(Current < 0) { return; }

        const auto CameraOf = [this](const SequenceSection& InSection) -> GameCore::EntityID
        {
            const int32_t Binding = Asset->FindBinding(InSection.Asset);
            const GameCore::EntityID Entity = Binding >= 0 ? ResolveBinding(size_t(Binding)) : GameCore::EntityID();
            return Entity.IsSet() && World.IsAlive(Entity) ? Entity : GameCore::EntityID();
        };
        const auto ViewOf = [this](const GameCore::EntityID InCamera, SequenceShot& OutShot)
        {
            const GameCore::Entity Entity = World.GetEntity(InCamera);
            OutShot.Pose = Entity.GetWorldPose();
            if(const RenderCore::CameraComponent* Camera = Entity.GetComponent<RenderCore::CameraComponent>())
            {
                OutShot.FovY  = Camera->FovY;
                OutShot.ZNear = Camera->ZNear;
                OutShot.ZFar  = Camera->ZFar;
            }
        };

        const SequenceSection& Section = InTrack.Sections[size_t(Current)];
        const GameCore::EntityID To = CameraOf(Section);
        if(!To.IsSet()) { return; }
        Shot.bValid = true;
        Shot.To     = To;
        ViewOf(To, Shot);

        if(Section.BlendIn > 0 && Current > 0 && InFrame < double(Section.Start + Section.BlendIn))
        {
            const GameCore::EntityID From = CameraOf(InTrack.Sections[size_t(Current - 1)]);
            if(From.IsSet() && From != To)
            {
                SequenceShot Start;
                ViewOf(From, Start);
                const float T = SmoothStep(float((InFrame - double(Section.Start)) / double(Section.BlendIn)));
                Shot.From  = From;
                Shot.Blend = T;
                Shot.Pose  = BlendPoses(Start.Pose, Shot.Pose, T);
                Shot.FovY  = Start.FovY + (Shot.FovY - Start.FovY) * T;
                Shot.ZNear = Start.ZNear + (Shot.ZNear - Start.ZNear) * T;
                Shot.ZFar  = Start.ZFar + (Shot.ZFar - Start.ZFar) * T;
            }
        }
    }

    GameCore::EntityID SequenceInstance::GetCameraEntity() const
    {
        return CameraEntity.IsSet() && World.IsAlive(CameraEntity) ? CameraEntity : GameCore::EntityID();
    }

    // --- Sub-sequences -------------------------------------------------------------

    SequenceInstance* SequenceInstance::GetSub(BoundTrack& InOutBound, const size_t InSection)
    {
        const SequenceTrack& Track = *InOutBound.Track;
        InOutBound.Subs.resize(Track.Sections.size());
        std::unique_ptr<SequenceInstance>& Sub = InOutBound.Subs[InSection];
        if(Sub) { return Sub.get(); }
        if(Options.Depth + 1 > MAX_DEPTH)
        {
            return nullptr;
        }
        const std::string& SubPath = Track.Sections[InSection].Asset;
        std::shared_ptr<const SequenceAsset> SubAsset = SequenceAssetCache::Find(SubPath);
        if(!SubAsset) { return nullptr; }

        SequenceInstanceOptions SubOptions = Options;
        SubOptions.Depth = Options.Depth + 1;
        Sub = std::make_unique<SequenceInstance>(World, std::move(SubAsset), SubPath, SubOptions);

        /* A binding the sub-sequence shares by id plays the same entity. */
        for(size_t Index = 0; Index < Asset->Bindings.size(); ++Index)
        {
            const GameCore::EntityID Entity = ResolveBinding(Index);
            if(Entity.IsSet()) { Sub->SetBindingOverride(Asset->Bindings[Index].Id, Entity); }
        }
        return Sub.get();
    }

    double SequenceInstance::SubFrame(const SequenceSection& InSection, const SequenceInstance& InSub, const double InFrame) const
    {
        return InSection.AssetSeconds(InFrame, Asset->FrameRate) * double(InSub.GetAsset().FrameRate);
    }

    void SequenceInstance::EvaluateSubs(BoundTrack& InOutBound, const double InFrame)
    {
        const SequenceTrack& Track = *InOutBound.Track;
        for(size_t Index = 0; Index < Track.Sections.size(); ++Index)
        {
            if(!Track.Sections[Index].Contains(InFrame)) { continue; }
            SequenceInstance* Sub = GetSub(InOutBound, Index);
            if(!Sub) { continue; }
            Sub->Evaluate(SubFrame(Track.Sections[Index], *Sub, InFrame));
            if(Fade < 0.0f)  { Fade = Sub->GetFade(); }
            if(!Shot.bValid) { Shot = Sub->GetShot(); }
            PostProcess.Append(Sub->GetPostProcess());
            for(const SequenceBusGain& Gain : Sub->GetBusGains()) { MultiplyGain(Gain.Bus, Gain.Gain); }
            if(Subtitle.IsEmpty()) { Subtitle = Sub->GetSubtitle(); }
        }
    }

    // === Sounds ==============================================================

    void SequenceInstance::UpdatePlayback(double InFrame, const bool bInPlaying)
    {
        InFrame = std::clamp(InFrame, 0.0, double(std::max(Asset->Length, 0)));
        ForEachTrack([&](const GameCore::EntityID InOwner, BoundTrack& InOutBound)
        {
            if(InOutBound.Track->Type == ESequenceTrackType::Audio && Options.bAudio)
            {
                UpdateSounds(InOwner, InOutBound, InFrame, bInPlaying && !InOutBound.Track->bMuted);
            }
            else if(InOutBound.Track->Type == ESequenceTrackType::SubSequence)
            {
                for(size_t Index = 0; Index < InOutBound.Subs.size(); ++Index)
                {
                    SequenceInstance* Sub = InOutBound.Subs[Index].get();
                    if(!Sub) { continue; }
                    const SequenceSection& Section = InOutBound.Track->Sections[Index];
                    if(bJumped) { Sub->NoteJump(); }
                    if(Section.Contains(InFrame)) { Sub->UpdatePlayback(SubFrame(Section, *Sub, InFrame), bInPlaying); }
                    else                          { Sub->StopPlayback(); }
                }
            }
        });
        bJumped = false;
    }

    void SequenceInstance::UpdateSounds(const GameCore::EntityID InOwner, BoundTrack& InOutBound, const double InFrame, const bool bInPlaying)
    {
        const SequenceTrack& Track = *InOutBound.Track;
        InOutBound.Voices.resize(Track.Sections.size());
        for(size_t Index = 0; Index < Track.Sections.size(); ++Index)
        {
            const SequenceSection& Section = Track.Sections[Index];
            SoundVoice&            Voice   = InOutBound.Voices[Index];
            const bool             bInside = Section.Contains(InFrame) && !Section.Asset.empty();

            GameCore::World& SoundWorld = Sounds();
            GameCore::AudioSourceComponent* Source = Voice.Helper.IsSet() && SoundWorld.IsAlive(Voice.Helper)
                ? SoundWorld.GetEntity(Voice.Helper).GetComponent<GameCore::AudioSourceComponent>() : nullptr;

            if(!bInside)
            {
                if(Source && Voice.State != SoundVoice::EState::Idle) { Source->Stop(); }
                Voice.State = SoundVoice::EState::Idle;
                continue;
            }
            if(!bInPlaying)
            {
                if(Source && Voice.State == SoundVoice::EState::Playing) { Source->Pause(); Voice.State = SoundVoice::EState::Paused; }
                continue;
            }

            if(!Source)
            {
                /* A helper of its own: under the binding, so it follows it;
                 * at the origin for a track of no binding. */
                GameCore::Entity Helper = SoundWorld.CreateEntity("Sequence Sound");
                Helper.AddComponent(GameCore::TransformComponent());
                GameCore::AudioSourceComponent Init;
                Init.bAutoPlay = 0;
                std::snprintf(Init.Sound, sizeof(Init.Sound), "%s", Section.Asset.c_str());
                /* Its bus and 2D/3D as the section says; a track of no binding is heard 2D unless told otherwise. */
                Init.ForceBus     = Section.Bus >= 0 ? uint8_t(Section.Bus + 1) : 0;
                Init.ForceSpatial = Section.Spatial >= 0 ? uint8_t(Section.Spatial + 1) : (InOwner.IsSet() ? 0 : 1);
                Helper.AddComponent(Init);
                if(&SoundWorld == &World && InOwner.IsSet() && World.IsAlive(InOwner)) { Helper.SetParent(InOwner, false); }
                Voice.Helper = Helper.GetID();
                Voice.State  = SoundVoice::EState::Idle;
                Source = Helper.GetComponent<GameCore::AudioSourceComponent>();
            }

            /* In another World it cannot follow by parenting: placed where its binding is. */
            if(&SoundWorld != &World && InOwner.IsSet() && World.IsAlive(InOwner))
            {
                SoundWorld.GetEntity(Voice.Helper).SetWorldPose(World.GetEntity(InOwner).GetWorldPose());
            }

            /* Its fades, as a volume envelope. */
            float Envelope = 1.0f;
            if(Section.BlendIn > 0)  { Envelope = std::min(Envelope, float((InFrame - double(Section.Start)) / double(Section.BlendIn))); }
            if(Section.BlendOut > 0) { Envelope = std::min(Envelope, float((double(Section.End) - InFrame) / double(Section.BlendOut))); }
            Source->Volume = Section.Volume * std::clamp(Envelope, 0.0f, 1.0f);
            Source->Pitch  = Section.PlayRate;

            /* Where in the sound: source seconds, as its cursor and seeks are (Pitch plays it PlayRate fast). */
            const float Expected = float(Section.AssetSeconds(InFrame, Asset->FrameRate));
            switch(Voice.State)
            {
                case SoundVoice::EState::Idle:
                    Source->Play();
                    if(Expected > 0.05f) { Source->Seek(Expected); }
                    Voice.State = SoundVoice::EState::Playing;
                    break;
                case SoundVoice::EState::Paused:
                    Source->Resume();
                    Source->Seek(Expected);
                    Voice.State = SoundVoice::EState::Playing;
                    break;
                case SoundVoice::EState::Playing:
                    /* A jump, or drift past a quarter second: back in step. */
                    if(bJumped || (Source->bPlaying && Source->LengthSeconds > Expected
                                   && std::fabs(Source->PlaybackSeconds - Expected) > 0.25f))
                    {
                        Source->Seek(Expected);
                    }
                    break;
            }
        }
    }

    GameCore::World& SequenceInstance::Sounds() const
    {
        return Options.SoundWorld ? *Options.SoundWorld : World;
    }

    void SequenceInstance::MultiplyGain(const std::string& InBus, const float InGain)
    {
        for(SequenceBusGain& Each : BusGains)
        {
            if(Each.Bus == InBus) { Each.Gain *= InGain; return; }
        }
        BusGains.push_back({ InBus, InGain });
    }

    void SequenceInstance::ReleaseSounds()
    {
        StopPlayback();
        ForEachTrack([this](const GameCore::EntityID, BoundTrack& InOutBound)
        {
            for(SoundVoice& Voice : InOutBound.Voices)
            {
                if(Voice.Helper.IsSet() && Sounds().IsAlive(Voice.Helper)) { Sounds().DestroyEntity(Voice.Helper); }
            }
            InOutBound.Voices.clear();
            for(std::unique_ptr<SequenceInstance>& Sub : InOutBound.Subs)
            {
                if(Sub) { Sub->ReleaseSounds(); }
            }
        });
    }

    void SequenceInstance::SetAudioEnabled(const bool bInOn)
    {
        if(Options.bAudio == bInOn) { return; }
        Options.bAudio = bInOn;
        if(!bInOn) { ReleaseSounds(); }
        ForEachTrack([bInOn](const GameCore::EntityID, BoundTrack& InOutBound)
        {
            for(std::unique_ptr<SequenceInstance>& Sub : InOutBound.Subs)
            {
                if(Sub) { Sub->SetAudioEnabled(bInOn); }
            }
        });
    }

    void SequenceInstance::NoteJump()
    {
        bJumped = true;
    }

    void SequenceInstance::StopPlayback()
    {
        ForEachTrack([&](const GameCore::EntityID, BoundTrack& InOutBound)
        {
            for(SoundVoice& Voice : InOutBound.Voices)
            {
                if(Voice.State != SoundVoice::EState::Idle && Voice.Helper.IsSet() && Sounds().IsAlive(Voice.Helper))
                {
                    if(auto* Source = Sounds().GetEntity(Voice.Helper).GetComponent<GameCore::AudioSourceComponent>()) { Source->Stop(); }
                }
                Voice.State = SoundVoice::EState::Idle;
            }
            for(std::unique_ptr<SequenceInstance>& Sub : InOutBound.Subs)
            {
                if(Sub) { Sub->StopPlayback(); }
            }
        });
    }

    // === Events ==============================================================

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
        std::vector<std::pair<SequenceInstance*, std::pair<double, double>>> SubRanges;

        const auto Collect = [&](const SequenceTrack& InTrack, BoundTrack* InBound)
        {
            if(InTrack.bMuted) { return; }
            if(InTrack.Type == ESequenceTrackType::SubSequence && InBound)
            {
                for(size_t Index = 0; Index < InTrack.Sections.size(); ++Index)
                {
                    const SequenceSection& Section = InTrack.Sections[Index];
                    const double From = std::max(InFrom, double(Section.Start));
                    const double To   = std::min(InTo, double(Section.End));
                    if(To < From || (To == From && !bInIncludeFrom && From == InFrom)) { continue; }
                    if(SequenceInstance* Sub = GetSub(*InBound, Index))
                    {
                        SubRanges.push_back({ Sub, { SubFrame(Section, *Sub, From), SubFrame(Section, *Sub, To) } });
                    }
                }
                return;
            }
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
        for(size_t Index = 0; Index < Bindings.size(); ++Index)
        {
            ResolveBinding(Index);
            for(BoundTrack& Bound : Bindings[Index].Tracks) { Collect(*Bound.Track, &Bound); }
        }
        for(BoundTrack& Bound : MasterTracks) { Collect(*Bound.Track, &Bound); }

        std::stable_sort(Fire.begin(), Fire.end(), [](const Pending& L, const Pending& R) { return L.Frame < R.Frame; });

        /* A handler may stop or restart the player that owns this instance,
         * destroying it: from here only locals are touched, and sub-sequences
         * only while it lives. */
        const std::shared_ptr<bool> StillAlive = Alive;
        GameCore::EventBus& Bus = World.GetEvents();
        for(const Pending& Event : Fire)
        {
            Bus.Emit(Event.Name.c_str(), Event.Args);
        }
        for(const auto& Range : SubRanges)
        {
            if(!*StillAlive) { return; }
            Range.first->FireEvents(Range.second.first, Range.second.second, bInIncludeFrom, bInSkipOnly);
        }
    }

    // === Saved values ==========================================================

    void SequenceInstance::Save(const GameCore::EntityID InEntity, const GameCore::ComponentTypeID InType, const uint32_t InOffset,
                                const uint32_t InSize, const void* InNow, const bool bInHeld)
    {
        const SavedKey Key{ InEntity.Pack(), InType, InOffset };
        if(SavedIndex.count(Key) || InSize > sizeof(SavedValue::Bytes)) { return; }
        SavedValue Value;
        Value.Entity = InEntity;
        Value.Type   = InType;
        Value.Offset = InOffset;
        Value.Size   = InSize;
        Value.bHeld  = bInHeld;
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

    void SequenceInstance::WriteSaved(const SavedValue& InValue)
    {
        if(!World.IsAlive(InValue.Entity)) { return; }
        const GameCore::Entity Entity = World.GetEntity(InValue.Entity);
        if(InValue.Offset == TRANSFORM_OFFSET)
        {
            if(GameCore::TransformComponent* Transform = Entity.GetComponent<GameCore::TransformComponent>())
            {
                Transform->SetPose(InValue.Pose);
            }
        }
        else if(auto* Component = static_cast<uint8_t*>(Entity.GetComponentByType(InValue.Type)))
        {
            std::memcpy(Component + InValue.Offset, InValue.Bytes, InValue.Size);
        }
    }

    void SequenceInstance::RestoreState()
    {
        /* Newest first, so where two saves overlap the oldest value wins. */
        for(auto It = Saved.rbegin(); It != Saved.rend(); ++It) { WriteSaved(*It); }
        Saved.clear();
        SavedIndex.clear();
        ForEachTrack([](const GameCore::EntityID, BoundTrack& InOutBound)
        {
            for(std::unique_ptr<SequenceInstance>& Sub : InOutBound.Subs)
            {
                if(Sub) { Sub->RestoreState(); }
            }
        });
    }

    void SequenceInstance::DiscardState()
    {
        for(auto It = Saved.rbegin(); It != Saved.rend(); ++It)
        {
            if(It->bHeld) { WriteSaved(*It); }
        }
        Saved.clear();
        SavedIndex.clear();
        ForEachTrack([](const GameCore::EntityID, BoundTrack& InOutBound)
        {
            for(std::unique_ptr<SequenceInstance>& Sub : InOutBound.Subs)
            {
                if(Sub) { Sub->DiscardState(); }
            }
        });
    }

    // === Letting go ============================================================

    void SequenceInstance::Release()
    {
        StopPlayback();
        ForEachTrack([&](const GameCore::EntityID InOwner, BoundTrack& InOutBound)
        {
            if(InOutBound.bControlling) { HandBackAnimation(InOwner, InOutBound); }
            for(SoundVoice& Voice : InOutBound.Voices)
            {
                if(Voice.Helper.IsSet() && Sounds().IsAlive(Voice.Helper)) { Sounds().DestroyEntity(Voice.Helper); }
            }
            InOutBound.Voices.clear();
            for(std::unique_ptr<SequenceInstance>& Sub : InOutBound.Subs)
            {
                if(Sub) { Sub->Release(); }
            }
        });

        /* What is held goes back now; the rest waits for Restore or Discard. */
        for(auto It = Saved.rbegin(); It != Saved.rend(); ++It)
        {
            if(It->bHeld) { WriteSaved(*It); }
        }
        Saved.erase(std::remove_if(Saved.begin(), Saved.end(), [](const SavedValue& InValue) { return InValue.bHeld; }), Saved.end());
        SavedIndex.clear();
        for(size_t Index = 0; Index < Saved.size(); ++Index)
        {
            SavedIndex.emplace(SavedKey{ Saved[Index].Entity.Pack(), Saved[Index].Type, Saved[Index].Offset }, Index);
        }

        if(CameraEntity.IsSet())
        {
            if(World.IsAlive(CameraEntity)) { World.DestroyEntity(CameraEntity); }
            CameraEntity = GameCore::EntityID();
        }
        Shot = SequenceShot();
        Fade = -1.0f;
        PostProcess = SequencePostProcess();
        BusGains.clear();
        Subtitle = SequenceSubtitle();
        ReleaseSpawned();
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
}
