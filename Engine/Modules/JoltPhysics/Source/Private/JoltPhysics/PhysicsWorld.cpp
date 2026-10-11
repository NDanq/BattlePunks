#include "Public/JoltPhysics/PhysicsWorld.h"

#include <algorithm>
#include <chrono>
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
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/BoundsComponent.h"
#include "Public/GameCore/Components/CollisionComponent.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/PhysicsConstraintComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/VehicleComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
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

        /* One Vehicle component's Jolt vehicle, by its entity. */
        struct VehicleRecord
        {
            JPH::Ref<JPH::VehicleConstraint>      Constraint;
            JPH::Ref<JPH::VehicleCollisionTester> Tester;
            JPH::BodyID                           Body;
            uint64_t                              Signature = 0;
            bool                                  bSeen     = false;
            float                                 LastForward = 0.0f;   // the reverse-brakes-first rule

            /* Each wheel slot (GameCore::EVehicleWheel), read once and kept,
             * as the step rewrites the wheels' Transforms from then on. */
            struct Wheel
            {
                GameCore::EntityID  Entity;
                int                 Index = -1;     // in the Jolt vehicle; -1: none
                bool                bRest = false;  // Rest* captured for Entity
                EngineCore::Vector3 RestPosition;   // relative to the chassis
                EngineCore::Vector3 RestRotation;
                EngineCore::Vector3 Scale = EngineCore::Vector3(1.0, 1.0, 1.0);
                JPH::Vec3           ModelRight = JPH::Vec3::sAxisX();   // the axle, in the wheel's own space
                JPH::Vec3           ModelUp    = JPH::Vec3::sAxisY();
            } Wheels[GameCore::VW_Count];
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
        std::unordered_map<uint64_t, VehicleRecord>    Vehicles;      // by the vehicle entity's Pack()

        /* One CreateRagdoll's Jolt ragdoll. Poses in metres. */
        struct RagdollRecord
        {
            JPH::Ref<JPH::Ragdoll>  Ragdoll;
            GameCore::EntityID      Owner;
            bool                    bDynamic = false;
            bool                    bTarget  = false;
            std::vector<JPH::RVec3> PrevPosition, CurrPosition, TargetPosition;
            std::vector<JPH::Quat>  PrevRotation, CurrRotation, TargetRotation;
        };
        struct RagdollPartRef
        {
            uint32_t Ragdoll = 0;
            int32_t  Part    = -1;
        };
        std::unordered_map<uint32_t, RagdollRecord>  Ragdolls;       // by handle
        std::unordered_map<uint32_t, RagdollPartRef> RagdollParts;   // BodyID -> its ragdoll and part
        uint32_t NextRagdoll      = 1;
        uint32_t NextRagdollGroup = 1;   // each ragdoll its own collision group (its parts' filter is per group)
        float    LastAlpha        = 1.0f;

        double   Accumulator = 0.0;
        uint64_t Steps       = 0;

        /* The last frame's work, for GetStats ("stat physics"). */
        uint32_t StepsLastFrame = 0;
        float    StepMs         = 0.0f;
        float    SyncMs         = 0.0f;

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
            if(const GameCore::TerrainProvider* Terrains = GameCore::GetTerrainProvider())
            {
                if(Terrains->FindTerrain)
                {
                    Request.Terrain = Terrains->FindTerrain(*World, InEntity.GetID(), Terrains->UserData);
                    if(Request.Terrain) { Request.TerrainRevision = Request.Terrain->GetRevision(); }
                }
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
            S.Add(uint64_t(reinterpret_cast<uintptr_t>(InRequest.Terrain.get())));
            S.Add(InRequest.TerrainRevision);
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
                for(auto& Entry : Vehicles)
                {
                    if(Entry.second.Constraint && Going.count(Entry.second.Body.GetIndexAndSequenceNumber()))
                    {
                        RemoveVehicle(Entry.second);
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

        // --- Vehicles --------------------------------------------------------

        void RemoveVehicle(VehicleRecord& InVehicle)
        {
            if(InVehicle.Constraint)
            {
                System->RemoveStepListener(InVehicle.Constraint);
                System->RemoveConstraint(InVehicle.Constraint);
                InVehicle.Constraint = nullptr;
                InVehicle.Tester     = nullptr;
            }
            InVehicle.Signature = 0;
            for(VehicleRecord::Wheel& Each : InVehicle.Wheels) { Each.Index = -1; }
        }

        /* Slot InSlot's wheel: Wheels[InSlot], else the child named for it. */
        GameCore::EntityID ResolveWheel(const GameCore::Entity& InVehicle, const GameCore::VehicleComponent& InComponent,
                                        const int InSlot) const
        {
            static const char* const Names[GameCore::VW_Count] = { "WheelFL", "WheelFR", "WheelRL", "WheelRR" };
            const GameCore::EntityID Named = InComponent.Wheels[InSlot];
            if(Named.IsSet() && World->IsAlive(Named)) { return Named; }
            for(GameCore::EntityID Child = InVehicle.GetFirstChild(); Child.IsSet(); Child = World->GetEntity(Child).GetNextSibling())
            {
                if(std::strcmp(World->GetEntity(Child).GetName(), Names[InSlot]) == 0) { return Child; }
            }
            return GameCore::EntityID();
        }

        /* InWheel's pose relative to InVehicle (whose own scale is not applied). */
        static GameCore::TransformPose RelativePose(const GameCore::Entity& InVehicle, const GameCore::Entity& InWheel)
        {
            if(InWheel.GetParent() == InVehicle.GetID())
            {
                if(const auto* Local = InWheel.GetComponent<GameCore::TransformComponent>()) { return Local->GetPose(); }
            }
            const GameCore::TransformPose Parent = InVehicle.GetWorldPose();
            const GameCore::TransformPose Child  = InWheel.GetWorldPose();
            const GameCore::Rotation3 Inverse = GameCore::TransformMath::Transposed(GameCore::TransformMath::EulerToRotation(Parent.Rotation));
            GameCore::TransformPose Out;
            Out.Position = Inverse * (Child.Position - Parent.Position);
            Out.Rotation = GameCore::TransformMath::RotationToEuler(Inverse * GameCore::TransformMath::EulerToRotation(Child.Rotation), Child.Rotation);
            Out.Scale    = Child.Scale;
            return Out;
        }

        /* InWheel's radius and width (units): its mesh's bounds through its
         * rest rotation and scale -- width along the chassis' X. */
        bool MeasureWheel(const VehicleRecord::Wheel& InWheel, float& OutRadius, float& OutWidth) const
        {
            const GameCore::CollisionMeshProvider* Provider = GameCore::GetCollisionMeshProvider();
            const char* Path = Provider ? Provider->GetMeshPath(*World, InWheel.Entity, Provider->UserData) : nullptr;
            GameCore::AABB Local;
            if(!Path || !Provider->GetMeshBounds(Path, Local, Provider->UserData)) { return false; }

            const GameCore::Rotation3 Turn = GameCore::TransformMath::EulerToRotation(InWheel.RestRotation);
            double Extent[3] = { 0.0, 0.0, 0.0 };
            for(int Corner = 0; Corner < 8; ++Corner)
            {
                const EngineCore::Vector3 Point((Corner & 1) ? Local.Max.GetX() : Local.Min.GetX(),
                                                (Corner & 2) ? Local.Max.GetY() : Local.Min.GetY(),
                                                (Corner & 4) ? Local.Max.GetZ() : Local.Min.GetZ());
                const EngineCore::Vector3 Scaled(double(Point.GetX()) * InWheel.Scale.GetX(), double(Point.GetY()) * InWheel.Scale.GetY(),
                                                 double(Point.GetZ()) * InWheel.Scale.GetZ());
                const EngineCore::Vector3 Placed = Turn * Scaled;
                Extent[0] = std::max(Extent[0], std::abs(double(Placed.GetX())));
                Extent[1] = std::max(Extent[1], std::abs(double(Placed.GetY())));
                Extent[2] = std::max(Extent[2], std::abs(double(Placed.GetZ())));
            }
            OutWidth  = float(2.0 * Extent[0]);
            OutRadius = float(std::max(Extent[1], Extent[2]));
            return OutRadius > 0.0f && OutWidth > 0.0f;
        }

        /* Every Vehicle to its Jolt vehicle -- made once its chassis has a
         * Dynamic body, remade when its fields, wheels or body change -- and
         * this frame's driving input in. */
        void SyncVehicles()
        {
            for(auto& Entry : Vehicles) { Entry.second.bSeen = false; }

            World->Each<GameCore::VehicleComponent>([&](GameCore::Entity InEntity, GameCore::VehicleComponent& InVehicle)
            {
                VehicleRecord& Record = Vehicles[InEntity.GetID().Pack()];
                Record.bSeen = true;

                const auto Found = InVehicle.bEnabled ? Bodies.find(InEntity.GetID().Pack()) : Bodies.end();
                if(Found == Bodies.end() || Found->second.Id.IsInvalid() || Found->second.Motion != GameCore::RBM_Dynamic)
                {
                    RemoveVehicle(Record);
                    InVehicle.Handle = 0;
                    return;
                }
                const BodyRecord& Chassis = Found->second;

                GameCore::EntityID Wheels[GameCore::VW_Count];
                Signature S;
                for(int Slot = 0; Slot < GameCore::VW_Count; ++Slot)
                {
                    Wheels[Slot] = ResolveWheel(InEntity, InVehicle, Slot);
                    S.Add(Wheels[Slot].Pack());
                }
                S.Add(Chassis.Id.GetIndexAndSequenceNumber());
                S.Add(InVehicle.Drive);
                S.Add(InVehicle.MaxSteerAngle);
                S.Add(InVehicle.EngineTorque);
                S.Add(InVehicle.MaxRPM);
                S.Add(InVehicle.BrakeTorque);
                S.Add(InVehicle.HandBrakeTorque);
                S.Add(InVehicle.SuspensionMinLength);
                S.Add(InVehicle.SuspensionMaxLength);
                S.Add(InVehicle.SuspensionFrequency);
                S.Add(InVehicle.SuspensionDamping);
                S.Add(InVehicle.WheelRadius);
                S.Add(InVehicle.WheelWidth);
                S.Add(InVehicle.AntiRollStiffness);
                S.Add(InVehicle.MaxPitchRollAngle);

                if(!Record.Constraint || Record.Signature != S.Hash)
                {
                    RemoveVehicle(Record);
                    Record.Signature = S.Hash;   // a failure is not retried until something changes
                    MakeVehicle(Record, InEntity, InVehicle, Chassis, Wheels);
                }
                InVehicle.Handle = Record.Constraint ? uint64_t(reinterpret_cast<uintptr_t>(Record.Constraint.GetPtr())) : 0;
                if(Record.Constraint)
                {
                    DriveVehicle(Record, InVehicle);
                }
            });

            for(auto It = Vehicles.begin(); It != Vehicles.end();)
            {
                if(!It->second.bSeen)
                {
                    RemoveVehicle(It->second);
                    It = Vehicles.erase(It);
                }
                else
                {
                    ++It;
                }
            }
        }

        void MakeVehicle(VehicleRecord& InRecord, const GameCore::Entity& InEntity, const GameCore::VehicleComponent& InVehicle,
                         const BodyRecord& InChassis, const GameCore::EntityID (&InWheels)[GameCore::VW_Count])
        {
            const double M = ToMeters();
            JPH::VehicleConstraintSettings VehicleSettings;
            VehicleSettings.mUp      = JPH::Vec3::sAxisY();
            VehicleSettings.mForward = JPH::Vec3::sAxisZ();
            VehicleSettings.mMaxPitchRollAngle = float(Convert::DegToRad(std::clamp(InVehicle.MaxPitchRollAngle, 1.0f, 180.0f)));

            const float MinLength = std::max(InVehicle.SuspensionMinLength, 0.0f);
            const float MaxLength = std::max(InVehicle.SuspensionMaxLength, MinLength + 1.0f);

            int Count = 0;
            for(int Slot = 0; Slot < GameCore::VW_Count; ++Slot)
            {
                VehicleRecord::Wheel& Wheel = InRecord.Wheels[Slot];
                Wheel.Index = -1;
                if(!InWheels[Slot].IsSet())
                {
                    Wheel = VehicleRecord::Wheel();
                    continue;
                }
                if(!Wheel.bRest || Wheel.Entity != InWheels[Slot])
                {
                    /* Where it was placed, read once: from here the step moves it. */
                    const GameCore::TransformPose Rest = RelativePose(InEntity, World->GetEntity(InWheels[Slot]));
                    Wheel.Entity       = InWheels[Slot];
                    Wheel.RestPosition = Rest.Position;
                    Wheel.RestRotation = Rest.Rotation;
                    Wheel.Scale        = Rest.Scale;
                    const GameCore::Rotation3 Inverse = GameCore::TransformMath::Transposed(GameCore::TransformMath::EulerToRotation(Rest.Rotation));
                    Wheel.ModelRight = Convert::ToVec3(Inverse * EngineCore::Vector3(1.0, 0.0, 0.0)).Normalized();
                    Wheel.ModelUp    = Convert::ToVec3(Inverse * EngineCore::Vector3(0.0, 1.0, 0.0)).Normalized();
                    Wheel.bRest      = true;
                }

                float Radius = 30.0f, Width = 20.0f;
                MeasureWheel(Wheel, Radius, Width);
                if(InVehicle.WheelRadius > 0.0f) { Radius = InVehicle.WheelRadius; }
                if(InVehicle.WheelWidth > 0.0f)  { Width  = InVehicle.WheelWidth; }

                const bool bFront = Slot == GameCore::VW_FrontLeft || Slot == GameCore::VW_FrontRight;
                JPH::Ref<JPH::WheelSettingsWV> Wheel1 = new JPH::WheelSettingsWV();
                /* The wheel was placed half way through its travel: the
                 * suspension hangs from above it. */
                const double Attach = 0.5 * double(MinLength + MaxLength);
                Wheel1->mPosition = Convert::ToVec3(Wheel.RestPosition + EngineCore::Vector3(0.0, Attach, 0.0), M);
                Wheel1->mSuspensionDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
                Wheel1->mSteeringAxis        = JPH::Vec3::sAxisY();
                Wheel1->mWheelUp             = JPH::Vec3::sAxisY();
                Wheel1->mWheelForward        = JPH::Vec3::sAxisZ();
                Wheel1->mSuspensionMinLength = float(MinLength * M);
                Wheel1->mSuspensionMaxLength = float(MaxLength * M);
                Wheel1->mSuspensionSpring.mFrequency = std::max(InVehicle.SuspensionFrequency, 0.1f);
                Wheel1->mSuspensionSpring.mDamping   = std::clamp(InVehicle.SuspensionDamping, 0.0f, 1.0f);
                Wheel1->mRadius = float(Radius * M);
                Wheel1->mWidth  = float(Width * M);
                Wheel1->mMaxSteerAngle      = bFront ? float(Convert::DegToRad(std::clamp(InVehicle.MaxSteerAngle, 0.0f, 89.0f))) : 0.0f;
                Wheel1->mMaxBrakeTorque     = std::max(InVehicle.BrakeTorque, 0.0f);
                Wheel1->mMaxHandBrakeTorque = bFront ? 0.0f : std::max(InVehicle.HandBrakeTorque, 0.0f);
                VehicleSettings.mWheels.push_back(JPH::Ref<JPH::WheelSettings>(Wheel1.GetPtr()));
                Wheel.Index = Count++;
            }
            if(Count == 0)
            {
                CORE_LOG_WARN(std::string("[Physics] vehicle '") + InEntity.GetName() + "' has no wheels (Wheels, or children named "
                              "WheelFL/FR/RL/RR); it is a plain body")
                return;
            }

            /* Axles: left and right of each, for the differentials and anti-roll bars. */
            const int FrontLeft  = InRecord.Wheels[GameCore::VW_FrontLeft].Index,  FrontRight = InRecord.Wheels[GameCore::VW_FrontRight].Index;
            const int RearLeft   = InRecord.Wheels[GameCore::VW_RearLeft].Index,   RearRight  = InRecord.Wheels[GameCore::VW_RearRight].Index;
            const bool bFrontAxle = FrontLeft >= 0 && FrontRight >= 0;
            const bool bRearAxle  = RearLeft >= 0 && RearRight >= 0;

            JPH::Ref<JPH::WheeledVehicleControllerSettings> Controller = new JPH::WheeledVehicleControllerSettings();
            Controller->mEngine.mMaxTorque = std::max(InVehicle.EngineTorque, 0.0f);
            Controller->mEngine.mMaxRPM    = std::max(InVehicle.MaxRPM, Controller->mEngine.mMinRPM + 100.0f);
            const bool bDriveFront = (InVehicle.Drive == GameCore::VD_Front || InVehicle.Drive == GameCore::VD_All) && bFrontAxle;
            const bool bDriveRear  = (InVehicle.Drive == GameCore::VD_Rear  || InVehicle.Drive == GameCore::VD_All) && bRearAxle;
            const auto AddDifferential = [&Controller](const int InLeft, const int InRight)
            {
                JPH::VehicleDifferentialSettings Differential;
                Differential.mLeftWheel  = InLeft;
                Differential.mRightWheel = InRight;
                Controller->mDifferentials.push_back(Differential);
            };
            if(bDriveFront) { AddDifferential(FrontLeft, FrontRight); }
            if(bDriveRear)  { AddDifferential(RearLeft, RearRight); }
            if(Controller->mDifferentials.empty())
            {
                /* Whatever axle there is. */
                if(bRearAxle)       { AddDifferential(RearLeft, RearRight); }
                else if(bFrontAxle) { AddDifferential(FrontLeft, FrontRight); }
            }
            for(JPH::VehicleDifferentialSettings& Differential : Controller->mDifferentials)
            {
                Differential.mEngineTorqueRatio = 1.0f / float(Controller->mDifferentials.size());
            }
            VehicleSettings.mController = Controller;

            if(InVehicle.AntiRollStiffness > 0.0f)
            {
                const auto AddBar = [&VehicleSettings, &InVehicle](const int InLeft, const int InRight)
                {
                    JPH::VehicleAntiRollBar Bar;
                    Bar.mLeftWheel  = InLeft;
                    Bar.mRightWheel = InRight;
                    Bar.mStiffness  = InVehicle.AntiRollStiffness;
                    VehicleSettings.mAntiRollBars.push_back(Bar);
                };
                if(bFrontAxle) { AddBar(FrontLeft, FrontRight); }
                if(bRearAxle)  { AddBar(RearLeft, RearRight); }
            }

            const JPH::BodyLockInterfaceNoLock& Locks = System->GetBodyLockInterfaceNoLock();
            JPH::Body* Body = Locks.TryGetBody(InChassis.Id);
            if(!Body) { return; }

            InRecord.Constraint = new JPH::VehicleConstraint(*Body, VehicleSettings);
            InRecord.Tester     = new JPH::VehicleCollisionTesterCastCylinder(Body->GetObjectLayer());
            InRecord.Constraint->SetVehicleCollisionTester(InRecord.Tester);
            InRecord.Body       = InChassis.Id;
            System->AddConstraint(InRecord.Constraint);
            System->AddStepListener(InRecord.Constraint);
        }

        /* The component's input into the controller. Asking for the other
         * direction while still moving the first way brakes instead. */
        void DriveVehicle(VehicleRecord& InRecord, const GameCore::VehicleComponent& InVehicle)
        {
            float Forward = std::clamp(InVehicle.Forward, -1.0f, 1.0f);
            float Brake   = std::clamp(InVehicle.Brake, 0.0f, 1.0f);
            const float HandBrake = std::clamp(InVehicle.HandBrake, 0.0f, 1.0f);
            /* Jolt's right is the vehicle's -X; the component's +Right is the
             * chassis' +X (the World's right). */
            const float Right = -std::clamp(InVehicle.Right, -1.0f, 1.0f);

            const JPH::Quat Rotation = Interface().GetRotation(InRecord.Body);
            const float Along = (Rotation.Conjugated() * Interface().GetLinearVelocity(InRecord.Body)).GetZ();
            if(Forward != 0.0f && InRecord.LastForward * Forward <= 0.0f
               && ((Forward > 0.0f && Along < -0.5f) || (Forward < 0.0f && Along > 0.5f)))
            {
                Brake   = std::max(Brake, std::abs(Forward));
                Forward = 0.0f;
            }
            else
            {
                InRecord.LastForward = Forward;
            }

            auto* Controller = static_cast<JPH::WheeledVehicleController*>(InRecord.Constraint->GetController());
            Controller->SetDriverInput(Forward, Right, Brake, HandBrake);
            if(Forward != 0.0f || Right != 0.0f || Brake != 0.0f)
            {
                Interface().ActivateBody(InRecord.Body);
            }
        }

        /* After the step: each wheel's Transform, and the readouts. */
        void WriteVehicles()
        {
            const double Units = FromMeters();
            for(auto& Entry : Vehicles)
            {
                VehicleRecord& Record = Entry.second;
                if(!Record.Constraint) { continue; }
                GameCore::Entity Vehicle = World->GetEntity(GameCore::EntityID::Unpack(Entry.first));
                GameCore::VehicleComponent* Component = Vehicle.GetComponent<GameCore::VehicleComponent>();
                if(!Component) { continue; }

                const auto* Controller = static_cast<const JPH::WheeledVehicleController*>(Record.Constraint->GetController());
                const JPH::Quat Rotation = Interface().GetRotation(Record.Body);
                Component->Speed     = float((Rotation.Conjugated() * Interface().GetLinearVelocity(Record.Body)).GetZ() * Units);
                Component->EngineRPM = Controller->GetEngine().GetCurrentRPM();
                Component->Gear      = Controller->GetTransmission().GetCurrentGear();

                int OnGround = 0;
                for(const VehicleRecord::Wheel& Wheel : Record.Wheels)
                {
                    if(Wheel.Index < 0 || !World->IsAlive(Wheel.Entity)) { continue; }
                    OnGround += Record.Constraint->GetWheel(JPH::uint(Wheel.Index))->HasContact() ? 1 : 0;

                    /* Body space, metres: where the suspension has the wheel,
                     * steered and spun. */
                    const JPH::Mat44 Local = Record.Constraint->GetWheelLocalTransform(JPH::uint(Wheel.Index), Wheel.ModelRight, Wheel.ModelUp);
                    GameCore::Entity WheelEntity = World->GetEntity(Wheel.Entity);
                    const EngineCore::Vector3 Position = Convert::FromVec3(Local.GetTranslation(), Units);
                    GameCore::TransformComponent* Transform = WheelEntity.GetComponent<GameCore::TransformComponent>();
                    if(!Transform) { continue; }
                    const EngineCore::Vector3 Euler = Convert::QuatToEuler(Local.GetQuaternion().Normalized(), Transform->Rotation);
                    if(WheelEntity.GetParent() == Vehicle.GetID())
                    {
                        Transform->Position = Position;
                        Transform->Rotation = Euler;
                    }
                    else
                    {
                        /* Not a child: through the chassis' world pose. */
                        const GameCore::TransformPose Chassis = Vehicle.GetWorldPose();
                        const GameCore::Rotation3 Turn = GameCore::TransformMath::EulerToRotation(Chassis.Rotation);
                        GameCore::TransformPose Pose = WheelEntity.GetWorldPose();
                        Pose.Position = Chassis.Position + Turn * Position;
                        Pose.Rotation = GameCore::TransformMath::RotationToEuler(Turn * GameCore::TransformMath::EulerToRotation(Euler), Pose.Rotation);
                        WheelEntity.SetWorldPose(Pose);
                    }
                }
                Component->WheelsOnGround = OnGround;
            }
        }

        // --- Ragdolls -----------------------------------------------------------

        JPH::RVec3 ToPosition(const RagdollTransform& InT) const
        {
            const double M = ToMeters();
            return JPH::RVec3(JPH::Real(double(InT.Position[0]) * M), JPH::Real(double(InT.Position[1]) * M),
                              JPH::Real(double(InT.Position[2]) * M));
        }

        static JPH::Quat ToRotation(const RagdollTransform& InT)
        {
            const JPH::Quat Q(InT.Rotation[0], InT.Rotation[1], InT.Rotation[2], InT.Rotation[3]);
            return Q.LengthSq() > 1.0e-12f ? Q.Normalized() : JPH::Quat::sIdentity();
        }

        static JPH::Vec3 ToAxis(const float InV[3], const JPH::Vec3 InFallback)
        {
            return JPH::Vec3(InV[0], InV[1], InV[2]).NormalizedOr(InFallback);
        }

        /* InPlane made perpendicular to InTwist (a unit vector). */
        static JPH::Vec3 PerpendicularTo(const JPH::Vec3 InTwist, const JPH::Vec3 InPlane)
        {
            const JPH::Vec3 Plane = InPlane - InTwist * InTwist.Dot(InPlane);
            return Plane.IsNearZero(1.0e-8f) ? InTwist.GetNormalizedPerpendicular() : Plane.Normalized();
        }

        /* A part's shape, in its frame, in metres. */
        JPH::RefConst<JPH::Shape> MakeRagdollShape(const RagdollPartDesc& InPart) const
        {
            const float M = float(ToMeters());
            const float Radius = std::max(InPart.Radius * M, 0.005f);
            JPH::RefConst<JPH::Shape> Inner;
            switch(InPart.Shape)
            {
                case RGS_Sphere:
                    Inner = new JPH::SphereShape(Radius);
                    break;
                case RGS_Box:
                {
                    const JPH::Vec3 Half(std::max(InPart.HalfExtent[0] * M, 0.005f), std::max(InPart.HalfExtent[1] * M, 0.005f),
                                         std::max(InPart.HalfExtent[2] * M, 0.005f));
                    Inner = new JPH::BoxShape(Half, std::min(JPH::cDefaultConvexRadius, 0.5f * Half.ReduceMin()));
                    break;
                }
                default:
                    Inner = new JPH::CapsuleShape(std::max(InPart.HalfHeight * M, 0.001f), Radius);
                    break;
            }

            const JPH::Vec3 Offset(InPart.ShapeLocal.Position[0] * M, InPart.ShapeLocal.Position[1] * M, InPart.ShapeLocal.Position[2] * M);
            const JPH::Quat Turn = ToRotation(InPart.ShapeLocal);
            if(Offset.IsNearZero(1.0e-10f) && Turn.IsClose(JPH::Quat::sIdentity()))
            {
                return Inner;
            }
            return new JPH::RotatedTranslatedShape(Offset, Turn, Inner);
        }

        /* A part's joint to its parent, in rest space (metres). */
        JPH::Ref<JPH::TwoBodyConstraintSettings> MakeRagdollJoint(const RagdollPartDesc& InPart) const
        {
            RagdollTransform At;
            std::memcpy(At.Position, InPart.JointPosition, sizeof(At.Position));
            const JPH::RVec3 Point = ToPosition(At);
            const JPH::Vec3 Twist       = ToAxis(InPart.TwistAxis, JPH::Vec3::sAxisX());
            const JPH::Vec3 Plane       = PerpendicularTo(Twist, ToAxis(InPart.PlaneAxis, JPH::Vec3::sAxisY()));
            const JPH::Vec3 ParentTwist = ToAxis(InPart.ParentTwistAxis, Twist);
            const JPH::Vec3 ParentPlane = PerpendicularTo(ParentTwist, ToAxis(InPart.ParentPlaneAxis, Plane));
            constexpr float Pi = 3.14159265358979f;

            switch(InPart.Joint)
            {
                case RGJ_Fixed:
                {
                    JPH::Ref<JPH::FixedConstraintSettings> Fixed = new JPH::FixedConstraintSettings();
                    Fixed->mAutoDetectPoint = true;
                    return JPH::Ref<JPH::TwoBodyConstraintSettings>(Fixed.GetPtr());
                }
                case RGJ_Hinge:
                {
                    JPH::Ref<JPH::HingeConstraintSettings> Hinge = new JPH::HingeConstraintSettings();
                    Hinge->mPoint1 = Hinge->mPoint2 = Point;
                    Hinge->mHingeAxis1  = ParentTwist;
                    Hinge->mNormalAxis1 = ParentPlane;
                    Hinge->mHingeAxis2  = Twist;
                    Hinge->mNormalAxis2 = Plane;
                    Hinge->mLimitsMin   = std::clamp(InPart.TwistMin, -Pi, 0.0f);
                    Hinge->mLimitsMax   = std::clamp(InPart.TwistMax, 0.0f, Pi);
                    Hinge->mMaxFrictionTorque = std::max(InPart.FrictionTorque, 0.0f);
                    return JPH::Ref<JPH::TwoBodyConstraintSettings>(Hinge.GetPtr());
                }
                default:
                {
                    JPH::Ref<JPH::SwingTwistConstraintSettings> Swing = new JPH::SwingTwistConstraintSettings();
                    Swing->mPosition1 = Swing->mPosition2 = Point;
                    Swing->mTwistAxis1 = ParentTwist;
                    Swing->mPlaneAxis1 = ParentPlane;
                    Swing->mTwistAxis2 = Twist;
                    Swing->mPlaneAxis2 = Plane;
                    Swing->mNormalHalfConeAngle = std::clamp(InPart.NormalHalfCone, 0.0f, Pi);
                    Swing->mPlaneHalfConeAngle  = std::clamp(InPart.PlaneHalfCone, 0.0f, Pi);
                    Swing->mTwistMinAngle = std::clamp(InPart.TwistMin, -Pi, Pi);
                    Swing->mTwistMaxAngle = std::clamp(std::max(InPart.TwistMax, InPart.TwistMin), -Pi, Pi);
                    Swing->mMaxFrictionTorque = std::max(InPart.FrictionTorque, 0.0f);
                    return JPH::Ref<JPH::TwoBodyConstraintSettings>(Swing.GetPtr());
                }
            }
        }

        uint32_t CreateRagdoll(const RagdollDesc& InDesc, const RagdollTransform* InPose, const bool bInDynamic)
        {
            const size_t Count = InDesc.Parts.size();
            bool bValid = Count > 0 && InPose != nullptr && InDesc.Parts[0].Parent == -1;
            for(size_t Index = 1; bValid && Index < Count; ++Index)
            {
                const int32_t Parent = InDesc.Parts[Index].Parent;
                bValid = Parent >= 0 && size_t(Parent) < Index;
            }
            if(!bValid)
            {
                CORE_LOG_ERROR("[Physics] CreateRagdoll: parts must be parents first, with one root (part 0)")
                return 0;
            }

            JPH::Ref<JPH::RagdollSettings> Settings = new JPH::RagdollSettings();
            Settings->mSkeleton = new JPH::Skeleton();
            Settings->mParts.resize(Count);
            std::vector<JPH::Mat44> Rest(Count);
            const JPH::ObjectLayer Layer = Layers::Make(InDesc.Layer, InDesc.Mask, true);
            for(size_t Index = 0; Index < Count; ++Index)
            {
                const RagdollPartDesc& Desc = InDesc.Parts[Index];
                Settings->mSkeleton->AddJoint("Part" + std::to_string(Index), int(Desc.Parent));

                JPH::RagdollSettings::Part& Part = Settings->mParts[Index];
                Part.SetShape(MakeRagdollShape(Desc));
                Part.mPosition    = ToPosition(Desc.Rest);
                Part.mRotation    = ToRotation(Desc.Rest);
                Part.mMotionType  = bInDynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Kinematic;
                Part.mAllowDynamicOrKinematic = true;
                Part.mObjectLayer    = Layer;
                Part.mFriction       = std::max(InDesc.Friction, 0.0f);
                Part.mRestitution    = std::clamp(InDesc.Restitution, 0.0f, 1.0f);
                Part.mLinearDamping  = std::max(InDesc.LinearDamping, 0.0f);
                Part.mAngularDamping = std::max(InDesc.AngularDamping, 0.0f);
                Part.mMotionQuality  = InDesc.bContinuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
                Part.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
                Part.mMassPropertiesOverride.mMass = std::max(Desc.Mass, 0.05f);
                if(Desc.Parent >= 0)
                {
                    Part.mToParent = MakeRagdollJoint(Desc);
                }
                Rest[Index] = JPH::Mat44::sRotationTranslation(Part.mRotation, JPH::Vec3(Part.mPosition));
            }

            /* Jolt's recipe (Samples/Utils/RagdollLoader.cpp): even out the
             * mass ratios across joints, keep parts that touch at rest from
             * colliding, solve the joints root first. */
            Settings->Stabilize();
            Settings->DisableParentChildCollisions(Rest.data(), 0.0f);
            Settings->CalculateConstraintPriorities();
            Settings->CalculateBodyIndexToConstraintIndex();
            Settings->CalculateConstraintIndexToBodyIdxPair();

            JPH::Ref<JPH::Ragdoll> Ragdoll = Settings->CreateRagdoll(JPH::CollisionGroup::GroupID(NextRagdollGroup++),
                                                                    InDesc.Owner.Pack(), System);
            if(!Ragdoll)
            {
                CORE_LOG_ERROR("[Physics] out of bodies ([Physics] MaxBodies) making a ragdoll")
                return 0;
            }

            const uint32_t Handle = NextRagdoll++;
            RagdollRecord& Record = Ragdolls[Handle];
            Record.Ragdoll  = Ragdoll;
            Record.Owner    = InDesc.Owner;
            Record.bDynamic = bInDynamic;
            Record.PrevPosition.resize(Count);
            Record.PrevRotation.resize(Count);
            Record.TargetPosition.resize(Count);
            Record.TargetRotation.resize(Count);
            for(size_t Index = 0; Index < Count; ++Index)
            {
                const JPH::BodyID Id = Ragdoll->GetBodyID(int(Index));
                Record.PrevPosition[Index] = Record.TargetPosition[Index] = ToPosition(InPose[Index]);
                Record.PrevRotation[Index] = Record.TargetRotation[Index] = ToRotation(InPose[Index]);
                Interface().SetPositionAndRotation(Id, Record.PrevPosition[Index], Record.PrevRotation[Index], JPH::EActivation::DontActivate);
                RagdollParts[Id.GetIndexAndSequenceNumber()] = RagdollPartRef{ Handle, int32_t(Index) };
            }
            Record.CurrPosition = Record.PrevPosition;
            Record.CurrRotation = Record.PrevRotation;
            Ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate, false);
            return Handle;
        }

        void DestroyRagdoll(RagdollRecord& InRecord)
        {
            if(!InRecord.Ragdoll) { return; }
            for(const JPH::BodyID Id : InRecord.Ragdoll->GetBodyIDs())
            {
                RagdollParts.erase(Id.GetIndexAndSequenceNumber());
            }
            InRecord.Ragdoll->RemoveFromPhysicsSystem(false);
            InRecord.Ragdoll = nullptr;   // the last reference: its bodies are destroyed
        }

        RagdollRecord* FindRagdoll(const uint32_t InHandle)
        {
            const auto Found = Ragdolls.find(InHandle);
            return Found != Ragdolls.end() && Found->second.Ragdoll ? &Found->second : nullptr;
        }

        const RagdollRecord* FindRagdoll(const uint32_t InHandle) const
        {
            const auto Found = Ragdolls.find(InHandle);
            return Found != Ragdolls.end() && Found->second.Ragdoll ? &Found->second : nullptr;
        }

        /* Kinematic ragdolls to their targets, over this frame's steps. */
        void DriveRagdolls(const uint32_t InSteps)
        {
            if(InSteps == 0) { return; }
            const float Seconds = float(InSteps) * FixedDelta();
            for(auto& Entry : Ragdolls)
            {
                RagdollRecord& Record = Entry.second;
                if(!Record.Ragdoll || Record.bDynamic || !Record.bTarget) { continue; }
                for(size_t Index = 0; Index < Record.TargetPosition.size(); ++Index)
                {
                    Interface().MoveKinematic(Record.Ragdoll->GetBodyID(int(Index)), Record.TargetPosition[Index],
                                              Record.TargetRotation[Index], Seconds);
                }
            }
        }

        void ReadRagdolls(const bool bInBefore)
        {
            for(auto& Entry : Ragdolls)
            {
                RagdollRecord& Record = Entry.second;
                if(!Record.Ragdoll) { continue; }
                if(bInBefore)
                {
                    Record.PrevPosition = Record.CurrPosition;
                    Record.PrevRotation = Record.CurrRotation;
                    continue;
                }
                for(size_t Index = 0; Index < Record.CurrPosition.size(); ++Index)
                {
                    Interface().GetPositionAndRotation(Record.Ragdoll->GetBodyID(int(Index)), Record.CurrPosition[Index],
                                                       Record.CurrRotation[Index]);
                }
            }
        }

        bool IsRagdollBody(const JPH::BodyID InBody) const
        {
            return RagdollParts.count(InBody.GetIndexAndSequenceNumber()) != 0;
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

            ReadRagdolls(true);

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
            ReadRagdolls(false);

            FlushContacts();
        }

        void WriteBack()
        {
            const float Alpha = Settings.bInterpolate ? float(std::clamp(Accumulator / FixedDelta(), 0.0, 1.0)) : 1.0f;
            LastAlpha = Alpha;
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
                /* A ragdoll's parts tell nothing: dozens of touches a fall. */
                if(IsRagdollBody(Event.A) || IsRagdollBody(Event.B)) { continue; }
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

            using Clock = std::chrono::steady_clock;
            const Clock::time_point SyncStart = Clock::now();
            SyncBodies(StepsNow);
            SyncConstraints();
            SyncVehicles();
            DriveRagdolls(StepsNow);
            const Clock::time_point StepStart = Clock::now();

            for(uint32_t Step = 0; Step < StepsNow; ++Step)
            {
                StepOnce();
            }
            StepsLastFrame = StepsNow;
            SyncMs = std::chrono::duration<float, std::milli>(StepStart - SyncStart).count();
            StepMs = std::chrono::duration<float, std::milli>(Clock::now() - StepStart).count();
            if(StepsNow > 0)
            {
                for(auto& Entry : Bodies)
                {
                    Entry.second.Force  = JPH::Vec3::sZero();
                    Entry.second.Torque = JPH::Vec3::sZero();
                }
            }

            WriteBack();
            WriteVehicles();
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

        for(auto& Entry : P->Vehicles) { P->RemoveVehicle(Entry.second); }
        P->Vehicles.clear();
        for(auto& Entry : P->Ragdolls) { P->DestroyRagdoll(Entry.second); }
        P->Ragdolls.clear();
        P->RagdollParts.clear();
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
        const auto Part = P->RagdollParts.find(Hit.mBodyID.GetIndexAndSequenceNumber());
        if(Part != P->RagdollParts.end())
        {
            const Impl::RagdollRecord* Ragdoll = P->FindRagdoll(Part->second.Ragdoll);
            OutHit.Entity      = Ragdoll ? Ragdoll->Owner : GameCore::EntityID();
            OutHit.Ragdoll     = Part->second.Ragdoll;
            OutHit.RagdollPart = Part->second.Part;
        }
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
        for(const auto& Entry : P->Bodies)
        {
            const Impl::BodyRecord& Record = Entry.second;
            if(Record.Id.IsInvalid()) { continue; }
            OutStats.DynamicBodies   += Record.Motion == GameCore::RBM_Dynamic ? 1 : 0;
            OutStats.KinematicBodies += Record.Motion == GameCore::RBM_Kinematic ? 1 : 0;
            OutStats.StaticBodies    += Record.Motion == GameCore::RBM_Static ? 1 : 0;
            OutStats.Sensors         += Record.bSensor ? 1 : 0;
        }
        OutStats.Contacts       = uint32_t(P->Touching.size());
        for(const auto& Entry : P->Vehicles)
        {
            if(Entry.second.Constraint) { ++OutStats.Vehicles; }
        }
        for(const auto& Entry : P->Ragdolls)
        {
            if(!Entry.second.Ragdoll) { continue; }
            ++OutStats.Ragdolls;
            OutStats.RagdollParts += uint32_t(Entry.second.Ragdoll->GetBodyCount());
        }
        OutStats.StepsLastFrame = P->StepsLastFrame;
        OutStats.StepMs         = P->StepMs;
        OutStats.SyncMs         = P->SyncMs;
    }

    // --- Ragdolls -------------------------------------------------------------

    uint32_t PhysicsWorld::CreateRagdoll(const RagdollDesc& InDesc, const RagdollTransform* InPose, const bool bInDynamic)
    {
        return P ? P->CreateRagdoll(InDesc, InPose, bInDynamic) : 0;
    }

    void PhysicsWorld::DestroyRagdoll(const uint32_t InRagdoll)
    {
        if(!P) { return; }
        const auto Found = P->Ragdolls.find(InRagdoll);
        if(Found == P->Ragdolls.end()) { return; }
        P->DestroyRagdoll(Found->second);
        P->Ragdolls.erase(Found);
    }

    bool PhysicsWorld::HasRagdoll(const uint32_t InRagdoll) const
    {
        return P && P->FindRagdoll(InRagdoll) != nullptr;
    }

    bool PhysicsWorld::IsRagdollDynamic(const uint32_t InRagdoll) const
    {
        const Impl::RagdollRecord* Record = P ? P->FindRagdoll(InRagdoll) : nullptr;
        return Record && Record->bDynamic;
    }

    void PhysicsWorld::SetRagdollTarget(const uint32_t InRagdoll, const RagdollTransform* InPose)
    {
        Impl::RagdollRecord* Record = P ? P->FindRagdoll(InRagdoll) : nullptr;
        if(!Record || !InPose || Record->bDynamic) { return; }
        for(size_t Index = 0; Index < Record->TargetPosition.size(); ++Index)
        {
            Record->TargetPosition[Index] = P->ToPosition(InPose[Index]);
            Record->TargetRotation[Index] = Impl::ToRotation(InPose[Index]);
        }
        Record->bTarget = true;
    }

    void PhysicsWorld::SetRagdollDynamic(const uint32_t InRagdoll, const EngineCore::Vector3* InLinear, const EngineCore::Vector3* InAngular)
    {
        Impl::RagdollRecord* Record = P ? P->FindRagdoll(InRagdoll) : nullptr;
        if(!Record) { return; }
        JPH::BodyInterface& BodiesI = P->Interface();
        const double M = P->ToMeters();
        for(int Index = 0; Index < int(Record->Ragdoll->GetBodyCount()); ++Index)
        {
            const JPH::BodyID Id = Record->Ragdoll->GetBodyID(Index);
            if(!Record->bDynamic)
            {
                BodiesI.SetMotionType(Id, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
            }
            if(InLinear)  { BodiesI.SetLinearVelocity(Id, Convert::ToVec3(InLinear[Index], M)); }
            if(InAngular) { BodiesI.SetAngularVelocity(Id, Convert::ToVec3(InAngular[Index], Convert::DegToRad(1.0))); }
            BodiesI.ActivateBody(Id);
        }
        Record->bDynamic = true;
        Record->bTarget  = false;
    }

    bool PhysicsWorld::GetRagdollPose(const uint32_t InRagdoll, RagdollTransform* OutPose) const
    {
        const Impl::RagdollRecord* Record = P ? P->FindRagdoll(InRagdoll) : nullptr;
        if(!Record || !OutPose) { return false; }
        const float  Alpha = Record->bDynamic ? P->LastAlpha : 1.0f;
        const double Units = P->FromMeters();
        for(size_t Index = 0; Index < Record->CurrPosition.size(); ++Index)
        {
            const JPH::RVec3 Position = Record->PrevPosition[Index] + (Record->CurrPosition[Index] - Record->PrevPosition[Index]) * JPH::Real(Alpha);
            const JPH::Quat  Rotation = Record->PrevRotation[Index].SLERP(Record->CurrRotation[Index], Alpha).Normalized();
            RagdollTransform& Out = OutPose[Index];
            Out.Position[0] = float(double(Position.GetX()) * Units);
            Out.Position[1] = float(double(Position.GetY()) * Units);
            Out.Position[2] = float(double(Position.GetZ()) * Units);
            Out.Rotation[0] = Rotation.GetX();
            Out.Rotation[1] = Rotation.GetY();
            Out.Rotation[2] = Rotation.GetZ();
            Out.Rotation[3] = Rotation.GetW();
        }
        return true;
    }

    bool PhysicsWorld::IsRagdollAwake(const uint32_t InRagdoll) const
    {
        const Impl::RagdollRecord* Record = P ? P->FindRagdoll(InRagdoll) : nullptr;
        return Record && Record->Ragdoll->IsActive(false);
    }

    void PhysicsWorld::AddRagdollImpulse(const uint32_t InRagdoll, const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt,
                                         const int32_t InPart)
    {
        Impl::RagdollRecord* Record = P ? P->FindRagdoll(InRagdoll) : nullptr;
        if(!Record || !Record->bDynamic) { return; }
        JPH::BodyInterface& BodiesI = P->Interface();
        const double     M  = P->ToMeters();
        const JPH::RVec3 At = Convert::ToRVec3(InAt, M);

        int32_t Part = InPart;
        if(Part < 0 || size_t(Part) >= Record->Ragdoll->GetBodyCount())
        {
            /* The part whose centre of mass is nearest. */
            float Nearest = 0.0f;
            Part = 0;
            for(int Index = 0; Index < int(Record->Ragdoll->GetBodyCount()); ++Index)
            {
                const float Distance = JPH::Vec3(BodiesI.GetCenterOfMassPosition(Record->Ragdoll->GetBodyID(Index)) - At).LengthSq();
                if(Index == 0 || Distance < Nearest)
                {
                    Nearest = Distance;
                    Part    = Index;
                }
            }
        }
        BodiesI.AddImpulse(Record->Ragdoll->GetBodyID(int(Part)), Convert::ToVec3(InImpulse, M), At);
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
