#include "Public/NimbleEngine/Ragdoll.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include "Public/RenderCore/HumanoidRig.h"
#include "Public/RenderCore/SkeletonAsset.h"

/*
 * BuildRagdollProfile: a humanoid skeleton's ragdoll, from its bones alone.
 * RAGDOLL.md has the reasoning and the sources (segment masses: Dempster via
 * Winter; joint ranges: typical adult ranges of motion, narrowed so a limp
 * body does not fold through itself).
 */

namespace NimbleEngine
{
    namespace
    {
        using RenderCore::AnimMatrix;
        using RenderCore::AnimQuat;
        using RenderCore::AnimTransform;
        using RenderCore::AnimVec3;
        namespace AM = RenderCore::AnimMath;

        constexpr float Pi = 3.14159265358979f;

        float    Dot(const AnimVec3& A, const AnimVec3& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
        AnimVec3 Cross(const AnimVec3& A, const AnimVec3& B)
        {
            return { A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X };
        }
        float    Length(const AnimVec3& A) { return std::sqrt(Dot(A, A)); }
        AnimVec3 Negate(const AnimVec3& A) { return { -A.X, -A.Y, -A.Z }; }

        AnimVec3 Normalized(const AnimVec3& A, const AnimVec3& InFallback)
        {
            const float L = Length(A);
            return L > 1.0e-8f ? AM::Scale(A, 1.0f / L) : InFallback;
        }

        /* Any unit vector perpendicular to the unit InAxis. */
        AnimVec3 AnyPerpendicular(const AnimVec3& InAxis)
        {
            const AnimVec3 Other = std::fabs(InAxis.X) < 0.9f ? AnimVec3{ 1.0f, 0.0f, 0.0f } : AnimVec3{ 0.0f, 1.0f, 0.0f };
            return Normalized(Cross(InAxis, Other), AnimVec3{ 0.0f, 0.0f, 1.0f });
        }

        /* InV with its InAxis (unit) component removed, normalised. */
        AnimVec3 Orthogonal(const AnimVec3& InV, const AnimVec3& InAxis)
        {
            const AnimVec3 Rest = AM::Sub(InV, AM::Scale(InAxis, Dot(InV, InAxis)));
            return Length(Rest) > 1.0e-6f ? Normalized(Rest, InAxis) : AnyPerpendicular(InAxis);
        }

        /* The unit InV turned InDegrees toward InToward. */
        AnimVec3 RotateToward(const AnimVec3& InV, const AnimVec3& InToward, const float InDegrees)
        {
            if(InDegrees == 0.0f) { return InV; }
            const AnimVec3 Side  = Orthogonal(InToward, InV);
            const float    Angle = InDegrees * Pi / 180.0f;
            return Normalized(AM::Add(AM::Scale(InV, std::cos(Angle)), AM::Scale(Side, std::sin(Angle))), InV);
        }

        /* The shortest rotation taking unit InFrom to unit InTo. */
        AnimQuat FromTo(const AnimVec3& InFrom, const AnimVec3& InTo)
        {
            const float Cos = Dot(InFrom, InTo);
            if(Cos < -0.99999f)
            {
                const AnimVec3 Axis = AnyPerpendicular(InFrom);
                return { Axis.X, Axis.Y, Axis.Z, 0.0f };
            }
            const AnimVec3 Axis = Cross(InFrom, InTo);
            return AM::Normalize(AnimQuat{ Axis.X, Axis.Y, Axis.Z, 1.0f + Cos });
        }

        /* A skeleton's bones at rest: where they are, and their children. */
        struct Bones
        {
            const RenderCore::SkeletonData& Skeleton;
            std::vector<AnimVec3>           Position;   // model space, at rest

            Bones(const RenderCore::SkeletonData& InSkeleton, const std::vector<AnimMatrix>& InRest)
                : Skeleton(InSkeleton)
            {
                Position.resize(InSkeleton.Bones.size());
                for(size_t Bone = 0; Bone < Position.size(); ++Bone)
                {
                    Position[Bone] = { InRest[Bone].M[0][3], InRest[Bone].M[1][3], InRest[Bone].M[2][3] };
                }
            }

            /* The average of InBone's children's positions; false if it has none. */
            bool ChildrenCentre(const int32_t InBone, AnimVec3& OutCentre) const
            {
                AnimVec3 Sum;
                int      Count = 0;
                for(size_t Bone = 0; Bone < Position.size(); ++Bone)
                {
                    if(Skeleton.Bones[Bone].Parent != InBone) { continue; }
                    Sum = AM::Add(Sum, Position[Bone]);
                    ++Count;
                }
                if(Count == 0) { return false; }
                OutCentre = AM::Scale(Sum, 1.0f / float(Count));
                return true;
            }
        };

        /* The parts being made, with the skeleton they come from. */
        struct Builder
        {
            const Bones&                 B;
            const std::vector<AnimMatrix>& Rest;
            RagdollProfile&              Out;

            int32_t Add(const char* InName, const int32_t InBone, const int32_t InParent, const float InMassShare)
            {
                RagdollProfilePart Part;
                Part.Name      = InName;
                Part.Bone      = InBone;
                Part.Parent    = InParent;
                Part.MassShare = InMassShare;
                Out.Parts.push_back(Part);
                return int32_t(Out.Parts.size()) - 1;
            }

            /* A capsule centred at InCentre along InDirection, in the part's bone's frame. */
            void Capsule(const int32_t InPart, const AnimVec3& InCentre, const AnimVec3& InDirection, const float InHalfHeight,
                         const float InRadius)
            {
                RagdollProfilePart& Part = Out.Parts[size_t(InPart)];
                const AnimTransform Bone = AM::ToTransform(Rest[size_t(Part.Bone)]);
                const AnimQuat      Into = AM::Conjugate(AM::Normalize(Bone.Rotation));
                Part.Shape      = RPS_Capsule;
                Part.Radius     = InRadius;
                Part.HalfHeight = std::max(InHalfHeight, 0.0f);
                Part.ShapeLocal.Translation = AM::Rotate(Into, AM::Sub(InCentre, Bone.Translation));
                Part.ShapeLocal.Rotation    = AM::Normalize(AM::Mul(Into, FromTo(AnimVec3{ 0.0f, 1.0f, 0.0f }, InDirection)));
            }

            /* A limb from InFrom to InTo: the capsule spans the two, its caps included. */
            void Segment(const int32_t InPart, const AnimVec3& InFrom, const AnimVec3& InTo, const float InRadius)
            {
                const AnimVec3 Along  = AM::Sub(InTo, InFrom);
                const float    Span   = std::max(::NimbleEngine::Length(Along), 1.0e-4f);
                Capsule(InPart, AM::Scale(AM::Add(InFrom, InTo), 0.5f), Normalized(Along, AnimVec3{ 0.0f, 1.0f, 0.0f }),
                        std::max(0.5f * Span - InRadius, 0.1f * Span), InRadius);
            }

            /* A swing-twist joint: InTwist the child's axis at rest, InPlane
             * the axis the PlaneHalfCone swings about; the parent's axis
             * turned InTowardDegrees toward InToward (and the second pair),
             * which centres the cone off the rest pose. */
            void Swing(const int32_t InPart, const AnimVec3& InTwist, const AnimVec3& InPlane, const float InPlaneCone,
                       const float InNormalCone, const float InTwistLimit, const float InFriction,
                       const AnimVec3& InToward = AnimVec3(), const float InTowardDegrees = 0.0f,
                       const AnimVec3& InToward2 = AnimVec3(), const float InToward2Degrees = 0.0f)
            {
                RagdollProfilePart& Part = Out.Parts[size_t(InPart)];
                Part.Joint     = RJT_SwingTwist;
                Part.TwistAxis = InTwist;
                Part.PlaneAxis = Orthogonal(InPlane, InTwist);
                AnimVec3 Centre = RotateToward(InTwist, InToward, InTowardDegrees);
                Centre = RotateToward(Centre, InToward2, InToward2Degrees);
                Part.ParentTwistAxis = Centre;
                Part.ParentPlaneAxis = Orthogonal(InPlane, Centre);
                Part.PlaneHalfCone   = InPlaneCone;
                Part.NormalHalfCone  = InNormalCone;
                Part.TwistMin        = -InTwistLimit;
                Part.TwistMax        = InTwistLimit;
                Part.FrictionTorque  = InFriction;
            }

            /* A hinge about InAxis, bending from InMin to InMax degrees (positive
             * turns the child's InNormal toward InAxis x InNormal). */
            void Hinge(const int32_t InPart, const AnimVec3& InAxis, const AnimVec3& InNormal, const float InMin, const float InMax,
                       const float InFriction)
            {
                RagdollProfilePart& Part = Out.Parts[size_t(InPart)];
                Part.Joint           = RJT_Hinge;
                Part.TwistAxis       = Part.ParentTwistAxis = InAxis;
                Part.PlaneAxis       = Part.ParentPlaneAxis = Orthogonal(InNormal, InAxis);
                Part.TwistMin        = InMin;
                Part.TwistMax        = InMax;
                Part.FrictionTorque  = InFriction;
            }
        };
    }

    bool BuildRagdollProfile(const RenderCore::SkeletonData& InSkeleton, RagdollProfile& OutProfile, std::string& OutError)
    {
        OutProfile = RagdollProfile();
        OutError.clear();
        if(InSkeleton.Bones.empty())
        {
            OutError = "the skeleton has no bones";
            return false;
        }
        InSkeleton.ComputeRefPoseModel(OutProfile.RestModel);
        const Bones B(InSkeleton, OutProfile.RestModel);
        const std::vector<AnimVec3>& P = B.Position;

        // --- Find the limbs, the spine and the head (RenderCore's HumanoidRig.h) ---
        RenderCore::HumanoidRoles Roles;
        if(!RenderCore::FindHumanoidRoles(InSkeleton, Roles, OutError))
        {
            return false;
        }
        const int32_t (&Thigh)[2]    = Roles.Thigh;
        const int32_t (&Shin)[2]     = Roles.Shin;
        const int32_t (&Foot)[2]     = Roles.Foot;
        const int32_t (&Toe)[2]      = Roles.Toe;
        const int32_t (&UpperArm)[2] = Roles.UpperArm;
        const int32_t (&Forearm)[2]  = Roles.Forearm;
        const int32_t (&Hand)[2]     = Roles.Hand;
        const int32_t Pelvis  = Roles.Pelvis;
        const int32_t Chest   = Roles.Chest;
        const int32_t Spine   = Roles.Spine;
        const int32_t Neck    = Roles.Neck;
        const int32_t HeadEnd = Roles.HeadEnd;
        const AnimVec3& Up      = Roles.Up;
        const AnimVec3& Lateral = Roles.Lateral;
        const AnimVec3& Forward = Roles.Forward;

        const float LegLength = Length(AM::Sub(P[size_t(Shin[0])], P[size_t(Thigh[0])]))
                              + (Foot[0] >= 0 ? Length(AM::Sub(P[size_t(Foot[0])], P[size_t(Shin[0])]))
                                              : Length(AM::Sub(P[size_t(Shin[0])], P[size_t(Thigh[0])])));
        const float Torso = Dot(AM::Sub(P[size_t(HeadEnd >= 0 ? HeadEnd : Chest)], P[size_t(Pelvis)]), Up);
        const float S     = std::max(LegLength + Torso, 1.0e-3f);   // ~0.93 of standing height

        // --- Parts, parents first ----------------------------------------------
        Builder Make{ B, OutProfile.RestModel, OutProfile };
        /* Dempster's segment masses (Winter, Table 4.1): pelvis .142, abdomen
         * .139, thorax .216, head and neck .081, thigh .100, leg .0465, foot
         * .0145, upper arm .028, forearm .016, hand .006. A segment without a
         * part gives its share to the next one up. */
        const int32_t PelvisPart = Make.Add("Pelvis", Pelvis, -1, Spine >= 0 ? 0.142f : 0.142f + 0.139f * 0.5f);
        const int32_t SpinePart  = Spine >= 0 ? Make.Add("Spine", Spine, PelvisPart, 0.139f) : -1;
        const int32_t ChestPart  = Make.Add("Chest", Chest, SpinePart >= 0 ? SpinePart : PelvisPart,
                                            Spine >= 0 ? 0.216f : 0.216f + 0.139f * 0.5f + (Neck >= 0 ? 0.0f : 0.081f));
        const int32_t HeadPart   = Neck >= 0 ? Make.Add("Head", Neck, ChestPart, 0.081f) : -1;

        const float HipHalf = 0.5f * Length(AM::Sub(P[size_t(Thigh[0])], P[size_t(Thigh[1])]));
        const float ArmHalf = 0.5f * Length(AM::Sub(P[size_t(UpperArm[0])], P[size_t(UpperArm[1])]));
        const AnimVec3 HipCentre = AM::Scale(AM::Add(P[size_t(Thigh[0])], P[size_t(Thigh[1])]), 0.5f);

        /* The torso: capsules across the body. */
        {
            const float Radius = 0.065f * S;
            const AnimVec3 Centre = AM::Sub(AM::Scale(AM::Add(P[size_t(Pelvis)], HipCentre), 0.5f), AM::Scale(Up, 0.02f * S));
            Make.Capsule(PelvisPart, Centre, Lateral, std::max(HipHalf, 0.25f * Radius), Radius);
        }
        const AnimVec3 NeckAt = Neck >= 0 ? P[size_t(Neck)] : AM::Add(P[size_t(Chest)], AM::Scale(Up, 0.11f * S));
        if(SpinePart >= 0)
        {
            const float Radius = 0.06f * S;
            const AnimVec3 Centre = AM::Scale(AM::Add(P[size_t(Spine)], P[size_t(Chest)]), 0.5f);
            Make.Capsule(SpinePart, Centre, Lateral, std::max(0.6f * HipHalf, 0.25f * Radius), Radius);
            Make.Swing(SpinePart, Normalized(AM::Sub(P[size_t(Chest)], P[size_t(Spine)]), Up), Lateral, 30.0f, 20.0f, 15.0f, 8.0f,
                       Forward, 10.0f);
        }
        {
            const float Radius = 0.07f * S;
            const AnimVec3 Centre = AM::Scale(AM::Add(P[size_t(Chest)], NeckAt), 0.5f);
            Make.Capsule(ChestPart, Centre, Lateral, std::max(ArmHalf - 0.5f * Radius, 0.25f * Radius), Radius);
            Make.Swing(ChestPart, Normalized(AM::Sub(NeckAt, P[size_t(Chest)]), Up), Lateral, SpinePart >= 0 ? 25.0f : 35.0f, 20.0f,
                       20.0f, 8.0f, Forward, 5.0f);
        }
        if(HeadPart >= 0)
        {
            const float    Radius = 0.06f * S;
            const AnimVec3 Base   = P[size_t(Neck)];
            const AnimVec3 Top    = HeadEnd != Neck ? AM::Add(P[size_t(HeadEnd)], AM::Scale(Up, 0.09f * S))
                                                    : AM::Add(Base, AM::Scale(Up, 0.16f * S));
            Make.Segment(HeadPart, Base, Top, Radius);
            Make.Swing(HeadPart, Normalized(AM::Sub(Top, Base), Up), Lateral, 45.0f, 35.0f, 60.0f, 2.0f, Forward, 10.0f);
        }

        const char* SideName[2] = { ".L", ".R" };
        for(int Side = 0; Side < 2; ++Side)
        {
            const AnimVec3 Out = Side == 0 ? Lateral : Negate(Lateral);   // away from the body
            const std::string Suffix = SideName[Side];

            /* Leg. */
            const AnimVec3 Hip  = P[size_t(Thigh[Side])];
            const AnimVec3 Knee = P[size_t(Shin[Side])];
            const AnimVec3 Ankle = Foot[Side] >= 0 ? P[size_t(Foot[Side])] : AM::Add(Knee, AM::Sub(Knee, Hip));
            const int32_t ThighPart = Make.Add(("Thigh" + Suffix).c_str(), Thigh[Side], PelvisPart, 0.100f);
            Make.Segment(ThighPart, Hip, Knee, 0.048f * S);
            const AnimVec3 ThighDir = Normalized(AM::Sub(Knee, Hip), Negate(Up));
            /* Forward 100, back 25; out 45, in 20. */
            Make.Swing(ThighPart, ThighDir, Lateral, 62.5f, 32.5f, 30.0f, 6.0f, Forward, 37.5f, Out, 12.5f);

            const int32_t ShinPart = Make.Add(("Shin" + Suffix).c_str(), Shin[Side], ThighPart, Foot[Side] >= 0 ? 0.0465f : 0.061f);
            Make.Segment(ShinPart, Knee, Ankle, 0.036f * S);
            const AnimVec3 ShinDir = Normalized(AM::Sub(Ankle, Knee), Negate(Up));
            /* Positive bends the foot backward: axis x shin = -forward. */
            Make.Hinge(ShinPart, Normalized(Cross(Orthogonal(Forward, ShinDir), ShinDir), Lateral), ShinDir, -3.0f, 140.0f, 3.0f);

            if(Foot[Side] >= 0)
            {
                const int32_t FootPart = Make.Add(("Foot" + Suffix).c_str(), Foot[Side], ShinPart, 0.0145f);
                const float    Radius = 0.022f * S;
                const AnimVec3 ToeAt  = Toe[Side] >= 0 ? P[size_t(Toe[Side])] : AM::Add(Ankle, AM::Scale(Forward, 0.07f * S));
                const AnimVec3 Tip    = AM::Add(AM::Add(ToeAt, AM::Scale(Forward, 0.03f * S)), AM::Scale(Up, 0.6f * Radius));
                Make.Segment(FootPart, Ankle, Tip, Radius);
                Make.Swing(FootPart, Normalized(AM::Sub(Tip, Ankle), Forward), Lateral, 35.0f, 20.0f, 10.0f, 1.0f);
            }

            /* Arm. */
            const AnimVec3 Shoulder = P[size_t(UpperArm[Side])];
            const AnimVec3 Elbow    = P[size_t(Forearm[Side])];
            const AnimVec3 Wrist    = Hand[Side] >= 0 ? P[size_t(Hand[Side])] : AM::Add(Elbow, AM::Sub(Elbow, Shoulder));
            const int32_t UpperArmPart = Make.Add(("UpperArm" + Suffix).c_str(), UpperArm[Side], ChestPart, 0.028f);
            Make.Segment(UpperArmPart, Shoulder, Elbow, 0.032f * S);
            const AnimVec3 UpperDir = Normalized(AM::Sub(Elbow, Shoulder), Out);
            /* About forward: raised and lowered, centred 18 above the rest
             * pose; about the other: forward 90, back 40. */
            Make.Swing(UpperArmPart, UpperDir, Orthogonal(Forward, UpperDir), 80.0f, 65.0f, 70.0f, 3.0f, Up, 18.0f, Forward, 25.0f);

            const int32_t ForearmPart = Make.Add(("Forearm" + Suffix).c_str(), Forearm[Side], UpperArmPart, Hand[Side] >= 0 ? 0.016f : 0.022f);
            Make.Segment(ForearmPart, Elbow, Wrist, 0.027f * S);
            const AnimVec3 ForeDir    = Normalized(AM::Sub(Wrist, Elbow), UpperDir);
            const AnimVec3 ElbowAxis  = Normalized(Cross(ForeDir, Orthogonal(Forward, ForeDir)), Up);   // positive brings the hand forward
            Make.Hinge(ForearmPart, ElbowAxis, ForeDir, -3.0f, 145.0f, 1.5f);

            if(Hand[Side] >= 0)
            {
                const int32_t HandPart = Make.Add(("Hand" + Suffix).c_str(), Hand[Side], ForearmPart, 0.006f);
                AnimVec3 Fingers;
                AnimVec3 Tip = AM::Add(Wrist, AM::Scale(ForeDir, 0.1f * S));
                if(B.ChildrenCentre(Hand[Side], Fingers))
                {
                    const AnimVec3 Reach = AM::Scale(AM::Sub(Fingers, Wrist), 2.5f);
                    const float    Long  = std::clamp(Length(Reach), 0.06f * S, 0.12f * S);
                    Tip = AM::Add(Wrist, AM::Scale(Normalized(Reach, ForeDir), Long));
                }
                Make.Segment(HandPart, Wrist, Tip, 0.022f * S);
                const AnimVec3 HandDir = Normalized(AM::Sub(Tip, Wrist), ForeDir);
                Make.Swing(HandPart, HandDir, Normalized(Cross(HandDir, Orthogonal(Forward, HandDir)), ElbowAxis), 60.0f, 30.0f, 50.0f,
                           0.5f);
            }
        }

        /* The shares to 1, and which part moves each bone. */
        float Total = 0.0f;
        for(const RagdollProfilePart& Part : OutProfile.Parts) { Total += Part.MassShare; }
        for(RagdollProfilePart& Part : OutProfile.Parts) { Part.MassShare /= Total; }
        OutProfile.BonePart.assign(InSkeleton.Bones.size(), -1);
        for(size_t Index = 0; Index < OutProfile.Parts.size(); ++Index)
        {
            OutProfile.BonePart[size_t(OutProfile.Parts[Index].Bone)] = int32_t(Index);
        }
        return true;
    }
}
