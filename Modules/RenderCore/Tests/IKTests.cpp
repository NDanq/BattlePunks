/*
 * RenderCoreIKTests -- inverse kinematics without a window (ANIMATION.md,
 * "IK"): the two-bone solver, an anim state's IK chunk, feet on the ground
 * (steps, holes, slopes, the pelvis lowered, eased), a hand on another bone,
 * a script goal, and the animation system tracing the ground through a
 * stand-in for the physics. The rig is built here: legs straight down, arms
 * out, every bone unrotated.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/AnimIK.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/MeshAsset.h"

using namespace RenderCore;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

static bool Near(const float A, const float B, const float Tolerance = 1e-2f) { return std::fabs(A - B) < Tolerance; }
static bool Near(const AnimVec3& A, const AnimVec3& B, const float Tolerance = 1e-2f)
{
    return Near(A.X, B.X, Tolerance) && Near(A.Y, B.Y, Tolerance) && Near(A.Z, B.Z, Tolerance);
}
static bool SameRotation(const AnimQuat& A, const AnimQuat& B, const float Tolerance = 1e-4f)
{
    return std::fabs(std::fabs(AnimMath::Dot(A, B)) - 1.0f) < Tolerance;
}

static const char* const SCRATCH = "IKTestScratch/";

/* Root at the origin; the pelvis at 90; each leg hip (+-10, 90), knee 40
 * below, ankle 40 below that (at 10: the ankle's height when standing); a
 * chest at 130; each arm shoulder (+-20, 130), elbow 30 out, wrist 30 more. */
static SkeletonData MakeBody()
{
    SkeletonData Body;
    const auto Add = [&Body](const char* InName, const char* InParent, const AnimVec3& InModel)
    {
        SkeletonBone Bone;
        Bone.Name   = InName;
        Bone.Parent = InParent[0] ? Body.FindBone(InParent) : -1;
        /* Every bone unrotated: its local translation is its model place less its parent's. */
        AnimVec3 ParentModel;
        for(int32_t At = Bone.Parent; At >= 0; At = Body.Bones[size_t(At)].Parent)
        {
            ParentModel = AnimMath::Add(ParentModel, Body.Bones[size_t(At)].RefPose.Translation);
        }
        Bone.RefPose.Translation = AnimMath::Sub(InModel, ParentModel);
        Body.Bones.push_back(Bone);
    };
    Add("Root",     "",         { 0.0f, 0.0f, 0.0f });
    Add("Pelvis",   "Root",     { 0.0f, 90.0f, 0.0f });
    Add("Thigh.L",  "Pelvis",   { 10.0f, 90.0f, 0.0f });
    Add("Shin.L",   "Thigh.L",  { 10.0f, 50.0f, 0.0f });
    Add("Foot.L",   "Shin.L",   { 10.0f, 10.0f, 0.0f });
    Add("Thigh.R",  "Pelvis",   { -10.0f, 90.0f, 0.0f });
    Add("Shin.R",   "Thigh.R",  { -10.0f, 50.0f, 0.0f });
    Add("Foot.R",   "Shin.R",   { -10.0f, 10.0f, 0.0f });
    Add("Chest",    "Pelvis",   { 0.0f, 130.0f, 0.0f });
    Add("Arm.L",    "Chest",    { 20.0f, 130.0f, 0.0f });
    Add("Forearm.L", "Arm.L",   { 50.0f, 130.0f, 0.0f });
    Add("Hand.L",   "Forearm.L", { 80.0f, 130.0f, 0.0f });
    Add("Arm.R",    "Chest",    { -20.0f, 130.0f, 0.0f });
    Add("Forearm.R", "Arm.R",   { -50.0f, 130.0f, 0.0f });
    Add("Hand.R",   "Forearm.R", { -80.0f, 130.0f, 0.0f });
    return Body;
}

static AnimVec3 At(const std::vector<AnimMatrix>& InModel, const SkeletonData& InBody, const char* InBone)
{
    const AnimMatrix& M = InModel[size_t(InBody.FindBone(InBone))];
    return { M.M[0][3], M.M[1][3], M.M[2][3] };
}

static float Distance(const AnimVec3& A, const AnimVec3& B) { return AnimMath::Length(AnimMath::Sub(A, B)); }

/* A state of nothing but the reference pose, with InIK. */
static std::shared_ptr<AnimStateData> MakeState(const AnimIK& InIK)
{
    auto State = std::make_shared<AnimStateData>();
    State->Parameters = { { "Armed", EAnimParameterType::Float, 0.0f } };
    AnimLayer Base;
    Base.Name = "Base";
    AnimStateNode Stand;
    Stand.Name = "Stand";
    Base.States.push_back(Stand);
    State->Layers.push_back(Base);
    State->IK = InIK;
    return State;
}

static AnimIKChain Chain(const char* InName, const char* InRoot, const char* InMid, const char* InEnd, const EAnimIKGoal InGoal)
{
    AnimIKChain Out;
    Out.Name = InName;
    Out.Root = InRoot;
    Out.Mid  = InMid;
    Out.End  = InEnd;
    Out.Goal = InGoal;
    return Out;
}

static AnimIK MakeLegs()
{
    AnimIK IK;
    IK.Chains = { Chain("LeftFoot", "Thigh.L", "Shin.L", "Foot.L", EAnimIKGoal::Ground),
                  Chain("RightFoot", "Thigh.R", "Shin.R", "Foot.R", EAnimIKGoal::Ground) };
    IK.PelvisBone = "Pelvis";
    IK.Response   = 0.0f;
    return IK;
}

static const AnimVec3 UP{ 0.0f, 1.0f, 0.0f };

// === Tests ==================================================================

static void TestSolver()
{
    std::printf("[solver]\n");
    const SkeletonData Body = MakeBody();
    AnimLocalPose Pose;
    std::vector<AnimMatrix> Model;
    const int32_t Thigh = Body.FindBone("Thigh.L"), Shin = Body.FindBone("Shin.L"), Foot = Body.FindBone("Foot.L");

    /* Raised 20: the knee bends, forward (the pole), the bones keep their lengths. */
    GetRefPose(Body, Pose);
    AnimTwoBoneIK Solve;
    Solve.Root = Thigh; Solve.Mid = Shin; Solve.End = Foot;
    Solve.Target = { 10.0f, 30.0f, 0.0f };
    SolveTwoBoneIK(Body, Pose, Solve);
    LocalToModel(Body, Pose, Model);
    Expect(Near(At(Model, Body, "Foot.L"), Solve.Target), "the foot reaches its target");
    Expect(Near(Distance(At(Model, Body, "Thigh.L"), At(Model, Body, "Shin.L")), 40.0f)
           && Near(Distance(At(Model, Body, "Shin.L"), At(Model, Body, "Foot.L")), 40.0f), "and the leg keeps its lengths");
    Expect(At(Model, Body, "Shin.L").Z > 10.0f, "a straight leg bends the way its pole says (knee forward)");
    Expect(SameRotation(AnimMath::ToTransform(Model[size_t(Foot)]).Rotation, AnimQuat()), "the foot keeps its model rotation");

    /* Bent backward in the animation: it stays bent that way. */
    GetRefPose(Body, Pose);
    Pose[size_t(Thigh)].Rotation = AnimMath::Normalize({ 0.2588f, 0.0f, 0.0f, 0.9659f });   // 30 degrees about X: the knee back
    Pose[size_t(Shin)].Rotation  = AnimMath::Normalize({ -0.5f, 0.0f, 0.0f, 0.866f });
    SolveTwoBoneIK(Body, Pose, Solve);
    LocalToModel(Body, Pose, Model);
    Expect(Near(At(Model, Body, "Foot.L"), Solve.Target) && At(Model, Body, "Shin.L").Z < -10.0f, "a bent knee keeps its bend's side");

    /* Out of reach: reached for, straight. */
    GetRefPose(Body, Pose);
    Solve.Target = { 10.0f, -100.0f, 0.0f };
    SolveTwoBoneIK(Body, Pose, Solve);
    LocalToModel(Body, Pose, Model);
    Expect(Near(At(Model, Body, "Foot.L"), { 10.0f, 10.0f, 0.0f }, 0.1f), "a target out of reach is reached toward, the leg straight");

    /* Half weight: half way; a rotation, by the weight. */
    GetRefPose(Body, Pose);
    Solve.Target = { 10.0f, 30.0f, 0.0f };
    Solve.Weight = 0.5f;
    Solve.bRotation = true;
    Solve.Rotation  = AnimMath::Normalize({ 0.0f, 0.0f, 0.3827f, 0.9239f });   // 45 degrees about Z
    SolveTwoBoneIK(Body, Pose, Solve);
    LocalToModel(Body, Pose, Model);
    Expect(Near(At(Model, Body, "Foot.L"), { 10.0f, 20.0f, 0.0f }), "half the weight goes half way");
    Expect(SameRotation(AnimMath::ToTransform(Model[size_t(Foot)]).Rotation, AnimMath::Nlerp(AnimQuat(), Solve.Rotation, 0.5f)),
           "and turns the end half way");
    Solve.Weight = 0.0f;
    GetRefPose(Body, Pose);
    AnimLocalPose Before = Pose;
    SolveTwoBoneIK(Body, Pose, Solve);
    bool bSame = true;
    for(size_t Bone = 0; Bone < Pose.size(); ++Bone) { bSame = bSame && SameRotation(Pose[Bone].Rotation, Before[Bone].Rotation); }
    Expect(bSame, "no weight, no change");
}

static void TestAsset()
{
    std::printf("[asset]\n");
    AnimIK IK = MakeLegs();
    AnimIKChain Hand = Chain("OffHand", "Arm.L", "Forearm.L", "Hand.L", EAnimIKGoal::Bone);
    Hand.GoalBone = "Hand.R";
    Hand.GoalOffset.Translation = { 0.0f, 0.0f, 20.0f };
    Hand.WeightParameter = "Armed";
    Hand.Pole[0] = 0.0f; Hand.Pole[1] = -1.0f; Hand.Pole[2] = 0.0f;
    Hand.bAlignEnd = false;
    IK.Chains.push_back(Hand);
    IK.TraceUp = 40.0f;
    const std::shared_ptr<AnimStateData> State = MakeState(IK);
    EngineCore::Archive Writer = EngineCore::Archive::ForWriting();
    Expect(SerializeAnimState(Writer, *State), "an anim state with IK saves");
    EngineCore::Archive Reader = EngineCore::Archive::ForReading(Writer.GetBytes());
    AnimStateData Loaded;
    Expect(SerializeAnimState(Reader, Loaded), "and loads");
    Expect(Loaded.IK.Chains.size() == 3 && Loaded.IK.PelvisBone == "Pelvis" && Near(Loaded.IK.TraceUp, 40.0f), "with its chains and settings");
    const AnimIKChain& Back = Loaded.IK.Chains[2];
    Expect(Back.Name == "OffHand" && Back.Goal == EAnimIKGoal::Bone && Back.GoalBone == "Hand.R" && Near(Back.GoalOffset.Translation.Z, 20.0f)
           && Back.WeightParameter == "Armed" && Near(Back.Pole[1], -1.0f) && !Back.bAlignEnd, "each chain whole");

    AnimStateData Bad = *State;
    Bad.IK.Chains[0].Mid.clear();
    Expect(Bad.FindDefect() != nullptr, "a chain without a bone is refused");
    Bad = *State;
    Bad.IK.Chains[2].GoalBone.clear();
    Expect(Bad.FindDefect() != nullptr, "a bone goal without a bone is refused");
    Bad = *State;
    Bad.IK.Chains[1].Name = "LeftFoot";
    Expect(Bad.FindDefect() != nullptr, "two chains of one name are refused");
    Bad = *State;
    Bad.IK.Response = -1.0f;
    Expect(Bad.FindDefect() != nullptr, "a negative response is refused");
}

static void TestFeet()
{
    std::printf("[feet]\n");
    const auto Body = std::make_shared<SkeletonData>(MakeBody());
    AnimInstance Instance;
    Instance.SetSkeleton(Body);
    Instance.SetState(MakeState(MakeLegs()));
    Instance.SetModelToWorld(AnimMatrix());
    Instance.Evaluate();
    Expect(Instance.HasIK() && Instance.GetIKProbes().size() == 3, "probes: where it stands, and under each foot");
    Expect(Instance.GetIKProbes().size() == 3 && Near(Instance.GetIKProbes()[1].From, { 10.0f, 60.0f, 0.0f })
           && Near(Instance.GetIKProbes()[1].Length, 125.0f), "from TraceUp above the foot, TraceUp + TraceDown long");

    /* A step of 10 under the left foot. */
    Instance.SetIKGround(-1, true, { 0.0f, 0.0f, 0.0f }, UP);
    Instance.SetIKGround(0, true, { 10.0f, 10.0f, 0.0f }, UP);
    Instance.SetIKGround(1, true, { -10.0f, 0.0f, 0.0f }, UP);
    Instance.Evaluate();
    Expect(Near(At(Instance.GetModelPose(), *Body, "Foot.L"), { 10.0f, 20.0f, 0.0f }) && Near(At(Instance.GetModelPose(), *Body, "Foot.R"), { -10.0f, 10.0f, 0.0f }),
           "a step raises the foot on it by its height, the other stays");
    Expect(Near(Instance.GetPelvisDrop(), 0.0f) && Near(At(Instance.GetModelPose(), *Body, "Pelvis").Y, 90.0f), "a step lowers no pelvis");

    /* A hole of 15 under the right: the pelvis comes down 15 for it. */
    Instance.SetIKGround(1, true, { -10.0f, -15.0f, 0.0f }, UP);
    Instance.Evaluate();
    Expect(Near(At(Instance.GetModelPose(), *Body, "Foot.R"), { -10.0f, -5.0f, 0.0f }), "a foot over a hole reaches down into it");
    Expect(Near(Instance.GetPelvisDrop(), 15.0f) && Near(At(Instance.GetModelPose(), *Body, "Pelvis").Y, 75.0f), "the pelvis drops so it can");
    Expect(Near(At(Instance.GetModelPose(), *Body, "Foot.L"), { 10.0f, 20.0f, 0.0f }), "and the other foot stays on its step, its knee bent");

    /* A slope under the left foot: on the plane, turned with it. */
    const float Tilt = 20.0f * 3.14159265f / 180.0f;
    const AnimVec3 Slope{ 0.0f, std::cos(Tilt), -std::sin(Tilt) };   // rising toward +Z
    Instance.SetIKGround(1, true, { -10.0f, 0.0f, 0.0f }, UP);
    Instance.SetIKGround(0, true, { 10.0f, 0.0f, 0.0f }, Slope);
    Instance.Evaluate();
    const AnimMatrix& LeftFoot = Instance.GetModelPose()[size_t(Body->FindBone("Foot.L"))];
    Expect(Near(At(Instance.GetModelPose(), *Body, "Foot.L"), { 10.0f, 10.0f, 0.0f }), "a slope through the foot's spot leaves it where it is");
    Expect(Near(AnimMath::Rotate(AnimMath::ToTransform(LeftFoot).Rotation, UP), Slope, 1e-3f), "and turns it to the slope");

    /* A steep slope -- stairs, to the physics: turned by MaxTilt (25) at most. */
    const float Steep = 42.0f * 3.14159265f / 180.0f;
    Instance.SetIKGround(0, true, { 10.0f, 0.0f, 0.0f }, { 0.0f, std::cos(Steep), -std::sin(Steep) });
    Instance.Evaluate();
    const AnimVec3 FootUp = AnimMath::Rotate(AnimMath::ToTransform(Instance.GetModelPose()[size_t(Body->FindBone("Foot.L"))]).Rotation, UP);
    Expect(Near(std::acos(std::min(AnimMath::Dot(FootUp, UP), 1.0f)) * 180.0f / 3.14159265f, 25.0f, 0.1f),
           "a foot turns with a steep slope by MaxTilt at most");

    /* Too far: clamped to MaxStep. */
    Instance.SetIKGround(0, true, { 10.0f, 200.0f, 0.0f }, UP);
    Instance.Evaluate();
    Expect(Near(At(Instance.GetModelPose(), *Body, "Foot.L").Y, 60.0f), "a foot moves at most MaxStep (50)");

    /* No ground: as animated. */
    Instance.SetIKGround(0, false, {}, {});
    Instance.SetIKGround(1, false, {}, {});
    Instance.Evaluate();
    Expect(Near(At(Instance.GetModelPose(), *Body, "Foot.L"), { 10.0f, 10.0f, 0.0f }) && Near(Instance.GetPelvisDrop(), 0.0f),
           "with no ground under them the feet are as animated");

    /* Eased: a step reached over time, not at once. */
    AnimIK Eased = MakeLegs();
    Eased.Response = 10.0f;
    Instance.SetState(MakeState(Eased));
    Instance.SetModelToWorld(AnimMatrix());
    Instance.Evaluate();                                  // placed: level ground
    Instance.SetIKGround(-1, true, {}, UP);
    Instance.SetIKGround(0, true, { 10.0f, 10.0f, 0.0f }, UP);
    Instance.SetIKGround(1, true, { -10.0f, 0.0f, 0.0f }, UP);
    Instance.Update(0.05f);
    const float Partial = At(Instance.GetModelPose(), *Body, "Foot.L").Y;
    for(int Step = 0; Step < 40; ++Step) { Instance.Update(0.05f); }
    Expect(Partial > 11.0f && Partial < 19.0f && Near(At(Instance.GetModelPose(), *Body, "Foot.L").Y, 20.0f, 0.05f),
           "with a response, a foot eases onto a step");

    /* In the world: the entity moved and the ground in world space. */
    AnimMatrix Placed;
    Placed.M[0][3] = 500.0f; Placed.M[1][3] = 30.0f;
    Instance.SetState(MakeState(MakeLegs()));
    Instance.SetModelToWorld(Placed);
    Instance.Evaluate();
    Expect(Near(Instance.GetIKProbes()[1].From, { 510.0f, 90.0f, 0.0f }), "probes are in the world");
    Instance.SetIKGround(-1, true, { 500.0f, 30.0f, 0.0f }, UP);
    Instance.SetIKGround(0, true, { 510.0f, 38.0f, 0.0f }, UP);
    Instance.SetIKGround(1, true, { 490.0f, 30.0f, 0.0f }, UP);
    Instance.Evaluate();
    Expect(Near(At(Instance.GetModelPose(), *Body, "Foot.L").Y, 18.0f), "and the ground is measured from where the entity stands");
}

static void TestHands()
{
    std::printf("[hands]\n");
    const auto Body = std::make_shared<SkeletonData>(MakeBody());
    AnimIK IK;
    AnimIKChain Hand = Chain("OffHand", "Arm.L", "Forearm.L", "Hand.L", EAnimIKGoal::Bone);
    Hand.GoalBone = "Hand.R";
    Hand.GoalOffset.Translation = { 90.0f, -20.0f, 30.0f };   // in front of the chest, a grip forward of the right hand
    Hand.GoalOffset.Rotation    = AnimMath::Normalize({ 0.0f, 0.7071f, 0.0f, 0.7071f });
    Hand.WeightParameter = "Armed";
    IK.Chains.push_back(Hand);
    AnimIKChain Reach = Chain("Reach", "Arm.R", "Forearm.R", "Hand.R", EAnimIKGoal::Script);
    IK.Chains.insert(IK.Chains.begin(), Reach);   // the right hand first: the left reaches for where it went
    AnimInstance Instance;
    Instance.SetSkeleton(Body);
    Instance.SetState(MakeState(IK));
    Instance.SetModelToWorld(AnimMatrix());
    Instance.Evaluate();
    Expect(Near(At(Instance.GetModelPose(), *Body, "Hand.L"), { 80.0f, 130.0f, 0.0f }), "a weight parameter at 0 leaves the hand as animated");

    Instance.SetParameter("Armed", 1.0f);
    Instance.Evaluate();
    Expect(Near(At(Instance.GetModelPose(), *Body, "Hand.L"), { 10.0f, 110.0f, 30.0f }), "at 1 the off hand goes to its place on the other hand");
    Expect(SameRotation(AnimMath::ToTransform(Instance.GetModelPose()[size_t(Body->FindBone("Hand.L"))]).Rotation, Hand.GoalOffset.Rotation),
           "turned as the goal is");
    Expect(Near(Distance(At(Instance.GetModelPose(), *Body, "Arm.L"), At(Instance.GetModelPose(), *Body, "Forearm.L")), 30.0f),
           "its arm keeps its lengths");

    /* A script goal, in the world; the off hand follows the hand it holds. */
    Expect(!Instance.SetIKGoal("Nothing", {}, 1.0f), "a goal for a chain it lacks is refused");
    Expect(Instance.SetIKGoal("Reach", { -30.0f, 150.0f, 0.0f }, 1.0f), "a script goal is set");
    Instance.Evaluate();
    const AnimVec3 Right = At(Instance.GetModelPose(), *Body, "Hand.R");
    Expect(Near(Right, { -30.0f, 150.0f, 0.0f }), "the hand reaches it");
    Expect(Near(At(Instance.GetModelPose(), *Body, "Hand.L"), AnimMath::Add(Right, AnimMath::Rotate(
                AnimMath::ToTransform(Instance.GetModelPose()[size_t(Body->FindBone("Hand.R"))]).Rotation, { 90.0f, -20.0f, 30.0f }))),
           "and the off hand stays on it");
    Instance.SetIKGoal("Reach", {}, 0.0f);
    Instance.Evaluate();
    Expect(Near(At(Instance.GetModelPose(), *Body, "Hand.R"), { -80.0f, 130.0f, 0.0f }), "cleared, it is as animated");
}

/* The stand-in for the physics: a floor at 0, a step of 12 for x > 0; never the character. */
static int GQueries = 0;
static bool StepGround(GameCore::World&, GameCore::EntityID, const EngineCore::Vector3& InFrom, const EngineCore::Vector3&, const float InLength,
                       EngineCore::Vector3& OutPoint, EngineCore::Vector3& OutNormal)
{
    ++GQueries;
    const double Height = InFrom.GetX() > 0.0 ? 12.0 : 0.0;
    if(InFrom.GetY() - Height > double(InLength) || InFrom.GetY() < Height) { return false; }
    OutPoint  = EngineCore::Vector3(InFrom.GetX(), Height, InFrom.GetZ());
    OutNormal = EngineCore::Vector3(0.0, 1.0, 0.0);
    return true;
}

static void TestSystem()
{
    std::printf("[system]\n");
    SkeletonData Body = MakeBody();
    const std::string SkeletonPath = std::string(SCRATCH) + "Body.nmskel";
    const std::string MeshPath     = std::string(SCRATCH) + "Body.nmmesh";
    const std::string StatePath    = std::string(SCRATCH) + "Body.nmanimstate";
    MeshData Mesh;
    Mesh.Positions = { 10.0f, 10.0f, 0.0f,   12.0f, 10.0f, 0.0f,   10.0f, 12.0f, 0.0f };
    Mesh.Normals   = { 0, 0, 1,  0, 0, 1,  0, 0, 1 };
    Mesh.UV0       = { 0, 0,  1, 0,  0, 1 };
    Mesh.Indices   = { 0, 1, 2 };
    Mesh.Sections.push_back({ "Tri", "", 0, 3 });
    Mesh.Skeleton  = SkeletonPath;
    std::vector<AnimMatrix> Model;
    Body.ComputeRefPoseModel(Model);
    for(const AnimMatrix& Each : Model) { Mesh.InverseBind.push_back(AnimMath::Inverse(Each)); }
    const uint16_t Foot = uint16_t(Body.FindBone("Foot.L"));
    Mesh.Joints  = { Foot, 0, 0, 0,  Foot, 0, 0, 0,  Foot, 0, 0, 0 };
    Mesh.Weights = { 1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0 };
    ComputeMeshBounds(Mesh);
    AnimStateData State = *MakeState(MakeLegs());
    Expect(SaveSkeletonFile(SkeletonPath, Body) && SaveMeshFile(MeshPath, Mesh) && SaveAnimStateFile(StatePath, State), "the assets are written");
    SkeletonAssetCache::Invalidate("");
    MeshAssetCache::Invalidate("");
    AnimStateAssetCache::Invalidate("");

    const AnimationSystem::GroundQueryFn Was = AnimationSystem::GetGroundQuery();
    AnimationSystem::SetGroundQuery(&StepGround);
    GameCore::World World;
    AnimationSystem* System = AnimationSystem::Attach(World);
    GameCore::Entity Actor = World.CreateEntity();
    Actor.AddComponent(GameCore::TransformComponent());
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", MeshPath.c_str());
    Actor.AddComponent(Render);
    AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%s", StatePath.c_str());
    Actor.AddComponent(Animation);

    World.Tick(0.1f);   // probes made
    World.Tick(0.1f);   // the ground they found, used
    GameCore::TransformPose Left, Right;
    Expect(GQueries >= 3, "the system traces each probe");
    Expect(System->GetBoneWorldPose(Actor, "Foot.L", Left) && Near(float(Left.Position.GetY()), 22.0f)
           && System->GetBoneWorldPose(Actor, "Foot.R", Right) && Near(float(Right.Position.GetY()), 10.0f),
           "standing at a step's edge, the foot over it is on it");

    AnimationSystem::SetGroundQuery(nullptr);
    World.Tick(0.1f);
    World.Tick(0.1f);
    Expect(System->GetBoneWorldPose(Actor, "Foot.L", Left) && Near(float(Left.Position.GetY()), 10.0f, 0.5f),
           "with no ground query, feet are as animated");
    AnimationSystem::Detach(World);
    AnimationSystem::SetGroundQuery(Was);
}

int main()
{
    std::printf("RenderCore IK tests\n");
    TestSolver();
    TestAsset();
    TestFeet();
    TestHands();
    TestSystem();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
