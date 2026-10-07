/*
 * RenderCoreAnimationTests -- the animation runtime without a window: pose
 * math, clip playback and notifies, and a World's AnimationSystem ticking an
 * Animation component (ANIMATION.md). Assets are built here and written
 * beside the executable, where runtime paths resolve.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimPose.h"
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

static bool Near(const float A, const float B, const float Tolerance = 1e-3f) { return std::fabs(A - B) < Tolerance; }

static const char* const SCRATCH = "AnimationTestScratch/";

/* Root at the origin; Arm 100 along X, a child of Root; Hand 50 further. */
static SkeletonData MakeSkeleton()
{
    SkeletonData Skeleton;
    Skeleton.Bones.resize(3);
    Skeleton.Bones[0].Name = "Root";
    Skeleton.Bones[1].Name = "Arm";
    Skeleton.Bones[1].Parent = 0;
    Skeleton.Bones[1].RefPose.Translation = { 100.0f, 0.0f, 0.0f };
    Skeleton.Bones[2].Name = "Hand";
    Skeleton.Bones[2].Parent = 1;
    Skeleton.Bones[2].RefPose.Translation = { 50.0f, 0.0f, 0.0f };
    return Skeleton;
}

/* Arm turns 0 -> 90 degrees about Z over one second (31 frames); a notify at 0.5 s. */
static AnimClipData MakeClip()
{
    AnimClipData Clip;
    Clip.SampleRate = 30.0f;
    Clip.FrameCount = 31;
    Clip.bLoop      = true;
    Clip.RootBone   = "Root";
    AnimTrack Track;
    Track.Bone = "Arm";
    Track.Translations.push_back({ 100.0f, 0.0f, 0.0f });
    Track.Scales.push_back({ 1.0f, 1.0f, 1.0f });
    for(uint32_t Frame = 0; Frame < Clip.FrameCount; ++Frame)
    {
        const float Angle = 0.5f * 1.5707963f * float(Frame) / 30.0f;
        Track.Rotations.push_back({ 0.0f, 0.0f, std::sin(Angle), std::cos(Angle) });
    }
    Clip.Tracks.push_back(Track);
    Clip.Notifies.push_back({ "Step", 0.5f });
    return Clip;
}

/* One triangle, every vertex on Hand, binding where the reference pose puts it. */
static MeshData MakeMesh(const SkeletonData& InSkeleton, const std::string& InSkeletonPath)
{
    MeshData Mesh;
    Mesh.Positions = { 150.0f, 0.0f, 0.0f,   160.0f, 0.0f, 0.0f,   150.0f, 10.0f, 0.0f };
    Mesh.Normals   = { 0, 0, 1,  0, 0, 1,  0, 0, 1 };
    Mesh.UV0       = { 0, 0,  1, 0,  0, 1 };
    Mesh.Indices   = { 0, 1, 2 };
    Mesh.Sections.push_back({ "Tri", "", 0, 3 });
    Mesh.Skeleton  = InSkeletonPath;
    std::vector<AnimMatrix> Model;
    InSkeleton.ComputeRefPoseModel(Model);
    for(const AnimMatrix& Each : Model) { Mesh.InverseBind.push_back(AnimMath::Inverse(Each)); }
    Mesh.Joints  = { 2, 0, 0, 0,  2, 0, 0, 0,  2, 0, 0, 0 };
    Mesh.Weights = { 1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0 };
    ComputeMeshBounds(Mesh);
    return Mesh;
}

static void TestPoseMath()
{
    std::printf("[pose math]\n");
    const SkeletonData Skeleton = MakeSkeleton();
    AnimLocalPose Pose;
    GetRefPose(Skeleton, Pose);
    std::vector<AnimMatrix> Model;
    LocalToModel(Skeleton, Pose, Model);
    Expect(Model.size() == 3 && Near(Model[2].M[0][3], 150.0f), "the reference pose composes parents first");

    const AnimClipData Clip = MakeClip();
    AnimClipBinding Binding;
    BindClip(Clip, Skeleton, Binding);
    Expect(Binding.TrackBone.size() == 1 && Binding.TrackBone[0] == 1, "a track binds to its bone by name");

    SampleClip(Clip, Binding, 1.0f, Pose);
    LocalToModel(Skeleton, Pose, Model);
    Expect(Near(Model[2].M[0][3], 100.0f) && Near(Model[2].M[1][3], 50.0f), "at the end, the arm has turned the hand to (100, 50)");

    AnimLocalPose Rest;
    GetRefPose(Skeleton, Rest);
    AnimLocalPose Half = Rest;
    BlendPoses(Half, Pose, 0.5f);
    LocalToModel(Skeleton, Half, Model);
    Expect(Near(Model[2].M[0][3], 100.0f + 50.0f * std::cos(0.7853982f), 0.01f), "a half blend turns it half way (45 degrees)");

    std::vector<float> Mask;
    MakeBoneMask(Skeleton, { "Hand" }, Mask);
    Expect(Mask.size() == 3 && Mask[0] == 0.0f && Mask[1] == 0.0f && Mask[2] == 1.0f, "a mask takes its root and what is below");
    MakeBoneMask(Skeleton, { "Arm" }, Mask);
    Expect(Mask[1] == 1.0f && Mask[2] == 1.0f && Mask[0] == 0.0f, "and nothing above");
    AnimLocalPose Masked = Rest;
    MakeBoneMask(Skeleton, { "Hand" }, Mask);
    BlendPoses(Masked, Pose, 1.0f, &Mask);
    LocalToModel(Skeleton, Masked, Model);
    Expect(Near(Model[2].M[0][3], 150.0f), "blending through a mask leaves the bones outside it");

    bool bWrapped = false;
    Expect(Near(AdvanceClipTime(Clip, true, 0.9f, 0.2f, bWrapped), 0.1f) && bWrapped, "a loop wraps");
    Expect(Near(AdvanceClipTime(Clip, false, 0.9f, 0.2f, bWrapped), 1.0f) && bWrapped, "a one-shot stops at its end");
    std::vector<std::string> Names;
    CollectNotifies(Clip, 0.4f, 0.6f, false, Names);
    CollectNotifies(Clip, 0.6f, 0.1f, true, Names);
    CollectNotifies(Clip, 0.9f, 0.6f, true, Names);
    Expect(Names.size() == 2, "a notify fires when passed, across a wrap too, and not otherwise");
}

static void TestInstance()
{
    std::printf("[instance]\n");
    auto Skeleton = std::make_shared<SkeletonData>(MakeSkeleton());
    auto Clip     = std::make_shared<AnimClipData>(MakeClip());

    AnimInstance Instance;
    Expect(!Instance.HasPose(), "nothing to pose without a skeleton");
    Instance.SetSkeleton(Skeleton);
    Expect(Instance.HasPose() && Near(Instance.GetModelPose()[2].M[0][3], 150.0f), "a skeleton alone: its reference pose");
    Instance.PlayClip(Clip);
    Instance.Update(0.6f);
    Expect(Near(Instance.GetTime(), 0.6f) && Instance.GetFiredNotifies().size() == 1, "playing passes the notify at 0.5 s");
    Instance.Update(0.6f);
    Expect(Near(Instance.GetTime(), 0.2f) && Instance.GetFiredNotifies().empty(), "it loops, and the notify waits for the next pass");
    Instance.SetPlayRate(0.0f);
    Instance.Update(1.0f);
    Expect(Near(Instance.GetTime(), 0.2f), "a zero play rate holds it");
    Instance.SetTime(1.0f);
    Expect(Near(Instance.GetModelPose()[2].M[1][3], 50.0f), "a seek evaluates where it lands");
}

static void TestSystem()
{
    std::printf("[system]\n");
    const SkeletonData Skeleton = MakeSkeleton();
    AnimClipData Clip = MakeClip();
    const std::string SkeletonPath = std::string(SCRATCH) + "Arm.nmskel";
    const std::string ClipPath     = std::string(SCRATCH) + "Arm_Bend.nmanim";
    const std::string MeshPath     = std::string(SCRATCH) + "Arm.nmmesh";
    SkeletonData SkeletonCopy = Skeleton;
    MeshData Mesh = MakeMesh(Skeleton, SkeletonPath);
    Expect(SaveSkeletonFile(SkeletonPath, SkeletonCopy) && SaveAnimClipFile(ClipPath, Clip) && SaveMeshFile(MeshPath, Mesh),
           "the assets are written");
    SkeletonAssetCache::Invalidate("");
    AnimClipAssetCache::Invalidate("");
    MeshAssetCache::Invalidate("");

    GameCore::World World;
    AnimationSystem* System = AnimationSystem::Attach(World);
    Expect(System && AnimationSystem::Find(&World) == System, "a world has its system once attached");

    GameCore::Entity Actor = World.CreateEntity();
    GameCore::TransformComponent Transform;
    Transform.Position = EngineCore::Vector3(1000.0, 0.0, 0.0);
    Actor.AddComponent(Transform);
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", MeshPath.c_str());
    Actor.AddComponent(Render);
    AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%s", ClipPath.c_str());
    Actor.AddComponent(Animation);

    int Notifies = 0;
    World.GetEvents().Subscribe("AnimNotify", [](GameCore::EventID, const GameCore::EventArgs& InArgs, void* InUserData)
    {
        if(InArgs.Count == 2 && std::strcmp(InArgs.Args[1].String, "Step") == 0) { ++*static_cast<int*>(InUserData); }
    }, &Notifies);

    World.Tick(0.6f);
    const AnimInstance* Instance = System->FindInstance(Actor);
    Expect(Instance && Instance->HasPose() && Near(Instance->GetTime(), 0.6f), "a tick plays the component's clip on its mesh's skeleton");
    Expect(Notifies == 1, "and emits the notify it passed as AnimNotify");

    GameCore::TransformPose Hand;
    Expect(System->GetBoneWorldPose(Actor, "Hand", Hand) && Hand.Position.GetX() > 1000.0 && Hand.Position.GetY() > 0.0,
           "a bone's world pose follows the entity and the clip");
    Expect(!System->GetBoneWorldPose(Actor, "Tail", Hand), "a bone the skeleton lacks is not found");

    Actor.GetComponent<AnimationComponent>()->bPaused = 1;
    World.Tick(0.3f);
    Expect(Near(System->FindInstance(Actor)->GetTime(), 0.6f), "paused, it holds");

    AnimationComponent* Live = Actor.GetComponent<AnimationComponent>();
    Expect(Live->SetFloat("Speed", 300.0f) && Near(Live->GetFloat("Speed"), 300.0f) && Live->SetTrigger("Jump") && Live->GetBool("Jump"),
           "parameters set and read by name");

    Live->Animation[0] = '\0';
    World.Tick(0.1f);
    Expect(System->FindInstance(Actor) && Near(System->FindInstance(Actor)->GetModelPose()[2].M[0][3], 150.0f),
           "with nothing to play, the skeleton holds its reference pose");

    Actor.RemoveComponent<AnimationComponent>();
    World.Tick(0.1f);
    Expect(System->FindInstance(Actor) == nullptr, "removing the component drops its instance");

    AnimationSystem::Detach(World);
    Expect(AnimationSystem::Find(&World) == nullptr, "the last detach removes the system");
}

int main()
{
    std::printf("RenderCore animation tests\n");
    TestPoseMath();
    TestInstance();
    TestSystem();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
