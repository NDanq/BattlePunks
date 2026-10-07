#include "Public/RenderCore/AnimationSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreJob.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/ParticleSystemAsset.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/RenderCore/SkeletonAsset.h"

/* The animation budget (ANIMATION.md, "Budget"): past FullRateDistance from
 * every view a character updates every (distance / FullRateDistance)th tick,
 * at most MaxSkipFrames skipped; past FreezeDistance its time passes but its
 * pose is not evaluated. 0 turns either off. A world with no view -- a
 * server -- runs everything at full rate. */
NIMBLE_CONFIG_FLOAT(GAnimationFullRateDistance, "Animation", "FullRateDistance", 3000.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GAnimationFreezeDistance, "Animation", "FreezeDistance", 20000.0f, nullptr);
NIMBLE_CONFIG_INT(GAnimationMaxSkipFrames, "Animation", "MaxSkipFrames", 3, nullptr);
/* Outside every view (behind the camera, off to the side): updated every
 * (OffscreenSkipFrames + 1)th tick, the time saved up; its pose still made
 * then, for its shadow and its sockets. 0 turns it off. */
NIMBLE_CONFIG_INT(GAnimationOffscreenSkipFrames, "Animation", "OffscreenSkipFrames", 3, nullptr);
/* At least this many updating in a tick, and they run across the job system. */
NIMBLE_CONFIG_INT(GAnimationParallelMin, "Animation", "ParallelMin", 16, nullptr);

namespace RenderCore
{
    namespace
    {
        /* The game thread's only: no lock. */
        std::unordered_map<const GameCore::World*, AnimationSystem*>& GetSystems()
        {
            static std::unordered_map<const GameCore::World*, AnimationSystem*> Systems;
            return Systems;
        }

        bool EndsWith(const std::string& InText, const char* InSuffix)
        {
            const size_t Length = std::strlen(InSuffix);
            return InText.size() >= Length && InText.compare(InText.size() - Length, Length, InSuffix) == 0;
        }

        /* A bone's model-space matrix as a TransformComponent would say it. */
        GameCore::TransformPose ToTransformPose(const AnimMatrix& InMatrix, const EngineCore::Vector3& InNear = EngineCore::Vector3())
        {
            GameCore::TransformPose Pose;
            Pose.Position = EngineCore::Vector3(InMatrix.M[0][3], InMatrix.M[1][3], InMatrix.M[2][3]);
            double Scale[3];
            GameCore::Rotation3 Rotation;
            for(int Col = 0; Col < 3; ++Col)
            {
                Scale[Col] = std::sqrt(double(InMatrix.M[0][Col]) * InMatrix.M[0][Col] + double(InMatrix.M[1][Col]) * InMatrix.M[1][Col]
                                       + double(InMatrix.M[2][Col]) * InMatrix.M[2][Col]);
                for(int Row = 0; Row < 3; ++Row)
                {
                    Rotation.M[Row][Col] = Scale[Col] > 1e-12 ? InMatrix.M[Row][Col] / Scale[Col] : (Row == Col ? 1.0 : 0.0);
                }
            }
            Pose.Rotation = GameCore::TransformMath::RotationToEuler(Rotation, InNear);
            Pose.Scale    = EngineCore::Vector3(Scale[0], Scale[1], Scale[2]);
            return Pose;
        }
    }

    AnimationSystem::AnimationSystem(GameCore::World& InWorld) : World(InWorld)
    {
    }

    AnimationSystem::~AnimationSystem() = default;

    AnimationSystem* AnimationSystem::Attach(GameCore::World& InWorld)
    {
        AnimationSystem*& System = GetSystems()[&InWorld];
        if(!System)
        {
            System = new AnimationSystem(InWorld);
            InWorld.RegisterSystem("RenderCore.Animation", GameCore::TP_PostUpdate, &AnimationSystem::TickSystem, System);
        }
        ++System->Attachments;
        return System;
    }

    void AnimationSystem::Detach(GameCore::World& InWorld)
    {
        auto& Systems = GetSystems();
        const auto Found = Systems.find(&InWorld);
        if(Found == Systems.end())
        {
            return;
        }
        AnimationSystem* System = Found->second;
        if(--System->Attachments > 0)
        {
            return;
        }
        InWorld.UnregisterSystem(&AnimationSystem::TickSystem, System);
        Systems.erase(Found);
        delete System;
    }

    AnimationSystem* AnimationSystem::Find(const GameCore::World* InWorld)
    {
        auto& Systems = GetSystems();
        const auto Found = Systems.find(InWorld);
        return Found != Systems.end() ? Found->second : nullptr;
    }

    void AnimationSystem::TickSystem(GameCore::World& InWorld, const float InDeltaSeconds, void* InUserData)
    {
        (void)InWorld;
        static_cast<AnimationSystem*>(InUserData)->Tick(InDeltaSeconds);
    }

    void AnimationSystem::Sync()
    {
        ++SyncSerial;
        World.Each<AnimationComponent>([this](const GameCore::Entity& InEntity, AnimationComponent& InComponent)
        {
            Entry& Each = Entries[InEntity.GetID().Pack()];
            Each.Seen = SyncSerial;

            /* What it should play, by the assets' current loads: a reimport
             * or an edit hands back a new pointer, and it rebinds. */
            const std::string Path(InComponent.Animation, strnlen(InComponent.Animation, ANIMATION_PATH_MAX));
            std::shared_ptr<const MeshData> Mesh;
            if(const RenderComponent* Render = InEntity.GetComponent<RenderComponent>())
            {
                const std::string MeshPath(Render->Mesh, strnlen(Render->Mesh, RENDER_MESH_PATH_MAX));
                Mesh = MeshPath.empty() ? nullptr : MeshAssetCache::Find(MeshPath);
            }
            std::shared_ptr<const SkeletonData> Skeleton = Mesh && Mesh->IsSkinned() ? SkeletonAssetCache::Find(Mesh->Skeleton) : nullptr;
            std::shared_ptr<const AnimClipData>  Clip  = EndsWith(Path, ".nmanim") ? AnimClipAssetCache::Find(Path) : nullptr;
            std::shared_ptr<const AnimStateData> State = EndsWith(Path, ANIM_STATE_EXTENSION) ? AnimStateAssetCache::Find(Path) : nullptr;

            /* An anim state names its clips: one reimported, it binds again. */
            const uint64_t ClipGeneration = State ? AnimClipAssetCache::GetGeneration() : 0;
            if(Each.Instance && Each.AnimationPath == Path && Each.Mesh == Mesh && Each.Skeleton == Skeleton && Each.Clip == Clip
               && Each.State == State && Each.ClipGeneration == ClipGeneration)
            {
                return;
            }
            if(!Each.Instance)
            {
                Each.Instance = std::make_unique<AnimInstance>();
            }
            if(!Path.empty() && !EndsWith(Path, ".nmanim") && !EndsWith(Path, ".nmanimstate") && Each.AnimationPath != Path)
            {
                RENDERCORE_LOG_WARN(std::string("Animation on '") + InEntity.GetName() + "': '" + Path
                                    + "' is neither an .nmanim nor an .nmanimstate")
            }
            Each.AnimationPath = Path;
            Each.Mesh          = Mesh;
            Each.Skeleton      = Skeleton;
            Each.Clip          = Clip;
            Each.State         = State;
            Each.ClipGeneration = ClipGeneration;
            Each.Instance->SetSkeleton(Skeleton);
            if(State) { Each.Instance->SetState(State); }
            else      { Each.Instance->PlayClip(Clip); }
            ++Generation;
        });

        /* Gone: the component, or the entity. */
        for(auto It = Entries.begin(); It != Entries.end(); )
        {
            It = It->second.Seen != SyncSerial ? Entries.erase(It) : std::next(It);
        }
    }

    void AnimationSystem::Tick(const float InDeltaSeconds)
    {
        bTicked = true;
        Advance(InDeltaSeconds, true);
    }

    void AnimationSystem::PrepareFrame(const uint64_t InFrame, const float InDeltaSeconds, const bool bInPreview)
    {
        if(InFrame == PreparedFrame)
        {
            return;
        }
        PreparedFrame = InFrame;
        if(bTicked)
        {
            bTicked = false;
            return;
        }
        if(bInPreview) { Advance(InDeltaSeconds, false); }
        else           { Sync(); }
    }

    void AnimationSystem::AddViewOrigin(const uint64_t InFrame, const EngineCore::Vector3& InOrigin)
    {
        if(InFrame != ViewFrame)
        {
            ViewFrame = InFrame;
            ViewOrigins.clear();
            ViewFrustums.clear();
        }
        ViewOrigins.push_back(InOrigin);
        ViewFrustums.push_back(ViewFrustum());
    }

    void AnimationSystem::AddView(const uint64_t InFrame, const EngineCore::Vector3& InOrigin, const EngineCore::Matrix4x4& InViewProjection)
    {
        AddViewOrigin(InFrame, InOrigin);

        /* The six planes from the clip matrix's rows (Gribb and Hartmann):
         * w +- x, w +- y, w +- z, each inward, made unit. */
        ViewFrustum& Frustum = ViewFrustums.back();
        const auto Row = [&InViewProjection](const int InRow, const int InColumn) { return float(InViewProjection.Data[InRow][InColumn]); };
        for(int Plane = 0; Plane < 6; ++Plane)
        {
            const int   Axis = Plane / 2;
            const float Sign = (Plane % 2 == 0) ? 1.0f : -1.0f;
            float* Out = Frustum.Planes[Plane];
            for(int Column = 0; Column < 4; ++Column) { Out[Column] = Row(3, Column) + Sign * Row(Axis, Column); }
            const float Length = std::sqrt(Out[0] * Out[0] + Out[1] * Out[1] + Out[2] * Out[2]);
            if(Length > 1e-12f) { for(int Column = 0; Column < 4; ++Column) { Out[Column] /= Length; } }
        }
        Frustum.bHas = true;
    }

    bool AnimationSystem::IsSeen(const GameCore::Entity& InEntity, const Entry& InEntry, const AnimInstance& InInstance) const
    {
        for(const ViewFrustum& Each : ViewFrustums)
        {
            if(!Each.bHas) { return true; }   // a view by origin alone sees everything
        }

        /* Model space: the mesh's own box and every bone where it is now --
         * root motion as authored walks it off its entity -- padded for the
         * skin past the bones. */
        float Min[3], Max[3];
        bool  bAny = false;
        const auto Grow = [&](const float InX, const float InY, const float InZ)
        {
            const float P[3] = { InX, InY, InZ };
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                Min[Axis] = bAny ? std::min(Min[Axis], P[Axis]) : P[Axis];
                Max[Axis] = bAny ? std::max(Max[Axis], P[Axis]) : P[Axis];
            }
            bAny = true;
        };
        if(InEntry.Mesh)
        {
            Grow(InEntry.Mesh->BoundsMin[0], InEntry.Mesh->BoundsMin[1], InEntry.Mesh->BoundsMin[2]);
            Grow(InEntry.Mesh->BoundsMax[0], InEntry.Mesh->BoundsMax[1], InEntry.Mesh->BoundsMax[2]);
        }
        for(const AnimMatrix& Bone : InInstance.GetModelPose()) { Grow(Bone.M[0][3], Bone.M[1][3], Bone.M[2][3]); }
        if(!bAny) { return true; }
        const float HalfX = 0.5f * (Max[0] - Min[0]), HalfY = 0.5f * (Max[1] - Min[1]), HalfZ = 0.5f * (Max[2] - Min[2]);
        const float Radius = 1.25f * std::sqrt(HalfX * HalfX + HalfY * HalfY + HalfZ * HalfZ) + 1.0f;

        /* Into the world by the entity's transform; its largest scale for the radius. */
        const GameCore::TransformPose Pose = InEntity.GetWorldPose();
        RenderTransform Placed;
        Placed.Position = Pose.Position;
        Placed.Rotation = Pose.Rotation;
        Placed.Scale    = Pose.Scale;
        const EngineCore::Vector4 Centre = Placed.ToMatrix() * EngineCore::Vector4(0.5 * (Min[0] + Max[0]), 0.5 * (Min[1] + Max[1]),
                                                                                0.5 * (Min[2] + Max[2]), 1.0);
        const double Scale = std::max({ std::fabs(Pose.Scale.GetX()), std::fabs(Pose.Scale.GetY()), std::fabs(Pose.Scale.GetZ()) });
        const float  Reach = float(double(Radius) * Scale);

        for(const ViewFrustum& Each : ViewFrustums)
        {
            bool bInside = true;
            for(int Plane = 0; Plane < 6 && bInside; ++Plane)
            {
                const float* N = Each.Planes[Plane];
                bInside = N[0] * float(Centre.GetX()) + N[1] * float(Centre.GetY()) + N[2] * float(Centre.GetZ()) + N[3] >= -Reach;
            }
            if(bInside) { return true; }
        }
        return false;
    }

    void AnimationSystem::Advance(const float InDeltaSeconds, const bool bInGame)
    {
        using Clock_t = std::chrono::steady_clock;
        const auto MillisecondsSince = [](const Clock_t::time_point InStart)
        {
            return float(std::chrono::duration<double, std::milli>(Clock_t::now() - InStart).count());
        };
        /* The whole tick's time, however it returns. */
        struct TickTimer
        {
            Stats&             Out;
            Clock_t::time_point Start;
            ~TickTimer() { Out.Milliseconds = float(std::chrono::duration<double, std::milli>(Clock_t::now() - Start).count()); }
        };
        const Clock_t::time_point TickStart = Clock_t::now();
        Sync();
        Clock += double(InDeltaSeconds);
        Pending.clear();
        LastStats = Stats();
        const TickTimer Timer{ LastStats, TickStart };

        /* 1. Gather, serially: parameters in, and how much each moves this
         *    tick -- by how far it is from the nearest view. */
        struct Work
        {
            AnimInstance*       Instance  = nullptr;
            AnimationComponent* Component = nullptr;
            GameCore::EntityID  Entity;
            float               Step      = 0.0f;
            bool                bEvaluate = true;
        };
        std::vector<Work> Works;
        Works.reserve(Entries.size());
        const double FullRate = double(GAnimationFullRateDistance.GetFloat());
        const double Freeze   = double(GAnimationFreezeDistance.GetFloat());
        const int32_t MaxSkip = std::max(0, GAnimationMaxSkipFrames.GetInt());
        const int32_t OffscreenSkip = std::max(0, GAnimationOffscreenSkipFrames.GetInt());
        World.Each<AnimationComponent>([&](const GameCore::Entity& InEntity, AnimationComponent& InComponent)
        {
            const auto Found = Entries.find(InEntity.GetID().Pack());
            if(Found == Entries.end() || !Found->second.Instance)
            {
                return;
            }
            Entry& Each = Found->second;
            AnimInstance& Instance = *Each.Instance;
            for(const AnimationParameter& Parameter : InComponent.Parameters)
            {
                if(Parameter.Name[0] != '\0')
                {
                    Instance.SetParameter(std::string(Parameter.Name, strnlen(Parameter.Name, ANIMATION_PARAMETER_NAME_MAX)), Parameter.Value);
                }
            }
            Instance.SetPlayRate(InComponent.PlayRate);

            Work Item;
            Item.Instance  = &Instance;
            Item.Component = &InComponent;
            Item.Entity    = InEntity.GetID();
            Item.Step      = InComponent.bPaused ? 0.0f : InDeltaSeconds;

            /* No view (a server), or the editor's preview -- one world, and a
             * camera that is often far off: everything at full rate. */
            if(bInGame && !ViewOrigins.empty() && Instance.HasPose())
            {
                const EngineCore::Vector3 At = InEntity.GetWorldPose().Position;
                double Nearest = 1e300;
                for(const EngineCore::Vector3& View : ViewOrigins) { Nearest = std::min(Nearest, (At - View).GetLength()); }

                if(Freeze > 0.0 && Nearest > Freeze)
                {
                    /* Far: time passes -- notifies, transitions, root motion -- but no pose. */
                    Item.bEvaluate = false;
                    ++LastStats.Frozen;
                }
                else
                {
                    /* Farther, fewer updates: every (distance / FullRate)th tick; outside
                     * every view, every (OffscreenSkipFrames + 1)th -- whichever is fewer. */
                    int32_t Every = 1;
                    if(FullRate > 0.0 && Nearest > FullRate && MaxSkip > 0)
                    {
                        Every = std::min(MaxSkip + 1, int32_t(std::ceil(Nearest / FullRate)));
                    }
                    if(OffscreenSkip > 0 && !IsSeen(InEntity, Each, Instance))
                    {
                        Every = std::max(Every, OffscreenSkip + 1);
                        ++LastStats.Offscreen;
                    }
                    if(Every <= 1)
                    {
                        /* At full rate -- again, if it was not: what it saved up, at once. */
                        Item.Step   += Each.Saved;
                        Each.Saved   = 0.0f;
                        Each.Skipped = 0;
                        Works.push_back(Item);
                        return;
                    }
                    /* The time saved up while skipped. */
                    Each.Saved += Item.Step;
                    if(++Each.Skipped < Every)
                    {
                        ++LastStats.Skipped;
                        return;
                    }
                    Item.Step    = Each.Saved;
                    Each.Saved   = 0.0f;
                    Each.Skipped = 0;
                    ++LastStats.Reduced;
                }
            }
            Works.push_back(Item);
        });
        LastStats.Updated = uint32_t(Works.size());

        /* 2. Update: each instance alone, so across the job system when there
         *    are enough of them to be worth it. */
        const auto UpdateRange = [&Works](const uint32_t InBegin, const uint32_t InEnd)
        {
            for(uint32_t Index = InBegin; Index < InEnd; ++Index)
            {
                Works[Index].Instance->Update(Works[Index].Step, Works[Index].bEvaluate);
            }
        };
        EngineCore::JobSystem* Jobs = EngineCore::GetJobSystem();
        const int32_t ParallelMin = GAnimationParallelMin.GetInt();
        const Clock_t::time_point UpdateStart = Clock_t::now();
        if(Jobs && ParallelMin > 0 && Works.size() >= size_t(ParallelMin))
        {
            Jobs->ParallelFor(uint32_t(Works.size()), 4, UpdateRange);
            LastStats.bParallel = true;
        }
        else
        {
            UpdateRange(0, uint32_t(Works.size()));
        }
        LastStats.UpdateMilliseconds = MillisecondsSince(UpdateStart);

        /* 3. What the updates did, serially: the world is written here only. */
        if(!bInGame)
        {
            UpdateSockets();
            return;
        }
        GameCore::EventBus& Events = World.GetEvents();
        for(const Work& Item : Works)
        {
            const AnimInstance& Instance = *Item.Instance;
            const GameCore::Entity Owner = World.GetEntity(Item.Entity);

            /* Root motion: the clip's travel moves the entity, turned and
             * sized as the entity is. Not in the editor's preview. */
            const AnimVec3& Travel = Instance.GetRootMotion();
            if(Travel.X != 0.0f || Travel.Z != 0.0f)
            {
                if(GameCore::TransformComponent* Transform = Owner.GetComponent<GameCore::TransformComponent>())
                {
                    const EngineCore::Vector3 Local(double(Travel.X) * Transform->Scale.GetX(), 0.0, double(Travel.Z) * Transform->Scale.GetZ());
                    Transform->Position = Transform->Position + GameCore::TransformMath::EulerToRotation(Transform->Rotation) * Local;
                }
            }

            /* A trigger a transition used is spent: the component's too. */
            for(const std::string& Trigger : Instance.GetConsumedTriggers())
            {
                const int32_t Slot = Item.Component->FindParameter(Trigger.c_str());
                if(Slot >= 0) { Item.Component->Parameters[Slot].Value = 0.0f; }
            }
            for(const AnimNotify& Notify : Instance.GetFiredNotifies())
            {
                GameCore::EventArgs Args;
                Args.Args[0] = GameCore::EventArg::Make(Item.Entity);
                Args.Args[1] = GameCore::EventArg::Make(Notify.Name.c_str());
                Args.Count   = 2;
                Events.Emit("AnimNotify", Args);   // ids are per bus: by name
                if(!Notify.ParticleSystem.empty())
                {
                    Pending.push_back({ Item.Entity, Notify.ParticleSystem, Notify.Bone });
                }
            }
            for(const AnimInstance::FinishedState& Done : Instance.GetFinishedStates())
            {
                GameCore::EventArgs Args;
                Args.Args[0] = GameCore::EventArg::Make(Item.Entity);
                Args.Args[1] = GameCore::EventArg::Make(Done.Name.c_str());
                Args.Args[2] = GameCore::EventArg::Make(double(Done.Layer));
                Args.Count   = 3;
                Events.Emit("AnimEnded", Args);
            }
        }
        UpdateSockets();

        /* After the walk over the pool: spawning adds entities. */
        for(const PendingEffect& Each : Pending) { SpawnEffect(Each); }
        UpdateEffects();
    }

    void AnimationSystem::SpawnEffect(const PendingEffect& InEffect)
    {
        /* Where: the bone, or the entity. Not attached: it plays where it starts. */
        GameCore::TransformPose Where = World.GetEntity(InEffect.Owner).GetWorldPose();
        if(!InEffect.Bone.empty())
        {
            GetBoneWorldPose(InEffect.Owner, InEffect.Bone.c_str(), Where);
        }
        const GameCore::Entity Effect = World.CreateEntity("AnimEffect");
        GameCore::TransformComponent Transform;
        Transform.Position = Where.Position;
        Transform.Rotation = Where.Rotation;
        Effect.AddComponent(Transform);
        ParticleSystemComponent Particles;
        std::snprintf(Particles.System, sizeof(Particles.System), "%s", InEffect.System.c_str());
        Effect.AddComponent(Particles);

        /* It emits a moment (its bursts, if it bursts), and goes once its
         * longest-lived particle has had time to die. */
        double Emit = 0.1, Live = 2.0;
        if(const std::shared_ptr<const ParticleSystemDesc> System = ParticleSystemCache::Find(InEffect.System))
        {
            Live = 0.0;
            for(const ParticleEmitterDesc& Emitter : System->Emitters)
            {
                Live = std::max(Live, double(Emitter.Lifetime) * (1.0 + double(Emitter.LifetimeVariance)));
                if(Emitter.BurstCycles > 1) { Emit = std::max(Emit, double(Emitter.BurstInterval) * double(Emitter.BurstCycles - 1) + 0.1); }
            }
        }
        Effects.push_back({ Effect.GetID(), Clock + Emit, Clock + Emit + Live + 0.1 });
    }

    void AnimationSystem::UpdateEffects()
    {
        for(auto It = Effects.begin(); It != Effects.end(); )
        {
            const GameCore::Entity Effect = World.GetEntity(It->Entity);
            if(!Effect.IsAlive() || Clock >= It->DestroyAt)
            {
                if(Effect.IsAlive()) { World.DestroyEntity(It->Entity); }
                It = Effects.erase(It);
                continue;
            }
            if(Clock >= It->StopAt)
            {
                if(ParticleSystemComponent* Particles = Effect.GetComponent<ParticleSystemComponent>()) { Particles->bEmitting = 0; }
            }
            ++It;
        }
    }

    void AnimationSystem::UpdateSockets()
    {
        World.Each<AnimSocketComponent>([this](const GameCore::Entity& InEntity, AnimSocketComponent& InSocket)
        {
            const GameCore::EntityID Parent = InEntity.GetParent();
            const AnimInstance* Instance = Parent.IsSet() ? FindInstance(Parent) : nullptr;
            GameCore::TransformComponent* Transform = InEntity.GetComponent<GameCore::TransformComponent>();
            if(!Instance || !Transform || !Instance->HasPose())
            {
                return;
            }
            const int32_t Bone = Instance->GetSkeleton()->FindBone(std::string(InSocket.Bone, strnlen(InSocket.Bone, ANIM_SOCKET_BONE_MAX)));
            if(Bone < 0 || size_t(Bone) >= Instance->GetModelPose().size())
            {
                return;
            }
            /* The bone in its entity's space, then the offset in the bone's;
             * Euler angles kept near the last, so they do not flip. */
            GameCore::TransformPose BonePose = ToTransformPose(Instance->GetModelPose()[size_t(Bone)], Transform->Rotation);
            BonePose.Scale = EngineCore::Vector3(1.0, 1.0, 1.0);
            GameCore::TransformPose Offset;
            Offset.Position = InSocket.PositionOffset;
            Offset.Rotation = InSocket.RotationOffset;
            const GameCore::TransformPose Placed = GameCore::TransformMath::Compose(BonePose, Offset);
            Transform->Position = Placed.Position;
            Transform->Rotation = Placed.Rotation;
        });
    }

    AnimInstance* AnimationSystem::FindInstance(const GameCore::EntityID InEntity)
    {
        const auto Found = Entries.find(InEntity.Pack());
        return Found != Entries.end() && Found->second.Instance && Found->second.Skeleton ? Found->second.Instance.get() : nullptr;
    }

    const AnimInstance* AnimationSystem::FindInstance(const GameCore::EntityID InEntity) const
    {
        const auto Found = Entries.find(InEntity.Pack());
        return Found != Entries.end() && Found->second.Instance && Found->second.Skeleton ? Found->second.Instance.get() : nullptr;
    }

    bool AnimationSystem::GetBoneWorldPose(const GameCore::EntityID InEntity, const char* InBone, GameCore::TransformPose& OutPose) const
    {
        const AnimInstance* Instance = FindInstance(InEntity);
        if(!Instance || !InBone || !Instance->HasPose())
        {
            return false;
        }
        const int32_t Bone = Instance->GetSkeleton()->FindBone(InBone);
        if(Bone < 0 || size_t(Bone) >= Instance->GetModelPose().size())
        {
            return false;
        }
        const GameCore::TransformPose Entity = World.GetEntity(InEntity).GetWorldPose();
        OutPose = GameCore::TransformMath::Compose(Entity, ToTransformPose(Instance->GetModelPose()[size_t(Bone)]));
        return true;
    }
}
