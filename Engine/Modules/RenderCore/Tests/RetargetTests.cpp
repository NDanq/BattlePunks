/*
 * RenderCoreRetargetTests -- retargeting without a window (ANIMATION.md,
 * "Retargeting"): the .nmretarget asset, then clips made on a Mixamo-style
 * rig (bones along local X) played on a Rigify-style one (bones along local
 * Y, rolled, longer arms, a lower pelvis, a twist bone) -- and on the same
 * rig in an A pose. Both rigs are built here, from model-space placements.
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
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimPose.h"
#include "Public/RenderCore/AnimRetarget.h"
#include "Public/RenderCore/AnimStateAsset.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/HumanoidRig.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RetargetAsset.h"

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
static bool Near(const AnimVec3& A, const AnimVec3& B, const float Tolerance = 1e-3f)
{
    return Near(A.X, B.X, Tolerance) && Near(A.Y, B.Y, Tolerance) && Near(A.Z, B.Z, Tolerance);
}
static bool SameRotation(const AnimQuat& A, const AnimQuat& B, const float Tolerance = 1e-4f)
{
    return std::fabs(std::fabs(AnimMath::Dot(A, B)) - 1.0f) < Tolerance;
}

static const char* const SCRATCH = "RetargetTestScratch/";

// === Rigs ===================================================================

static AnimQuat AxisAngle(const AnimVec3& InAxis, const float InDegrees)
{
    const float Half = 0.5f * InDegrees * 3.14159265358979f / 180.0f;
    const float Sin  = std::sin(Half);
    return AnimMath::Normalize({ InAxis.X * Sin, InAxis.Y * Sin, InAxis.Z * Sin, std::cos(Half) });
}

/* The shortest turn from unit InFrom to unit InTo; opposite: half a turn about Z (from Y) or Y (else). */
static AnimQuat FromTo(const AnimVec3& InFrom, const AnimVec3& InTo)
{
    const AnimVec3 Cross{ InFrom.Y * InTo.Z - InFrom.Z * InTo.Y, InFrom.Z * InTo.X - InFrom.X * InTo.Z, InFrom.X * InTo.Y - InFrom.Y * InTo.X };
    const float    Dot = InFrom.X * InTo.X + InFrom.Y * InTo.Y + InFrom.Z * InTo.Z;
    if(Dot < -0.9999f) { return std::fabs(InFrom.Y) > 0.9f ? AnimQuat{ 0.0f, 0.0f, 1.0f, 0.0f } : AnimQuat{ 0.0f, 1.0f, 0.0f, 0.0f }; }
    return AnimMath::Normalize({ Cross.X, Cross.Y, Cross.Z, 1.0f + Dot });
}

static const AnimVec3 UP{ 0.0f, 1.0f, 0.0f };
static const AnimVec3 DOWN{ 0.0f, -1.0f, 0.0f };
static const AnimVec3 LEFT{ 1.0f, 0.0f, 0.0f };   // the character's left: +X

/* A bone whose local X runs along InDirection (Mixamo's convention here). */
static AnimQuat AlongX(const AnimVec3& InDirection) { return FromTo({ 1.0f, 0.0f, 0.0f }, InDirection); }
/* A bone whose local Y runs along InDirection, rolled InRoll degrees about it (Rigify's). */
static AnimQuat AlongY(const AnimVec3& InDirection, const float InRoll)
{
    return AnimMath::Normalize(AnimMath::Mul(FromTo(UP, InDirection), AxisAngle(UP, InRoll)));
}

/* Bones placed in model space; their local reference poses worked out. */
struct Rig
{
    SkeletonData               Skeleton;
    std::vector<AnimTransform> Model;

    void Add(const std::string& InName, const std::string& InParent, const AnimVec3& InPosition, const AnimQuat& InRotation)
    {
        SkeletonBone Bone;
        Bone.Name   = InName;
        Bone.Parent = InParent.empty() ? -1 : Skeleton.FindBone(InParent);
        if(Bone.Parent < 0)
        {
            Bone.RefPose.Translation = InPosition;
            Bone.RefPose.Rotation    = InRotation;
        }
        else
        {
            const AnimTransform& Parent = Model[size_t(Bone.Parent)];
            const AnimQuat Inverse = AnimMath::Conjugate(Parent.Rotation);
            Bone.RefPose.Translation = AnimMath::Rotate(Inverse, AnimMath::Sub(InPosition, Parent.Translation));
            Bone.RefPose.Rotation    = AnimMath::Normalize(AnimMath::Mul(Inverse, InRotation));
        }
        Skeleton.Bones.push_back(Bone);
        AnimTransform Placed;
        Placed.Translation = InPosition;
        Placed.Rotation    = InRotation;
        Model.push_back(Placed);
    }
};

static AnimVec3 Mirror(const AnimVec3& InV) { return { -InV.X, InV.Y, InV.Z }; }

static AnimVec3 Normalized(const AnimVec3& InV)
{
    const float Length = std::sqrt(InV.X * InV.X + InV.Y * InV.Y + InV.Z * InV.Z);
    return Length > 0.0f ? AnimMath::Scale(InV, 1.0f / Length) : InV;
}

/* Rig A: Mixamo names, bones along local X, hips at 100, arm 30 + 25;
 * both sides, the right mirrored. */
static Rig MakeMixamo()
{
    Rig A;
    A.Add("mixamorig:Hips",        "",                { 0.0f, 100.0f, 0.0f }, AlongX(UP));
    A.Add("mixamorig:Spine",       "mixamorig:Hips",  { 0.0f, 130.0f, 0.0f }, AlongX(UP));
    A.Add("mixamorig:Neck",        "mixamorig:Spine", { 0.0f, 145.0f, 0.0f }, AlongX(UP));
    A.Add("mixamorig:Head",        "mixamorig:Neck",  { 0.0f, 155.0f, 0.0f }, AlongX(UP));
    A.Add("mixamorig:HeadTop_End", "mixamorig:Head",  { 0.0f, 175.0f, 0.0f }, AlongX(UP));
    for(const char* Side : { "Left", "Right" })
    {
        const bool     bLeft = Side[0] == 'L';
        const auto     At    = [&](const AnimVec3& InV) { return bLeft ? InV : Mirror(InV); };
        const AnimVec3 Out   = bLeft ? LEFT : Mirror(LEFT);
        const std::string S  = std::string("mixamorig:") + Side;
        A.Add(S + "Shoulder",    "mixamorig:Spine", At({ 5.0f, 138.0f, 0.0f }), AlongX(Normalized(At({ 10.0f, 2.0f, 0.0f }))));
        A.Add(S + "Arm",         S + "Shoulder",    At({ 15.0f, 140.0f, 0.0f }), AlongX(Out));
        A.Add(S + "ForeArm",     S + "Arm",         At({ 45.0f, 140.0f, 0.0f }), AlongX(Out));
        A.Add(S + "Hand",        S + "ForeArm",     At({ 70.0f, 140.0f, 0.0f }), AlongX(Out));
        A.Add(S + "HandIndex1",  S + "Hand",        At({ 80.0f, 140.0f, 0.0f }), AlongX(Out));
        A.Add(S + "HandIndex2",  S + "HandIndex1",  At({ 84.0f, 140.0f, 0.0f }), AlongX(Out));
        A.Add(S + "UpLeg",       "mixamorig:Hips",  At({ 10.0f, 95.0f, 0.0f }), AlongX(DOWN));
        A.Add(S + "Leg",         S + "UpLeg",       At({ 10.0f, 50.0f, 0.0f }), AlongX(DOWN));
        A.Add(S + "Foot",        S + "Leg",         At({ 10.0f, 8.0f, 0.0f }), AlongX(Normalized({ 0.0f, -8.0f, 12.0f })));
        A.Add(S + "ToeBase",     S + "Foot",        At({ 10.0f, 0.0f, 12.0f }), AlongX({ 0.0f, 0.0f, 1.0f }));
    }
    return A;
}

/* Rig B: Rigify names, bones along local Y rolled 30-37 degrees, a root at
 * the origin, the pelvis at 80 (0.8x), arms 1.3x with a twist bone, a
 * three-bone neck. With bInAPose, the arms hang 45 degrees down. */
static Rig MakeRigify(const bool bInAPose)
{
    const float Diagonal = 0.70710678f;
    Rig B;
    B.Add("Root",          "",              { 0.0f, 0.0f, 0.0f }, AnimQuat());
    B.Add("DEF-spine",     "Root",          { 0.0f, 80.0f, 0.0f }, AlongY(UP, 30.0f));
    B.Add("DEF-spine.003", "DEF-spine",     { 0.0f, 104.0f, 0.0f }, AlongY(UP, 30.0f));
    B.Add("DEF-spine.004", "DEF-spine.003", { 0.0f, 118.0f, 0.0f }, AlongY(UP, 30.0f));
    B.Add("DEF-spine.005", "DEF-spine.004", { 0.0f, 124.0f, 0.0f }, AlongY(UP, 30.0f));
    B.Add("DEF-spine.006", "DEF-spine.005", { 0.0f, 130.0f, 0.0f }, AlongY(UP, 30.0f));
    for(const char* Side : { ".L", ".R" })
    {
        const bool     bLeft = Side[1] == 'L';
        const auto     At    = [&](const AnimVec3& InV) { return bLeft ? InV : Mirror(InV); };
        const AnimVec3 Arm   = At(bInAPose ? AnimVec3{ Diagonal, -Diagonal, 0.0f } : LEFT);
        const AnimVec3 Shoulder = At({ 12.0f, 112.0f, 0.0f });
        const auto     Along = [&](const float InDistance) { return AnimMath::Add(Shoulder, AnimMath::Scale(Arm, InDistance)); };
        const auto     N     = [&](const char* InName) { return std::string(InName) + Side; };
        B.Add(N("DEF-shoulder"),       "DEF-spine.003",    At({ 3.0f, 110.0f, 0.0f }), AlongY(Normalized(At({ 9.0f, 2.0f, 0.0f })), 20.0f));
        B.Add(N("DEF-upper_arm"),      N("DEF-shoulder"),  Shoulder,     AlongY(Arm, 37.0f));
        B.Add(N("DEF-upper_arm") + ".001", N("DEF-upper_arm"), Along(19.5f), AlongY(Arm, 37.0f));
        B.Add(N("DEF-forearm"),        N("DEF-upper_arm"), Along(39.0f), AlongY(Arm, 37.0f));
        B.Add(N("DEF-hand"),           N("DEF-forearm"),   Along(71.5f), AlongY(Arm, 37.0f));
        B.Add(std::string("DEF-f_index.01") + Side, N("DEF-hand"), Along(84.5f), AlongY(Arm, 37.0f));
        B.Add(std::string("DEF-f_index.02") + Side, std::string("DEF-f_index.01") + Side, Along(89.5f), AlongY(Arm, 37.0f));
        B.Add(N("DEF-thigh"),          "DEF-spine",        At({ 8.0f, 76.0f, 0.0f }), AlongY(DOWN, 10.0f));
        B.Add(N("DEF-shin"),           N("DEF-thigh"),     At({ 8.0f, 40.0f, 0.0f }), AlongY(DOWN, 10.0f));
        B.Add(N("DEF-foot"),           N("DEF-shin"),      At({ 8.0f, 6.0f, 0.0f }), AlongY(Normalized({ 0.0f, -6.0f, 10.0f }), 10.0f));
        B.Add(N("DEF-toe"),            N("DEF-foot"),      At({ 8.0f, 0.0f, 10.0f }), AlongY({ 0.0f, 0.0f, 1.0f }, 10.0f));
    }
    return B;
}

static RetargetSource MakeMap(const std::string& InSourcePath)
{
    RetargetSource Map;
    Map.Skeleton = InSourcePath;
    Map.Bones = {
        { "DEF-spine",       "mixamorig:Hips",        ERetargetTranslation::AnimationScaled, {} },
        { "DEF-spine.003",   "mixamorig:Spine",       ERetargetTranslation::Skeleton, {} },
        { "DEF-upper_arm.L", "mixamorig:LeftArm",     ERetargetTranslation::Skeleton, {} },
        { "DEF-forearm.L",   "mixamorig:LeftForeArm", ERetargetTranslation::Skeleton, {} },
        { "DEF-hand.L",      "mixamorig:LeftHand",    ERetargetTranslation::Skeleton, {} },
        { "DEF-thigh.L",     "mixamorig:LeftUpLeg",   ERetargetTranslation::Skeleton, {} },
        { "DEF-shin.L",      "mixamorig:LeftLeg",     ERetargetTranslation::Skeleton, {} },
    };
    return Map;
}

// === Clips ==================================================================

/* A two-frame (one second) track for InBone of InRig: its reference, then InEnd. */
static AnimTrack MakeTrack(const Rig& InRig, const std::string& InBone, const AnimTransform& InEnd)
{
    const SkeletonBone& Bone = InRig.Skeleton.Bones[size_t(InRig.Skeleton.FindBone(InBone))];
    AnimTrack Track;
    Track.Bone         = InBone;
    Track.Translations = { Bone.RefPose.Translation, InEnd.Translation };
    Track.Rotations    = { Bone.RefPose.Rotation, InEnd.Rotation };
    Track.Scales       = { Bone.RefPose.Scale };
    return Track;
}

/* On rig A: the left arm raised 90 degrees in model space (straight up),
 * the hips walking 100 along +X (root motion extracted), two notifies. */
static AnimClipData MakeRaiseAndWalk(const Rig& InA)
{
    AnimClipData Clip;
    Clip.SampleRate = 1.0f;
    Clip.FrameCount = 2;
    Clip.bLoop      = false;
    Clip.Skeleton   = "Mixamo.nmskel";
    Clip.RootBone   = "mixamorig:Hips";
    Clip.RootMotion = EAnimRootMotion::Extract;

    /* The arm's model rotation turned about Z, said under its (unmoving) parent. */
    const int32_t  Arm    = InA.Skeleton.FindBone("mixamorig:LeftArm");
    const AnimQuat Spine  = InA.Model[size_t(InA.Skeleton.Bones[size_t(Arm)].Parent)].Rotation;
    const AnimQuat Raised = AnimMath::Mul(AxisAngle({ 0.0f, 0.0f, 1.0f }, 90.0f), InA.Model[size_t(Arm)].Rotation);
    AnimTransform ArmEnd  = InA.Skeleton.Bones[size_t(Arm)].RefPose;
    ArmEnd.Rotation       = AnimMath::Normalize(AnimMath::Mul(AnimMath::Conjugate(Spine), Raised));
    Clip.Tracks.push_back(MakeTrack(InA, "mixamorig:LeftArm", ArmEnd));

    AnimTransform HipsEnd = InA.Skeleton.Bones[0].RefPose;
    HipsEnd.Translation.X += 100.0f;
    Clip.Tracks.push_back(MakeTrack(InA, "mixamorig:Hips", HipsEnd));

    Clip.Notifies.push_back({ "Clap", 0.5f, "", "mixamorig:LeftHand", "" });
    Clip.Notifies.push_back({ "Snap", 0.6f, "", "mixamorig:LeftHandIndex1", "" });
    Clip.Notifies.push_back({ "Step", 0.7f, "", "", "" });
    return Clip;
}

static AnimVec3 PositionOf(const std::vector<AnimMatrix>& InModel, const SkeletonData& InSkeleton, const char* InBone)
{
    const AnimMatrix& M = InModel[size_t(InSkeleton.FindBone(InBone))];
    return { M.M[0][3], M.M[1][3], M.M[2][3] };
}

static float Length(const AnimVec3& InV) { return std::sqrt(InV.X * InV.X + InV.Y * InV.Y + InV.Z * InV.Z); }

// === Tests ==================================================================

static void TestAsset()
{
    std::printf("[asset]\n");
    RetargetData Retarget;
    Retarget.Target = "Rigify.nmskel";
    Retarget.Sources.push_back(MakeMap("Mixamo.nmskel"));
    Retarget.Sources[0].RootScale = 0.75f;
    Retarget.Sources[0].Bones[2].PoseOffset = AxisAngle({ 0.0f, 0.0f, 1.0f }, -45.0f);
    RetargetSource Second;
    Second.Skeleton = "Other.nmskel";
    Second.Bones.push_back({ "DEF-hand.L", "hand_l", ERetargetTranslation::AnimationRelative, {} });
    Retarget.Sources.push_back(Second);
    Expect(Retarget.FindDefect() == nullptr, "a well-formed retarget has no defect");

    EngineCore::Archive Writer = EngineCore::Archive::ForWriting();
    Expect(SerializeRetarget(Writer, Retarget), "it saves");
    EngineCore::Archive Reader = EngineCore::Archive::ForReading(Writer.GetBytes());
    RetargetData Loaded;
    Expect(SerializeRetarget(Reader, Loaded), "and loads");
    Expect(Loaded.Target == "Rigify.nmskel" && Loaded.Sources.size() == 2, "with its target and sources");
    const RetargetSource* Mixamo = Loaded.FindSource("Mixamo.nmskel");
    Expect(Mixamo && Mixamo->Bones.size() == 7 && Near(Mixamo->RootScale, 0.75f), "a source keeps its bones and root scale");
    Expect(Mixamo && Mixamo->Bones[0].Translation == ERetargetTranslation::AnimationScaled
           && Mixamo->Bones[2].Source == "mixamorig:LeftArm", "and each bone its source and translation");
    Expect(Mixamo && SameRotation(Mixamo->Bones[2].PoseOffset, AxisAngle({ 0.0f, 0.0f, 1.0f }, -45.0f)), "and its pose offset");
    Expect(Loaded.FindSource("Other.nmskel") && Loaded.FindSource("Other.nmskel")->Bones[0].Translation == ERetargetTranslation::AnimationRelative,
           "the second source too");
    Expect(Loaded.FindSource("Missing.nmskel") == nullptr && Loaded.FindSource("") == nullptr, "a skeleton with no source finds none");
    Expect(Mixamo && Mixamo->FindBone("DEF-hand.L") && !Mixamo->FindBone("DEF-upper_arm.L.001"), "a source finds its mapped bones only");

    const auto Refused = [](RetargetData InBad)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        return InBad.FindDefect() != nullptr && !SerializeRetarget(Ar, InBad);
    };
    RetargetData Bad = Retarget;
    Bad.Sources[0].Bones.push_back({ "DEF-hand.L", "mixamorig:LeftForeArm", ERetargetTranslation::Skeleton, {} });
    Expect(Refused(Bad), "a target bone mapped twice is refused");
    Bad = Retarget;
    Bad.Sources[1].Skeleton = "Mixamo.nmskel";
    Expect(Refused(Bad), "two sources for one skeleton are refused");
    Bad = Retarget;
    Bad.Target.clear();
    Expect(Refused(Bad), "no target is refused");
    Bad = Retarget;
    Bad.Sources[0].Bones[0].Translation = ERetargetTranslation(9);
    Expect(Refused(Bad), "an unknown translation is refused");
    Bad = Retarget;
    Bad.Sources[0].RootScale = -1.0f;
    Expect(Refused(Bad), "a negative root scale is refused");

    const std::string Path = std::string(SCRATCH) + "Mixamo_to_Rigify.nmretarget";
    Expect(SaveRetargetFile(Path, Retarget), "it saves to a file");
    RetargetAssetCache::Invalidate(Path);
    const auto Cached = RetargetAssetCache::Find(Path);
    Expect(Cached && Cached->Sources.size() == 2, "and the cache loads it");
    Expect(RetargetAssetCache::Find(std::string(SCRATCH) + "None.nmretarget") == nullptr, "a missing file loads as null");
}

static void TestIdentity()
{
    std::printf("[identity]\n");
    const Rig A = MakeMixamo();
    const AnimClipData Clip = MakeRaiseAndWalk(A);
    RetargetSource Self;
    Self.Skeleton = "Mixamo.nmskel";
    for(const SkeletonBone& Bone : A.Skeleton.Bones) { Self.Bones.push_back({ Bone.Name, Bone.Name, ERetargetTranslation::Animation, {} }); }

    RetargetBinding Binding;
    Expect(BindClipRetargeted(Clip, A.Skeleton, A.Skeleton, Self, Binding), "a clip binds onto its own skeleton");
    AnimClipBinding Plain;
    BindClip(Clip, A.Skeleton, Plain);
    RetargetScratch Scratch;
    bool bSame = true;
    for(const float Time : { 0.0f, 0.3f, 1.0f })
    {
        AnimLocalPose Expected, Retargeted;
        GetRefPose(A.Skeleton, Expected);
        GetRefPose(A.Skeleton, Retargeted);
        SampleClip(Clip, Plain, Time, Expected);
        SampleClipRetargeted(Clip, Binding, Time, Scratch, Retargeted);
        for(size_t Bone = 0; Bone < Expected.size(); ++Bone)
        {
            bSame = bSame && Near(Expected[Bone].Translation, Retargeted[Bone].Translation, 1e-3f)
                          && SameRotation(Expected[Bone].Rotation, Retargeted[Bone].Rotation);
        }
    }
    Expect(bSame, "retargeting A onto A is SampleClip");
    Expect(Near(Binding.RootScale, 1.0f), "with no scaled bone the root scale is 1");
}

static void TestRotationTransfer()
{
    std::printf("[rotation, unmapped, root]\n");
    const Rig A = MakeMixamo();
    const Rig B = MakeRigify(false);
    const AnimClipData Clip = MakeRaiseAndWalk(A);
    RetargetBinding Binding;
    Expect(BindClipRetargeted(Clip, A.Skeleton, B.Skeleton, MakeMap("Mixamo.nmskel"), Binding), "A's clip binds onto B");

    RetargetScratch Scratch;
    AnimLocalPose   Pose;
    std::vector<AnimMatrix> Model;
    GetRefPose(B.Skeleton, Pose);
    SampleClipRetargeted(Clip, Binding, 0.0f, Scratch, Pose);
    LocalToModel(B.Skeleton, Pose, Model);
    std::vector<AnimMatrix> Ref;
    B.Skeleton.ComputeRefPoseModel(Ref);
    bool bAtRest = true;
    for(size_t Bone = 0; Bone < Ref.size(); ++Bone)
    {
        bAtRest = bAtRest && Near(PositionOf(Model, B.Skeleton, B.Skeleton.Bones[Bone].Name.c_str()),
                                  PositionOf(Ref, B.Skeleton, B.Skeleton.Bones[Bone].Name.c_str()));
    }
    Expect(bAtRest, "A's reference pose is B's reference pose");

    GetRefPose(B.Skeleton, Pose);
    SampleClipRetargeted(Clip, Binding, 1.0f, Scratch, Pose);
    LocalToModel(B.Skeleton, Pose, Model);
    const AnimVec3 Shoulder = PositionOf(Model, B.Skeleton, "DEF-upper_arm.L");
    const AnimVec3 Elbow    = PositionOf(Model, B.Skeleton, "DEF-forearm.L");
    const AnimVec3 Hand     = PositionOf(Model, B.Skeleton, "DEF-hand.L");
    Expect(Near(Normalized(AnimMath::Sub(Hand, Shoulder)), UP, 1e-4f), "raising A's arm 90 degrees raises B's straight up, whatever its bone axes");
    Expect(Near(Length(AnimMath::Sub(Elbow, Shoulder)), 39.0f) && Near(Length(AnimMath::Sub(Hand, Elbow)), 32.5f), "and B's arm keeps its own lengths");
    const int32_t Forearm = B.Skeleton.FindBone("DEF-forearm.L");
    Expect(Near(Normalized(AnimMath::Rotate(AnimMath::ToTransform(Model[size_t(Forearm)]).Rotation, UP)), UP, 1e-4f),
           "B's forearm's own axis (local Y) points up");

    /* The twist bone: unmapped, so its local stays, and it rides its parent. */
    const int32_t Twist = B.Skeleton.FindBone("DEF-upper_arm.L.001");
    Expect(SameRotation(Pose[size_t(Twist)].Rotation, B.Skeleton.Bones[size_t(Twist)].RefPose.Rotation)
           && Near(Pose[size_t(Twist)].Translation, B.Skeleton.Bones[size_t(Twist)].RefPose.Translation),
           "the unmapped twist bone keeps its reference local");
    Expect(Near(PositionOf(Model, B.Skeleton, "DEF-upper_arm.L.001"), AnimMath::Add(Shoulder, { 0.0f, 19.5f, 0.0f })),
           "and follows its parent up");

    /* The root: the hips' 100 along X is the pelvis's 80 (0.8: 80 / 100 high). */
    Expect(Near(Binding.RootScale, 0.8f), "the automatic root scale is pelvis height over pelvis height");
    Expect(Near(PositionOf(Model, B.Skeleton, "DEF-spine"), { 80.0f, 80.0f, 0.0f }), "the pelvis travels 80, at its own height");
    Expect(Binding.TargetRootBone == B.Skeleton.FindBone("DEF-spine"), "the root bone is the one the hips drive");
    Expect(Near(GetRootTravelRetargeted(Clip, Binding, 0.0f, 1.0f, false), { 80.0f, 0.0f, 0.0f }), "and its root travel is 80 too");
    Expect(Near(Binding.FirstRootTranslation, B.Skeleton.Bones[size_t(Binding.TargetRootBone)].RefPose.Translation),
           "its first frame is where B's pelvis rests");

    RetargetSource Fixed = MakeMap("Mixamo.nmskel");
    Fixed.RootScale = 0.5f;
    BindClipRetargeted(Clip, A.Skeleton, B.Skeleton, Fixed, Binding);
    Expect(Near(GetRootTravelRetargeted(Clip, Binding, 0.0f, 1.0f, false), { 50.0f, 0.0f, 0.0f }), "a set root scale is used as set");

    /* No source for the root bone: no root motion. */
    RetargetSource NoHips = MakeMap("Mixamo.nmskel");
    NoHips.Bones.erase(NoHips.Bones.begin());
    BindClipRetargeted(Clip, A.Skeleton, B.Skeleton, NoHips, Binding);
    Expect(Binding.TargetRootBone < 0 && Near(GetRootTravelRetargeted(Clip, Binding, 0.0f, 1.0f, false), {}),
           "with the hips unmapped, the clip has no root motion on B");
}

static void TestPoseOffset()
{
    std::printf("[pose offset]\n");
    const Rig A  = MakeMixamo();
    const Rig B2 = MakeRigify(true);
    const AnimClipData Clip = MakeRaiseAndWalk(A);
    RetargetSource Map = MakeMap("Mixamo.nmskel");
    for(const char* Bone : { "DEF-upper_arm.L", "DEF-forearm.L", "DEF-hand.L" })
    {
        for(RetargetBoneMap& Each : Map.Bones)
        {
            if(Each.Target == Bone) { Each.PoseOffset = ComputeRetargetPoseOffset(A.Skeleton, B2.Skeleton, Map, Bone); }
        }
    }
    Expect(SameRotation(Map.Bones[2].PoseOffset, AxisAngle({ 0.0f, 0.0f, 1.0f }, -45.0f)), "aligning the upper arm turns it 45 degrees down");
    Expect(SameRotation(Map.Bones[4].PoseOffset, Map.Bones[3].PoseOffset), "a hand with nothing below takes its forearm's offset");
    Expect(SameRotation(ComputeRetargetPoseOffset(A.Skeleton, B2.Skeleton, Map, "DEF-upper_arm.L.001"), AnimQuat()),
           "an unmapped bone has none");

    RetargetBinding Binding;
    BindClipRetargeted(Clip, A.Skeleton, B2.Skeleton, Map, Binding);
    RetargetScratch Scratch;
    AnimLocalPose   Pose;
    std::vector<AnimMatrix> Model;
    GetRefPose(B2.Skeleton, Pose);
    SampleClipRetargeted(Clip, Binding, 0.0f, Scratch, Pose);
    LocalToModel(B2.Skeleton, Pose, Model);
    const AnimVec3 Shoulder = PositionOf(Model, B2.Skeleton, "DEF-upper_arm.L");
    Expect(Near(Normalized(AnimMath::Sub(PositionOf(Model, B2.Skeleton, "DEF-hand.L"), Shoulder)), LEFT, 1e-4f),
           "A's T pose lifts B's A-posed arm level");

    GetRefPose(B2.Skeleton, Pose);
    SampleClipRetargeted(Clip, Binding, 1.0f, Scratch, Pose);
    LocalToModel(B2.Skeleton, Pose, Model);
    const AnimVec3 Hand = PositionOf(Model, B2.Skeleton, "DEF-hand.L");
    Expect(Near(Normalized(AnimMath::Sub(Hand, PositionOf(Model, B2.Skeleton, "DEF-upper_arm.L"))), UP, 1e-4f),
           "and raised, it points straight up, as on the T-posed B");
    const int32_t HandBone = B2.Skeleton.FindBone("DEF-hand.L");
    Expect(Near(Normalized(AnimMath::Rotate(AnimMath::ToTransform(Model[size_t(HandBone)]).Rotation, UP)), UP, 1e-4f),
           "the hand along with it");
}

static void TestNotifies()
{
    std::printf("[notifies]\n");
    const Rig A = MakeMixamo();
    const Rig B = MakeRigify(false);
    const AnimClipData Clip = MakeRaiseAndWalk(A);
    RetargetBinding Binding;
    BindClipRetargeted(Clip, A.Skeleton, B.Skeleton, MakeMap("Mixamo.nmskel"), Binding);
    std::vector<AnimNotify> Fired;
    Fired.push_back({ "Before", 0.0f, "", "mixamorig:LeftHand", "" });
    CollectNotifies(Clip, 0.0f, 1.0f, false, Fired);
    RemapNotifies(Binding, Fired, 1);
    Expect(Fired.size() == 4, "the clip's notifies fire");
    Expect(Fired[0].Bone == "mixamorig:LeftHand", "those before InFirst are left alone");
    Expect(Fired[1].Bone == "DEF-hand.L", "a notify at LeftHand is at DEF-hand.L");
    Expect(Fired[2].Bone == "DEF-hand.L", "one at an unmapped finger is at its nearest mapped ancestor");
    Expect(Fired[3].Bone.empty(), "one at no bone stays at the entity");
}

static void TestAutoMap()
{
    std::printf("[auto-map]\n");
    const Rig A = MakeMixamo(), B = MakeRigify(false), B2 = MakeRigify(true);
    Expect(SimplifyBoneName("mixamorig:LeftArm") == "leftarm" && SimplifyBoneName("DEF-upper_arm.L") == "upper_arm.l",
           "names simplify without namespace or layer prefix");
    Expect(NormalizeBoneName("DEF-upper_arm.L") == "l:upperarm" && NormalizeBoneName("Left_UpperArm") == "l:upperarm"
           && NormalizeBoneName("upper_arm_l") == "l:upperarm" && NormalizeBoneName("mixamorig:RightArm") == "r:arm"
           && NormalizeBoneName("Spine") == ":spine", "and normalise their side");

    HumanoidRoles Roles;
    std::string   Error;
    Expect(FindHumanoidRoles(B.Skeleton, Roles, Error), "the Rigify-style rig is a humanoid");
    Expect(Roles.Pelvis == B.Skeleton.FindBone("DEF-spine") && Roles.Chest == B.Skeleton.FindBone("DEF-spine.003")
           && Roles.Head == B.Skeleton.FindBone("DEF-spine.006") && Roles.Clavicle[0] == B.Skeleton.FindBone("DEF-shoulder.L")
           && Roles.Toe[1] == B.Skeleton.FindBone("DEF-toe.R"), "its roles: pelvis, chest, head, clavicle, toe");
    Expect(FindHumanoidRoles(A.Skeleton, Roles, Error) && Roles.Head == A.Skeleton.FindBone("mixamorig:Head")
           && Roles.Neck == A.Skeleton.FindBone("mixamorig:Neck"), "the Mixamo-style one, its head named, not its end");
    SkeletonData Arm;
    Arm.Bones.resize(1);
    Arm.Bones[0].Name = "Arm";
    Expect(!FindHumanoidRoles(Arm, Roles, Error) && !Error.empty(), "a skeleton without limbs is not, and says why");

    RetargetSource Map;
    Map.Skeleton = "Mixamo.nmskel";
    const size_t Mapped = AutoMapRetarget(A.Skeleton, B.Skeleton, Map);
    const auto SourceOf = [&](const char* InTarget) { const RetargetBoneMap* Bone = Map.FindBone(InTarget); return Bone ? Bone->Source : std::string("-"); };
    bool bRoles = true;
    const char* Pairs[][2] = {
        { "DEF-spine", "mixamorig:Hips" }, { "DEF-spine.003", "mixamorig:Spine" }, { "DEF-spine.004", "mixamorig:Neck" },
        { "DEF-spine.006", "mixamorig:Head" },
        { "DEF-shoulder.L", "mixamorig:LeftShoulder" }, { "DEF-upper_arm.L", "mixamorig:LeftArm" }, { "DEF-forearm.L", "mixamorig:LeftForeArm" },
        { "DEF-hand.L", "mixamorig:LeftHand" }, { "DEF-f_index.01.L", "mixamorig:LeftHandIndex1" }, { "DEF-f_index.02.L", "mixamorig:LeftHandIndex2" },
        { "DEF-thigh.L", "mixamorig:LeftUpLeg" }, { "DEF-shin.L", "mixamorig:LeftLeg" }, { "DEF-foot.L", "mixamorig:LeftFoot" },
        { "DEF-toe.L", "mixamorig:LeftToeBase" },
        { "DEF-shoulder.R", "mixamorig:RightShoulder" }, { "DEF-upper_arm.R", "mixamorig:RightArm" }, { "DEF-forearm.R", "mixamorig:RightForeArm" },
        { "DEF-hand.R", "mixamorig:RightHand" }, { "DEF-thigh.R", "mixamorig:RightUpLeg" }, { "DEF-shin.R", "mixamorig:RightLeg" },
        { "DEF-foot.R", "mixamorig:RightFoot" }, { "DEF-toe.R", "mixamorig:RightToeBase" },
    };
    for(const auto& Pair : Pairs)
    {
        if(SourceOf(Pair[0]) != Pair[1])
        {
            std::printf("    %s <- %s, expected %s\n", Pair[0], SourceOf(Pair[0]).c_str(), Pair[1]);
            bRoles = false;
        }
    }
    Expect(bRoles, "A onto B maps every role bone");
    Expect(SourceOf("DEF-upper_arm.L.001") == "-" && SourceOf("DEF-upper_arm.R.001") == "-" && SourceOf("Root") == "-"
           && SourceOf("DEF-spine.005") == "-", "and leaves the twist bones, the root and the middle of the neck unmapped");
    Expect(Mapped == Map.Bones.size() && Mapped == 24, "24 bones in all");
    Expect(Map.FindBone("DEF-spine") && Map.FindBone("DEF-spine")->Translation == ERetargetTranslation::AnimationScaled
           && Map.FindBone("DEF-hand.L")->Translation == ERetargetTranslation::Skeleton, "the pelvis's translation is scaled, the rest the target's");

    /* Auto-mapped and arms aligned, the A-posed B plays the raise as the T-posed one does. */
    RetargetSource Aligned;
    Aligned.Skeleton = "Mixamo.nmskel";
    AutoMapRetarget(A.Skeleton, B2.Skeleton, Aligned);
    Expect(AlignRetargetArms(A.Skeleton, B2.Skeleton, Aligned), "the arms align");
    Expect(SameRotation(Aligned.FindBone("DEF-upper_arm.L")->PoseOffset, AxisAngle({ 0.0f, 0.0f, 1.0f }, -45.0f))
           && SameRotation(Aligned.FindBone("DEF-upper_arm.R")->PoseOffset, AxisAngle({ 0.0f, 0.0f, 1.0f }, 45.0f)),
           "each upper arm turned 45 degrees down, on its side");
    Expect(SameRotation(Aligned.FindBone("DEF-thigh.L")->PoseOffset, AnimQuat()), "the legs are left alone");
    const AnimClipData Clip = MakeRaiseAndWalk(A);
    RetargetBinding Binding;
    BindClipRetargeted(Clip, A.Skeleton, B2.Skeleton, Aligned, Binding);
    RetargetScratch Scratch;
    AnimLocalPose   Pose;
    GetRefPose(B2.Skeleton, Pose);
    SampleClipRetargeted(Clip, Binding, 1.0f, Scratch, Pose);
    std::vector<AnimMatrix> Model;
    LocalToModel(B2.Skeleton, Pose, Model);
    Expect(Near(Normalized(AnimMath::Sub(PositionOf(Model, B2.Skeleton, "DEF-hand.L"), PositionOf(Model, B2.Skeleton, "DEF-upper_arm.L"))), UP, 1e-4f),
           "auto-mapped and aligned, B's A-posed arm is raised straight up");

    /* The engine's own Rigify rig, staged with the engine's content. */
    const auto Dummy = SkeletonAssetCache::Find("Engine/Content/Characters/CrashDummy/CrashDummy_Skeleton.nmskel");
    Expect(Dummy != nullptr, "CrashDummy's skeleton loads");
    if(Dummy)
    {
        RetargetSource OntoDummy;
        AutoMapRetarget(A.Skeleton, *Dummy, OntoDummy);
        const auto From = [&](const char* InTarget) { const RetargetBoneMap* Bone = OntoDummy.FindBone(InTarget); return Bone ? Bone->Source : std::string("-"); };
        Expect(From("DEF-spine") == "mixamorig:Hips" && From("DEF-spine.004") == "mixamorig:Neck" && From("DEF-spine.006") == "mixamorig:Head"
               && From("DEF-shoulder.L") == "mixamorig:LeftShoulder" && From("DEF-hand.R") == "mixamorig:RightHand"
               && From("DEF-toe.L") == "mixamorig:LeftToeBase" && From("DEF-f_index.01.L") == "mixamorig:LeftHandIndex1",
               "the Mixamo-style rig maps onto CrashDummy by role");
        Expect(From("CrashDummy_Rig") == "-" && From("DEF-upper_arm.L.001") == "-" && From("DEF-thigh.R.001") == "-",
               "its root and twist bones stay unmapped");
    }
}

// === Playback ===============================================================

/* The assets the playback tests load by path, written to SCRATCH. */
struct Assets
{
    std::string Mixamo   = std::string(SCRATCH) + "Mixamo.nmskel";
    std::string Rigify   = std::string(SCRATCH) + "Rigify.nmskel";
    std::string RigifyA  = std::string(SCRATCH) + "RigifyA.nmskel";
    std::string Retarget = std::string(SCRATCH) + "Mixamo_to_Rigify.nmretarget";
    std::string RetargetA = std::string(SCRATCH) + "Mixamo_to_RigifyA.nmretarget";
    std::string Raise    = std::string(SCRATCH) + "Mixamo_RaiseWalk.nmanim";
    std::string RaiseOnly = std::string(SCRATCH) + "Mixamo_Raise.nmanim";
    std::string Wave     = std::string(SCRATCH) + "Mixamo_Wave.nmanim";
    std::string Breathe  = std::string(SCRATCH) + "Mixamo_Breathe.nmanim";
    std::string Foreign  = std::string(SCRATCH) + "Other_Bend.nmanim";
    std::string Mesh     = std::string(SCRATCH) + "Rigify.nmmesh";
};

/* A one-second clip on rig A turning InBone InDegrees about InAxis (model space). */
static AnimClipData MakeTurnClip(const Rig& InA, const char* InBone, const AnimVec3& InAxis, const float InDegrees, const std::string& InSkeleton)
{
    AnimClipData Clip;
    Clip.SampleRate = 1.0f;
    Clip.FrameCount = 2;
    Clip.bLoop      = true;
    Clip.Skeleton   = InSkeleton;
    const int32_t  Bone   = InA.Skeleton.FindBone(InBone);
    const int32_t  Parent = InA.Skeleton.Bones[size_t(Bone)].Parent;
    const AnimQuat ParentModel = Parent >= 0 ? InA.Model[size_t(Parent)].Rotation : AnimQuat();
    AnimTransform End = InA.Skeleton.Bones[size_t(Bone)].RefPose;
    End.Rotation = AnimMath::Normalize(AnimMath::Mul(AnimMath::Conjugate(ParentModel),
                                                     AnimMath::Mul(AxisAngle(InAxis, InDegrees), InA.Model[size_t(Bone)].Rotation)));
    Clip.Tracks.push_back(MakeTrack(InA, InBone, End));
    return Clip;
}

static void WriteAssets(const Assets& InPaths)
{
    Rig A = MakeMixamo(), B = MakeRigify(false), B2 = MakeRigify(true);
    AnimClipData Raise = MakeRaiseAndWalk(A);
    Raise.Skeleton = InPaths.Mixamo;
    AnimClipData RaiseOnly = Raise;
    RaiseOnly.Tracks.pop_back();
    RaiseOnly.RootMotion = EAnimRootMotion::Keep;
    RaiseOnly.bLoop      = false;
    AnimClipData Wave    = MakeTurnClip(A, "mixamorig:LeftForeArm", { 0.0f, 1.0f, 0.0f }, 60.0f, InPaths.Mixamo);
    AnimClipData Breathe = MakeTurnClip(A, "mixamorig:Spine", { 1.0f, 0.0f, 0.0f }, 10.0f, InPaths.Mixamo);
    /* Made for a skeleton no retarget has a source for, with B's names: it plays by name. */
    AnimClipData Foreign;
    Foreign.SampleRate = 1.0f;
    Foreign.FrameCount = 2;
    Foreign.Skeleton   = std::string(SCRATCH) + "Other.nmskel";
    AnimTransform Bent = B.Skeleton.Bones[size_t(B.Skeleton.FindBone("DEF-forearm.L"))].RefPose;
    Bent.Rotation = AnimMath::Normalize(AnimMath::Mul(Bent.Rotation, AxisAngle({ 1.0f, 0.0f, 0.0f }, 45.0f)));
    Foreign.Tracks.push_back(MakeTrack(B, "DEF-forearm.L", Bent));

    RetargetData Retarget;
    Retarget.Target = InPaths.Rigify;
    Retarget.Sources.push_back(MakeMap(InPaths.Mixamo));
    RetargetData RetargetA;
    RetargetA.Target = InPaths.RigifyA;
    RetargetA.Sources.push_back(MakeMap(InPaths.Mixamo));

    /* One triangle on the hand, bound where B's reference pose puts it. */
    MeshData Mesh;
    Mesh.Positions = { 83.5f, 112.0f, 0.0f,   93.5f, 112.0f, 0.0f,   83.5f, 122.0f, 0.0f };
    Mesh.Normals   = { 0, 0, 1,  0, 0, 1,  0, 0, 1 };
    Mesh.UV0       = { 0, 0,  1, 0,  0, 1 };
    Mesh.Indices   = { 0, 1, 2 };
    Mesh.Sections.push_back({ "Tri", "", 0, 3 });
    Mesh.Skeleton  = InPaths.Rigify;
    std::vector<AnimMatrix> Model;
    B.Skeleton.ComputeRefPoseModel(Model);
    for(const AnimMatrix& Each : Model) { Mesh.InverseBind.push_back(AnimMath::Inverse(Each)); }
    const uint16_t Hand = uint16_t(B.Skeleton.FindBone("DEF-hand.L"));
    Mesh.Joints  = { Hand, 0, 0, 0,  Hand, 0, 0, 0,  Hand, 0, 0, 0 };
    Mesh.Weights = { 1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0 };
    ComputeMeshBounds(Mesh);

    const bool bWritten = SaveSkeletonFile(InPaths.Mixamo, A.Skeleton) && SaveSkeletonFile(InPaths.Rigify, B.Skeleton)
                       && SaveSkeletonFile(InPaths.RigifyA, B2.Skeleton)
                       && SaveRetargetFile(InPaths.Retarget, Retarget) && SaveRetargetFile(InPaths.RetargetA, RetargetA)
                       && SaveAnimClipFile(InPaths.Raise, Raise) && SaveAnimClipFile(InPaths.RaiseOnly, RaiseOnly)
                       && SaveAnimClipFile(InPaths.Wave, Wave) && SaveAnimClipFile(InPaths.Breathe, Breathe)
                       && SaveAnimClipFile(InPaths.Foreign, Foreign) && SaveMeshFile(InPaths.Mesh, Mesh);
    Expect(bWritten, "the playback assets are written");
    SkeletonAssetCache::Invalidate("");
    AnimClipAssetCache::Invalidate("");
    RetargetAssetCache::Invalidate("");
    MeshAssetCache::Invalidate("");
    AnimStateAssetCache::Invalidate("");
}

static bool SamePose(const std::vector<AnimMatrix>& InA, const std::vector<AnimMatrix>& InB, const float InTolerance = 1e-3f)
{
    if(InA.size() != InB.size() || InA.empty()) { return false; }
    for(size_t Bone = 0; Bone < InA.size(); ++Bone)
    {
        for(int Row = 0; Row < 3; ++Row)
        {
            for(int Col = 0; Col < 4; ++Col)
            {
                if(!Near(InA[Bone].M[Row][Col], InB[Bone].M[Row][Col], InTolerance)) { return false; }
            }
        }
    }
    return true;
}

static void TestInstance(const Assets& InPaths)
{
    std::printf("[instance]\n");
    const auto Rigify   = SkeletonAssetCache::Find(InPaths.Rigify);
    const auto Retarget = RetargetAssetCache::Find(InPaths.Retarget);
    const auto Raise    = AnimClipAssetCache::Find(InPaths.Raise);
    Expect(Rigify && Retarget && Raise, "the assets load");
    if(!Rigify || !Retarget || !Raise) { return; }

    AnimInstance Instance;
    Instance.SetSkeleton(Rigify);
    Instance.SetRetarget(Retarget);
    Instance.PlayClip(Raise);
    Instance.Update(0.5f);
    Expect(Near(Instance.GetRootMotion(), { 40.0f, 0.0f, 0.0f }), "a retargeted clip's root motion is scaled to the target (80 a second)");
    Instance.Update(0.5f);
    const std::vector<AnimMatrix>& Model = Instance.GetModelPose();
    const AnimVec3 Shoulder = PositionOf(Model, *Rigify, "DEF-upper_arm.L");
    Expect(Near(Normalized(AnimMath::Sub(PositionOf(Model, *Rigify, "DEF-hand.L"), Shoulder)), UP, 1e-4f), "and it raises the target's arm");
    Expect(Near(PositionOf(Model, *Rigify, "DEF-spine"), { 0.0f, 80.0f, 0.0f }), "with the extracted root held where it starts, on the target");

    /* No retarget: the clip binds by name, and A's names move none of B's bones. */
    Instance.SetRetarget(nullptr);
    Instance.PlayClip(Raise);
    Instance.SetTime(1.0f);
    std::vector<AnimMatrix> Ref;
    Rigify->ComputeRefPoseModel(Ref);
    Expect(SamePose(Instance.GetModelPose(), Ref), "without a retarget, it plays by name: B holds its reference pose");

    /* The overlay clip retargets too. */
    Instance.SetRetarget(Retarget);
    Instance.PlayClip(nullptr);
    Instance.SetOverlayClip(Raise, 1.0f, 1.0f);
    Instance.Evaluate();
    Expect(Near(Normalized(AnimMath::Sub(PositionOf(Instance.GetModelPose(), *Rigify, "DEF-hand.L"),
                                         PositionOf(Instance.GetModelPose(), *Rigify, "DEF-upper_arm.L"))), UP, 1e-4f),
           "an overlay clip plays through the retarget");
}

static void TestFallback(const Assets& InPaths)
{
    std::printf("[fallback]\n");
    const auto Rigify   = SkeletonAssetCache::Find(InPaths.Rigify);
    const auto Retarget = RetargetAssetCache::Find(InPaths.Retarget);
    const auto Foreign  = AnimClipAssetCache::Find(InPaths.Foreign);
    if(!Rigify || !Retarget || !Foreign) { Expect(false, "the fallback assets load"); return; }
    Expect(RetargetBindingCache::Find(Foreign, Retarget, Rigify) == nullptr, "a clip whose skeleton has no source has no binding");

    AnimInstance Instance;
    Instance.SetSkeleton(Rigify);
    Instance.SetRetarget(Retarget);
    Instance.PlayClip(Foreign);
    Instance.SetTime(1.0f);
    AnimLocalPose Expected;
    GetRefPose(*Rigify, Expected);
    AnimClipBinding Binding;
    BindClip(*Foreign, *Rigify, Binding);
    SampleClip(*Foreign, Binding, 1.0f, Expected);
    std::vector<AnimMatrix> Model;
    LocalToModel(*Rigify, Expected, Model);
    Expect(SamePose(Instance.GetModelPose(), Model), "it plays by bone name, as before retargeting");

    const auto Raise = AnimClipAssetCache::Find(InPaths.Raise);
    Expect(RetargetBindingCache::Find(Raise, Retarget, Rigify) == RetargetBindingCache::Find(Raise, Retarget, Rigify)
           && RetargetBindingCache::Find(Raise, Retarget, Rigify) != nullptr, "a binding is shared");
}

/* A state like a character's: a base layer (root motion), an upper-body
 * override layer through a mask, an additive breathing layer, a look-at.
 * InClips: the raise, wave and breathe clips' paths. */
static AnimStateData MakeCharacterState(const std::string InClips[3], const std::string& InRetarget)
{
    AnimStateData State;
    State.Parameters = { { "Pitch", EAnimParameterType::Float, 0.0f } };
    AnimLayer Base;
    Base.Name = "Base";
    AnimStateNode Raise;
    Raise.Name    = "Raise";
    Raise.Samples = { { InClips[0], 0.0f, 0.0f } };
    Base.States.push_back(Raise);
    AnimLayer Upper;
    Upper.Name      = "Upper";
    Upper.Weight    = 0.5f;
    Upper.MaskBones = { "DEF-forearm.L" };
    AnimStateNode Wave;
    Wave.Name    = "Wave";
    Wave.Samples = { { InClips[1], 0.0f, 0.0f } };
    Upper.States.push_back(Wave);
    AnimLayer Breathe;
    Breathe.Name  = "Breathe";
    Breathe.Blend = EAnimLayerBlend::Additive;
    AnimStateNode In;
    In.Name    = "In";
    In.Samples = { { InClips[2], 0.0f, 0.0f } };
    Breathe.States.push_back(In);
    State.Layers = { Base, Upper, Breathe };
    State.LookAt.Parameter = "Pitch";
    State.LookAt.Response  = 0.0f;
    State.LookAt.Bones     = { { "DEF-spine.003", 1.0f } };
    State.Retarget = InRetarget;
    return State;
}

static void TestStateAndBake(const Assets& InPaths)
{
    std::printf("[layers and bake]\n");
    const auto Mixamo   = SkeletonAssetCache::Find(InPaths.Mixamo);
    const auto Rigify   = SkeletonAssetCache::Find(InPaths.Rigify);
    const auto Retarget = RetargetAssetCache::Find(InPaths.Retarget);
    if(!Mixamo || !Rigify || !Retarget) { Expect(false, "the state assets load"); return; }

    /* Each clip baked onto B: ordinary clips that play there by name. */
    const std::string Sources[3] = { InPaths.Raise, InPaths.Wave, InPaths.Breathe };
    std::string Baked[3];
    bool bBaked = true, bMatches = true;
    for(int Index = 0; Index < 3; ++Index)
    {
        const auto Clip = AnimClipAssetCache::Find(Sources[Index]);
        const auto Binding = RetargetBindingCache::Find(Clip, Retarget, Rigify);
        if(!Clip || !Binding) { bBaked = false; continue; }
        AnimClipData Out;
        BakeRetargetedClip(*Clip, *Binding, *Rigify, InPaths.Rigify, Out);
        Baked[Index] = std::string(SCRATCH) + "Baked_" + std::to_string(Index) + ".nmanim";
        bBaked = bBaked && SaveAnimClipFile(Baked[Index], Out);

        /* Every frame of the baked clip, by SampleClip, is the runtime retarget. */
        AnimClipBinding Plain;
        BindClip(Out, *Rigify, Plain);
        RetargetScratch Scratch;
        for(uint32_t Frame = 0; Frame < Clip->FrameCount; ++Frame)
        {
            const float Time = float(Frame) / Clip->SampleRate;
            AnimLocalPose Runtime, Played;
            GetRefPose(*Rigify, Runtime);
            GetRefPose(*Rigify, Played);
            SampleClipRetargeted(*Clip, *Binding, Time, Scratch, Runtime);
            SampleClip(Out, Plain, Time, Played);
            for(size_t Bone = 0; Bone < Runtime.size(); ++Bone)
            {
                bMatches = bMatches && Near(Runtime[Bone].Translation, Played[Bone].Translation, 1e-4f)
                                    && SameRotation(Runtime[Bone].Rotation, Played[Bone].Rotation, 1e-6f);
            }
        }
        if(Index == 0)
        {
            Expect(Out.Skeleton == InPaths.Rigify && Out.RootBone == "DEF-spine" && Out.RootMotion == EAnimRootMotion::Extract,
                   "a baked clip names the target skeleton and its root bone");
            Expect(Out.Notifies.size() == 3 && Out.Notifies[0].Bone == "DEF-hand.L", "and its notifies' target bones");
            Expect(Out.Tracks.size() == 7, "a track per mapped bone");
        }
    }
    Expect(bBaked, "the clips bake");
    Expect(bMatches, "a baked clip sampled by SampleClip is the runtime retarget at every frame (1e-4)");
    AnimClipAssetCache::Invalidate("");

    /* The state through the retarget, and the same state on the baked clips. */
    const auto Retargeted = std::make_shared<AnimStateData>(MakeCharacterState(Sources, InPaths.Retarget));
    const auto Native     = std::make_shared<AnimStateData>(MakeCharacterState(Baked, ""));
    AnimInstance Through, Plain;
    Through.SetSkeleton(Rigify);
    Through.SetRetarget(Retarget);
    Through.SetState(Retargeted);
    Plain.SetSkeleton(Rigify);
    Plain.SetState(Native);
    bool bSame = true, bMotion = true, bNotifies = true;
    for(int Step = 0; Step < 6; ++Step)
    {
        Through.SetParameter("Pitch", 15.0f);
        Plain.SetParameter("Pitch", 15.0f);
        Through.Update(0.15f);
        Plain.Update(0.15f);
        bSame = bSame && SamePose(Through.GetModelPose(), Plain.GetModelPose());
        bMotion = bMotion && Near(Through.GetRootMotion(), Plain.GetRootMotion());
        bNotifies = bNotifies && Through.GetFiredNotifies().size() == Plain.GetFiredNotifies().size();
        for(size_t Index = 0; bNotifies && Index < Through.GetFiredNotifies().size(); ++Index)
        {
            bNotifies = Through.GetFiredNotifies()[Index].Bone == Plain.GetFiredNotifies()[Index].Bone;
        }
    }
    Expect(bSame, "a retargeted state -- masked layer, additive layer, look-at -- poses as it does on baked clips");
    Expect(bMotion, "with the same root motion");
    Expect(bNotifies, "and the same notifies, at the same bones");
    std::vector<AnimMatrix> Ref;
    Rigify->ComputeRefPoseModel(Ref);
    Expect(!SamePose(Through.GetModelPose(), Ref), "(and it does move)");
}

static void TestSystem(const Assets& InPaths)
{
    std::printf("[system]\n");
    const std::string Clips[3] = { InPaths.Raise, InPaths.Wave, InPaths.Breathe };
    AnimStateData State = MakeCharacterState(Clips, InPaths.Retarget);
    const std::string StatePath = std::string(SCRATCH) + "Character.nmanimstate";
    Expect(SaveAnimStateFile(StatePath, State), "an anim state naming a retarget saves");
    AnimStateAssetCache::Invalidate("");
    const auto Loaded = AnimStateAssetCache::Find(StatePath);
    Expect(Loaded && Loaded->Retarget == InPaths.Retarget, "and loads with it");

    GameCore::World World;
    AnimationSystem* System = AnimationSystem::Attach(World);
    const auto Spawn = [&](const std::string& InAnimation, const std::string& InRetarget)
    {
        GameCore::Entity Actor = World.CreateEntity();
        Actor.AddComponent(GameCore::TransformComponent());
        RenderComponent Render;
        std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", InPaths.Mesh.c_str());
        Actor.AddComponent(Render);
        AnimationComponent Animation;
        std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%s", InAnimation.c_str());
        std::snprintf(Animation.Retarget, sizeof(Animation.Retarget), "%s", InRetarget.c_str());
        Actor.AddComponent(Animation);
        return Actor;
    };
    GameCore::Entity ByComponent = Spawn(InPaths.RaiseOnly, InPaths.Retarget);
    GameCore::Entity ByState     = Spawn(StatePath, "");
    GameCore::Entity Mismatched  = Spawn(InPaths.RaiseOnly, InPaths.RetargetA);
    World.Tick(1.0f);

    GameCore::TransformPose Hand, Shoulder;
    Expect(System->GetBoneWorldPose(ByComponent, "DEF-hand.L", Hand) && System->GetBoneWorldPose(ByComponent, "DEF-upper_arm.L", Shoulder)
           && Near(float(Hand.Position.GetY() - Shoulder.Position.GetY()), 71.5f) && Near(float(Hand.Position.GetX() - Shoulder.Position.GetX()), 0.0f),
           "the component's retarget plays the Mixamo clip on the Rigify mesh");
    const AnimInstance* Instance = System->FindInstance(ByState);
    Expect(Instance && Instance->GetRetarget() && !Instance->GetModelPose().empty(), "an anim state's default retarget is used when the component names none");
    const AnimInstance* Plain = System->FindInstance(Mismatched);
    Expect(Plain && !Plain->GetRetarget(), "a retarget for another skeleton is not used (warned)");

    /* Clearing the component's field drops the retarget; the clip binds by name. */
    std::memset(ByComponent.GetComponent<AnimationComponent>()->Retarget, 0, ANIMATION_PATH_MAX);
    World.Tick(0.1f);
    Expect(System->FindInstance(ByComponent) && !System->FindInstance(ByComponent)->GetRetarget(), "clearing the field rebinds without it");
    AnimationSystem::Detach(World);
}

int main()
{
    std::printf("RenderCore retarget tests\n");
    TestAsset();
    TestIdentity();
    TestRotationTransfer();
    TestPoseOffset();
    TestNotifies();
    TestAutoMap();
    const Assets Paths;
    WriteAssets(Paths);
    TestInstance(Paths);
    TestFallback(Paths);
    TestStateAndBake(Paths);
    TestSystem(Paths);
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
