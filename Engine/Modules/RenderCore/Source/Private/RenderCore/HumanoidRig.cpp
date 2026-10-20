#include "Public/RenderCore/HumanoidRig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <unordered_map>

#include "Public/RenderCore/AnimRetarget.h"

namespace RenderCore
{
    namespace
    {
        namespace AM = AnimMath;

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

        /* A skeleton's bones at rest, with what finding roles asks of them. */
        struct Bones
        {
            const SkeletonData&      Skeleton;
            std::vector<AnimVec3>    Position;   // model space, at rest
            std::vector<std::string> Names;      // SimplifyBoneName()d
            std::vector<int32_t>     Depth;
            std::vector<int32_t>     Subtree;    // descendants, not counting itself
            float                    MidX = 0.0f;

            explicit Bones(const SkeletonData& InSkeleton)
                : Skeleton(InSkeleton)
            {
                std::vector<AnimMatrix> Rest;
                InSkeleton.ComputeRefPoseModel(Rest);
                const size_t Count = InSkeleton.Bones.size();
                Position.resize(Count);
                Names.resize(Count);
                Depth.assign(Count, 0);
                Subtree.assign(Count, 0);
                for(size_t Bone = 0; Bone < Count; ++Bone)
                {
                    Position[Bone] = { Rest[Bone].M[0][3], Rest[Bone].M[1][3], Rest[Bone].M[2][3] };
                    Names[Bone]    = SimplifyBoneName(InSkeleton.Bones[Bone].Name);
                    const int32_t Parent = InSkeleton.Bones[Bone].Parent;
                    Depth[Bone] = Parent >= 0 ? Depth[size_t(Parent)] + 1 : 0;
                    MidX += Position[Bone].X / float(Count);
                }
                /* Parents first, so children are counted before their parents are reached. */
                for(size_t Bone = Count; Bone-- > 0;)
                {
                    const int32_t Parent = InSkeleton.Bones[Bone].Parent;
                    if(Parent >= 0) { Subtree[size_t(Parent)] += Subtree[Bone] + 1; }
                }
            }

            int32_t ParentOf(const int32_t InBone) const { return Skeleton.Bones[size_t(InBone)].Parent; }

            /* InAncestor is InBone or above it. */
            bool IsAncestor(const int32_t InAncestor, int32_t InBone) const
            {
                for(; InBone >= 0; InBone = ParentOf(InBone))
                {
                    if(InBone == InAncestor) { return true; }
                }
                return false;
            }

            int32_t CommonAncestor(int32_t InA, int32_t InB) const
            {
                while(InA >= 0 && !IsAncestor(InA, InB)) { InA = ParentOf(InA); }
                return InA;
            }

            /*
             * The shallowest bone named like one of InWords and none of
             * InNot, below InUnder (when >= 0), on InSide (+1: the side of
             * positive X, -1 the other, 0 either); -1 if none.
             */
            int32_t Find(const std::vector<const char*>& InWords, const std::vector<const char*>& InNot,
                         const int32_t InUnder, const int InSide) const
            {
                int32_t Best = -1;
                for(size_t Bone = 0; Bone < Names.size(); ++Bone)
                {
                    const std::string& Name = Names[Bone];
                    const auto Has = [&Name](const char* InWord) { return Name.find(InWord) != std::string::npos; };
                    if(std::none_of(InWords.begin(), InWords.end(), Has) || std::any_of(InNot.begin(), InNot.end(), Has))
                    {
                        continue;
                    }
                    if(InUnder >= 0 && (int32_t(Bone) == InUnder || !IsAncestor(InUnder, int32_t(Bone)))) { continue; }
                    if(InSide > 0 && Position[Bone].X <= MidX) { continue; }
                    if(InSide < 0 && Position[Bone].X >= MidX) { continue; }
                    if(Best < 0 || Depth[Bone] < Depth[size_t(Best)]) { Best = int32_t(Bone); }
                }
                return Best;
            }

            /* InBone's child with the most below it (on InAvoid's side: not an
             * ancestor of InAvoidA or InAvoidB); -1 if none. */
            int32_t LongestChild(const int32_t InBone, const int32_t InAvoidA = -1, const int32_t InAvoidB = -1,
                                 const AnimVec3* InPrefer = nullptr) const
            {
                int32_t Best = -1;
                float   BestRise = 0.0f;
                for(size_t Bone = 0; Bone < Names.size(); ++Bone)
                {
                    if(ParentOf(int32_t(Bone)) != InBone) { continue; }
                    if((InAvoidA >= 0 && IsAncestor(int32_t(Bone), InAvoidA)) || (InAvoidB >= 0 && IsAncestor(int32_t(Bone), InAvoidB)))
                    {
                        continue;
                    }
                    const float Rise = InPrefer ? Dot(AM::Sub(Position[Bone], Position[size_t(InBone)]), *InPrefer) : 0.0f;
                    if(Best < 0 || Subtree[Bone] > Subtree[size_t(Best)] || (Subtree[Bone] == Subtree[size_t(Best)] && Rise > BestRise))
                    {
                        Best     = int32_t(Bone);
                        BestRise = Rise;
                    }
                }
                return Best;
            }
        };

        const std::vector<const char*> NotTwist = { "twist", "roll", "ik", "pole", "target" };

        std::vector<const char*> Join(std::vector<const char*> InA, const std::vector<const char*>& InB)
        {
            InA.insert(InA.end(), InB.begin(), InB.end());
            return InA;
        }

        bool Contains(const std::string& InName, const char* InWord) { return InName.find(InWord) != std::string::npos; }

        /* A twist or helper bone: matched by name only. */
        bool IsTwist(const std::string& InSimplified)
        {
            if(std::any_of(NotTwist.begin(), NotTwist.end(), [&](const char* InWord) { return Contains(InSimplified, InWord); })) { return true; }
            /* Rigify's numbered segments: "upper_arm.l.001". */
            const size_t Dot = InSimplified.find_last_of('.');
            return Dot != std::string::npos && Dot + 4 == InSimplified.size()
                && std::all_of(InSimplified.begin() + std::ptrdiff_t(Dot) + 1, InSimplified.end(), [](const char InC) { return InC >= '0' && InC <= '9'; })
                && (Contains(InSimplified, "arm") || Contains(InSimplified, "thigh") || Contains(InSimplified, "shin") || Contains(InSimplified, "leg"));
        }

        /* A finger's chain under InHand: the bones named InWord, shallowest first. */
        std::vector<int32_t> FingerChain(const Bones& InBones, const int32_t InHand, const char* InWord)
        {
            std::vector<int32_t> Chain;
            if(InHand < 0) { return Chain; }
            for(size_t Bone = 0; Bone < InBones.Names.size(); ++Bone)
            {
                if(int32_t(Bone) != InHand && InBones.IsAncestor(InHand, int32_t(Bone)) && Contains(InBones.Names[Bone], InWord)
                   && !IsTwist(InBones.Names[Bone]))
                {
                    Chain.push_back(int32_t(Bone));
                }
            }
            std::stable_sort(Chain.begin(), Chain.end(), [&](const int32_t InA, const int32_t InB) { return InBones.Depth[size_t(InA)] < InBones.Depth[size_t(InB)]; });
            return Chain;
        }
    }

    std::string SimplifyBoneName(const std::string& InName)
    {
        std::string Name = InName;
        const size_t Colon = Name.find_last_of(':');
        if(Colon != std::string::npos) { Name = Name.substr(Colon + 1); }
        for(char& Each : Name) { Each = char(std::tolower(static_cast<unsigned char>(Each))); }
        for(const char* Prefix : { "def-", "def_", "org-", "org_", "b_", "bip01 ", "bip01_", "bip001 ", "bip001_" })
        {
            const size_t Length = std::string(Prefix).size();
            if(Name.compare(0, Length, Prefix) == 0) { Name = Name.substr(Length); }
        }
        return Name;
    }

    std::string NormalizeBoneName(const std::string& InName)
    {
        /* Tokens between separators; a side token, or a word starting "left" / "right", says the side. */
        const std::string Simple = SimplifyBoneName(InName);
        std::string Side, Rest, Token;
        const auto Flush = [&]()
        {
            if(Token == "l" || Token == "left")       { Side = "l"; }
            else if(Token == "r" || Token == "right") { Side = "r"; }
            else if(Token.compare(0, 4, "left") == 0)  { Side = "l"; Rest += Token.substr(4); }
            else if(Token.compare(0, 5, "right") == 0) { Side = "r"; Rest += Token.substr(5); }
            else                                       { Rest += Token; }
            Token.clear();
        };
        for(const char Each : Simple)
        {
            if(Each == '.' || Each == '_' || Each == '-' || Each == ' ') { Flush(); }
            else                                                         { Token += Each; }
        }
        Flush();
        return Side + ":" + Rest;
    }

    bool FindHumanoidRoles(const SkeletonData& InSkeleton, HumanoidRoles& OutRoles, std::string& OutError)
    {
        OutRoles = HumanoidRoles();
        OutError.clear();
        if(InSkeleton.Bones.empty())
        {
            OutError = "the skeleton has no bones";
            return false;
        }
        const Bones B(InSkeleton);
        const std::vector<AnimVec3>& P = B.Position;

        // --- The limbs ------------------------------------------------------
        const std::vector<const char*> ThighWords   = { "thigh", "upleg", "upperleg", "upper_leg", "leg_upper", "upper leg" };
        const std::vector<const char*> ShinWords    = { "shin", "calf", "lowerleg", "lower_leg", "leg_lower", "lower leg", "knee", "leg" };
        const std::vector<const char*> FootWords    = { "foot", "ankle" };
        const std::vector<const char*> UpperArmWords = { "upper_arm", "upperarm", "uparm", "arm_upper", "upper arm", "arm" };
        const std::vector<const char*> ForearmWords = { "forearm", "lowerarm", "lower_arm", "arm_lower", "lower arm", "elbow" };
        const std::vector<const char*> HandWords    = { "hand", "wrist" };
        const std::vector<const char*> NotUpperLeg  = Join(NotTwist, { "upleg", "upperleg", "upper_leg", "leg_upper", "upper leg", "thigh" });
        const std::vector<const char*> NotUpperArm  = Join(NotTwist, { "forearm", "lowerarm", "lower_arm", "arm_lower", "lower arm", "armature", "hand" });

        std::string Missing;
        for(int Side = 0; Side < 2; ++Side)
        {
            const int Sign   = Side == 0 ? 1 : -1;
            const char* Name = Side == 0 ? ".L" : ".R";
            OutRoles.Thigh[Side]    = B.Find(ThighWords, NotTwist, -1, Sign);
            OutRoles.Shin[Side]     = OutRoles.Thigh[Side] >= 0 ? B.Find(ShinWords, NotUpperLeg, OutRoles.Thigh[Side], Sign) : -1;
            OutRoles.Foot[Side]     = OutRoles.Shin[Side] >= 0 ? B.Find(FootWords, NotTwist, OutRoles.Shin[Side], Sign) : -1;
            OutRoles.UpperArm[Side] = B.Find(UpperArmWords, NotUpperArm, -1, Sign);
            OutRoles.Forearm[Side]  = OutRoles.UpperArm[Side] >= 0 ? B.Find(ForearmWords, NotTwist, OutRoles.UpperArm[Side], Sign) : -1;
            OutRoles.Hand[Side]     = OutRoles.Forearm[Side] >= 0 ? B.Find(HandWords, NotTwist, OutRoles.Forearm[Side], Sign) : -1;
            if(OutRoles.Thigh[Side] < 0)    { Missing += std::string(" thigh") + Name; }
            if(OutRoles.Shin[Side] < 0)     { Missing += std::string(" shin") + Name; }
            if(OutRoles.UpperArm[Side] < 0) { Missing += std::string(" upper arm") + Name; }
            if(OutRoles.Forearm[Side] < 0)  { Missing += std::string(" forearm") + Name; }
        }
        if(!Missing.empty())
        {
            OutError = "not a humanoid skeleton: no" + Missing;
            return false;
        }

        const int32_t Pelvis = B.CommonAncestor(OutRoles.Thigh[0], OutRoles.Thigh[1]);
        const int32_t Chest  = B.CommonAncestor(OutRoles.UpperArm[0], OutRoles.UpperArm[1]);
        if(Pelvis < 0 || Chest < 0 || Chest == Pelvis || !B.IsAncestor(Pelvis, Chest))
        {
            OutError = "not a humanoid skeleton: the arms do not meet above where the legs do";
            return false;
        }
        OutRoles.Pelvis = Pelvis;
        OutRoles.Chest  = Chest;

        /* The spine between them: its middle bone, if there is one. */
        for(int32_t Bone = B.ParentOf(Chest); Bone >= 0 && Bone != Pelvis; Bone = B.ParentOf(Bone)) { OutRoles.SpineChain.push_back(Bone); }
        std::reverse(OutRoles.SpineChain.begin(), OutRoles.SpineChain.end());
        OutRoles.Spine = OutRoles.SpineChain.empty() ? -1 : OutRoles.SpineChain[(OutRoles.SpineChain.size() - 1) / 2];

        // --- The body's axes -----------------------------------------------------
        const AnimVec3 Up      = Normalized(AM::Sub(P[size_t(Chest)], P[size_t(Pelvis)]), AnimVec3{ 0.0f, 1.0f, 0.0f });
        const AnimVec3 Lateral = Orthogonal(AM::Sub(P[size_t(OutRoles.Thigh[0])], P[size_t(OutRoles.Thigh[1])]), Up);   // toward "L"
        AnimVec3 Forward = Normalized(Cross(Lateral, Up), AnimVec3{ 0.0f, 0.0f, 1.0f });
        for(int Side = 0; Side < 2; ++Side)
        {
            OutRoles.Toe[Side] = OutRoles.Foot[Side] >= 0 ? B.LongestChild(OutRoles.Foot[Side]) : -1;
        }
        if(OutRoles.Toe[0] >= 0 && Dot(Forward, AM::Sub(P[size_t(OutRoles.Toe[0])], P[size_t(OutRoles.Foot[0])])) < 0.0f)
        {
            /* The toes say which way it faces (an import may have mirrored the rig). */
            Forward = Negate(Forward);
        }
        OutRoles.Up      = Up;
        OutRoles.Lateral = Lateral;
        OutRoles.Forward = Forward;

        /* The neck and head: the chest's longest chain that is not an arm. */
        OutRoles.Neck    = B.LongestChild(Chest, OutRoles.UpperArm[0], OutRoles.UpperArm[1], &Up);
        OutRoles.HeadEnd = OutRoles.Neck;
        while(OutRoles.HeadEnd >= 0)
        {
            OutRoles.NeckChain.push_back(OutRoles.HeadEnd);
            const int32_t Next = B.LongestChild(OutRoles.HeadEnd, -1, -1, &Up);
            if(Next < 0) { break; }
            OutRoles.HeadEnd = Next;
        }
        for(const int32_t Bone : OutRoles.NeckChain)
        {
            const std::string& Name = B.Names[size_t(Bone)];
            if(Contains(Name, "head") && !Contains(Name, "end") && !Contains(Name, "top")) { OutRoles.Head = Bone; break; }
        }
        for(size_t Index = OutRoles.NeckChain.size(); OutRoles.Head < 0 && Index-- > 0;)
        {
            if(!Contains(B.Names[size_t(OutRoles.NeckChain[Index])], "end")) { OutRoles.Head = OutRoles.NeckChain[Index]; }
        }

        /* A clavicle: what the upper arm hangs from, below the chest. */
        for(int Side = 0; Side < 2; ++Side)
        {
            const int32_t Parent = B.ParentOf(OutRoles.UpperArm[Side]);
            if(Parent >= 0 && Parent != Chest && B.IsAncestor(Chest, Parent)) { OutRoles.Clavicle[Side] = Parent; }
        }
        return true;
    }

    size_t AutoMapRetarget(const SkeletonData& InSource, const SkeletonData& InTarget, RetargetSource& InOutMap)
    {
        InOutMap.Bones.clear();
        InOutMap.RootScale = 0.0f;
        const size_t Targets = InTarget.Bones.size();
        std::vector<int32_t>              Source(Targets, -1);
        std::vector<ERetargetTranslation> Mode(Targets, ERetargetTranslation::Skeleton);
        std::vector<bool>                 Used(InSource.Bones.size(), false);
        const auto Map = [&](const int32_t InTarget, const int32_t InSource, const ERetargetTranslation InMode = ERetargetTranslation::Skeleton)
        {
            if(InTarget < 0 || InSource < 0 || Source[size_t(InTarget)] >= 0 || Used[size_t(InSource)]) { return; }
            Source[size_t(InTarget)] = InSource;
            Mode[size_t(InTarget)]   = InMode;
            Used[size_t(InSource)]   = true;
        };

        // --- 1. Humanoid roles ---------------------------------------------------
        HumanoidRoles S, T;
        std::string   Error;
        if(FindHumanoidRoles(InSource, S, Error) && FindHumanoidRoles(InTarget, T, Error))
        {
            /* A root above the pelvis drives one above the target's: root motion. */
            if(T.Pelvis != 0 && S.Pelvis != 0 && InTarget.Bones[0].Parent < 0 && InSource.Bones[0].Parent < 0)
            {
                Map(0, 0, ERetargetTranslation::AnimationScaled);
            }
            Map(T.Pelvis, S.Pelvis, ERetargetTranslation::AnimationScaled);
            /* The spine by position: one to one if the counts match, else its ends. */
            const std::vector<int32_t>& TS = T.SpineChain;
            const std::vector<int32_t>& SS = S.SpineChain;
            if(TS.size() == SS.size())
            {
                for(size_t Index = 0; Index < TS.size(); ++Index) { Map(TS[Index], SS[Index]); }
            }
            else if(!TS.empty() && !SS.empty())
            {
                Map(TS.front(), SS.front());
                if(TS.size() > 1 && SS.size() > 1) { Map(TS.back(), SS.back()); }
            }
            Map(T.Chest, S.Chest);
            if(T.Neck >= 0 && T.Neck != T.Head && S.Neck >= 0 && S.Neck != S.Head) { Map(T.Neck, S.Neck); }
            Map(T.Head, S.Head);
            const Bones SourceBones(InSource), TargetBones(InTarget);
            for(int Side = 0; Side < 2; ++Side)
            {
                Map(T.Clavicle[Side], S.Clavicle[Side]);
                Map(T.UpperArm[Side], S.UpperArm[Side]);
                Map(T.Forearm[Side], S.Forearm[Side]);
                Map(T.Hand[Side], S.Hand[Side]);
                Map(T.Thigh[Side], S.Thigh[Side]);
                Map(T.Shin[Side], S.Shin[Side]);
                Map(T.Foot[Side], S.Foot[Side]);
                Map(T.Toe[Side], S.Toe[Side]);
                for(const char* Finger : { "thumb", "index", "middle", "ring", "pinky" })
                {
                    const std::vector<int32_t> From = FingerChain(SourceBones, S.Hand[Side], Finger);
                    const std::vector<int32_t> To   = FingerChain(TargetBones, T.Hand[Side], Finger);
                    for(size_t Index = 0; Index < std::min(From.size(), To.size()); ++Index) { Map(To[Index], From[Index]); }
                }
            }
        }

        // --- 2. Exact names, 3. normalised names -----------------------------------
        std::unordered_map<std::string, int32_t> Exact, Normal;
        for(size_t Bone = 0; Bone < InSource.Bones.size(); ++Bone)
        {
            Exact.emplace(InSource.Bones[Bone].Name, int32_t(Bone));
            Normal.emplace(NormalizeBoneName(InSource.Bones[Bone].Name), int32_t(Bone));
        }
        for(size_t Bone = 0; Bone < Targets; ++Bone)
        {
            const auto Found = Exact.find(InTarget.Bones[Bone].Name);
            if(Found != Exact.end()) { Map(int32_t(Bone), Found->second); }
        }
        for(size_t Bone = 0; Bone < Targets; ++Bone)
        {
            const auto Found = Normal.find(NormalizeBoneName(InTarget.Bones[Bone].Name));
            if(Found != Normal.end()) { Map(int32_t(Bone), Found->second); }
        }

        size_t Mapped = 0;
        for(size_t Bone = 0; Bone < Targets; ++Bone)
        {
            if(Source[Bone] < 0) { continue; }
            InOutMap.Bones.push_back({ InTarget.Bones[Bone].Name, InSource.Bones[size_t(Source[Bone])].Name, Mode[Bone], AnimQuat() });
            ++Mapped;
        }
        return Mapped;
    }

    bool AlignRetargetArms(const SkeletonData& InSource, const SkeletonData& InTarget, RetargetSource& InOutMap)
    {
        HumanoidRoles T;
        std::string   Error;
        if(!FindHumanoidRoles(InTarget, T, Error))
        {
            return false;
        }
        /* Offsets worked out from the map as it was, then set together. */
        std::vector<std::pair<std::string, AnimQuat>> Offsets;
        for(int Side = 0; Side < 2; ++Side)
        {
            for(const int32_t Bone : { T.Clavicle[Side], T.UpperArm[Side], T.Forearm[Side], T.Hand[Side] })
            {
                if(Bone < 0) { continue; }
                const std::string& Name = InTarget.Bones[size_t(Bone)].Name;
                Offsets.emplace_back(Name, ComputeRetargetPoseOffset(InSource, InTarget, InOutMap, Name));
            }
        }
        for(const auto& Each : Offsets)
        {
            for(RetargetBoneMap& Bone : InOutMap.Bones)
            {
                if(Bone.Target == Each.first) { Bone.PoseOffset = Each.second; }
            }
        }
        return true;
    }
}
