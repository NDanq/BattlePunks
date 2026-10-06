#include "Public/JoltPhysics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/BoundsComponent.h"
#include "Public/GameCore/Components/CollisionComponent.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/PhysicsConstraintComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"

#include "Private/JoltPhysics/JoltConvert.h"
#include "Private/JoltPhysics/JoltLayers.h"
#include "Private/JoltPhysics/JoltRuntime.h"
#include "Private/JoltPhysics/JoltShapes.h"

/* PhysicsWorld.h has the contract; README.md the model. */

namespace JoltPhysics
{
    namespace
    {
        /* World -> PhysicsWorld, for Find. A handful at most. */
        std::vector<std::pair<const GameCore::World*, PhysicsWorld*>>& Attached()
        {
            static std::vector<std::pair<const GameCore::World*, PhysicsWorld*>> Entries;
            return Entries;
        }

        /* FNV-1a over the fields that shape a body or a joint: any change remakes it. */
        struct Signature
        {
            uint64_t Hash = 14695981039346656037ull;

            void Bytes(const void* InData, const size_t InSize)
            {
                const auto* Data = static_cast<const unsigned char*>(InData);
                for(size_t Index = 0; Index < InSize; ++Index)
                {
                    Hash ^= Data[Index];
                    Hash *= 1099511628211ull;
                }
            }
            template<typename T> void Add(const T& InValue) { Bytes(&InValue, sizeof(T)); }
            void Add(const EngineCore::Vector3& InV)
            {
                /* Quantised, so float noise in a re-read scale does not remake the shape. */
                for(const double Each : { double(InV.GetX()), double(InV.GetY()), double(InV.GetZ()) })
                {
                    Add(int64_t(std::llround(Each * 10000.0)));
                }
            }
            void Add(const std::string& InText) { Bytes(InText.data(), InText.size()); Add(uint8_t(0)); }
        };

        bool NearlyEqual(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, const double InTolerance)
        {
            return std::abs(double(InA.GetX()) - double(InB.GetX())) <= InTolerance
                && std::abs(double(InA.GetY()) - double(InB.GetY())) <= InTolerance
                && std::abs(double(InA.GetZ()) - double(InB.GetZ())) <= InTolerance;
        }

        bool IsZero(const EngineCore::Vector3& InV)
        {
            return InV.GetX() == 0.0f && InV.GetY() == 0.0f && InV.GetZ() == 0.0f;
        }

        uint64_t PairKey(const JPH::BodyID InA, const JPH::BodyID InB)
        {
            const uint32_t A = InA.GetIndexAndSequenceNumber(), B = InB.GetIndexAndSequenceNumber();
            return A < B ? (uint64_t(A) << 32) | B : (uint64_t(B) << 32) | A;
        }

        /* Any unit vector perpendicular to InAxis. */
        JPH::Vec3 Perpendicular(const JPH::Vec3 InAxis)
        {
            return InAxis.GetNormalizedPerpendicular();
        }

        /* Jolt calls these on its worker threads, during the step: queued and
         * told on the game thread afterwards (Impl::FlushContacts). */
        class ContactQueue final : public JPH::ContactListener
        {
        public:
            struct Event
            {
                JPH::BodyID A;
                JPH::BodyID B;
                bool        bAdded = true;
            };

            void OnContactAdded(const JPH::Body& InA, const JPH::Body& InB, const JPH::ContactManifold&, JPH::ContactSettings&) override
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                Events.push_back(Event{ InA.GetID(), InB.GetID(), true });
            }

            void OnContactRemoved(const JPH::SubShapeIDPair& InPair) override
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                Events.push_back(Event{ InPair.GetBody1ID(), InPair.GetBody2ID(), false });
            }

            std::vector<Event> Take()
            {
                std::lock_guard<std::mutex> Lock(Mutex);
                std::vector<Event> Out;
                Out.swap(Events);
                return Out;
            }

        private:
            std::mutex         Mutex;
            std::vector<Event> Events;
        };

        /* Raycasts skip sensors. */
        class SkipSensors final : public JPH::BodyFilter
        {
        public:
            bool ShouldCollideLocked(const JPH::Body& InBody) const override { return !InBody.IsSensor(); }
        };
    }

    // === Impl ====================================================================

    struct PhysicsWorld::Impl
    {
        /* One Rigid Body's body, by its entity. */
        struct BodyRecord
        {
            JPH::BodyID        Id;
            GameCore::EntityID Entity;
            uint64_t           Signature = 0;
            uint8_t            Motion    = GameCore::RBM_Dynamic;   // as simulated (a replica's dynamic is kinematic)
            bool               bSensor   = false;
            bool               bSeen     = false;

            /* The two latest steps' poses (metres), blended for a Dynamic body. */
            JPH::RVec3 PrevPosition, CurrPosition;
            JPH::Quat  PrevRotation = JPH::Quat::sIdentity(), CurrRotation = JPH::Quat::sIdentity();

            /* The world pose this body last wrote or was placed at, as read back
             * from the entity: anything else means game code moved it. */
            EngineCore::Vector3 WrittenPosition;
            EngineCore::Vector3 WrittenRotation;

            /* Forces and torques asked for (Jolt units), applied before every
             * step until one has happened. */
            JPH::Vec3 Force  = JPH::Vec3::sZero();
            JPH::Vec3 Torque = JPH::Vec3::sZero();
        };

        struct ConstraintRecord
        {
            JPH::Ref<JPH::Constraint> Constraint;
            JPH::BodyID               A;
            JPH::BodyID               B;   // invalid: the world
            uint64_t                  Signature = 0;
            bool                      bSeen     = false;
        };

        GameCore::World* World = nullptr;
        PhysicsSettings  Settings;
        bool             bRuntime = false;

        Layers::BroadPhaseLayers   BroadPhaseLayers;
        Layers::ObjectVsBroadPhase ObjectVsBroadPhase;
        Layers::ObjectPairs        ObjectPairs;
        JPH::PhysicsSystem*        System = nullptr;
        ContactQueue               Contacts;
        ShapeBuilder               Shapes;

        std::unordered_map<uint64_t, BodyRecord>       Bodies;        // by EntityID::Pack()
        std::unordered_map<uint32_t, uint64_t>         BodyEntities;  // BodyID -> EntityID::Pack()
        std::unordered_map<uint64_t, ConstraintRecord> Constraints;   // by the constraint entity's Pack()
        std::unordered_set<uint64_t>                   Touching;      // body pairs told as touching (PairKey)

        double   Accumulator = 0.0;
        uint64_t Steps       = 0;

        GameCore::EventID ContactBeginId = 0, ContactEndId = 0, SensorEnterId = 0, SensorExitId = 0;

        double ToMeters() const   { return 1.0 / Settings.UnitsPerMeter; }
        double FromMeters() const { return Settings.UnitsPerMeter; }
        float  FixedDelta() const { return 1.0f / Settings.FixedHz; }

        JPH::BodyInterface& Interface() { return System->GetBodyInterfaceNoLock(); }

        // --- Bodies ----------------------------------------------------------

        /* What InEntity's body would be made from. */
        ShapeRequest MakeRequest(const GameCore::Entity& InEntity, const GameCore::RigidBodyComponent& InBody,
                                 const uint8_t InMotion, const EngineCore::Vector3& InScale) const
        {
            ShapeRequest Request;
            Request.Shape         = InBody.Shape;
            Request.Source        = InBody.ShapeSource;
            Request.Motion        = InMotion;
            Request.Scale         = InScale;
            Request.UnitsPerMeter = Settings.UnitsPerMeter;

            if(const GameCore::CollisionMeshProvider* Provider = GameCore::GetCollisionMeshProvider())
            {
                if(const char* Path = Provider->GetMeshPath(*World, InEntity.GetID(), Provider->UserData))
                {
                    Request.bHasMesh = true;
                    Request.MeshPath = Path;
                }
            }
            if(const auto* Bounds = InEntity.GetComponent<GameCore::BoundsComponent>())
            {
                Request.bHasBounds  = true;
                Request.BoundsLocal = Bounds->Local;
            }
            return Request;
        }

        uint64_t SignatureOf(const GameCore::RigidBodyComponent& InBody, const ShapeRequest& InRequest) const
        {
            Signature S;
            S.Add(InRequest.Motion);
            S.Add(InBody.Shape);
            S.Add(InBody.ShapeSource);
            S.Add(InBody.Mass);
            S.Add(InBody.Friction);
            S.Add(InBody.Restitution);
            S.Add(InBody.LinearDamping);
            S.Add(InBody.AngularDamping);
            S.Add(InBody.GravityFactor);
            S.Add(InBody.bContinuous);
            S.Add(InBody.bSensor);
            S.Add(InBody.bAllowSleeping);
            S.Add(InBody.Layer);
            S.Add(InBody.Mask);
            S.Add(InRequest.bHasMesh);
            S.Add(InRequest.MeshPath);
            S.Add(InRequest.bHasBounds);
            if(InRequest.bHasBounds)
            {
                S.Add(InRequest.BoundsLocal.Min);
                S.Add(InRequest.BoundsLocal.Max);
            }
            S.Add(InRequest.Scale);
            return S.Hash;
        }

        void DestroyBody(BodyRecord& InRecord)
        {
            if(InRecord.Id.IsInvalid()) { return; }

            /* A survivor touching it hears the contact end now: the entity may
             * be gone by the time Jolt would say so. */
            for(auto It = Touching.begin(); It != Touching.end();)
            {
                const uint32_t Low = uint32_t(*It & 0xFFFFFFFFu), High = uint32_t(*It >> 32);
                const uint32_t Mine = InRecord.Id.GetIndexAndSequenceNumber();
                if(Low == Mine || High == Mine)
                {
                    const JPH::BodyID Other(Low == Mine ? High : Low);
                    TellPair(InRecord.Id, Other, false, true);
                    It = Touching.erase(It);
                }
                else
                {
                    ++It;
                }
            }

            BodyEntities.erase(InRecord.Id.GetIndexAndSequenceNumber());
            Interface().RemoveBody(InRecord.Id);
            Interface().DestroyBody(InRecord.Id);
            InRecord.Id = JPH::BodyID();
        }

        /* A new body for InEntity at InPose, keeping InLinear / InAngular
         * (metres, radians) when it replaces one. */
        bool CreateBody(BodyRecord& InRecord, const GameCore::Entity& InEntity, GameCore::RigidBodyComponent& InBody,
                        const ShapeRequest& InRequest, const GameCore::TransformPose& InPose, const bool bInFirst,
                        const JPH::Vec3 InLinear, const JPH::Vec3 InAngular)
        {
            std::string Kind;
            JPH::RefConst<JPH::Shape> Shape = Shapes.Build(InRequest, Kind);
            if(!Shape)
            {
                return false;
            }

            const uint8_t Motion = InRequest.Motion;
            const JPH::EMotionType MotionType = Motion == GameCore::RBM_Static    ? JPH::EMotionType::Static
                                              : Motion == GameCore::RBM_Kinematic ? JPH::EMotionType::Kinematic
                                                                                  : JPH::EMotionType::Dynamic;
            const JPH::RVec3 Position = Convert::ToRVec3(InPose.Position, ToMeters());
            const JPH::Quat  Rotation = Convert::EulerToQuat(InPose.Rotation);

            JPH::BodyCreationSettings Create(Shape, Position, Rotation, MotionType,
                                             Layers::Make(InBody.Layer, InBody.Mask, Motion != GameCore::RBM_Static));
            Create.mFriction        = std::max(InBody.Friction, 0.0f);
            Create.mRestitution     = std::clamp(InBody.Restitution, 0.0f, 1.0f);
            Create.mLinearDamping   = std::max(InBody.LinearDamping, 0.0f);
            Create.mAngularDamping  = std::max(InBody.AngularDamping, 0.0f);
            Create.mGravityFactor   = InBody.GravityFactor;
            Create.mIsSensor        = InBody.bSensor != 0;
            Create.mAllowSleeping   = InBody.bAllowSleeping != 0;
            Create.mMotionQuality   = InBody.bContinuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            Create.mUserData        = InEntity.GetID().Pack();
            /* A kinematic sensor also notices what does not move. */
            Create.mCollideKinematicVsNonDynamic = InBody.bSensor != 0 && Motion == GameCore::RBM_Kinematic;
            if(InBody.Mass > 0.0f && Motion == GameCore::RBM_Dynamic)
            {
                Create.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
                Create.mMassPropertiesOverride.mMass = InBody.Mass;
            }
            if(Motion == GameCore::RBM_Dynamic)
            {
                Create.mLinearVelocity  = bInFirst ? Convert::ToVec3(InBody.InitialLinearVelocity, ToMeters()) : InLinear;
                Create.mAngularVelocity = bInFirst ? Convert::ToVec3(InBody.InitialAngularVelocity, Convert::DegToRad(1.0)) : InAngular;
            }

            JPH::Body* Body = Interface().CreateBody(Create);
            if(!Body)
            {
                CORE_LOG_ERROR(std::string("[Physics] out of bodies ([Physics] MaxBodies) making '") + InEntity.GetName() + "'")
                return false;
            }
            Interface().AddBody(Body->GetID(), Motion == GameCore::RBM_Static ? JPH::EActivation::DontActivate
                                                                              : JPH::EActivation::Activate);

            InRecord.Id           = Body->GetID();
            InRecord.Entity       = InEntity.GetID();
            InRecord.Motion       = Motion;
            InRecord.bSensor      = InBody.bSensor != 0;
            InRecord.PrevPosition = InRecord.CurrPosition = Position;
            InRecord.PrevRotation = InRecord.CurrRotation = Rotation;
            InRecord.WrittenPosition = InPose.Position;
            InRecord.WrittenRotation = InPose.Rotation;
            BodyEntities[InRecord.Id.GetIndexAndSequenceNumber()] = InEntity.GetID().Pack();
            InBody.BodyHandle = InRecord.Id.GetIndexAndSequenceNumber();
            return true;
        }

        /* Every Rigid Body to its body: made, remade, moved, pushed. Then the
         * bodies whose component went. InSteps * FixedDelta() is the time this
         * frame's steps cover, for kinematic moves. */
        void SyncBodies(const uint32_t InSteps)
        {
            for(auto& Entry : Bodies) { Entry.second.bSeen = false; }

            struct Pending
            {
                GameCore::EntityID      Entity;
                ShapeRequest            Request;
                uint64_t                Signature = 0;
                GameCore::TransformPose Pose;
            };
            std::vector<Pending> Remake;   // new, or changed

            const bool bReplica = Settings.bReplicaKinematic;
            World->Each<GameCore::RigidBodyComponent>([&](GameCore::Entity InEntity, GameCore::RigidBodyComponent& InBody)
            {
                if(!InBody.bEnabled)
                {
                    InBody.BodyHandle = 0xFFFFFFFFu;
                    return;
                }

                uint8_t Motion = InBody.Motion <= GameCore::RBM_Static ? InBody.Motion : GameCore::RBM_Dynamic;
                if(bReplica && Motion == GameCore::RBM_Dynamic && InEntity.HasComponent<GameCore::NetComponent>())
                {
                    Motion = GameCore::RBM_Kinematic;
                }

                const GameCore::TransformPose Pose = InEntity.GetWorldPose();
                const ShapeRequest Request = MakeRequest(InEntity, InBody, Motion, Pose.Scale);
                const uint64_t     Sig     = SignatureOf(InBody, Request);

                auto Found = Bodies.find(InEntity.GetID().Pack());
                if(Found == Bodies.end() || Found->second.Signature != Sig)
                {
                    if(Found != Bodies.end()) { Found->second.bSeen = true; }
                    Remake.push_back(Pending{ InEntity.GetID(), Request, Sig, Pose });
                    return;
                }

                BodyRecord& Record = Found->second;
                Record.bSeen = true;
                if(!Record.Id.IsInvalid())   // invalid: it could not be made, and nothing has changed since
                {
                    UpdateBody(Record, InBody, Pose, InSteps);
                }
            });

            /* Joints go before their bodies do. */
            std::unordered_set<uint32_t> Going;
            for(auto& Entry : Bodies)
            {
                if(!Entry.second.bSeen) { Going.insert(Entry.second.Id.GetIndexAndSequenceNumber()); }
            }
            for(const Pending& Each : Remake)
            {
                auto Found = Bodies.find(Each.Entity.Pack());
                if(Found != Bodies.end()) { Going.insert(Found->second.Id.GetIndexAndSequenceNumber()); }
            }
            if(!Going.empty())
            {
                for(auto& Entry : Constraints)
                {
                    ConstraintRecord& Joint = Entry.second;
                    if(Joint.Constraint && (Going.count(Joint.A.GetIndexAndSequenceNumber())
                                            || (!Joint.B.IsInvalid() && Going.count(Joint.B.GetIndexAndSequenceNumber()))))
                    {
                        RemoveConstraint(Joint);
                    }
                }
            }

            for(auto It = Bodies.begin(); It != Bodies.end();)
            {
                if(!It->second.bSeen)
                {
                    DestroyBody(It->second);
                    It = Bodies.erase(It);
                }
                else
                {
                    ++It;
                }
            }

            size_t Made = 0;
            for(const Pending& Each : Remake)
            {
                GameCore::Entity Entity = World->GetEntity(Each.Entity);
                GameCore::RigidBodyComponent* Body = Entity.GetComponent<GameCore::RigidBodyComponent>();
                if(!Body) { continue; }

                BodyRecord& Record = Bodies[Each.Entity.Pack()];
                const bool bFirst = Record.Id.IsInvalid() && Record.Signature == 0;
                JPH::Vec3 Linear = JPH::Vec3::sZero(), Angular = JPH::Vec3::sZero();
                if(!Record.Id.IsInvalid())
                {
                    Linear  = Interface().GetLinearVelocity(Record.Id);
                    Angular = Interface().GetAngularVelocity(Record.Id);
                    DestroyBody(Record);
                }
                Record.Signature = Each.Signature;   // a failure is not retried until something changes
                if(CreateBody(Record, Entity, *Body, Each.Request, Each.Pose, bFirst, Linear, Angular))
                {
                    ++Made;
                    UpdateBody(Record, *Body, Each.Pose, InSteps);
                }
            }

            /* A level's worth at once: rebuild the broad phase's trees. */
            if(Made >= 64)
            {
                System->OptimizeBroadPhase();
            }
        }

        /* An existing body: teleports, kinematic moves, what the component asked for. */
        void UpdateBody(BodyRecord& InRecord, GameCore::RigidBodyComponent& InBody, const GameCore::TransformPose& InPose,
                        const uint32_t InSteps)
        {
            JPH::BodyInterface& BodyI = Interface();
            const bool bMoved = !NearlyEqual(InPose.Position, InRecord.WrittenPosition, 1.0e-2)
                             || !NearlyEqual(InPose.Rotation, InRecord.WrittenRotation, 1.0e-2);

            const JPH::RVec3 Position = Convert::ToRVec3(InPose.Position, ToMeters());
            const JPH::Quat  Rotation = Convert::EulerToQuat(InPose.Rotation);

            switch(InRecord.Motion)
            {
                case GameCore::RBM_Static:
                    if(bMoved)
                    {
                        BodyI.SetPositionAndRotation(InRecord.Id, Position, Rotation, JPH::EActivation::DontActivate);
                        InRecord.WrittenPosition = InPose.Position;
                        InRecord.WrittenRotation = InPose.Rotation;
                    }
                    return;

                case GameCore::RBM_Kinematic:
                    if(InSteps > 0)
                    {
                        BodyI.MoveKinematic(InRecord.Id, Position, Rotation, float(InSteps) * FixedDelta());
                        InRecord.WrittenPosition = InPose.Position;
                        InRecord.WrittenRotation = InPose.Rotation;
                    }
                    break;

                default:
                    if(bMoved)
                    {
                        /* Game code moved it: a teleport, without a blend. */
                        BodyI.SetPositionAndRotation(InRecord.Id, Position, Rotation, JPH::EActivation::Activate);
                        InRecord.PrevPosition = InRecord.CurrPosition = Position;
                        InRecord.PrevRotation = InRecord.CurrRotation = Rotation;
                        InRecord.WrittenPosition = InPose.Position;
                        InRecord.WrittenRotation = InPose.Rotation;
                    }
                    break;
            }

            if(InRecord.Motion != GameCore::RBM_Dynamic)
            {
                InBody.PendingImpulse = InBody.PendingAngularImpulse = InBody.PendingForce = InBody.PendingTorque = EngineCore::Vector3();
                InBody.bSetLinearVelocity = InBody.bSetAngularVelocity = 0;
                return;
            }

            const double M = ToMeters();
            if(InBody.bSetLinearVelocity)
            {
                BodyI.SetLinearVelocity(InRecord.Id, Convert::ToVec3(InBody.PendingLinearVelocity, M));
                BodyI.ActivateBody(InRecord.Id);
            }
            if(InBody.bSetAngularVelocity)
            {
                BodyI.SetAngularVelocity(InRecord.Id, Convert::ToVec3(InBody.PendingAngularVelocity, Convert::DegToRad(1.0)));
                BodyI.ActivateBody(InRecord.Id);
            }
            if(!IsZero(InBody.PendingImpulse))        { BodyI.AddImpulse(InRecord.Id, Convert::ToVec3(InBody.PendingImpulse, M)); }
            if(!IsZero(InBody.PendingAngularImpulse)) { BodyI.AddAngularImpulse(InRecord.Id, Convert::ToVec3(InBody.PendingAngularImpulse, M * M)); }
            InRecord.Force  += Convert::ToVec3(InBody.PendingForce, M);
            InRecord.Torque += Convert::ToVec3(InBody.PendingTorque, M * M);

            InBody.PendingImpulse = InBody.PendingAngularImpulse = InBody.PendingForce = InBody.PendingTorque = EngineCore::Vector3();
            InBody.bSetLinearVelocity = InBody.bSetAngularVelocity = 0;
        }

        // --- Constraints -----------------------------------------------------

        void RemoveConstraint(ConstraintRecord& InJoint)
        {
            if(InJoint.Constraint)
            {
                System->RemoveConstraint(InJoint.Constraint);
                InJoint.Constraint = nullptr;
            }
            InJoint.Signature = 0;
        }

        const BodyRecord* FindBody(const GameCore::EntityID InEntity) const
        {
            if(!InEntity.IsSet()) { return nullptr; }
            const auto Found = Bodies.find(InEntity.Pack());
            return Found != Bodies.end() && !Found->second.Id.IsInvalid() ? &Found->second : nullptr;
        }

        void SyncConstraints()
        {
            for(auto& Entry : Constraints) { Entry.second.bSeen = false; }

            World->Each<GameCore::PhysicsConstraintComponent>([&](GameCore::Entity InEntity, GameCore::PhysicsConstraintComponent& InJoint)
            {
                ConstraintRecord& Record = Constraints[InEntity.GetID().Pack()];
                Record.bSeen = true;

                const BodyRecord* A = InJoint.bEnabled ? FindBody(InEntity.GetID()) : nullptr;

                /* The other end: Target, else TargetName, else the world. */
                GameCore::EntityID Other = InJoint.Target;
                if((!Other.IsSet() || !World->IsAlive(Other)) && InJoint.TargetName[0] != '\0')
                {
                    Other = World->FindEntityByName(InJoint.TargetName);
                }
                const bool bToWorld = !Other.IsSet() && InJoint.TargetName[0] == '\0';
                const BodyRecord* B = bToWorld ? nullptr : FindBody(Other);

                if(!A || (!bToWorld && !B) || (B && B->Id == A->Id))
                {
                    /* An end without a body yet (or none at all): wait. */
                    RemoveConstraint(Record);
                    InJoint.Handle = 0;
                    return;
                }

                Signature S;
                S.Add(InJoint.Type);
                S.Add(InJoint.Pivot);
                S.Add(InJoint.Axis);
                S.Add(InJoint.LimitMin);
                S.Add(InJoint.LimitMax);
                S.Add(A->Id.GetIndexAndSequenceNumber());
                S.Add(B ? B->Id.GetIndexAndSequenceNumber() : 0u);
                if(Record.Constraint && Record.Signature == S.Hash)
                {
                    ApplyMotor(Record, InJoint);
                    return;
                }

                RemoveConstraint(Record);
                Record.Constraint = MakeConstraint(InEntity, InJoint, *A, B);
                Record.A          = A->Id;
                Record.B          = B ? B->Id : JPH::BodyID();
                Record.Signature  = S.Hash;
                InJoint.Handle    = Record.Constraint ? uint64_t(reinterpret_cast<uintptr_t>(Record.Constraint.GetPtr())) : 0;
                if(Record.Constraint)
                {
                    System->AddConstraint(Record.Constraint);
                    ApplyMotor(Record, InJoint);
                    Interface().ActivateBody(A->Id);
                }
            });

            for(auto It = Constraints.begin(); It != Constraints.end();)
            {
                if(!It->second.bSeen)
                {
                    RemoveConstraint(It->second);
                    It = Constraints.erase(It);
                }
                else
                {
                    ++It;
                }
            }
        }

        JPH::Ref<JPH::Constraint> MakeConstraint(const GameCore::Entity& InEntity, const GameCore::PhysicsConstraintComponent& InJoint,
                                                 const BodyRecord& InA, const BodyRecord* InB)
        {
            const JPH::BodyLockInterfaceNoLock& Locks = System->GetBodyLockInterfaceNoLock();
            JPH::Body* BodyA = Locks.TryGetBody(InA.Id);
            JPH::Body* BodyB = InB ? Locks.TryGetBody(InB->Id) : &JPH::Body::sFixedToWorld;
            if(!BodyA || !BodyB) { return nullptr; }

            /* Pivot and axis from this entity's space into the world, as it stands. */
            const GameCore::TransformPose Pose = InEntity.GetWorldPose();
            const GameCore::Rotation3 Turn = GameCore::TransformMath::EulerToRotation(Pose.Rotation);
            const EngineCore::Vector3 PivotWorld = Pose.Position + Turn * InJoint.Pivot;
            const JPH::RVec3 Pivot = Convert::ToRVec3(PivotWorld, ToMeters());
            JPH::Vec3 Axis = Convert::ToVec3(Turn * InJoint.Axis);
            Axis = Axis.IsNearZero() ? JPH::Vec3::sAxisY() : Axis.Normalized();
            const JPH::Vec3 Normal = Perpendicular(Axis);
            const bool bLimited = InJoint.LimitMax > InJoint.LimitMin;

            switch(InJoint.Type)
            {
                case GameCore::PCT_Fixed:
                {
                    JPH::FixedConstraintSettings Settings;
                    Settings.mAutoDetectPoint = true;
                    return Settings.Create(*BodyA, *BodyB);
                }
                case GameCore::PCT_Hinge:
                {
                    JPH::HingeConstraintSettings Settings;
                    Settings.mPoint1 = Settings.mPoint2 = Pivot;
                    Settings.mHingeAxis1 = Settings.mHingeAxis2 = Axis;
                    Settings.mNormalAxis1 = Settings.mNormalAxis2 = Normal;
                    if(bLimited)
                    {
                        Settings.mLimitsMin = float(Convert::DegToRad(std::max(InJoint.LimitMin, -180.0f)));
                        Settings.mLimitsMax = float(Convert::DegToRad(std::min(InJoint.LimitMax, 180.0f)));
                    }
                    return Settings.Create(*BodyA, *BodyB);
                }
                case GameCore::PCT_Slider:
                {
                    JPH::SliderConstraintSettings Settings;
                    Settings.mAutoDetectPoint = true;
                    Settings.SetSliderAxis(Axis);
                    if(bLimited)
                    {
                        Settings.mLimitsMin = float(InJoint.LimitMin * ToMeters());
                        Settings.mLimitsMax = float(InJoint.LimitMax * ToMeters());
                    }
                    return Settings.Create(*BodyA, *BodyB);
                }
                case GameCore::PCT_Distance:
                {
                    /* A rope from this body's origin to Pivot, which stays
                     * where it is on the other end (or in the world). */
                    JPH::DistanceConstraintSettings Settings;
                    Settings.mPoint1 = Convert::ToRVec3(Pose.Position, ToMeters());
                    Settings.mPoint2 = Pivot;
                    if(InJoint.LimitMax > 0.0f)
                    {
                        Settings.mMinDistance = float(std::max(InJoint.LimitMin, 0.0f) * ToMeters());
                        Settings.mMaxDistance = float(InJoint.LimitMax * ToMeters());
                    }
                    return Settings.Create(*BodyA, *BodyB);
                }
                default:
                {
                    JPH::PointConstraintSettings Settings;
                    Settings.mPoint1 = Settings.mPoint2 = Pivot;
                    return Settings.Create(*BodyA, *BodyB);
                }
            }
        }

        void ApplyMotor(ConstraintRecord& InRecord, const GameCore::PhysicsConstraintComponent& InJoint)
        {
            if(!InRecord.Constraint) { return; }
            const JPH::EMotorState State = InJoint.bMotor ? JPH::EMotorState::Velocity : JPH::EMotorState::Off;
            if(InJoint.Type == GameCore::PCT_Hinge)
            {
                auto* Hinge = static_cast<JPH::HingeConstraint*>(InRecord.Constraint.GetPtr());
                Hinge->SetMotorState(State);
                Hinge->SetTargetAngularVelocity(float(Convert::DegToRad(InJoint.MotorSpeed)));
            }
            else if(InJoint.Type == GameCore::PCT_Slider)
            {
                auto* Slider = static_cast<JPH::SliderConstraint*>(InRecord.Constraint.GetPtr());
                Slider->SetMotorState(State);
                Slider->SetTargetVelocity(float(InJoint.MotorSpeed * ToMeters()));
            }
            if(InJoint.bMotor)
            {
                Interface().ActivateBody(InRecord.A);
            }
        }

        // --- Stepping and writing back ---------------------------------------

        void StepOnce()
        {
            JPH::BodyInterface& BodiesI = Interface();
            for(auto& Entry : Bodies)
            {
                BodyRecord& Record = Entry.second;
                if(Record.Motion != GameCore::RBM_Dynamic || Record.Id.IsInvalid()) { continue; }
                if(!Record.Force.IsNearZero(0.0f))  { BodiesI.AddForce(Record.Id, Record.Force); }
                if(!Record.Torque.IsNearZero(0.0f)) { BodiesI.AddTorque(Record.Id, Record.Torque); }
                Record.PrevPosition = Record.CurrPosition;
                Record.PrevRotation = Record.CurrRotation;
            }

            const JPH::EPhysicsUpdateError Error = System->Update(FixedDelta(), 1, &Runtime::GetTempAllocator(), &Runtime::GetJobSystem());
            if(Error != JPH::EPhysicsUpdateError::None)
            {
                CORE_LOG_WARN("[Physics] Jolt ran out of room in a step (raise [Physics] MaxBodies, or fewer bodies touching)")
            }
            ++Steps;

            for(auto& Entry : Bodies)
            {
                BodyRecord& Record = Entry.second;
                if(Record.Motion != GameCore::RBM_Dynamic || Record.Id.IsInvalid()) { continue; }
                BodiesI.GetPositionAndRotation(Record.Id, Record.CurrPosition, Record.CurrRotation);
            }

            FlushContacts();
        }

        void WriteBack()
        {
            const float Alpha = Settings.bInterpolate ? float(std::clamp(Accumulator / FixedDelta(), 0.0, 1.0)) : 1.0f;
            JPH::BodyInterface& BodiesI = Interface();

            for(auto& Entry : Bodies)
            {
                BodyRecord& Record = Entry.second;
                if(Record.Id.IsInvalid()) { continue; }
                GameCore::Entity Entity = World->GetEntity(Record.Entity);
                GameCore::RigidBodyComponent* Body = Entity.GetComponent<GameCore::RigidBodyComponent>();
                if(!Body) { continue; }

                Body->bAwake = BodiesI.IsActive(Record.Id) ? 1 : 0;
                if(Record.Motion == GameCore::RBM_Static)
                {
                    continue;
                }
                Body->LinearVelocity  = Convert::FromVec3(BodiesI.GetLinearVelocity(Record.Id), FromMeters());
                Body->AngularVelocity = Convert::FromVec3(BodiesI.GetAngularVelocity(Record.Id), Convert::RadToDeg(1.0));
                if(Record.Motion != GameCore::RBM_Dynamic)
                {
                    continue;
                }

                const JPH::RVec3 Position = Record.PrevPosition + (Record.CurrPosition - Record.PrevPosition) * JPH::Real(Alpha);
                const JPH::Quat  Rotation = Record.PrevRotation.SLERP(Record.CurrRotation, Alpha).Normalized();

                GameCore::TransformPose Pose = Entity.GetWorldPose();
                Pose.Position = Convert::FromRVec3(Position, FromMeters());
                Pose.Rotation = Convert::QuatToEuler(Rotation, Pose.Rotation);
                if(Entity.SetWorldPose(Pose))
                {
                    /* As the entity reads it back, so float rounding is not a teleport. */
                    const GameCore::TransformPose Read = Entity.GetWorldPose();
                    Record.WrittenPosition = Read.Position;
                    Record.WrittenRotation = Read.Rotation;
                }
            }
        }

        // --- Contacts ----------------------------------------------------------

        GameCore::EntityID EntityOf(const JPH::BodyID InBody) const
        {
            const auto Found = BodyEntities.find(InBody.GetIndexAndSequenceNumber());
            return Found != BodyEntities.end() ? GameCore::EntityID::Unpack(Found->second) : GameCore::EntityID();
        }

        bool IsSensorBody(const JPH::BodyID InBody) const
        {
            const GameCore::EntityID Entity = EntityOf(InBody);
            const auto Found = Entity.IsSet() ? Bodies.find(Entity.Pack()) : Bodies.end();
            return Found != Bodies.end() && Found->second.bSensor;
        }

        /* One pair began or ended: each side told, or a sensor told what it holds. */
        void TellPair(const JPH::BodyID InA, const JPH::BodyID InB, const bool bInBegin, const bool bInSurvivorOnlyOfA)
        {
            const GameCore::EntityID A = EntityOf(InA), B = EntityOf(InB);
            const bool bSensorA = IsSensorBody(InA), bSensorB = IsSensorBody(InB);
            GameCore::EventBus& Events = World->GetEvents();

            auto Tell = [&](const GameCore::EntityID InSelf, const GameCore::EntityID InOther, const bool bInSensor)
            {
                if(!InSelf.IsSet() || !World->IsAlive(InSelf)) { return; }
                const GameCore::EventID Id = bInSensor ? (bInBegin ? SensorEnterId : SensorExitId)
                                                       : (bInBegin ? ContactBeginId : ContactEndId);
                const char* Method = bInSensor ? (bInBegin ? "OnSensorEnter" : "OnSensorExit")
                                               : (bInBegin ? "OnContactBegin" : "OnContactEnd");
                GameCore::EventArgs Args;
                Args.Args[0] = GameCore::EventArg::Make(InSelf);
                Args.Args[1] = GameCore::EventArg::Make(InOther);
                Args.Count   = 2;
                Events.Emit(Id, Args);
                World->CallScriptMethod(InSelf, Method, InOther);
            };

            if(bSensorA || bSensorB)
            {
                /* The sensor is Self; a sensor touching a sensor tells neither. */
                if(bSensorA && !bSensorB && !bInSurvivorOnlyOfA) { Tell(A, B, true); }
                if(bSensorB && !bSensorA)                        { Tell(B, A, true); }
                return;
            }
            if(!bInSurvivorOnlyOfA) { Tell(A, B, false); }
            Tell(B, A, false);
        }

        /*
         * Jolt reports each sub-shape contact, and removes a pair's contacts
         * when either body falls asleep. A pair is told once when it begins
         * and once when its bodies really part: a removal while they still
         * touch (another sub-shape) or while one sleeps (dormant, still
         * resting there) is not an end. Bodies removed are told by
         * DestroyBody, so their late removals find no pair.
         */
        void FlushContacts()
        {
            for(const ContactQueue::Event& Event : Contacts.Take())
            {
                const uint64_t Key = PairKey(Event.A, Event.B);
                if(Event.bAdded)
                {
                    if(Touching.insert(Key).second)
                    {
                        TellPair(Event.A, Event.B, true, false);
                    }
                    continue;
                }

                if(Touching.find(Key) == Touching.end()) { continue; }
                if(!BodyEntities.count(Event.A.GetIndexAndSequenceNumber())
                   || !BodyEntities.count(Event.B.GetIndexAndSequenceNumber()))
                {
                    continue;
                }
                if(System->WereBodiesInContact(Event.A, Event.B)) { continue; }
                if(!Interface().IsActive(Event.A) || !Interface().IsActive(Event.B))
                {
                    /* A sleeping side: if it was resting there, it still is. Only
                     * a pair one of whose bodies cannot sleep in place (both
                     * awake) has parted. A static body is never active, so
                     * look at the other. */
                    const bool bAStatic = Interface().GetMotionType(Event.A) == JPH::EMotionType::Static;
                    const bool bBStatic = Interface().GetMotionType(Event.B) == JPH::EMotionType::Static;
                    const bool bASleeps = !bAStatic && !Interface().IsActive(Event.A);
                    const bool bBSleeps = !bBStatic && !Interface().IsActive(Event.B);
                    if(bASleeps || bBSleeps) { continue; }
                }

                Touching.erase(Key);
                TellPair(Event.A, Event.B, false, false);
            }
        }

        // --- The frame ----------------------------------------------------------

        void Frame(const float InDeltaSeconds)
        {
            /* How many fixed steps this frame holds; the rest waits. */
            Accumulator += double(std::max(InDeltaSeconds, 0.0f));
            uint32_t StepsNow = uint32_t(Accumulator / FixedDelta());
            if(StepsNow > Settings.MaxSubSteps)
            {
                StepsNow    = Settings.MaxSubSteps;
                Accumulator = double(FixedDelta()) * StepsNow;   // drop the backlog
            }
            Accumulator -= double(FixedDelta()) * StepsNow;

            SyncBodies(StepsNow);
            SyncConstraints();

            for(uint32_t Step = 0; Step < StepsNow; ++Step)
            {
                StepOnce();
            }
            if(StepsNow > 0)
            {
                for(auto& Entry : Bodies)
                {
                    Entry.second.Force  = JPH::Vec3::sZero();
                    Entry.second.Torque = JPH::Vec3::sZero();
                }
            }

            WriteBack();
        }

        static void StepSystem(GameCore::World& /*InWorld*/, const float InDeltaSeconds, void* InUserData)
        {
            static_cast<Impl*>(InUserData)->Frame(InDeltaSeconds);
        }
    };

    // === PhysicsWorld ============================================================

    PhysicsWorld::PhysicsWorld() = default;

    PhysicsWorld::~PhysicsWorld()
    {
        Detach();
    }

    bool PhysicsWorld::Attach(GameCore::World& InWorld, const PhysicsSettings& InSettings)
    {
        if(P)
        {
            CORE_LOG_WARN("[Physics] PhysicsWorld::Attach: already attached")
            return false;
        }
        if(Find(InWorld))
        {
            CORE_LOG_WARN("[Physics] PhysicsWorld::Attach: that World already has physics")
            return false;
        }
        if(!Runtime::Acquire())
        {
            return false;
        }

        P = new Impl();
        P->World    = &InWorld;
        P->Settings = InSettings;
        P->bRuntime = true;

        P->System = new JPH::PhysicsSystem();
        P->System->Init(InSettings.MaxBodies, 0, InSettings.MaxBodyPairs, InSettings.MaxContactConstraints,
                        P->BroadPhaseLayers, P->ObjectVsBroadPhase, P->ObjectPairs);
        P->System->SetGravity(Convert::ToVec3(InSettings.Gravity, P->ToMeters()));

        /* Jolt lets resting bodies sink up to 2 cm into each other; at the
         * World's centimetre scale that shows. Half a centimetre still stacks. */
        JPH::PhysicsSettings Tuning = P->System->GetPhysicsSettings();
        Tuning.mPenetrationSlop = 0.005f;
        P->System->SetPhysicsSettings(Tuning);
        P->System->SetContactListener(&P->Contacts);

        GameCore::EventBus& Events = InWorld.GetEvents();
        P->ContactBeginId = Events.GetEventID("ContactBegin");
        P->ContactEndId   = Events.GetEventID("ContactEnd");
        P->SensorEnterId  = Events.GetEventID("SensorEnter");
        P->SensorExitId   = Events.GetEventID("SensorExit");

        InWorld.RegisterSystem("Physics.Step", GameCore::TP_Physics, &Impl::StepSystem, P);
        Attached().emplace_back(&InWorld, this);
        return true;
    }

    void PhysicsWorld::Detach()
    {
        if(!P) { return; }

        P->World->UnregisterSystem(&Impl::StepSystem, P);
        std::vector<std::pair<const GameCore::World*, PhysicsWorld*>>& Entries = Attached();
        Entries.erase(std::remove_if(Entries.begin(), Entries.end(),
                                     [this](const auto& InEntry) { return InEntry.second == this; }),
                      Entries.end());

        for(auto& Entry : P->Constraints) { P->RemoveConstraint(Entry.second); }
        P->Constraints.clear();
        P->Touching.clear();   // no ends told: the World is going
        for(auto& Entry : P->Bodies)
        {
            if(!Entry.second.Id.IsInvalid())
            {
                P->Interface().RemoveBody(Entry.second.Id);
                P->Interface().DestroyBody(Entry.second.Id);
            }
        }
        P->Bodies.clear();
        P->BodyEntities.clear();
        P->Shapes.Clear();

        delete P->System;
        P->System = nullptr;
        const bool bRuntime = P->bRuntime;
        delete P;
        P = nullptr;
        if(bRuntime) { Runtime::Release(); }
    }

    bool PhysicsWorld::IsAttached() const         { return P != nullptr; }
    GameCore::World* PhysicsWorld::GetWorld() const { return P ? P->World : nullptr; }

    void PhysicsWorld::Step(const float InDeltaSeconds)
    {
        if(P) { P->Frame(InDeltaSeconds); }
    }

    bool PhysicsWorld::Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDirection,
                               const float InMaxDistance, PhysicsHit& OutHit, const int32_t InMask) const
    {
        OutHit = PhysicsHit();
        if(!P) { return false; }

        JPH::Vec3 Direction = Convert::ToVec3(InDirection);
        if(Direction.IsNearZero() || InMaxDistance <= 0.0f) { return false; }
        Direction = Direction.Normalized();

        const double M = P->ToMeters();
        const JPH::RRayCast Ray(Convert::ToRVec3(InOrigin, M), Direction * float(InMaxDistance * M));
        JPH::RayCastResult Hit;
        const Layers::QueryMask Mask(InMask);
        const SkipSensors       NoSensors;
        if(!P->System->GetNarrowPhaseQuery().CastRay(Ray, Hit, JPH::BroadPhaseLayerFilter(), Mask, NoSensors))
        {
            return false;
        }

        const JPH::RVec3 Point = Ray.GetPointOnRay(Hit.mFraction);
        OutHit.Entity   = P->EntityOf(Hit.mBodyID);
        OutHit.Distance = Hit.mFraction * InMaxDistance;
        OutHit.Point    = Convert::FromRVec3(Point, P->FromMeters());

        JPH::BodyLockRead Lock(P->System->GetBodyLockInterface(), Hit.mBodyID);
        if(Lock.Succeeded())
        {
            OutHit.Normal = Convert::FromVec3(Lock.GetBody().GetWorldSpaceSurfaceNormal(Hit.mSubShapeID2, Point));
        }
        return true;
    }

    void PhysicsWorld::GetStats(PhysicsStats& OutStats) const
    {
        OutStats = PhysicsStats();
        if(!P) { return; }
        OutStats.Bodies       = uint32_t(P->Bodies.size());
        OutStats.ActiveBodies = P->System->GetNumActiveBodies(JPH::EBodyType::RigidBody);
        OutStats.Steps        = P->Steps;
        for(const auto& Entry : P->Constraints)
        {
            if(Entry.second.Constraint) { ++OutStats.Constraints; }
        }
    }

    PhysicsWorld* PhysicsWorld::Find(const GameCore::World& InWorld)
    {
        for(const auto& Entry : Attached())
        {
            if(Entry.first == &InWorld) { return Entry.second; }
        }
        return nullptr;
    }
}
