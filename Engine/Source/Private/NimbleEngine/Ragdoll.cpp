#include "Public/NimbleEngine/Ragdoll.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentScript.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/ScriptLibrary.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/SkeletonAsset.h"

#if WITH_JOLTPHYSICS
#include "Public/JoltPhysics/PhysicsWorld.h"
#endif

/* Ragdoll.h has the contract; RAGDOLL.md the design. */

namespace NimbleEngine
{
    // === The component ============================================================

    void RagdollComponent::Activate()
    {
        bPendingActivate = 1;
        bPendingReset    = 0;
    }

    void RagdollComponent::AddImpulse(const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt)
    {
        if(PendingImpulses >= RAGDOLL_IMPULSES_MAX)
        {
            /* Full: the newest joins the last. */
            ImpulseAmounts[RAGDOLL_IMPULSES_MAX - 1] = ImpulseAmounts[RAGDOLL_IMPULSES_MAX - 1] + InImpulse;
            return;
        }
        ImpulseAmounts[PendingImpulses] = InImpulse;
        ImpulsePoints[PendingImpulses]  = InAt;
        ++PendingImpulses;
    }

    void RagdollComponent::ActivateWithImpulse(const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt)
    {
        Activate();
        AddImpulse(InImpulse, InAt);
    }

    std::string RagdollComponent::GetPartAt(const EngineCore::Vector3& InAt) const
    {
        const RagdollWorld* Ragdolls = Owner.OwningWorld ? RagdollWorld::Find(Owner.OwningWorld) : nullptr;
        return Ragdolls ? Ragdolls->FindPart(Owner.Entity, InAt) : std::string();
    }

    void RagdollComponent::Reset()
    {
        bPendingReset    = 1;
        bPendingActivate = 0;
        PendingImpulses  = 0;
    }

    // === RagdollWorld =============================================================

    namespace
    {
        using RenderCore::AnimMatrix;
        using RenderCore::AnimQuat;
        using RenderCore::AnimTransform;
        using RenderCore::AnimVec3;
        namespace AM = RenderCore::AnimMath;

        std::unordered_map<const GameCore::World*, RagdollWorld*>& Systems()
        {
            static std::unordered_map<const GameCore::World*, RagdollWorld*> Map;
            return Map;
        }

        /* An entity's world transform as an affine matrix (its scale included). */
        AnimMatrix ToMatrix(const GameCore::TransformPose& InPose)
        {
            const GameCore::Rotation3 R = GameCore::TransformMath::EulerToRotation(InPose.Rotation);
            const double Scale[3] = { InPose.Scale.GetX(), InPose.Scale.GetY(), InPose.Scale.GetZ() };
            AnimMatrix Out;
            for(int Row = 0; Row < 3; ++Row)
            {
                for(int Col = 0; Col < 3; ++Col)
                {
                    Out.M[Row][Col] = float(R.M[Row][Col] * Scale[Col]);
                }
            }
            Out.M[0][3] = float(InPose.Position.GetX());
            Out.M[1][3] = float(InPose.Position.GetY());
            Out.M[2][3] = float(InPose.Position.GetZ());
            return Out;
        }

        /* The uniform scale a ragdoll is made at: the entity's largest. */
        float UniformScale(const GameCore::TransformPose& InPose)
        {
            const double Largest = std::max({ std::abs(InPose.Scale.GetX()), std::abs(InPose.Scale.GetY()), std::abs(InPose.Scale.GetZ()) });
            return Largest > 1.0e-6 ? float(Largest) : 1.0f;
        }

        /* Degrees per second turning InFrom into InTo over InSeconds. */
        AnimVec3 AngularVelocity(const AnimQuat& InFrom, const AnimQuat& InTo, const float InSeconds)
        {
            AnimQuat Delta = AM::Normalize(AM::Mul(InTo, AM::Conjugate(InFrom)));
            if(Delta.W < 0.0f) { Delta = { -Delta.X, -Delta.Y, -Delta.Z, -Delta.W }; }
            const float Sine = std::sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y + Delta.Z * Delta.Z);
            if(Sine < 1.0e-7f || InSeconds <= 0.0f) { return AnimVec3(); }
            const float Angle = 2.0f * std::atan2(Sine, Delta.W);
            const float Rate  = Angle * 180.0f / 3.14159265358979f / InSeconds / Sine;
            return { Delta.X * Rate, Delta.Y * Rate, Delta.Z * Rate };
        }
    }

    struct RagdollWorld::Impl
    {
        /* A part's place in the world. */
        struct PartPose
        {
            AnimVec3 Position;
            AnimQuat Rotation;
        };

        /* One Ragdoll component's state, by its entity. */
        struct Record
        {
            std::shared_ptr<const RenderCore::SkeletonData> Skeleton;
            std::shared_ptr<const RagdollProfile>           Profile;
            uint32_t Handle   = 0;
            float    Scale    = 1.0f;    // the entity's, when it was made
            bool     bDynamic = false;
            bool     bSeen    = false;
            bool     bPaused  = false;   // it paused the Animation component (Reset unpauses)
            bool     bStarted = false;   // StartActive has been looked at
            bool     bWarned  = false;   // its "cannot ragdoll" has been logged
            std::vector<PartPose>   Pose, PrevPose;   // the animated pose, this update and the last
            bool                    bHasPrev  = false;
            float                   PrevDelta = 0.0f;
            std::vector<AnimMatrix> Model;   // the override handed to the animation
            std::vector<uint8_t>    Mask;
        };

        GameCore::World& World;
        int              Attachments = 0;
        std::unordered_map<uint64_t, Record> Records;

        /* Profiles by skeleton; a skeleton that is not a humanoid holds null. */
        struct ProfileEntry
        {
            std::shared_ptr<const RenderCore::SkeletonData> Skeleton;   // keeps the key alive
            std::shared_ptr<const RagdollProfile>           Profile;
            std::string                                     Error;
        };
        std::unordered_map<const RenderCore::SkeletonData*, ProfileEntry> Profiles;

        bool bNoPhysicsWarned = false;

        explicit Impl(GameCore::World& InWorld) : World(InWorld) {}

        ProfileEntry& ProfileFor(const std::shared_ptr<const RenderCore::SkeletonData>& InSkeleton)
        {
            ProfileEntry& Entry = Profiles[InSkeleton.get()];
            if(!Entry.Skeleton)
            {
                Entry.Skeleton = InSkeleton;
                auto Profile = std::make_shared<RagdollProfile>();
                if(BuildRagdollProfile(*InSkeleton, *Profile, Entry.Error)) { Entry.Profile = Profile; }
            }
            return Entry;
        }

        /* The animated pose's parts, in the world. */
        static void PoseFromAnimation(const RagdollProfile& InProfile, const std::vector<AnimMatrix>& InModel,
                                      const AnimMatrix& InEntity, std::vector<PartPose>& OutPose)
        {
            OutPose.resize(InProfile.Parts.size());
            for(size_t Index = 0; Index < InProfile.Parts.size(); ++Index)
            {
                const AnimTransform Placed = AM::ToTransform(AM::Mul(InEntity, InModel[size_t(InProfile.Parts[Index].Bone)]));
                OutPose[Index].Position = Placed.Translation;
                OutPose[Index].Rotation = AM::Normalize(Placed.Rotation);
            }
        }

#if WITH_JOLTPHYSICS
        static JoltPhysics::RagdollTransform ToPhysics(const AnimVec3& InPosition, const AnimQuat& InRotation)
        {
            JoltPhysics::RagdollTransform Out;
            Out.Position[0] = InPosition.X;
            Out.Position[1] = InPosition.Y;
            Out.Position[2] = InPosition.Z;
            Out.Rotation[0] = InRotation.X;
            Out.Rotation[1] = InRotation.Y;
            Out.Rotation[2] = InRotation.Z;
            Out.Rotation[3] = InRotation.W;
            return Out;
        }

        static void CopyAxis(const AnimVec3& InV, float OutV[3])
        {
            OutV[0] = InV.X;
            OutV[1] = InV.Y;
            OutV[2] = InV.Z;
        }

        static std::vector<JoltPhysics::RagdollTransform> ToPhysics(const std::vector<PartPose>& InPose)
        {
            std::vector<JoltPhysics::RagdollTransform> Out(InPose.size());
            for(size_t Index = 0; Index < InPose.size(); ++Index) { Out[Index] = ToPhysics(InPose[Index].Position, InPose[Index].Rotation); }
            return Out;
        }

        /* The profile at InScale, for InComponent's entity. */
        static JoltPhysics::RagdollDesc MakeDesc(const RagdollProfile& InProfile, const RagdollComponent& InComponent,
                                                 const GameCore::EntityID InOwner, const float InScale)
        {
            JoltPhysics::RagdollDesc Desc;
            Desc.Owner          = InOwner;
            Desc.Layer          = InComponent.Layer;
            Desc.Mask           = InComponent.Mask;
            Desc.Friction       = InComponent.Friction;
            Desc.LinearDamping  = InComponent.LinearDamping;
            Desc.AngularDamping = InComponent.AngularDamping;
            const float Mass      = std::max(InComponent.Mass, 1.0f);
            const float Thickness = std::max(InComponent.Thickness, 0.05f);
            const float Friction  = std::max(InComponent.JointFriction, 0.0f) * Mass / 70.0f;
            constexpr float Radians = 3.14159265358979f / 180.0f;

            Desc.Parts.resize(InProfile.Parts.size());
            for(size_t Index = 0; Index < InProfile.Parts.size(); ++Index)
            {
                const RagdollProfilePart& From = InProfile.Parts[Index];
                JoltPhysics::RagdollPartDesc& To = Desc.Parts[Index];
                const AnimTransform Rest = AM::ToTransform(InProfile.RestModel[size_t(From.Bone)]);
                To.Parent = From.Parent;
                To.Rest   = ToPhysics(AM::Scale(Rest.Translation, InScale), AM::Normalize(Rest.Rotation));

                To.Shape      = From.Shape;
                To.Radius     = From.Radius * InScale * Thickness;
                To.HalfHeight = From.HalfHeight * InScale;
                To.HalfExtent[0] = From.HalfExtent.X * InScale * Thickness;
                To.HalfExtent[1] = From.HalfExtent.Y * InScale * Thickness;
                To.HalfExtent[2] = From.HalfExtent.Z * InScale * Thickness;
                To.ShapeLocal = ToPhysics(AM::Scale(From.ShapeLocal.Translation, InScale), From.ShapeLocal.Rotation);
                To.Mass       = From.MassShare * Mass;

                To.Joint = From.Joint;
                To.JointPosition[0] = To.Rest.Position[0];
                To.JointPosition[1] = To.Rest.Position[1];
                To.JointPosition[2] = To.Rest.Position[2];
                CopyAxis(From.TwistAxis, To.TwistAxis);
                CopyAxis(From.PlaneAxis, To.PlaneAxis);
                CopyAxis(From.ParentTwistAxis, To.ParentTwistAxis);
                CopyAxis(From.ParentPlaneAxis, To.ParentPlaneAxis);
                To.TwistMin       = From.TwistMin * Radians;
                To.TwistMax       = From.TwistMax * Radians;
                To.NormalHalfCone = From.NormalHalfCone * Radians;
                To.PlaneHalfCone  = From.PlaneHalfCone * Radians;
                To.FrictionTorque = From.FrictionTorque * Friction;
            }
            return Desc;
        }
#endif

        /* InSocketBone's part: the bone's, else its nearest ancestor's with one. */
        static int32_t PartCarrying(const RagdollProfile& InProfile, const RenderCore::SkeletonData& InSkeleton, int32_t InBone)
        {
            for(; InBone >= 0; InBone = InSkeleton.Bones[size_t(InBone)].Parent)
            {
                if(InProfile.BonePart[size_t(InBone)] >= 0) { return InProfile.BonePart[size_t(InBone)]; }
            }
            return 0;
        }

        /* A prop let go: its body enabled, flung as the part carrying it moved. */
        struct Drop
        {
            GameCore::EntityID  Entity;
            EngineCore::Vector3 Velocity;
        };

        /* Socketed children of InOwner holding a disabled Rigid Body. */
        void FindDrops(const GameCore::EntityID InOwner, const Record& InRecord, std::vector<Drop>& OutDrops) const
        {
            const GameCore::Entity Owner = World.GetEntity(InOwner);
            for(GameCore::EntityID Child = Owner.GetFirstChild(); Child.IsSet(); Child = World.GetEntity(Child).GetNextSibling())
            {
                const GameCore::Entity Prop = World.GetEntity(Child);
                const RenderCore::AnimSocketComponent* Socket = Prop.GetComponent<RenderCore::AnimSocketComponent>();
                const GameCore::RigidBodyComponent*    Body   = Prop.GetComponent<GameCore::RigidBodyComponent>();
                if(!Socket || !Body || Body->bEnabled) { continue; }

                Drop Each;
                Each.Entity = Child;
                const int32_t Bone = InRecord.Skeleton->FindBone(std::string(Socket->Bone, strnlen(Socket->Bone, RenderCore::ANIM_SOCKET_BONE_MAX)));
                const int32_t Part = Bone >= 0 ? PartCarrying(*InRecord.Profile, *InRecord.Skeleton, Bone) : 0;
                if(InRecord.bHasPrev && InRecord.PrevDelta > 0.0f && size_t(Part) < InRecord.Pose.size())
                {
                    const AnimVec3 Moved = AM::Sub(InRecord.Pose[size_t(Part)].Position, InRecord.PrevPose[size_t(Part)].Position);
                    Each.Velocity = EngineCore::Vector3(Moved.X, Moved.Y, Moved.Z) * (1.0 / double(InRecord.PrevDelta));
                }
                OutDrops.push_back(Each);
            }
        }

        void LetGo(const Drop& InDrop)
        {
            const GameCore::Entity Prop = World.GetEntity(InDrop.Entity);
            if(!Prop.IsAlive()) { return; }
            Prop.RemoveComponent<RenderCore::AnimSocketComponent>();
            /* Out of the hierarchy, so it stays put whatever moves the body's
             * entity -- unless a prefab instance holds it: instances keep their
             * members, and the paused body no longer moves anyway. */
            if(!Prop.GetInstance().IsSet()) { Prop.ClearParent(true); }
            if(GameCore::RigidBodyComponent* Body = Prop.GetComponent<GameCore::RigidBodyComponent>())
            {
                Body->Motion  = GameCore::RBM_Dynamic;
                Body->InitialLinearVelocity = InDrop.Velocity;
                Body->bEnabled = 1;
            }
        }

        void Emit(const char* InEvent, const GameCore::EntityID InEntity)
        {
            GameCore::EventArgs Args;
            Args.Args[0] = GameCore::EventArg::Make(InEntity);
            Args.Count   = 1;
            World.GetEvents().Emit(InEvent, Args);
        }

        /* The animation is the skeleton's again. */
        void Release(const GameCore::EntityID InEntity, Record& InRecord, RagdollComponent* InComponent)
        {
#if WITH_JOLTPHYSICS
            if(InRecord.Handle != 0)
            {
                if(JoltPhysics::PhysicsWorld* Physics = JoltPhysics::PhysicsWorld::Find(World)) { Physics->DestroyRagdoll(InRecord.Handle); }
            }
#endif
            InRecord.Handle   = 0;
            InRecord.bDynamic = false;
            InRecord.bHasPrev = false;
            if(RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(&World))
            {
                if(RenderCore::AnimInstance* Instance = Animation->FindInstance(InEntity)) { Instance->ClearPoseOverride(); }
            }
            if(InRecord.bPaused)
            {
                if(RenderCore::AnimationComponent* Anim = World.GetEntity(InEntity).GetComponent<RenderCore::AnimationComponent>())
                {
                    Anim->bPaused = 0;
                }
                InRecord.bPaused = false;
            }
            if(InComponent)
            {
                InComponent->bActive  = 0;
                InComponent->bSettled = 0;
                InComponent->Handle   = 0;
            }
        }

        // --- TP_PostPhysics: the simulated parts become the skeleton's pose -------

        void Pose()
        {
#if WITH_JOLTPHYSICS
            JoltPhysics::PhysicsWorld* Physics = JoltPhysics::PhysicsWorld::Find(World);
            RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(&World);
            if(!Physics || !Animation) { return; }

            std::vector<GameCore::EntityID> Settled;
            std::vector<JoltPhysics::RagdollTransform> Parts;
            World.Each<RagdollComponent>([&](const GameCore::Entity& InEntity, RagdollComponent& InComponent)
            {
                const auto Found = Records.find(InEntity.GetID().Pack());
                if(Found == Records.end()) { return; }
                Record& Each = Found->second;
                RenderCore::AnimInstance* Instance = Animation->FindInstance(InEntity.GetID());
                if(!Each.bDynamic || Each.Handle == 0 || !Instance || Instance->GetSkeleton() != Each.Skeleton) { return; }

                Parts.resize(Each.Profile->Parts.size());
                if(!Physics->GetRagdollPose(Each.Handle, Parts.data())) { return; }

                /* Into the entity's model space: wherever the entity is, the
                 * mesh draws the parts where physics has them. */
                const AnimMatrix ToModel = AM::Inverse(ToMatrix(InEntity.GetWorldPose()));
                const size_t Bones = Each.Skeleton->Bones.size();
                Each.Model.resize(Bones);
                if(Each.Mask.size() != Bones)
                {
                    Each.Mask.assign(Bones, 0);
                    for(const RagdollProfilePart& Part : Each.Profile->Parts) { Each.Mask[size_t(Part.Bone)] = 1; }
                }
                for(size_t Index = 0; Index < Parts.size(); ++Index)
                {
                    AnimTransform Placed;
                    Placed.Translation = { Parts[Index].Position[0], Parts[Index].Position[1], Parts[Index].Position[2] };
                    Placed.Rotation    = { Parts[Index].Rotation[0], Parts[Index].Rotation[1], Parts[Index].Rotation[2], Parts[Index].Rotation[3] };
                    Each.Model[size_t(Each.Profile->Parts[Index].Bone)] = AM::Mul(ToModel, AM::ToMatrix(Placed));
                }
                Instance->SetPoseOverride(Each.Model, Each.Mask, 1.0f);

                if(!InComponent.bSettled && !Physics->IsRagdollAwake(Each.Handle))
                {
                    InComponent.bSettled = 1;
                    Settled.push_back(InEntity.GetID());
                }
            });
            for(const GameCore::EntityID Entity : Settled) { Emit("RagdollSettled", Entity); }
#endif
        }

        // --- TP_PostUpdate: make, drive, activate, reset ---------------------------

        void Update(const float InDeltaSeconds)
        {
            for(auto& Entry : Records) { Entry.second.bSeen = false; }

            RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(&World);
#if WITH_JOLTPHYSICS
            JoltPhysics::PhysicsWorld* Physics = JoltPhysics::PhysicsWorld::Find(World);
#endif
            std::vector<Drop>               Drops;
            std::vector<GameCore::EntityID> Activated;

            World.Each<RagdollComponent>([&](const GameCore::Entity& InEntity, RagdollComponent& InComponent)
            {
                const GameCore::EntityID Id = InEntity.GetID();
                Record& Each = Records[Id.Pack()];
                Each.bSeen = true;

                if(InComponent.bPendingReset || !InComponent.bEnabled)
                {
                    if(Each.Handle != 0 || Each.bPaused || InComponent.bActive) { Release(Id, Each, &InComponent); }
                    InComponent.bPendingReset = 0;
                    if(!InComponent.bEnabled)
                    {
                        InComponent.bPendingActivate = 0;
                        InComponent.PendingImpulses  = 0;
                        return;
                    }
                }

                const bool bStart = !Each.bStarted && InComponent.bStartActive;
                const bool bWantsActivate = (InComponent.bPendingActivate || bStart) && !InComponent.bActive;

#if WITH_JOLTPHYSICS
                RenderCore::AnimInstance* Instance = Animation ? Animation->FindInstance(Id) : nullptr;
                if(!Physics)
                {
                    if(bWantsActivate && !bNoPhysicsWarned)
                    {
                        CORE_LOG_WARN("[Ragdoll] '" + std::string(InEntity.GetName()) + "' cannot ragdoll: this world is not simulated")
                        bNoPhysicsWarned = true;
                    }
                    InComponent.bPendingActivate = 0;
                    InComponent.PendingImpulses  = 0;
                    return;
                }
                if(!Instance || !Instance->HasPose() || !Instance->GetSkeleton())
                {
                    return;   // nothing posed yet: the requests wait
                }

                /* A new skeleton: a new profile, and the old ragdoll goes. */
                if(Each.Skeleton != Instance->GetSkeleton())
                {
                    if(Each.Handle != 0) { Release(Id, Each, &InComponent); }
                    Each.Skeleton = Instance->GetSkeleton();
                    const ProfileEntry& Entry = ProfileFor(Each.Skeleton);
                    Each.Profile = Entry.Profile;
                    Each.Mask.clear();
                    if(!Each.Profile && !Each.bWarned)
                    {
                        CORE_LOG_WARN("[Ragdoll] '" + std::string(InEntity.GetName()) + "': " + Entry.Error)
                        Each.bWarned = true;
                    }
                }
                Each.bStarted = true;
                if(!Each.Profile)
                {
                    InComponent.bPendingActivate = 0;
                    InComponent.PendingImpulses  = 0;
                    return;
                }

                if(InComponent.bActive)
                {
                    /* Simulating: only impulses to apply. */
                    for(uint8_t Index = 0; Index < InComponent.PendingImpulses; ++Index)
                    {
                        Physics->AddRagdollImpulse(Each.Handle, InComponent.ImpulseAmounts[Index], InComponent.ImpulsePoints[Index]);
                    }
                    InComponent.PendingImpulses  = 0;
                    InComponent.bPendingActivate = 0;
                    InComponent.Handle = Each.Handle;
                    return;
                }

                /* Animated: where its parts are now. */
                const GameCore::TransformPose EntityPose = InEntity.GetWorldPose();
                Each.PrevPose.swap(Each.Pose);
                PoseFromAnimation(*Each.Profile, Instance->GetModelPose(), ToMatrix(EntityPose), Each.Pose);
                const bool bHadPrev = Each.bHasPrev && Each.PrevPose.size() == Each.Pose.size();

                if(Each.Handle != 0 && !Physics->HasRagdoll(Each.Handle))
                {
                    Each.Handle = 0;   // the physics world was remade
                }
                const bool bWantsBody = InComponent.bHitBoxes || bWantsActivate;
                if(Each.Handle == 0 && bWantsBody)
                {
                    Each.Scale = UniformScale(EntityPose);
                    const JoltPhysics::RagdollDesc Desc = MakeDesc(*Each.Profile, InComponent, Id, Each.Scale);
                    const std::vector<JoltPhysics::RagdollTransform> At = ToPhysics(Each.Pose);
                    Each.Handle   = Physics->CreateRagdoll(Desc, At.data(), false);
                    Each.bDynamic = false;
                    if(Each.Handle == 0)
                    {
                        /* Logged by physics; not retried every frame. */
                        InComponent.bEnabled = 0;
                        return;
                    }
                }
                else if(Each.Handle != 0 && !bWantsBody)
                {
                    Physics->DestroyRagdoll(Each.Handle);
                    Each.Handle = 0;
                }

                if(bWantsActivate)
                {
                    /* A ragdoll that has been following the animation moves as
                     * it did; one made just now is given the animation's
                     * velocity from the last two poses. */
                    const bool bFollowed = Physics->HasRagdoll(Each.Handle) && bHadPrev && InComponent.Handle == Each.Handle;
                    if(bFollowed)
                    {
                        Physics->SetRagdollDynamic(Each.Handle);
                    }
                    else
                    {
                        std::vector<EngineCore::Vector3> Linear(Each.Pose.size()), Angular(Each.Pose.size());
                        if(bHadPrev && Each.PrevDelta > 0.0f)
                        {
                            for(size_t Index = 0; Index < Each.Pose.size(); ++Index)
                            {
                                const AnimVec3 Moved = AM::Scale(AM::Sub(Each.Pose[Index].Position, Each.PrevPose[Index].Position), 1.0f / Each.PrevDelta);
                                const AnimVec3 Turn  = AngularVelocity(Each.PrevPose[Index].Rotation, Each.Pose[Index].Rotation, Each.PrevDelta);
                                Linear[Index]  = EngineCore::Vector3(Moved.X, Moved.Y, Moved.Z);
                                Angular[Index] = EngineCore::Vector3(Turn.X, Turn.Y, Turn.Z);
                            }
                        }
                        Physics->SetRagdollDynamic(Each.Handle, Linear.data(), Angular.data());
                    }
                    for(uint8_t Index = 0; Index < InComponent.PendingImpulses; ++Index)
                    {
                        Physics->AddRagdollImpulse(Each.Handle, InComponent.ImpulseAmounts[Index], InComponent.ImpulsePoints[Index]);
                    }
                    Each.bDynamic = true;

                    /* The animation stops moving it (root motion, notifies). */
                    if(RenderCore::AnimationComponent* Anim = InEntity.GetComponent<RenderCore::AnimationComponent>())
                    {
                        if(!Anim->bPaused)
                        {
                            Anim->bPaused = 1;
                            Each.bPaused  = true;
                        }
                    }
                    if(InComponent.bDropProps) { FindDrops(Id, Each, Drops); }

                    InComponent.bActive          = 1;
                    InComponent.bSettled         = 0;
                    InComponent.bPendingActivate = 0;
                    InComponent.PendingImpulses  = 0;
                    Activated.push_back(Id);
                }
                else if(Each.Handle != 0)
                {
                    /* Hit boxes: where the animation has the parts now. */
                    const std::vector<JoltPhysics::RagdollTransform> Target = ToPhysics(Each.Pose);
                    Physics->SetRagdollTarget(Each.Handle, Target.data());
                }
                InComponent.Handle = Each.Handle;
                Each.bHasPrev  = true;
                Each.PrevDelta = InDeltaSeconds;
#else
                (void)Animation;
                (void)InDeltaSeconds;
                if(bWantsActivate && !bNoPhysicsWarned)
                {
                    CORE_LOG_WARN("[Ragdoll] '" + std::string(InEntity.GetName()) + "' cannot ragdoll: the JoltPhysics module is "
                                  "not compiled in (NIMBLE_WITH_JOLTPHYSICS) and there is no fallback solver yet (RAGDOLL.md)")
                    bNoPhysicsWarned = true;
                }
                Each.bStarted = true;
                InComponent.bPendingActivate = 0;
                InComponent.PendingImpulses  = 0;
#endif
            });

            /* Components gone, entities destroyed: their ragdolls go too. */
            for(auto It = Records.begin(); It != Records.end();)
            {
                if(It->second.bSeen)
                {
                    ++It;
                    continue;
                }
                const GameCore::EntityID Entity = GameCore::EntityID::Unpack(It->first);
                Release(Entity, It->second, nullptr);
                It = Records.erase(It);
            }

            /* After the walk: these change the World. */
            for(const Drop& Each : Drops) { LetGo(Each); }
            for(const GameCore::EntityID Entity : Activated) { Emit("RagdollActivated", Entity); }
        }

        void DestroyAll()
        {
            for(auto& Entry : Records)
            {
                Release(GameCore::EntityID::Unpack(Entry.first), Entry.second, nullptr);
            }
            Records.clear();
        }

        static void PoseSystem(GameCore::World&, float, void* InUserData) { static_cast<Impl*>(InUserData)->Pose(); }
        static void UpdateSystem(GameCore::World&, const float InDeltaSeconds, void* InUserData)
        {
            static_cast<Impl*>(InUserData)->Update(InDeltaSeconds);
        }
    };

    RagdollWorld::RagdollWorld(GameCore::World& InWorld) : P(new Impl(InWorld)) {}

    RagdollWorld::~RagdollWorld()
    {
        delete P;
    }

    RagdollWorld* RagdollWorld::Attach(GameCore::World& InWorld)
    {
        RagdollWorld*& System = Systems()[&InWorld];
        if(!System)
        {
            System = new RagdollWorld(InWorld);
            InWorld.RegisterSystem("Engine.Ragdoll.Pose", GameCore::TP_PostPhysics, &Impl::PoseSystem, System->P);
            InWorld.RegisterSystem("Engine.Ragdoll", GameCore::TP_PostUpdate, &Impl::UpdateSystem, System->P);
        }
        ++System->P->Attachments;
        return System;
    }

    void RagdollWorld::Detach(GameCore::World& InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(&InWorld);
        if(Found == Map.end()) { return; }
        RagdollWorld* System = Found->second;
        if(--System->P->Attachments > 0) { return; }
        InWorld.UnregisterSystem(&Impl::PoseSystem, System->P);
        InWorld.UnregisterSystem(&Impl::UpdateSystem, System->P);
        System->P->DestroyAll();
        Map.erase(Found);
        delete System;
    }

    RagdollWorld* RagdollWorld::Find(const GameCore::World* InWorld)
    {
        auto& Map = Systems();
        const auto Found = Map.find(InWorld);
        return Found != Map.end() ? Found->second : nullptr;
    }

    size_t RagdollWorld::GetActiveCount() const
    {
        size_t Count = 0;
        for(const auto& Entry : P->Records) { Count += Entry.second.bDynamic ? 1 : 0; }
        return Count;
    }

    std::string RagdollWorld::FindPart(const GameCore::EntityID InEntity, const EngineCore::Vector3& InAt) const
    {
        const auto Found = P->Records.find(InEntity.Pack());
        RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(&P->World);
        const RenderCore::AnimInstance* Instance = Animation ? Animation->FindInstance(InEntity) : nullptr;
        if(Found == P->Records.end() || !Found->second.Profile || !Instance || Instance->GetSkeleton() != Found->second.Skeleton)
        {
            return std::string();
        }
        const RagdollProfile& Profile = *Found->second.Profile;
        const GameCore::TransformPose Pose = P->World.GetEntity(InEntity).GetWorldPose();
        const AnimMatrix Entity = ToMatrix(Pose);
        const float      Scale  = UniformScale(Pose);
        const AnimVec3   At{ float(InAt.GetX()), float(InAt.GetY()), float(InAt.GetZ()) };

        /* Each part's capsule as the skeleton is posed now (the animation's,
         * or physics' once active): the distance from InAt to its surface. */
        float  Nearest = 0.0f;
        size_t Best    = 0;
        for(size_t Index = 0; Index < Profile.Parts.size(); ++Index)
        {
            const RagdollProfilePart& Part = Profile.Parts[Index];
            const AnimMatrix Bone   = AM::Mul(Entity, Instance->GetModelPose()[size_t(Part.Bone)]);
            const AnimVec3   Centre = AM::TransformPoint(Bone, Part.ShapeLocal.Translation);
            const AnimVec3   Tip    = AM::TransformPoint(Bone, AM::Add(Part.ShapeLocal.Translation,
                                                                AM::Rotate(Part.ShapeLocal.Rotation, AnimVec3{ 0.0f, Part.HalfHeight, 0.0f })));
            const AnimVec3   Half   = AM::Sub(Tip, Centre);
            const float      Length = std::max(Half.X * Half.X + Half.Y * Half.Y + Half.Z * Half.Z, 1.0e-8f);
            const AnimVec3   From   = AM::Sub(At, Centre);
            const float      Along  = std::clamp((From.X * Half.X + From.Y * Half.Y + From.Z * Half.Z) / Length, -1.0f, 1.0f);
            const AnimVec3   Off    = AM::Sub(From, AM::Scale(Half, Along));
            const float      Gap    = std::sqrt(Off.X * Off.X + Off.Y * Off.Y + Off.Z * Off.Z) - Part.Radius * Scale;
            if(Index == 0 || Gap < Nearest)
            {
                Nearest = Gap;
                Best    = Index;
            }
        }
        return Profile.Parts[Best].Name;
    }

    size_t RagdollWorld::GetRagdollCount() const
    {
        size_t Count = 0;
        for(const auto& Entry : P->Records) { Count += Entry.second.Handle != 0 ? 1 : 0; }
        return Count;
    }
}

// === Registration =============================================================

using NimbleEngine::RagdollComponent;

GAMECORE_DEFINE_COMPONENT(RagdollComponent, "Ragdoll")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(RagdollComponent, 1)
{
    Ar.Flag("Enabled",        C.bEnabled);
    Ar.Flag("HitBoxes",       C.bHitBoxes);
    Ar.Flag("StartActive",    C.bStartActive);
    Ar.Flag("DropProps",      C.bDropProps);
    Ar.Field("Mass",          C.Mass);
    Ar.Field("Thickness",     C.Thickness);
    Ar.Field("JointFriction", C.JointFriction);
    Ar.Field("Friction",      C.Friction);
    Ar.Field("LinearDamping", C.LinearDamping);
    Ar.Field("AngularDamping", C.AngularDamping);
    Ar.Field("Layer",         C.Layer);
    Ar.Field("Mask",          C.Mask);
}

/* ragdoll:Activate(), :ActivateWithImpulse(impulse, at), :AddImpulse(impulse,
 * at), :GetPartAt(at), :Reset(), :IsActive(), :IsSettled(). */
GAMECORE_DEFINE_COMPONENT_SCRIPT(RagdollComponent)
{
    Bind.Flag("Enabled",     &RagdollComponent::bEnabled);
    Bind.Flag("HitBoxes",    &RagdollComponent::bHitBoxes);
    Bind.Flag("DropProps",   &RagdollComponent::bDropProps);
    Bind.Field("Mass",       &RagdollComponent::Mass);
    Bind.ReadOnly("Active",  &RagdollComponent::bActive);
    Bind.ReadOnly("Settled", &RagdollComponent::bSettled);

    Bind.Native<&RagdollComponent::Activate>("Activate", "");
    Bind.Native<&RagdollComponent::Reset>("Reset", "");
    Bind.NativePure<&RagdollComponent::IsActive>("IsActive", "-> Active");
    Bind.NativePure<&RagdollComponent::IsSettled>("IsSettled", "-> Settled");
    Bind.Method("AddImpulse", [](RagdollComponent& C, GameCore::ScriptCall& Call)
    {
        const EngineCore::Vector3 Impulse = Call.GetVector3(1);
        const EngineCore::Vector3 At      = Call.GetVector3(4);
        if(Call.HasError()) { return; }
        C.AddImpulse(Impulse, At);
    }, "Impulse:Vector3, At:Vector3");
    Bind.Method("GetPartAt", [](RagdollComponent& C, GameCore::ScriptCall& Call)
    {
        const EngineCore::Vector3 At = Call.GetVector3(1);
        if(Call.HasError()) { return; }
        Call.PushString(C.GetPartAt(At).c_str());
    }, "At:Vector3 -> Part:string");
    Bind.Method("ActivateWithImpulse", [](RagdollComponent& C, GameCore::ScriptCall& Call)
    {
        const EngineCore::Vector3 Impulse = Call.GetVector3(1);
        const EngineCore::Vector3 At      = Call.GetVector3(4);
        if(Call.HasError()) { return; }
        C.ActivateWithImpulse(Impulse, At);
    }, "Impulse:Vector3, At:Vector3");
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Ragdoll)
{
    Lib.Event("RagdollActivated", "Entity:Entity").Tooltip("A Ragdoll component handed its skeleton to physics");
    Lib.Event("RagdollSettled",   "Entity:Entity").Tooltip("An active ragdoll came to rest");
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(RagdollComponent)
{
    Props.TypeCategory("Physics");
    Props.Flag("Enabled",      &RagdollComponent::bEnabled);
    Props.Flag("Hit Boxes",    &RagdollComponent::bHitBoxes);
    Props.Flag("Start Active", &RagdollComponent::bStartActive);
    Props.Flag("Drop Props",   &RagdollComponent::bDropProps);
    Props.Category("Body");
    Props.Field("Mass",           &RagdollComponent::Mass);
    Props.Field("Thickness",      &RagdollComponent::Thickness);
    Props.Field("Joint Friction", &RagdollComponent::JointFriction);
    Props.Category("Material");
    Props.Field("Friction",        &RagdollComponent::Friction);
    Props.Field("Linear Damping",  &RagdollComponent::LinearDamping);
    Props.Field("Angular Damping", &RagdollComponent::AngularDamping);
    Props.Category("Filtering");
    Props.Field("Layer", &RagdollComponent::Layer);
    Props.Field("Mask",  &RagdollComponent::Mask);
    Props.Category("State");
    Props.Flag("Active",  &RagdollComponent::bActive, GameCore::PF_ReadOnly);
    Props.Flag("Settled", &RagdollComponent::bSettled, GameCore::PF_ReadOnly);
}
#endif
