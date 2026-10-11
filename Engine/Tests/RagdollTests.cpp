/*
 * NimbleEngineRagdollTests -- the Ragdoll component on Jolt: CrashDummy's
 * skeleton read as a humanoid, a ragdoll's joint limits, and the whole death:
 * hit boxes following the animation, a shot that activates it, the pistol
 * let go as a prop, the body coming to rest on the floor, and a reset.
 *
 * Built only with the module (NIMBLE_WITH_JOLTPHYSICS). Runs from the
 * binaries folder, where Engine/Content/Characters is staged.
 */

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/JoltPhysics/PhysicsWorld.h"
#include "Public/NimbleEngine/Ragdoll.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/SkeletonAsset.h"

using namespace GameCore;

static int  GChecks   = 0;
static int  GFailures = 0;
static bool GVerbose  = false;   // -v: every check, with its numbers

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
    else if(GVerbose)
    {
        std::printf("  ok    %s\n", What.c_str());
    }
}

namespace
{
    constexpr float Frame = 1.0f / 60.0f;
    constexpr const char* DummyMesh     = "Engine/Content/Characters/CrashDummy/CrashDummy.nmmesh";
    constexpr const char* DummyState    = "Engine/Content/Characters/CrashDummy/CrashDummy.nmanimstate";
    constexpr const char* DummySkeleton = "Engine/Content/Characters/CrashDummy/CrashDummy_Skeleton.nmskel";

    std::string Num(const double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.2f", InValue);
        return Buffer;
    }

    JoltPhysics::PhysicsSettings TestSettings()
    {
        JoltPhysics::PhysicsSettings Settings;
        Settings.bInterpolate = false;
        return Settings;
    }

    void Run(World& InWorld, const double InSeconds)
    {
        const int Frames = int(std::lround(InSeconds / Frame));
        for(int Index = 0; Index < Frames; ++Index) { InWorld.Tick(Frame); }
    }

    const NimbleEngine::RagdollProfilePart* FindPart(const NimbleEngine::RagdollProfile& InProfile, const char* InName)
    {
        for(const NimbleEngine::RagdollProfilePart& Part : InProfile.Parts)
        {
            if(Part.Name == InName) { return &Part; }
        }
        return nullptr;
    }

    std::string BoneOf(const RenderCore::SkeletonData& InSkeleton, const NimbleEngine::RagdollProfile& InProfile, const char* InPart)
    {
        const NimbleEngine::RagdollProfilePart* Part = FindPart(InProfile, InPart);
        return Part && Part->Bone >= 0 ? InSkeleton.Bones[size_t(Part->Bone)].Name : std::string("(none)");
    }

    void TestProfile()
    {
        std::printf("[profile]\n");
        const std::shared_ptr<const RenderCore::SkeletonData> Skeleton = RenderCore::SkeletonAssetCache::Find(DummySkeleton);
        Expect(Skeleton != nullptr, "CrashDummy's skeleton loads");
        if(!Skeleton) { return; }

        NimbleEngine::RagdollProfile Profile;
        std::string Error;
        const bool bBuilt = NimbleEngine::BuildRagdollProfile(*Skeleton, Profile, Error);
        Expect(bBuilt, "it reads as a humanoid" + (Error.empty() ? std::string() : " (" + Error + ")"));
        Expect(Profile.Parts.size() == 16, "sixteen parts: pelvis, spine, chest, head, and each side's six (" +
                                           std::to_string(Profile.Parts.size()) + ")");
        Expect(BoneOf(*Skeleton, Profile, "Pelvis") == "DEF-spine", "the pelvis is where the thighs meet (" + BoneOf(*Skeleton, Profile, "Pelvis") + ")");
        Expect(BoneOf(*Skeleton, Profile, "Chest") == "DEF-spine.003", "the chest where the arms do (" + BoneOf(*Skeleton, Profile, "Chest") + ")");
        Expect(BoneOf(*Skeleton, Profile, "Head") == "DEF-spine.004", "the head is the neck's chain (" + BoneOf(*Skeleton, Profile, "Head") + ")");
        Expect(BoneOf(*Skeleton, Profile, "Thigh.L") == "DEF-thigh.L" && BoneOf(*Skeleton, Profile, "Shin.R") == "DEF-shin.R"
               && BoneOf(*Skeleton, Profile, "Foot.L") == "DEF-foot.L", "legs by name, not their twist segments");
        Expect(BoneOf(*Skeleton, Profile, "UpperArm.R") == "DEF-upper_arm.R" && BoneOf(*Skeleton, Profile, "Forearm.L") == "DEF-forearm.L"
               && BoneOf(*Skeleton, Profile, "Hand.R") == "DEF-hand.R", "arms likewise");

        float Shares = 0.0f;
        bool  bParentsFirst = Profile.Parts[0].Parent == -1, bSized = true;
        for(size_t Index = 0; Index < Profile.Parts.size(); ++Index)
        {
            const NimbleEngine::RagdollProfilePart& Part = Profile.Parts[Index];
            Shares += Part.MassShare;
            if(Index > 0 && (Part.Parent < 0 || size_t(Part.Parent) >= Index)) { bParentsFirst = false; }
            if(Part.Radius <= 0.0f || Part.HalfHeight < 0.0f) { bSized = false; }
            if(Profile.BonePart[size_t(Part.Bone)] != int32_t(Index)) { bParentsFirst = false; }
        }
        Expect(std::fabs(Shares - 1.0f) < 1.0e-4f, "the mass shares sum to 1 (" + Num(Shares) + ")");
        Expect(bParentsFirst, "parts are parents first, and each bone knows its part");
        Expect(bSized, "every part has a shape");
        const NimbleEngine::RagdollProfilePart* Thigh = FindPart(Profile, "Thigh.L");
        const NimbleEngine::RagdollProfilePart* Hand  = FindPart(Profile, "Hand.L");
        Expect(Thigh && Hand && Thigh->Radius > 2.0f * Hand->Radius, "a thigh is thicker than a hand");
        const NimbleEngine::RagdollProfilePart* Knee = FindPart(Profile, "Shin.L");
        Expect(Knee && Knee->Joint == NimbleEngine::RJT_Hinge && FindPart(Profile, "Forearm.R")->Joint == NimbleEngine::RJT_Hinge,
               "knees and elbows are hinges");

        /* A chain of three is no body. */
        RenderCore::SkeletonData Chain;
        for(int Index = 0; Index < 3; ++Index)
        {
            RenderCore::SkeletonBone Bone;
            Bone.Name   = "Bone" + std::to_string(Index);
            Bone.Parent = Index - 1;
            Bone.RefPose.Translation = { 0.0f, 10.0f, 0.0f };
            Chain.Bones.push_back(Bone);
        }
        NimbleEngine::RagdollProfile None;
        const bool bRefused = !NimbleEngine::BuildRagdollProfile(Chain, None, Error);
        Expect(bRefused && Error.find("not a humanoid") != std::string::npos, "a skeleton without limbs is refused, saying why (" + Error + ")");
    }

    /* The child's turn about X relative to the parent, degrees. */
    float HingeAngle(const JoltPhysics::RagdollTransform& InParent, const JoltPhysics::RagdollTransform& InChild)
    {
        const RenderCore::AnimQuat Parent{ InParent.Rotation[0], InParent.Rotation[1], InParent.Rotation[2], InParent.Rotation[3] };
        const RenderCore::AnimQuat Child{ InChild.Rotation[0], InChild.Rotation[1], InChild.Rotation[2], InChild.Rotation[3] };
        RenderCore::AnimQuat Relative = RenderCore::AnimMath::Mul(RenderCore::AnimMath::Conjugate(Parent), Child);
        if(Relative.W < 0.0f) { Relative = { -Relative.X, -Relative.Y, -Relative.Z, -Relative.W }; }
        return 2.0f * std::atan2(Relative.X, Relative.W) * 180.0f / 3.14159265f;
    }

    /* Two parts on a hinge about +X, 0..90 degrees; spun each way. */
    float SpinHinge(const float InSpin)
    {
        World W;
        JoltPhysics::PhysicsSettings Settings = TestSettings();
        Settings.Gravity = EngineCore::Vector3(0.0, 0.0, 0.0);
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, Settings);

        JoltPhysics::RagdollDesc Desc;
        Desc.Parts.resize(2);
        Desc.AngularDamping = 0.0f;
        JoltPhysics::RagdollPartDesc& Upper = Desc.Parts[0];
        Upper.Rest.Position[1] = 100.0f;
        Upper.Radius = 5.0f;
        Upper.HalfHeight = 20.0f;
        Upper.ShapeLocal.Position[1] = -25.0f;
        Upper.Mass = 1000.0f;
        JoltPhysics::RagdollPartDesc& Lower = Desc.Parts[1];
        Lower.Parent = 0;
        Lower.Rest.Position[1] = 50.0f;
        Lower.Radius = 5.0f;
        Lower.HalfHeight = 20.0f;
        Lower.ShapeLocal.Position[1] = -25.0f;
        Lower.Mass = 1.0f;
        Lower.Joint = JoltPhysics::RGJ_Hinge;
        Lower.JointPosition[1] = 50.0f;
        const float Axis[3] = { 1.0f, 0.0f, 0.0f }, Normal[3] = { 0.0f, -1.0f, 0.0f };
        for(int Index = 0; Index < 3; ++Index)
        {
            Lower.TwistAxis[Index] = Lower.ParentTwistAxis[Index] = Axis[Index];
            Lower.PlaneAxis[Index] = Lower.ParentPlaneAxis[Index] = Normal[Index];
        }
        Lower.TwistMin = 0.0f;
        Lower.TwistMax = 3.14159265f * 0.5f;

        JoltPhysics::RagdollTransform Pose[2] = { Upper.Rest, Lower.Rest };
        const uint32_t Handle = Physics.CreateRagdoll(Desc, Pose, true);
        Expect(Handle != 0 && Physics.HasRagdoll(Handle) && Physics.IsRagdollDynamic(Handle), "a two-part ragdoll is made");
        const EngineCore::Vector3 Linear[2];
        const EngineCore::Vector3 Angular[2] = { EngineCore::Vector3(), EngineCore::Vector3(InSpin, 0.0, 0.0) };
        Physics.SetRagdollDynamic(Handle, Linear, Angular);
        float Extreme = 0.0f;
        for(int Step = 0; Step < 90; ++Step)
        {
            W.Tick(Frame);
            Physics.GetRagdollPose(Handle, Pose);
            const float Angle = HingeAngle(Pose[0], Pose[1]);
            if(std::fabs(Angle) > std::fabs(Extreme)) { Extreme = Angle; }
        }
        JoltPhysics::PhysicsStats Stats;
        Physics.GetStats(Stats);
        Expect(Stats.Ragdolls == 1 && Stats.RagdollParts == 2, "stats count it");
        Physics.DestroyRagdoll(Handle);
        Expect(!Physics.HasRagdoll(Handle), "and it can be destroyed");
        return Extreme;
    }

    void TestHingeLimits()
    {
        std::printf("[hinge limits]\n");
        const float Forward = SpinHinge(360.0f);
        Expect(Forward > 80.0f && Forward < 96.0f, "spun the positive way it stops at its 90 degree limit (" + Num(Forward) + ")");
        const float Back = SpinHinge(-360.0f);
        Expect(Back > -6.0f, "spun the other way it stops at 0 (" + Num(Back) + ")");
    }

    struct Counts
    {
        int Activated = 0;
        int Settled   = 0;
    };

    void TestDeath()
    {
        std::printf("[death]\n");
        World W;
        JoltPhysics::PhysicsWorld Physics;
        Physics.Attach(W, TestSettings());
        RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Attach(W);
        NimbleEngine::RagdollWorld* Ragdolls = NimbleEngine::RagdollWorld::Attach(W);
        Expect(NimbleEngine::RagdollWorld::Find(&W) == Ragdolls, "a world has its ragdolls once attached");

        Entity Floor = W.CreateEntity("Floor");
        {
            TransformComponent T;
            T.Position = EngineCore::Vector3(0.0, -10.0, 0.0);
            Floor.AddComponent(T);
            BoundsComponent B;
            B.Local = AABB(EngineCore::Vector3(-2000.0, -10.0, -2000.0), EngineCore::Vector3(2000.0, 10.0, 2000.0));
            Floor.AddComponent(B);
            RigidBodyComponent Body;
            Body.Motion = RBM_Static;
            Floor.AddComponent(Body);
        }

        Entity Dummy = W.CreateEntity("Dummy");
        Dummy.AddComponent(TransformComponent());
        RenderCore::RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", DummyMesh);
        Dummy.AddComponent(Render);
        RenderCore::AnimationComponent Anim;
        std::snprintf(Anim.Animation, sizeof(Anim.Animation), "%s", DummyState);
        Dummy.AddComponent(Anim);
        Dummy.AddComponent(NimbleEngine::RagdollComponent());

        /* The pistol: socketed to the right hand, its body off while held. */
        Entity Pistol = W.CreateEntity("Pistol");
        Pistol.AddComponent(TransformComponent());
        Pistol.SetParent(Dummy.GetID(), false);
        RenderCore::AnimSocketComponent Socket;
        std::snprintf(Socket.Bone, sizeof(Socket.Bone), "%s", "DEF-hand.R");
        Pistol.AddComponent(Socket);
        BoundsComponent PistolBounds;
        PistolBounds.Local = AABB(EngineCore::Vector3(-2.0, -6.0, -10.0), EngineCore::Vector3(2.0, 6.0, 10.0));
        Pistol.AddComponent(PistolBounds);
        RigidBodyComponent PistolBody;
        PistolBody.bEnabled = 0;
        PistolBody.Mass     = 1.2f;
        PistolBody.Shape    = RBS_Box;
        Pistol.AddComponent(PistolBody);

        Counts Seen;
        W.GetEvents().Subscribe("RagdollActivated", [](EventID, const EventArgs&, void* InUserData)
        {
            ++static_cast<Counts*>(InUserData)->Activated;
        }, &Seen);
        W.GetEvents().Subscribe("RagdollSettled", [](EventID, const EventArgs&, void* InUserData)
        {
            ++static_cast<Counts*>(InUserData)->Settled;
        }, &Seen);

        Run(W, 0.5);
        Expect(Ragdolls->GetRagdollCount() == 1 && Ragdolls->GetActiveCount() == 0, "alive, its hit boxes are made and not simulated");

        TransformPose Chest, Head, Hand;
        Expect(Animation->GetBoneWorldPose(Dummy, "DEF-spine.003", Chest) && Animation->GetBoneWorldPose(Dummy, "DEF-spine.006", Head)
               && Animation->GetBoneWorldPose(Dummy, "DEF-hand.R", Hand), "its bones are posed");
        const double Standing = Head.Position.GetY();
        Expect(Standing > 120.0, "it stands (head at " + Num(Standing) + ")");

        /* A shot from in front, at the chest. */
        const EngineCore::Vector3 From = Chest.Position + EngineCore::Vector3(0.0, 0.0, -400.0);
        JoltPhysics::PhysicsHit Hit;
        const bool bHit = Physics.Raycast(From, EngineCore::Vector3(0.0, 0.0, 1.0), 800.0f, Hit);
        Expect(bHit && Hit.Entity == Dummy.GetID() && Hit.Ragdoll != 0 && Hit.RagdollPart >= 0,
               "a raycast hits a hit box and reports the dummy (part " + std::to_string(Hit.RagdollPart) + ")");

        NimbleEngine::RagdollComponent* Parts = Dummy.GetComponent<NimbleEngine::RagdollComponent>();
        const std::string ShotPart = bHit ? Parts->GetPartAt(Hit.Point) : std::string();
        const std::string HeadPart = Parts->GetPartAt(Head.Position + EngineCore::Vector3(0.0, 4.0, 0.0));
        const std::string HandPart = Parts->GetPartAt(Hand.Position);
        Expect(ShotPart == "Chest" || ShotPart == "Spine", "the chest shot lands on the torso (" + ShotPart + ")");
        Expect(HeadPart == "Head" && HandPart == "Hand.R", "a point names its part (" + HeadPart + ", " + HandPart + ")");

        const double PistolHeld = Pistol.GetWorldPose().Position.GetY();
        NimbleEngine::RagdollComponent* Ragdoll = Dummy.GetComponent<NimbleEngine::RagdollComponent>();
        Ragdoll->ActivateWithImpulse(EngineCore::Vector3(0.0, 0.0, 20000.0), bHit ? Hit.Point : Chest.Position);
        Expect(Ragdoll->IsActive(), "activating reads as active at once");
        Run(W, 0.1);
        Expect(Seen.Activated == 1 && Ragdoll->bActive && Ragdolls->GetActiveCount() == 1, "the next update hands it to physics, and says so");
        Expect(Dummy.GetComponent<RenderCore::AnimationComponent>()->bPaused != 0, "its animation pauses");
        Expect(!Pistol.GetParent().IsSet() && Pistol.GetComponent<RenderCore::AnimSocketComponent>() == nullptr
               && Pistol.GetComponent<RigidBodyComponent>()->bEnabled, "the pistol is let go: unsocketed, its body on");
        const RenderCore::AnimInstance* Instance = Animation->FindInstance(Dummy);
        Expect(Instance && Instance->HasPoseOverride(), "physics poses the skeleton");

        double SettledAfter = -1.0;
        for(int Step = 0; Step < int(8.0 / Frame) && Seen.Settled == 0; ++Step)
        {
            W.Tick(Frame);
            if(Seen.Settled > 0) { SettledAfter = double(Step) * Frame; }
        }
        Expect(Seen.Settled == 1 && Ragdoll->IsSettled(), "it comes to rest and says so (after " + Num(SettledAfter + 0.1) + " s)");

        Animation->GetBoneWorldPose(Dummy, "DEF-spine.006", Head);
        Animation->GetBoneWorldPose(Dummy, "DEF-spine", Chest);
        Expect(Head.Position.GetY() < 45.0 && Head.Position.GetY() > 0.0, "it lies on the floor (head at " + Num(Head.Position.GetY()) + ")");
        Expect(Chest.Position.GetZ() > 10.0, "pushed back by the shot (pelvis z " + Num(Chest.Position.GetZ()) + ")");
        const double PistolDown = Pistol.GetWorldPose().Position.GetY();
        Expect(PistolDown < PistolHeld && PistolDown < 30.0, "the pistol fell to the floor (y " + Num(PistolHeld) + " -> " + Num(PistolDown) + ")");

        /* Back to the animation. */
        Ragdoll->Reset();
        Run(W, 0.1);
        Animation->GetBoneWorldPose(Dummy, "DEF-spine.006", Head);
        Expect(!Ragdoll->bActive && !Instance->HasPoseOverride() && Dummy.GetComponent<RenderCore::AnimationComponent>()->bPaused == 0,
               "a reset gives the skeleton back and unpauses the animation");
        Expect(std::fabs(Head.Position.GetY() - Standing) < 15.0, "and it stands again (head at " + Num(Head.Position.GetY()) + ")");
        Expect(Ragdolls->GetRagdollCount() == 1 && Ragdolls->GetActiveCount() == 0, "with its hit boxes back");

        Dummy.RemoveComponent<NimbleEngine::RagdollComponent>();
        Run(W, 0.05);
        JoltPhysics::PhysicsStats Stats;
        Physics.GetStats(Stats);
        Expect(Ragdolls->GetRagdollCount() == 0 && Stats.Ragdolls == 0, "removing the component takes its ragdoll");

        /* No hit boxes: made at activation, with the animation's velocity. */
        NimbleEngine::RagdollComponent Bare;
        Bare.bHitBoxes = 0;
        Dummy.AddComponent(Bare);
        Run(W, 0.2);
        Expect(Ragdolls->GetRagdollCount() == 0, "without hit boxes nothing is made while alive");
        Dummy.GetComponent<NimbleEngine::RagdollComponent>()->Activate();
        Run(W, 2.0);
        Animation->GetBoneWorldPose(Dummy, "DEF-spine.006", Head);
        Expect(Ragdolls->GetActiveCount() == 1 && Head.Position.GetY() < 60.0, "but activation makes one, and it falls (head at " +
                                                                                  Num(Head.Position.GetY()) + ")");

        NimbleEngine::RagdollWorld::Detach(W);
        Physics.GetStats(Stats);
        Expect(Stats.Ragdolls == 0, "detaching takes every ragdoll");
        RenderCore::AnimationSystem::Detach(W);
        Physics.Detach();
    }
}

int main(const int InArgc, char** InArgv)
{
    GVerbose = InArgc > 1 && std::string(InArgv[1]) == "-v";
    std::printf("NimbleEngineRagdollTests\n");
    TestProfile();
    TestHingeLimits();
    TestDeath();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
