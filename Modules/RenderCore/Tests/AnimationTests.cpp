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
#include "Public/RenderCore/RenderView.h"
#include "Private/RenderCore/RenderComponentProxy.h"

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

static void TestTrim()
{
    std::printf("[trim]\n");
    AnimClipData Clip = MakeClip();   // 31 frames: the arm 0 -> 90 degrees; a notify at 0.5 s
    Clip.Notifies.push_back({ "Late", 0.9f });
    Expect(!TrimClip(Clip, 10, 40) && !TrimClip(Clip, 20, 10) && Clip.FrameCount == 31, "a range it does not hold is refused");
    Expect(TrimClip(Clip, 15, 24), "frames 15..24 are kept");
    Expect(Clip.FrameCount == 10 && Clip.Tracks[0].Rotations.size() == 10 && Clip.Tracks[0].Translations.size() == 1,
           "ten frames, each per-frame track sliced, a one-key track kept");
    Expect(Near(Clip.Tracks[0].Rotations[0].Z, std::sin(0.5f * 1.5707963f * 15.0f / 30.0f)), "the first frame is the old fifteenth");
    Expect(Clip.Notifies.size() == 1 && Near(Clip.Notifies[0].Time, 0.0f), "the 0.5 s notify now starts it; the 0.9 s one, past the end, is gone");
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

/* SetOverlayClip: a second clip blended over the first by a weight (the Sequencer's crossfade). */
static void TestOverlay()
{
    std::printf("[overlay]\n");
    auto Skeleton = std::make_shared<SkeletonData>(MakeSkeleton());
    auto Clip     = std::make_shared<AnimClipData>(MakeClip());

    AnimInstance Instance;
    Instance.SetSkeleton(Skeleton);
    Instance.PlayClip(Clip);
    Instance.SetTime(0.0f);
    Expect(Near(Instance.GetModelPose()[2].M[1][3], 0.0f) && !Instance.HasOverlayClip(), "the base clip at 0: the arm along X");

    Instance.SetOverlayClip(Clip, 1.0f, 1.0f);
    Instance.Evaluate();
    Expect(Instance.HasOverlayClip() && Near(Instance.GetModelPose()[2].M[1][3], 50.0f), "a whole overlay is its own pose");

    Instance.SetOverlayClip(Clip, 1.0f, 0.5f);
    Instance.Evaluate();
    const float Half = Instance.GetModelPose()[2].M[1][3];
    Expect(Half > 10.0f && Half < 40.0f, "half an overlay is between the two");

    Instance.SetOverlayClip(nullptr, 0.0f, 0.0f);
    Instance.Evaluate();
    Expect(!Instance.HasOverlayClip() && Near(Instance.GetModelPose()[2].M[1][3], 0.0f), "cleared, the base clip alone");
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

/* A 2D blend space on X and Y: Idle at the centre, Bend at +X, its mirror
 * at -X, Wave (the hand) at +Y. */
static void TestBlendSpace2D()
{
    std::printf("[2D blend space]\n");
    WriteStateClips();
    AnimClipData Back = MakeTurn("Arm", 100.0f, -90.0f, true);
    SaveAnimClipFile(std::string(SCRATCH) + "BendBack.nmanim", Back);
    AnimClipAssetCache::Invalidate("");

    AnimStateData Data;
    Data.Parameters = { { "X", EAnimParameterType::Float, 0.0f }, { "Y", EAnimParameterType::Float, 0.0f } };
    AnimLayer Layer;
    Layer.Name = "Base";
    AnimStateNode Move;
    Move.Name            = "Move";
    Move.BlendParameter  = "X";
    Move.BlendParameterY = "Y";
    Move.Samples = { { std::string(SCRATCH) + "Idle.nmanim", 0.0f, 0.0f }, { std::string(SCRATCH) + "Bend.nmanim", 1.0f, 0.0f },
                     { std::string(SCRATCH) + "BendBack.nmanim", -1.0f, 0.0f }, { std::string(SCRATCH) + "Wave.nmanim", 0.0f, 1.0f } };
    Layer.States.push_back(Move);
    Data.Layers.push_back(Layer);

    EngineCore::Archive Out = EngineCore::Archive::ForWriting();
    Expect(SerializeAnimState(Out, Data), "it saves");
    EngineCore::Archive In = EngineCore::Archive::ForReading(Out.GetBytes());
    AnimStateData Read;
    Expect(SerializeAnimState(In, Read) && Read.Layers[0].States[0].BlendParameterY == "Y"
           && Near(Read.Layers[0].States[0].Samples[3].PositionY, 1.0f), "the second parameter and each sample's Y read back");

    AnimInstance Instance;
    Instance.SetSkeleton(std::make_shared<SkeletonData>(MakeSkeleton()));
    Instance.SetState(std::make_shared<AnimStateData>(Data));
    const auto At = [&Instance](const float InX, const float InY)
    {
        Instance.SetParameter("X", InX);
        Instance.SetParameter("Y", InY);
        Instance.SetTime(1.0f);
        return Instance.GetModelPose()[2];
    };
    Expect(Near(At(0.0f, 0.0f).M[0][3], 150.0f), "at the centre: Idle alone");
    const AnimMatrix Right = At(1.0f, 0.0f);
    Expect(Near(Right.M[0][3], 100.0f) && Near(Right.M[1][3], 50.0f), "at +X: Bend alone");
    const AnimMatrix Left = At(-1.0f, 0.0f);
    Expect(Near(Left.M[0][3], 100.0f) && Near(Left.M[1][3], -50.0f), "at -X: its mirror");
    const AnimMatrix Up = At(0.0f, 1.0f);
    Expect(Near(Up.M[0][3], 150.0f) && Up.M[1][0] > 0.95f, "at +Y: Wave, the arm straight and the hand turned");
    Expect(Near(At(0.5f, 0.0f).M[1][3], 50.0f * std::sin(0.7853982f), 0.05f), "half way to +X: half of each, as in 1D");
    const AnimMatrix Far = At(3.0f, 0.0f);
    Expect(Near(Far.M[0][3], 100.0f) && Near(Far.M[1][3], 50.0f), "past the last sample: that sample");
    const AnimMatrix Corner = At(0.5f, 0.5f);
    Expect(Corner.M[1][3] > 1.0f && Corner.M[1][3] < 35.0f && Corner.M[1][0] > 0.05f, "between +X and +Y: some of both");
}

/* An additive layer over a base that holds the hand at 45 degrees: Wave
 * (the hand turning 0 -> 90) adds its turn to that, where an override
 * layer would replace it. */
static void TestAdditiveLayer()
{
    std::printf("[additive layer]\n");
    WriteStateClips();
    AnimClipData Held = MakeTurn("Hand", 50.0f, 0.0f, true);
    Held.Tracks[0].Rotations = { { 0.0f, 0.0f, std::sin(0.3926991f), std::cos(0.3926991f) } };   // 45 degrees, every frame
    SaveAnimClipFile(std::string(SCRATCH) + "HandHeld.nmanim", Held);
    AnimClipAssetCache::Invalidate("");

    const auto Make = [](const EAnimLayerBlend InBlend, const EAnimAdditiveBase InBase, const std::string& InClip)
    {
        AnimStateData Data;
        Data.Parameters = { { "Add", EAnimParameterType::Float, 1.0f } };
        AnimLayer Base;
        Base.Name = "Base";
        AnimStateNode Hold;
        Hold.Name = "Hold";
        Hold.Samples = { { std::string(SCRATCH) + "HandHeld.nmanim", 0.0f, 0.0f } };
        Base.States.push_back(Hold);
        AnimLayer Over;
        Over.Name            = "Over";
        Over.Blend           = InBlend;
        Over.AdditiveBase    = InBase;
        Over.WeightParameter = "Add";
        AnimStateNode Play;
        Play.Name = "Play";
        Play.Samples = { { std::string(SCRATCH) + InClip, 0.0f, 0.0f } };
        Over.States.push_back(Play);
        Data.Layers = { Base, Over };
        return Data;
    };
    /* The hand's angle in the model, in degrees (the arm stays straight). */
    const auto HandAngle = [](const AnimStateData& InData, const float InTime, const float InAdd)
    {
        AnimInstance Instance;
        Instance.SetSkeleton(std::make_shared<SkeletonData>(MakeSkeleton()));
        Instance.SetState(std::make_shared<AnimStateData>(InData));
        Instance.SetParameter("Add", InAdd);
        Instance.Update(InTime);   // every layer -- SetTime moves only the first
        const AnimMatrix& Hand = Instance.GetModelPose()[2];
        return std::atan2(Hand.M[1][0], Hand.M[0][0]) * 57.29578f;
    };

    const AnimStateData Additive = Make(EAnimLayerBlend::Additive, EAnimAdditiveBase::FirstFrame, "Wave.nmanim");
    EngineCore::Archive Out = EngineCore::Archive::ForWriting();
    Expect(SerializeAnimState(Out, const_cast<AnimStateData&>(Additive)), "it saves");
    EngineCore::Archive In = EngineCore::Archive::ForReading(Out.GetBytes());
    AnimStateData Read;
    Expect(SerializeAnimState(In, Read) && Read.Layers[1].Blend == EAnimLayerBlend::Additive
           && Read.Layers[1].AdditiveBase == EAnimAdditiveBase::FirstFrame, "a layer's blend and base read back");

    Expect(Near(HandAngle(Additive, 0.0f, 1.0f), 45.0f, 0.1f), "at the clip's first frame it adds nothing");
    /* At 0.75 s Wave has turned 67.5 degrees. */
    Expect(Near(HandAngle(Additive, 0.75f, 1.0f), 112.5f, 0.1f), "later, its 67.5 degrees on the base's 45");
    Expect(Near(HandAngle(Additive, 0.75f, 0.5f), 78.75f, 0.1f), "at half weight, half of it");
    Expect(Near(HandAngle(Make(EAnimLayerBlend::Override, EAnimAdditiveBase::FirstFrame, "Wave.nmanim"), 0.75f, 1.0f), 67.5f, 0.1f),
           "an override layer replaces the base instead");

    /* A clip that only holds 45 degrees: nothing, from its first frame; 45, from the reference pose. */
    Expect(Near(HandAngle(Make(EAnimLayerBlend::Additive, EAnimAdditiveBase::FirstFrame, "HandHeld.nmanim"), 0.5f, 1.0f), 45.0f, 0.1f),
           "a still clip, from its first frame, adds nothing");
    Expect(Near(HandAngle(Make(EAnimLayerBlend::Additive, EAnimAdditiveBase::ReferencePose, "HandHeld.nmanim"), 0.5f, 1.0f), 90.0f, 0.1f),
           "from the reference pose, it adds its 45");
}

/*
 * Sync groups. Base: "Walk" (Bend: the arm turning 90 over 1 s) and "Stride"
 * (Bend again), both in group "Loco", and "Rest" (Idle) in none; Alt and
 * Rest switch between them at once. Upper, masked to the hand: "Swing", the
 * hand turning 90 over 2 s, in "Loco" -- half the base's pace unless synced.
 */
static void TestSyncGroups()
{
    std::printf("[sync groups]\n");
    WriteStateClips();
    AnimClipData Slow;
    Slow.SampleRate = 30.0f;
    Slow.FrameCount = 61;
    AnimTrack Track;
    Track.Bone = "Hand";
    Track.Translations.push_back({ 50.0f, 0.0f, 0.0f });
    Track.Scales.push_back({ 1.0f, 1.0f, 1.0f });
    for(uint32_t Frame = 0; Frame < Slow.FrameCount; ++Frame)
    {
        const float Angle = 0.5f * 1.5707963f * float(Frame) / 60.0f;
        Track.Rotations.push_back({ 0.0f, 0.0f, std::sin(Angle), std::cos(Angle) });
    }
    Slow.Tracks.push_back(Track);
    Slow.Notifies.push_back({ "Swung", 1.2f });   // phase 0.6
    SaveAnimClipFile(std::string(SCRATCH) + "SwingSlow.nmanim", Slow);
    AnimClipAssetCache::Invalidate("");

    const auto Make = [](const std::string& InGroup)
    {
        AnimStateData Data;
        Data.Parameters = { { "Alt", EAnimParameterType::Bool, 0.0f }, { "Rest", EAnimParameterType::Bool, 0.0f } };
        AnimLayer Base;
        Base.Name = "Base";
        AnimStateNode Walk;
        Walk.Name      = "Walk";
        Walk.SyncGroup = InGroup;
        Walk.Samples   = { { std::string(SCRATCH) + "Bend.nmanim", 0.0f, 0.0f } };
        AnimStateNode Stride = Walk;
        Stride.Name = "Stride";
        AnimStateNode Rest;
        Rest.Name    = "Rest";
        Rest.Samples = { { std::string(SCRATCH) + "Bend.nmanim", 0.0f, 0.0f } };
        Base.States = { Walk, Stride, Rest };
        AnimTransition ToStride;
        ToStride.From = 0;
        ToStride.To   = 1;
        ToStride.Duration = 0.0f;
        ToStride.Conditions.push_back({ "Alt", EAnimCompare::IsTrue, 0.0f });
        AnimTransition ToRest;
        ToRest.From = -1;
        ToRest.To   = 2;
        ToRest.Duration = 0.0f;
        ToRest.Conditions.push_back({ "Rest", EAnimCompare::IsTrue, 0.0f });
        Base.Transitions = { ToStride, ToRest };
        AnimLayer Upper;
        Upper.Name      = "Upper";
        Upper.MaskBones = { "Hand" };
        AnimStateNode Swing;
        Swing.Name      = "Swing";
        Swing.SyncGroup = InGroup;
        Swing.Samples   = { { std::string(SCRATCH) + "SwingSlow.nmanim", 0.0f, 0.0f } };
        Upper.States = { Swing };
        Data.Layers = { Base, Upper };
        return Data;
    };
    /* The hand's angle from the arm's, in degrees. */
    const auto HandTurn = [](const AnimInstance& InInstance)
    {
        const AnimMatrix& Arm  = InInstance.GetModelPose()[1];
        const AnimMatrix& Hand = InInstance.GetModelPose()[2];
        return (std::atan2(Hand.M[1][0], Hand.M[0][0]) - std::atan2(Arm.M[1][0], Arm.M[0][0])) * 57.29578f;
    };

    AnimStateData Synced = Make("Loco");
    EngineCore::Archive Out = EngineCore::Archive::ForWriting();
    Expect(SerializeAnimState(Out, Synced), "it saves");
    EngineCore::Archive In = EngineCore::Archive::ForReading(Out.GetBytes());
    AnimStateData Read;
    Expect(SerializeAnimState(In, Read) && Read.Layers[0].States[0].SyncGroup == "Loco" && Read.Layers[0].States[2].SyncGroup.empty()
           && Read.Layers[1].States[0].SyncGroup == "Loco", "a state's sync group reads back");

    AnimInstance Free;
    Free.SetSkeleton(std::make_shared<SkeletonData>(MakeSkeleton()));
    Free.SetState(std::make_shared<AnimStateData>(Make("")));
    Free.Update(0.5f);
    Expect(Near(HandTurn(Free), 22.5f, 0.2f), "unsynced, each layer keeps its own pace: the 2 s swing a quarter through");

    AnimInstance Instance;
    Instance.SetSkeleton(std::make_shared<SkeletonData>(MakeSkeleton()));
    Instance.SetState(std::make_shared<AnimStateData>(Synced));
    Instance.Update(0.5f);
    Expect(Near(HandTurn(Instance), 45.0f, 0.2f), "synced, the later layer's state takes the leader's phase: half through");
    Expect(Instance.GetFiredNotifies().empty(), "locking on to the leader passes no notifies");
    Instance.Update(0.25f);
    Expect(Near(HandTurn(Instance), 67.5f, 0.2f), "and keeps to it");
    Expect(Instance.GetFiredNotifies().size() == 1 && Instance.GetFiredNotifies()[0].Name == "Swung",
           "following, it fires the notifies it passes");
    Instance.Update(0.5f);
    Expect(Near(HandTurn(Instance), 22.5f, 0.2f), "it wraps when the leader does");

    /* Phase 0.25 now. Into Stride, the same group: it goes on from there. */
    Instance.SetParameter("Alt", 1.0f);
    Instance.Update(0.0f);
    Instance.Update(0.25f);
    Expect(std::string(Instance.GetCurrentStateName(0)) == "Stride" && Near(Instance.GetTime(), 0.5f, 0.02f),
           "a transition within a sync group starts where the state it left was");
    Instance.SetParameter("Rest", 1.0f);
    Instance.Update(0.0f);
    Expect(std::string(Instance.GetCurrentStateName(0)) == "Rest" && Near(Instance.GetTime(), 0.0f),
           "one out of the group starts from the beginning");
    Instance.Update(0.3f);
    Expect(Near(HandTurn(Instance), 45.0f + 0.3f * 45.0f, 0.3f), "with no leader in its group, a follower plays on at its own pace");
}

static void TestLookAt()
{
    std::printf("[look-at]\n");
    /* No clips: the reference pose, so only the look-at turns anything. */
    AnimStateData Data;
    Data.Parameters = { { "Pitch", EAnimParameterType::Float, 0.0f }, { "Armed", EAnimParameterType::Bool, 1.0f } };
    AnimLayer Base;
    Base.Name = "Base";
    AnimStateNode Still;
    Still.Name = "Still";
    Base.States = { Still };
    Data.Layers = { Base };
    Data.LookAt.Parameter       = "Pitch";
    Data.LookAt.WeightParameter = "Armed";
    Data.LookAt.MinAngle = -30.0f;
    Data.LookAt.MaxAngle =  60.0f;
    Data.LookAt.Axis[0] = 0.0f;
    Data.LookAt.Axis[2] = 1.0f;
    Data.LookAt.Response = 0.0f;
    Data.LookAt.Bones = { { "Hand", 0.5f }, { "Arm", 0.5f } };   // out of order: turned root first anyway

    EngineCore::Archive Out = EngineCore::Archive::ForWriting();
    Expect(SerializeAnimState(Out, Data), "it saves");
    EngineCore::Archive In = EngineCore::Archive::ForReading(Out.GetBytes());
    AnimStateData Read;
    Expect(SerializeAnimState(In, Read) && Read.LookAt.Parameter == "Pitch" && Read.LookAt.WeightParameter == "Armed"
           && Read.LookAt.Bones.size() == 2 && Read.LookAt.Bones[1].Bone == "Arm" && Near(Read.LookAt.MaxAngle, 60.0f)
           && Near(Read.LookAt.Axis[2], 1.0f), "the look-at reads back");
    AnimStateData Reversed = Data;
    Reversed.LookAt.MinAngle = 90.0f;
    EngineCore::Archive Refused = EngineCore::Archive::ForWriting();
    Expect(!SerializeAnimState(Refused, Reversed), "a reversed range is refused");

    /* Model-space angles of a bone's X axis about Z, degrees. */
    const auto Turn = [](const AnimInstance& InInstance, const size_t InBone)
    {
        const AnimMatrix& Bone = InInstance.GetModelPose()[InBone];
        return std::atan2(Bone.M[1][0], Bone.M[0][0]) * 57.29578f;
    };
    AnimInstance Instance;
    Instance.SetSkeleton(std::make_shared<SkeletonData>(MakeSkeleton()));
    Instance.SetState(std::make_shared<AnimStateData>(Data));
    Instance.SetParameter("Pitch", 40.0f);
    Instance.Update(0.1f);
    Expect(Near(Turn(Instance, 1), 20.0f, 0.05f) && Near(Turn(Instance, 2), 40.0f, 0.05f),
           "each bone takes its share, and the turns add up along the chain");
    Expect(Near(Instance.GetModelPose()[2].M[1][3], 50.0f * std::sin(20.0f / 57.29578f), 0.05f), "a positive angle turns right-handed about the axis");
    Expect(Near(Instance.GetLookAngle(), 40.0f), "GetLookAngle says where it is");
    Instance.SetParameter("Pitch", 100.0f);
    Instance.Update(0.1f);
    Expect(Near(Turn(Instance, 2), 60.0f, 0.05f), "the angle is clamped to the range");
    Instance.SetParameter("Armed", 0.0f);
    Instance.Update(0.1f);
    Expect(Near(Turn(Instance, 2), 0.0f, 0.05f) && Near(Instance.GetModelPose()[2].M[0][3], 150.0f, 0.05f),
           "a weight of 0 leaves the pose as the layers made it");

    Data.LookAt.Response = 10.0f;
    AnimInstance Eased;
    Eased.SetSkeleton(std::make_shared<SkeletonData>(MakeSkeleton()));
    Eased.SetState(std::make_shared<AnimStateData>(Data));
    Eased.SetParameter("Pitch", 20.0f);
    Eased.Update(0.05f);
    Expect(Near(Eased.GetLookAngle(), 20.0f), "the first update starts it where it heads");
    Eased.SetParameter("Pitch", 40.0f);
    Eased.Update(0.05f);
    Expect(Near(Eased.GetLookAngle(), 20.0f + 20.0f * (1.0f - std::exp(-0.5f)), 0.01f), "after that it eases there");
    Eased.SetPlayRate(0.0f);
    Eased.Update(1.0f);
    Expect(Near(Eased.GetLookAngle(), 40.0f, 0.01f), "in real time: a play rate of 0 still looks");
    Eased.SetParameter("Pitch", -10.0f);
    Eased.Evaluate();
    Expect(Near(Eased.GetLookAngle(), -10.0f) && Near(Turn(Eased, 2), -10.0f, 0.05f), "Evaluate, after a change, goes straight there");

    Data.LookAt.Bones = { { "NoSuchBone", 1.0f } };
    AnimInstance Missing;
    Missing.SetSkeleton(std::make_shared<SkeletonData>(MakeSkeleton()));
    Missing.SetState(std::make_shared<AnimStateData>(Data));
    Missing.SetParameter("Pitch", 40.0f);
    Missing.Update(0.1f);
    Expect(Near(Missing.GetLookAngle(), 0.0f) && Near(Turn(Missing, 2), 0.0f, 0.05f), "bones the skeleton lacks: no look-at");
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

namespace
{
    struct HeardSound
    {
        int                 Count = 0;
        std::string         Sound;
        EngineCore::Vector3 At;
    };
}

static void TestNotifySounds()
{
    std::printf("[notify sounds]\n");
    AnimClipData Clip = MakeClip();
    AnimNotify Step;
    Step.Name  = "Step";
    Step.Time  = 0.5f;
    Step.Bone  = "Hand";
    Step.Sound = "Content/Sounds/A/Rather/Long/Path/To/Show/It/Is/Not/Cut/Short/Footstep_Gravel_Left.nmsound";
    Clip.Notifies = { Step, { "Quiet", 0.7f } };
    const std::string Path = std::string(SCRATCH) + "Arm_Step.nmanim";
    SaveAnimClipFile(Path, Clip);
    AnimClipData Back;
    Expect(LoadAnimClipFile(Path, Back) && Back.Notifies.size() == 2 && Back.Notifies[0].Sound == Step.Sound
           && Back.Notifies[1].Sound.empty(), "a notify keeps its sound (ANTF v3), and one without stays without");
    AnimClipAssetCache::Invalidate(Path);

    GameCore::World World;
    AnimationSystem* System = AnimationSystem::Attach(World);
    HeardSound Heard;
    System->SetSoundSink([](const char* InSound, const EngineCore::Vector3& InAt, void* InUserData)
    {
        auto& Into = *static_cast<HeardSound*>(InUserData);
        ++Into.Count;
        Into.Sound = InSound;
        Into.At    = InAt;
    }, &Heard);

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

    World.Tick(0.8f);
    Expect(Heard.Count == 1, "passing both notifies plays only the one with a sound");
    Expect(Heard.Sound == Step.Sound, "the whole path, however long");
    Expect(std::fabs(Heard.At.GetZ() - 500.0) < 0.5 && Heard.At.GetX() > 50.0, "at its bone, in the world");

    System->SetSoundSink(nullptr, nullptr);
    World.Tick(1.0f);
    Expect(Heard.Count == 1, "with no sink, a notify's sound is ignored");
    AnimationSystem::Detach(World);
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

    /* Visibility: a view that says what it sees -- one arm in front, one
     * behind, one far to the side, all near enough for full rate. */
    GameCore::World Seen;
    AnimationSystem* SeenSystem = AnimationSystem::Attach(Seen);
    const GameCore::Entity Front = SpawnArm(Seen, EngineCore::Vector3(0.0, 0.0, 300.0));
    const GameCore::Entity Back  = SpawnArm(Seen, EngineCore::Vector3(0.0, 0.0, -300.0));
    const GameCore::Entity Side  = SpawnArm(Seen, EngineCore::Vector3(2000.0, 0.0, 300.0));
    Seen.Tick(0.0f);   // posed: the bones are where they are
    const RenderView Camera = RenderView::MakePerspective(EngineCore::Size(256, 256), EngineCore::Vector3(), EngineCore::Vector3(0.0, 0.0, 1.0),
                                                          60.0f, 10.0f, 50000.0f);
    SeenSystem->AddView(1, Camera.Origin, Camera.Projection * Camera.View);
    Seen.Tick(0.1f);
    const AnimationSystem::Stats Glance = SeenSystem->GetLastStats();
    Expect(Glance.Offscreen == 2 && Glance.Skipped == 2 && Glance.Updated == 1,
           "behind the camera and off to the side: outside the view, skipped; in front: updated");
    Seen.Tick(0.1f);
    Back.GetComponent<GameCore::TransformComponent>()->Position = EngineCore::Vector3(0.0, 0.0, 600.0);   // into view
    Seen.Tick(0.1f);
    Expect(Near(SeenSystem->FindInstance(Back)->GetTime(), SeenSystem->FindInstance(Front)->GetTime()),
           "come into view, it catches up at once: the time it saved");
    Seen.Tick(0.1f);
    Expect(SeenSystem->GetLastStats().Reduced == 1 && Near(SeenSystem->FindInstance(Side)->GetTime(), SeenSystem->FindInstance(Front)->GetTime()),
           "still outside, every fourth tick (OffscreenSkipFrames 3), its time saved up");
    SeenSystem->AddViewOrigin(2, Camera.Origin);
    Seen.Tick(0.1f);
    Expect(SeenSystem->GetLastStats().Offscreen == 0, "a view by origin alone sees everything");
    AnimationSystem::Detach(Seen);

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
/* A skinned proxy's bounds follow its pose: culling and shadows see the
 * mesh where the animation has put it, not where it binds. */
static void TestPosedBounds()
{
    std::printf("[posed bounds]\n");
    RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%sArm.nmmesh", SCRATCH);
    RenderComponentProxy Proxy(GameCore::EntityID(), Render);
    const GameCore::AABB Bind = Proxy.GetLocalBounds();
    Expect(!Bind.IsEmpty() && Bind.Max.GetY() < 11.0 && Bind.Min.GetX() > 149.0, "unposed: the mesh's own box");

    /* The arm turned 90 degrees: the hand, and its triangle, at (100, 50). */
    const SkeletonData Skeleton = MakeSkeleton();
    const AnimClipData Clip = MakeClip();
    AnimClipBinding Binding;
    BindClip(Clip, Skeleton, Binding);
    AnimLocalPose Pose;
    GetRefPose(Skeleton, Pose);
    SampleClip(Clip, Binding, 1.0f, Pose);
    std::vector<AnimMatrix> Model;
    LocalToModel(Skeleton, Pose, Model);
    Proxy.SetSkinPose(Model.data(), uint32_t(Model.size()));
    const GameCore::AABB Posed = Proxy.GetLocalBounds();
    Expect(Posed.Min.GetX() <= 90.0 && Posed.Max.GetX() >= 100.0 && Posed.Min.GetY() <= 50.0 && Posed.Max.GetY() >= 60.0,
           "posed: the box holds the triangle where the hand has taken it");
    Expect(Posed.Max.GetX() < 150.0, "and no longer where it bound");

    Proxy.SetSkinPose(nullptr, 0);
    Expect(Proxy.GetLocalBounds().Max.GetY() < 11.0, "no pose: the mesh's box again");
}

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
    TestTrim();
    TestInstance();
    TestOverlay();
    TestSystem();
    TestStateAsset();
    TestStateMachine();
    TestBlendSpace2D();
    TestAdditiveLayer();
    TestSyncGroups();
    TestLookAt();
    TestStateSystem();
    TestSocket();
    TestRootMotion();
    TestNotifyEffects();
    TestNotifySounds();
    TestBudget();
    TestPosedBounds();
    TestHeadless();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
