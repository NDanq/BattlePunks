#include "Public/RenderCore/AnimIK.h"

#include <algorithm>
#include <cmath>

namespace RenderCore
{
    namespace
    {
        namespace AM = AnimMath;

        AnimVec3 PositionOf(const AnimMatrix& InModel) { return { InModel.M[0][3], InModel.M[1][3], InModel.M[2][3] }; }
        AnimQuat RotationOf(const AnimMatrix& InModel) { return AM::ToTransform(InModel).Rotation; }

        /* InBone's local rotation set so its model rotation is InModel, under its parent as it is in InPose. */
        void SetModelRotation(const SkeletonData& InSkeleton, AnimLocalPose& InOutPose, const int32_t InBone, const AnimQuat& InModel)
        {
            const int32_t Parent = InSkeleton.Bones[size_t(InBone)].Parent;
            const AnimQuat ParentRotation = Parent >= 0 ? RotationOf(ComputeBoneModel(InSkeleton, InOutPose, Parent)) : AnimQuat();
            InOutPose[size_t(InBone)].Rotation = AM::Normalize(AM::Mul(AM::Conjugate(ParentRotation), InModel));
        }

        AnimVec3 Normalized(const AnimVec3& InV, const AnimVec3& InFallback)
        {
            const float Length = AM::Length(InV);
            return Length > 1e-6f ? AM::Scale(InV, 1.0f / Length) : InFallback;
        }

        /* InV without its InAxis (unit) part. */
        AnimVec3 Across(const AnimVec3& InV, const AnimVec3& InAxis) { return AM::Sub(InV, AM::Scale(InAxis, AM::Dot(InV, InAxis))); }
    }

    AnimMatrix ComputeBoneModel(const SkeletonData& InSkeleton, const AnimLocalPose& InPose, const int32_t InBone)
    {
        AnimMatrix Model;
        if(InBone < 0 || size_t(InBone) >= InPose.size()) { return Model; }
        /* Up the chain, then composed root first. */
        int32_t Chain[SKELETON_MAX_BONES];
        int32_t Depth = 0;
        for(int32_t At = InBone; At >= 0 && Depth < int32_t(SKELETON_MAX_BONES); At = InSkeleton.Bones[size_t(At)].Parent)
        {
            Chain[Depth++] = At;
        }
        for(int32_t Index = Depth - 1; Index >= 0; --Index)
        {
            Model = AM::Mul(Model, AM::ToMatrix(InPose[size_t(Chain[Index])]));
        }
        return Model;
    }

    void SolveTwoBoneIK(const SkeletonData& InSkeleton, AnimLocalPose& InOutPose, const AnimTwoBoneIK& InSolve)
    {
        const size_t Bones = std::min(InSkeleton.Bones.size(), InOutPose.size());
        if(InSolve.Weight <= 0.0f || InSolve.Root < 0 || InSolve.Mid < 0 || InSolve.End < 0
           || size_t(InSolve.Root) >= Bones || size_t(InSolve.Mid) >= Bones || size_t(InSolve.End) >= Bones)
        {
            return;
        }
        const float Weight = std::min(InSolve.Weight, 1.0f);
        const AnimMatrix RootModel = ComputeBoneModel(InSkeleton, InOutPose, InSolve.Root);
        const AnimMatrix MidModel  = ComputeBoneModel(InSkeleton, InOutPose, InSolve.Mid);
        const AnimMatrix EndModel  = ComputeBoneModel(InSkeleton, InOutPose, InSolve.End);
        const AnimVec3 A = PositionOf(RootModel), B = PositionOf(MidModel), C = PositionOf(EndModel);
        const AnimQuat EndRotation = RotationOf(EndModel);   // as animated: kept, or turned toward the goal's

        const float Upper = AM::Length(AM::Sub(B, A));
        const float Lower = AM::Length(AM::Sub(C, B));
        if(Upper < 1e-4f || Lower < 1e-4f) { return; }

        /* Where it reaches: the target by the weight, within what the chain can span. */
        const AnimVec3 Target  = AM::Lerp(C, InSolve.Target, Weight);
        const AnimVec3 ToGoal  = AM::Sub(Target, A);
        const AnimVec3 Toward  = Normalized(ToGoal, Normalized(AM::Sub(C, A), AnimVec3{ 0.0f, -1.0f, 0.0f }));
        const float    Reach   = std::clamp(AM::Length(ToGoal), std::fabs(Upper - Lower) + 1e-3f, Upper + Lower - 1e-3f);

        /* The bend: the way the middle sits off the line now, with a little of
         * the pole so a near-straight leg does not flick from side to side. */
        AnimVec3 Bend = AM::Add(Across(AM::Sub(B, A), Toward), AM::Scale(Across(Normalized(InSolve.Pole, AnimVec3()), Toward), 0.05f * Upper));
        if(AM::Length(Bend) < 1e-6f)
        {
            Bend = Across(std::fabs(Toward.Y) < 0.9f ? AnimVec3{ 0.0f, 1.0f, 0.0f } : AnimVec3{ 1.0f, 0.0f, 0.0f }, Toward);
        }
        Bend = Normalized(Bend, AnimVec3{ 1.0f, 0.0f, 0.0f });

        /* The middle on the circle the law of cosines gives; the end at full reach along Toward. */
        const float Cos = std::clamp((Upper * Upper + Reach * Reach - Lower * Lower) / (2.0f * Upper * Reach), -1.0f, 1.0f);
        const float Sin = std::sqrt(std::max(0.0f, 1.0f - Cos * Cos));
        const AnimVec3 NewMid = AM::Add(A, AM::Add(AM::Scale(Toward, Upper * Cos), AM::Scale(Bend, Upper * Sin)));
        const AnimVec3 NewEnd = AM::Add(A, AM::Scale(Toward, Reach));

        /* The root turned to point at the new middle; then the middle, as the root left it, at the new end. */
        const AnimQuat RootTurn = AM::FromTo(AM::Sub(B, A), AM::Sub(NewMid, A));
        SetModelRotation(InSkeleton, InOutPose, InSolve.Root, AM::Normalize(AM::Mul(RootTurn, RotationOf(RootModel))));
        const AnimMatrix MidNow = ComputeBoneModel(InSkeleton, InOutPose, InSolve.Mid);
        const AnimVec3   EndNow = PositionOf(ComputeBoneModel(InSkeleton, InOutPose, InSolve.End));
        const AnimQuat   MidTurn = AM::FromTo(AM::Sub(EndNow, PositionOf(MidNow)), AM::Sub(NewEnd, PositionOf(MidNow)));
        SetModelRotation(InSkeleton, InOutPose, InSolve.Mid, AM::Normalize(AM::Mul(MidTurn, RotationOf(MidNow))));

        /* The end: as it was in model space, or turned toward the goal's rotation. */
        const AnimQuat EndWanted = InSolve.bRotation ? AM::Nlerp(EndRotation, InSolve.Rotation, Weight) : EndRotation;
        SetModelRotation(InSkeleton, InOutPose, InSolve.End, EndWanted);
    }
}
