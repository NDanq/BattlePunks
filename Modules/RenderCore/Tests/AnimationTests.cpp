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
#include "Public/Core/CoreJob.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimPose.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/Renderer.h"

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
    std::vector<AnimNotify> Names;
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

/* A clip that turns InBone 0 -> InDegrees about Z over one second; constant at 0 if InDegrees is 0. */
static AnimClipData MakeTurn(const char* InBone, const float InTranslationX, const float InDegrees, const bool bInLoop)
{
    AnimClipData Clip;
    Clip.SampleRate = 30.0f;
    Clip.FrameCount = 31;
    Clip.bLoop      = bInLoop;
    AnimTrack Track;
    Track.Bone = InBone;
    Track.Translations.push_back({ InTranslationX, 0.0f, 0.0f });
    Track.Scales.push_back({ 1.0f, 1.0f, 1.0f });
    for(uint32_t Frame = 0; Frame < (InDegrees != 0.0f ? Clip.FrameCount : 1u); ++Frame)
    {
        const float Angle = 0.5f * InDegrees * 0.01745329f * float(Frame) / 30.0f;
        Track.Rotations.push_back({ 0.0f, 0.0f, std::sin(Angle), std::cos(Angle) });
    }
    Clip.Tracks.push_back(Track);
    return Clip;
}

/*
 * Base: "Locomotion", a blend space on Speed (Idle at 0, Bend at 100); any
 * state -> "Jump" (Bend, once) on the Jump trigger; Jump -> Locomotion at its
 * end, fading 0.1 s. Upper: "Wave" (the hand turning), masked to Hand,
 * weighted by Armed.
 */
static AnimStateData MakeState()
{
    AnimStateData State;
    State.Parameters = { { "Speed", EAnimParameterType::Float, 0.0f }, { "Armed", EAnimParameterType::Float, 0.0f },
                         { "Jump", EAnimParameterType::Trigger, 0.0f } };

    AnimLayer Base;
    Base.Name = "Base";
    AnimStateNode Locomotion;
    Locomotion.Name = "Locomotion";
    Locomotion.BlendParameter = "Speed";
    Locomotion.Samples = { { std::string(SCRATCH) + "Bend.nmanim", 100.0f }, { std::string(SCRATCH) + "Idle.nmanim", 0.0f } };
    AnimStateNode Jump;
    Jump.Name = "Jump";
    Jump.Samples = { { std::string(SCRATCH) + "Bend.nmanim", 0.0f } };
    Jump.Loop = EAnimLoop::Once;
    Base.States = { Locomotion, Jump };
    AnimTransition ToJump;
    ToJump.From = -1;
    ToJump.To   = 1;
    ToJump.Duration = 0.0f;
    ToJump.Conditions.push_back({ "Jump", EAnimCompare::Triggered, 0.0f });
    AnimTransition Land;
    Land.From = 1;
    Land.To   = 0;
    Land.Duration = 0.1f;
    Land.bAtEnd = true;
    Base.Transitions = { ToJump, Land };

    AnimLayer Upper;
    Upper.Name = "Upper";
    Upper.WeightParameter = "Armed";
    Upper.MaskBones = { "Hand" };
    AnimStateNode Wave;
    Wave.Name = "Wave";
    Wave.Samples = { { std::string(SCRATCH) + "Wave.nmanim", 0.0f } };
    Upper.States = { Wave };

    State.Layers = { Base, Upper };
    return State;
}

static void WriteStateClips()
{
    AnimClipData Idle = MakeTurn("Arm", 100.0f, 0.0f, true);
    AnimClipData Bend = MakeTurn("Arm", 100.0f, 90.0f, true);
    AnimClipData Wave = MakeTurn("Hand", 50.0f, 90.0f, true);
    SaveAnimClipFile(std::string(SCRATCH) + "Idle.nmanim", Idle);
    SaveAnimClipFile(std::string(SCRATCH) + "Bend.nmanim", Bend);
    SaveAnimClipFile(std::string(SCRATCH) + "Wave.nmanim", Wave);
    AnimClipAssetCache::Invalidate("");
}

static void TestStateAsset()
{
    std::printf("[anim state asset]\n");
    AnimStateData State = MakeState();
    Expect(State.FindDefect() == nullptr, "a well-formed anim state has no defect");

    EngineCore::Archive Out = EngineCore::Archive::ForWriting();
    Expect(SerializeAnimState(Out, State), "it saves");
    EngineCore::Archive In = EngineCore::Archive::ForReading(Out.GetBytes());
    AnimStateData Back;
    Expect(SerializeAnimState(In, Back) && Back.Parameters.size() == 3 && Back.Layers.size() == 2
           && Back.Layers[0].Transitions.size() == 2 && Back.Layers[0].Transitions[1].bAtEnd
           && Back.Layers[1].MaskBones.size() == 1 && Back.Layers[0].States[0].Samples.size() == 2
           && Back.Layers[0].Transitions[0].Conditions[0].Compare == EAnimCompare::Triggered,
           "and reads back whole");

    AnimStateData Broken = MakeState();
    Broken.Layers[0].Transitions[0].To = 7;
    EngineCore::Archive Refused = EngineCore::Archive::ForWriting();
    Expect(!SerializeAnimState(Refused, Broken), "a transition to a state the layer lacks is refused");
}

static void TestStateMachine()
{
    std::printf("[state machine]\n");
    WriteStateClips();
    auto Skeleton = std::make_shared<SkeletonData>(MakeSkeleton());
    auto State    = std::make_shared<AnimStateData>(MakeState());

    AnimInstance Instance;
    Instance.SetSkeleton(Skeleton);
    Instance.SetState(State);
    Expect(Instance.GetLayerCount() == 2 && std::string(Instance.GetCurrentStateName(0)) == "Locomotion"
           && std::string(Instance.GetCurrentStateName(1)) == "Wave", "each layer starts in its entry state");

    /* Speed 0: Idle alone -- the arm straight. */
    Instance.SetTime(1.0f);
    Expect(Near(Instance.GetModelPose()[2].M[0][3], 150.0f), "at Speed 0 the blend space is its Idle sample");

    /* Speed 100: Bend alone; at its end the hand is at (100, 50). */
    Instance.SetParameter("Speed", 100.0f);
    Instance.SetTime(1.0f);
    Expect(Near(Instance.GetModelPose()[2].M[0][3], 100.0f) && Near(Instance.GetModelPose()[2].M[1][3], 50.0f),
           "at Speed 100 it is its Bend sample");

    /* Speed 50: half of each -- 45 degrees. */
    Instance.SetParameter("Speed", 50.0f);
    Instance.SetTime(1.0f);
    Expect(Near(Instance.GetModelPose()[2].M[1][3], 50.0f * std::sin(0.7853982f), 0.05f), "between, it blends by distance");

    /* The upper layer, masked to the hand: nothing at Armed 0, all at 1. */
    Instance.SetParameter("Speed", 0.0f);
    Instance.SetTime(0.0f);
    Instance.SetParameter("Armed", 1.0f);
    Instance.Update(1.0f - 1.0f / 60.0f);   // the wave nearly at its 90 degrees
    const AnimMatrix& Hand = Instance.GetModelPose()[2];
    Expect(Near(Hand.M[0][3], 150.0f) && Hand.M[1][0] > 0.95f, "a masked layer turns the hand and leaves the arm");
    Instance.SetParameter("Armed", 0.0f);
    Instance.Evaluate();
    Expect(Near(Instance.GetModelPose()[2].M[1][0], 0.0f), "its weight parameter at 0 turns it off");

    /* A trigger: any state -> Jump at once, the trigger spent. */
    Instance.SetParameter("Jump", 1.0f);
    Instance.Update(0.01f);
    Expect(std::string(Instance.GetCurrentStateName(0)) == "Jump" && Instance.GetConsumedTriggers().size() == 1
           && Instance.GetParameter("Jump") == 0.0f, "a trigger fires its transition and is spent");
    Instance.Update(0.5f);
    Expect(std::string(Instance.GetCurrentStateName(0)) == "Jump", "a one-shot plays out");
    Instance.Update(0.45f);
    Expect(std::string(Instance.GetCurrentStateName(0)) == "Locomotion" && Instance.IsInTransition(0),
           "at its end it returns, crossfading");
    Instance.Update(0.2f);
    Expect(!Instance.IsInTransition(0), "the crossfade finishes after its duration");
}

static void TestStateSystem()
{
    std::printf("[anim state on a component]\n");
    WriteStateClips();
    AnimStateData State = MakeState();
    const std::string StatePath = std::string(SCRATCH) + "Arm.nmanimstate";
    Expect(SaveAnimStateFile(StatePath, State), "the anim state is written");
    AnimStateAssetCache::Invalidate("");

    GameCore::World World;
    AnimationSystem* System = AnimationSystem::Attach(World);
    GameCore::Entity Actor = World.CreateEntity();
    Actor.AddComponent(GameCore::TransformComponent());
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%sArm.nmmesh", SCRATCH);
    Actor.AddComponent(Render);
    AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%s", StatePath.c_str());
    Actor.AddComponent(Animation);

    World.Tick(0.1f);
    Expect(System->FindInstance(Actor) && std::string(System->FindInstance(Actor)->GetCurrentStateName(0)) == "Locomotion",
           "a component naming an .nmanimstate runs it");
    Actor.GetComponent<AnimationComponent>()->SetTrigger("Jump");
    World.Tick(0.1f);
    Expect(std::string(System->FindInstance(Actor)->GetCurrentStateName(0)) == "Jump", "the component's parameters drive it");
    Expect(Actor.GetComponent<AnimationComponent>()->GetFloat("Jump", -1.0f) == 0.0f, "and a spent trigger is cleared on the component");

    int Ended = 0;
    World.GetEvents().Subscribe("AnimEnded", [](GameCore::EventID, const GameCore::EventArgs& InArgs, void* InUserData)
    {
        if(InArgs.Count >= 2 && std::strcmp(InArgs.Args[1].String, "Jump") == 0) { ++*static_cast<int*>(InUserData); }
    }, &Ended);
    for(int Step = 0; Step < 12; ++Step) { World.Tick(0.1f); }
    Expect(Ended == 1, "a one-shot state emits AnimEnded once, as it ends");
    AnimationSystem::Detach(World);
}

static void TestSocket()
{
    std::printf("[socket]\n");
    GameCore::World World;
    AnimationSystem* System = AnimationSystem::Attach(World);
    GameCore::Entity Actor = World.CreateEntity();
    Actor.AddComponent(GameCore::TransformComponent());
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%sArm.nmmesh", SCRATCH);
    Actor.AddComponent(Render);
    AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%sArm_Bend.nmanim", SCRATCH);
    Actor.AddComponent(Animation);

    GameCore::Entity Sword = World.CreateEntity();
    Sword.AddComponent(GameCore::TransformComponent());
    Sword.SetParent(Actor, false);
    AnimSocketComponent Socket;
    std::snprintf(Socket.Bone, sizeof(Socket.Bone), "Hand");
    Sword.AddComponent(Socket);

    /* At the clip's end the hand is at (100, 50), turned 90 degrees about Z. */
    World.Tick(0.999f);
    const GameCore::TransformComponent* Held = Sword.GetComponent<GameCore::TransformComponent>();
    Expect(std::fabs(Held->Position.GetX() - 100.0) < 0.5 && std::fabs(Held->Position.GetY() - 50.0) < 0.5,
           "a socket puts its entity on its bone");
    Expect(std::fabs(Held->Rotation.GetZ() - 90.0) < 1.0, "turned as the bone is");

    Sword.GetComponent<AnimSocketComponent>()->PositionOffset = EngineCore::Vector3(10.0, 0.0, 0.0);
    World.Tick(0.0f);
    Expect(std::fabs(Sword.GetComponent<GameCore::TransformComponent>()->Position.GetY() - 60.0) < 0.5,
           "its offset is in the bone's frame");
    (void)System;
    AnimationSystem::Detach(World);
}

static void TestNotifyEffects()
{
    std::printf("[notify effects]\n");
    AnimClipData Clip = MakeClip();
    Clip.Notifies = { { "Puff", 0.5f, std::string(SCRATCH) + "Puff.nmparticle", "Hand" } };
    const std::string Path = std::string(SCRATCH) + "Arm_Puff.nmanim";
    SaveAnimClipFile(Path, Clip);
    AnimClipData Back;
    Expect(LoadAnimClipFile(Path, Back) && Back.Notifies.size() == 1 && Back.Notifies[0].Bone == "Hand"
           && Back.Notifies[0].ParticleSystem == std::string(SCRATCH) + "Puff.nmparticle", "a notify keeps its particle system and bone");
    AnimClipAssetCache::Invalidate(Path);

    GameCore::World World;
    AnimationSystem::Attach(World);
    GameCore::Entity Actor = World.CreateEntity();
    GameCore::TransformComponent Transform;
    Transform.Position = EngineCore::Vector3(0.0, 0.0, 500.0);
    Actor.AddComponent(Transform);
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%sArm.nmmesh", SCRATCH);
    Actor.AddComponent(Render);
    AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%s", Path.c_str());
    Actor.AddComponent(Animation);

    World.Tick(0.6f);
    const GameCore::EntityID Effect = World.FindEntityByName("AnimEffect");
    const GameCore::TransformComponent* Placed = World.GetEntity(Effect).GetComponent<GameCore::TransformComponent>();
    Expect(Effect.IsSet() && World.GetEntity(Effect).HasComponent<ParticleSystemComponent>(), "a notify with a particle system starts it");
    Expect(Placed && std::fabs(Placed->Position.GetZ() - 500.0) < 0.5 && Placed->Position.GetX() > 50.0,
           "at its bone, in the world");
    for(int Step = 0; Step < 40; ++Step) { World.Tick(0.1f); }
    Expect(!World.IsAlive(Effect), "and it goes once its particles are done");
    AnimationSystem::Detach(World);

    /* An older clip: notifies as names and times only (ANTF v1) still read. */
    AnimClipData Plain = MakeClip();
    EngineCore::Archive Out = EngineCore::Archive::ForWriting();
    Expect(SerializeAnimClip(Out, Plain), "a clip without effects saves");
}

/* An animated arm at InPosition in InWorld. */
static GameCore::Entity SpawnArm(GameCore::World& InWorld, const EngineCore::Vector3& InPosition)
{
    GameCore::Entity Actor = InWorld.CreateEntity();
    GameCore::TransformComponent Transform;
    Transform.Position = InPosition;
    Actor.AddComponent(Transform);
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%sArm.nmmesh", SCRATCH);
    Actor.AddComponent(Render);
    AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%sArm_Bend.nmanim", SCRATCH);
    Actor.AddComponent(Animation);
    return Actor;
}

static void TestBudget()
{
    std::printf("[budget]\n");
    GameCore::World World;
    AnimationSystem* System = AnimationSystem::Attach(World);
    const GameCore::Entity Close   = SpawnArm(World, EngineCore::Vector3(0.0, 0.0, 100.0));
    const GameCore::Entity Middle  = SpawnArm(World, EngineCore::Vector3(0.0, 0.0, 5000.0));    // past 3000: every 2nd tick
    const GameCore::Entity Distant = SpawnArm(World, EngineCore::Vector3(0.0, 0.0, 30000.0));   // past 20000: no pose

    World.Tick(0.1f);
    Expect(System->GetLastStats().Updated == 3, "with no view -- a server -- everything updates");

    System->AddViewOrigin(1, EngineCore::Vector3());
    World.Tick(0.1f);
    const AnimationSystem::Stats First = System->GetLastStats();
    World.Tick(0.1f);
    const AnimationSystem::Stats Second = System->GetLastStats();
    Expect(First.Skipped == 1 && Second.Reduced == 1, "past FullRateDistance, an update every other tick");
    Expect(First.Frozen == 1 && Second.Frozen == 1, "past FreezeDistance, no pose");
    Expect(Near(System->FindInstance(Close)->GetTime(), 0.3f) && Near(System->FindInstance(Middle)->GetTime(), 0.3f),
           "a reduced one's time is saved up, not lost");
    const float FrozenAngle = System->FindInstance(Distant)->GetModelPose()[2].M[1][3];
    World.Tick(0.3f);
    Expect(Near(System->FindInstance(Distant)->GetTime(), 0.6f) && Near(System->FindInstance(Distant)->GetModelPose()[2].M[1][3], FrozenAngle),
           "a frozen one's time passes while its pose holds");
    AnimationSystem::Detach(World);

    /* Enough of them, and they update across the job system -- to the same result. */
    EngineCore::JobSystem Jobs(2);
    EngineCore::JobSystem* Before = EngineCore::GetJobSystem();
    EngineCore::SetJobSystem(&Jobs);
    GameCore::World Crowd;
    AnimationSystem* CrowdSystem = AnimationSystem::Attach(Crowd);
    std::vector<GameCore::Entity> Arms;
    for(int Index = 0; Index < 24; ++Index) { Arms.push_back(SpawnArm(Crowd, EngineCore::Vector3(double(Index) * 10.0, 0.0, 0.0))); }
    Crowd.Tick(0.5f);   // half way: the arm at 45 degrees
    bool bAllPosed = true;
    for(const GameCore::Entity& Arm : Arms)
    {
        bAllPosed = bAllPosed && Near(CrowdSystem->FindInstance(Arm)->GetModelPose()[2].M[1][3], 50.0f * std::sin(0.7853982f), 0.5f);
    }
    Expect(CrowdSystem->GetLastStats().bParallel && bAllPosed, "a crowd updates in parallel, every pose as alone");
    AnimationSystem::Detach(Crowd);
    EngineCore::SetJobSystem(Before);
}

/* The root walks 100 along X in one second, looping. */
static AnimClipData MakeStride(const EAnimRootMotion InRootMotion)
{
    AnimClipData Clip;
    Clip.SampleRate = 30.0f;
    Clip.FrameCount = 31;
    Clip.bLoop      = true;
    Clip.RootBone   = "Root";
    Clip.RootMotion = InRootMotion;
    AnimTrack Track;
    Track.Bone = "Root";
    for(uint32_t Frame = 0; Frame < Clip.FrameCount; ++Frame) { Track.Translations.push_back({ 100.0f * float(Frame) / 30.0f, 0.0f, 0.0f }); }
    Track.Rotations.push_back(AnimQuat());
    Track.Scales.push_back({ 1.0f, 1.0f, 1.0f });
    Clip.Tracks.push_back(Track);
    return Clip;
}

static void TestRootMotion()
{
    std::printf("[root motion]\n");
    auto Skeleton = std::make_shared<SkeletonData>(MakeSkeleton());

    AnimInstance Extracted;
    Extracted.SetSkeleton(Skeleton);
    Extracted.PlayClip(std::make_shared<AnimClipData>(MakeStride(EAnimRootMotion::Extract)));
    Extracted.Update(0.5f);
    Expect(Near(Extracted.GetRootMotion().X, 50.0f, 0.5f) && Near(Extracted.GetRootMotion().Z, 0.0f), "Extract hands out the root's travel");
    Expect(Near(Extracted.GetModelPose()[0].M[0][3], 0.0f), "and holds the root where it started");
    Extracted.Update(0.7f);
    Expect(Near(Extracted.GetRootMotion().X, 70.0f, 0.5f), "across the loop's end too");

    AnimInstance Held;
    Held.SetSkeleton(Skeleton);
    Held.PlayClip(std::make_shared<AnimClipData>(MakeStride(EAnimRootMotion::InPlace)));
    Held.Update(0.5f);
    Expect(Near(Held.GetRootMotion().X, 0.0f) && Near(Held.GetModelPose()[0].M[0][3], 0.0f), "In Place holds it and moves nothing");

    AnimInstance Kept;
    Kept.SetSkeleton(Skeleton);
    Kept.PlayClip(std::make_shared<AnimClipData>(MakeStride(EAnimRootMotion::Keep)));
    Kept.Update(0.5f);
    Expect(Near(Kept.GetModelPose()[0].M[0][3], 50.0f, 0.5f), "Keep plays it as authored");

    /* On a component: the entity walks, along its own (turned) X. */
    AnimClipData Stride = MakeStride(EAnimRootMotion::Extract);
    const std::string Path = std::string(SCRATCH) + "Stride.nmanim";
    SaveAnimClipFile(Path, Stride);
    AnimClipAssetCache::Invalidate(Path);
    GameCore::World World;
    AnimationSystem::Attach(World);
    GameCore::Entity Walker = World.CreateEntity();
    GameCore::TransformComponent Transform;
    Transform.Rotation = EngineCore::Vector3(0.0, 90.0, 0.0);
    Walker.AddComponent(Transform);
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%sArm.nmmesh", SCRATCH);
    Walker.AddComponent(Render);
    AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%s", Path.c_str());
    Walker.AddComponent(Animation);
    World.Tick(0.5f);
    const EngineCore::Vector3 Moved = Walker.GetComponent<GameCore::TransformComponent>()->Position;
    Expect(std::fabs(std::hypot(Moved.GetX(), Moved.GetZ()) - 50.0) < 1.0 && std::fabs(Moved.GetX()) < 1.0,
           "root motion moves the entity, turned as it is");
    AnimationSystem::Detach(World);
}

/* A dedicated server: a headless session's renderer is the Null backend
 * (no GPU, no window), and binding its world brings animation with it --
 * the server has poses for sockets and bone queries. */
static void TestHeadless()
{
    std::printf("[headless]\n");
    Renderer Rend;
    Expect(Rend.Initialize(ERenderBackend::Null, EngineCore::WindowHandle()), "the Null renderer starts with no window");

    GameCore::World World;
    Rend.BindWorld(World);
    Expect(AnimationSystem::Find(&World) != nullptr, "binding a world attaches its animation, on the Null backend too");

    GameCore::Entity Actor = World.CreateEntity();
    Actor.AddComponent(GameCore::TransformComponent());
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%sArm.nmmesh", SCRATCH);
    Actor.AddComponent(Render);
    AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%sArm_Bend.nmanim", SCRATCH);
    Actor.AddComponent(Animation);

    World.Tick(0.999f);
    FrameInfo Frame;
    Frame.FrameNumber = 1;
    Frame.DeltaSeconds = 0.999;
    Rend.EndFrame(Frame);
    GameCore::TransformPose Hand;
    const AnimationSystem* System = AnimationSystem::Find(&World);
    Expect(System && System->GetBoneWorldPose(Actor, "Hand", Hand) && std::fabs(Hand.Position.GetY() - 50.0) < 0.5,
           "and a tick poses it, as a client would");

    Rend.UnbindWorld();
    Expect(AnimationSystem::Find(&World) == nullptr, "unbinding detaches it");
    Rend.Shutdown();
}

int main()
{
    std::printf("RenderCore animation tests\n");
    TestPoseMath();
    TestInstance();
    TestSystem();
    TestStateAsset();
    TestStateMachine();
    TestStateSystem();
    TestSocket();
    TestRootMotion();
    TestNotifyEffects();
    TestBudget();
    TestHeadless();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
