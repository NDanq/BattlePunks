/*
 * RenderCoreRetargetTests -- retargeting without a window (ANIMATION.md,
 * "Retargeting"): the .nmretarget asset, then clips made on a Mixamo-style
 * rig (bones along local X) played on a Rigify-style one (bones along local
 * Y, rolled, longer arms, a lower pelvis, a twist bone) -- and on the same
 * rig in an A pose. Both rigs are built here, from model-space placements.
 */

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/AnimPose.h"
#include "Public/RenderCore/AnimRetarget.h"
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

/* The shortest turn from unit InFrom to unit InTo (not opposite). */
static AnimQuat FromTo(const AnimVec3& InFrom, const AnimVec3& InTo)
{
    const AnimVec3 Cross{ InFrom.Y * InTo.Z - InFrom.Z * InTo.Y, InFrom.Z * InTo.X - InFrom.X * InTo.Z, InFrom.X * InTo.Y - InFrom.Y * InTo.X };
    const float    Dot = InFrom.X * InTo.X + InFrom.Y * InTo.Y + InFrom.Z * InTo.Z;
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

/* Rig A: Mixamo names, bones along local X, hips at 100, arm 30 + 25. */
static Rig MakeMixamo()
{
    Rig A;
    A.Add("mixamorig:Hips",          "",                       { 0.0f, 100.0f, 0.0f }, AlongX(UP));
    A.Add("mixamorig:Spine",         "mixamorig:Hips",         { 0.0f, 130.0f, 0.0f }, AlongX(UP));
    A.Add("mixamorig:LeftArm",       "mixamorig:Spine",        { 15.0f, 140.0f, 0.0f }, AlongX(LEFT));
    A.Add("mixamorig:LeftForeArm",   "mixamorig:LeftArm",      { 45.0f, 140.0f, 0.0f }, AlongX(LEFT));
    A.Add("mixamorig:LeftHand",      "mixamorig:LeftForeArm",  { 70.0f, 140.0f, 0.0f }, AlongX(LEFT));
    A.Add("mixamorig:LeftHandIndex1","mixamorig:LeftHand",     { 80.0f, 140.0f, 0.0f }, AlongX(LEFT));
    A.Add("mixamorig:LeftUpLeg",     "mixamorig:Hips",         { 10.0f, 95.0f, 0.0f }, AlongX(DOWN));
    A.Add("mixamorig:LeftLeg",       "mixamorig:LeftUpLeg",    { 10.0f, 50.0f, 0.0f }, AlongX(DOWN));
    return A;
}

/* Rig B: Rigify names, bones along local Y rolled 30-37 degrees, a root at
 * the origin, the pelvis at 80 (0.8x), arms 1.3x with a twist bone. With
 * bInAPose, the arms hang 45 degrees down. */
static Rig MakeRigify(const bool bInAPose)
{
    const float    Diagonal = 0.70710678f;
    const AnimVec3 Arm = bInAPose ? AnimVec3{ Diagonal, -Diagonal, 0.0f } : LEFT;
    const AnimVec3 Shoulder{ 12.0f, 112.0f, 0.0f };
    const auto     Along = [&](const float InDistance) { return AnimMath::Add(Shoulder, AnimMath::Scale(Arm, InDistance)); };
    Rig B;
    B.Add("Root",                "",                { 0.0f, 0.0f, 0.0f }, AnimQuat());
    B.Add("DEF-spine",           "Root",            { 0.0f, 80.0f, 0.0f }, AlongY(UP, 30.0f));
    B.Add("DEF-spine.003",       "DEF-spine",       { 0.0f, 104.0f, 0.0f }, AlongY(UP, 30.0f));
    B.Add("DEF-upper_arm.L",     "DEF-spine.003",   Shoulder,     AlongY(Arm, 37.0f));
    B.Add("DEF-upper_arm.L.001", "DEF-upper_arm.L", Along(19.5f), AlongY(Arm, 37.0f));
    B.Add("DEF-forearm.L",       "DEF-upper_arm.L", Along(39.0f), AlongY(Arm, 37.0f));
    B.Add("DEF-hand.L",          "DEF-forearm.L",   Along(71.5f), AlongY(Arm, 37.0f));
    B.Add("DEF-thigh.L",         "DEF-spine",       { 8.0f, 76.0f, 0.0f }, AlongY(DOWN, 10.0f));
    B.Add("DEF-shin.L",          "DEF-thigh.L",     { 8.0f, 40.0f, 0.0f }, AlongY(DOWN, 10.0f));
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

static AnimVec3 Normalized(const AnimVec3& InV)
{
    const float Length = std::sqrt(InV.X * InV.X + InV.Y * InV.Y + InV.Z * InV.Z);
    return Length > 0.0f ? AnimMath::Scale(InV, 1.0f / Length) : InV;
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

int main()
{
    std::printf("RenderCore retarget tests\n");
    TestAsset();
    TestIdentity();
    TestRotationTransfer();
    TestPoseOffset();
    TestNotifies();
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
