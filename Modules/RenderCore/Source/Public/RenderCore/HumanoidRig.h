#ifndef RENDERCORE_HUMANOIDRIG_H
#define RENDERCORE_HUMANOIDRIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RetargetAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"

/*
 * Which bone of a humanoid skeleton is which -- found from names and the
 * reference pose alone, whatever the rig's naming (Mixamo's "mixamorig:",
 * Rigify's "DEF-", 3ds Max's "Bip01"). What a ragdoll profile is built from
 * (NimbleEngine's Ragdoll.h) and what retargets are auto-mapped by
 * (ANIMATION.md, "Retargeting").
 */
namespace RenderCore
{
    /* Lower case, without a namespace ("mixamorig:") or a rig layer prefix ("DEF-", "ORG-", "b_", "Bip01 "). */
    RENDERCORE_API std::string SimplifyBoneName(const std::string& InName);

    /* SimplifyBoneName with its side said one way and separators dropped:
     * "DEF-upper_arm.L", "upper_arm_l" and "Left_UpperArm" are all "l:upperarm"
     * ("LeftArm": "l:arm"); no side: ":name". */
    RENDERCORE_API std::string NormalizeBoneName(const std::string& InName);

    /* A humanoid's bones by role; -1 where it has none. Sides: [0] is the
     * side of positive model X (Rigify's "L"), [1] the other. */
    struct HumanoidRoles
    {
        int32_t Pelvis = -1;
        int32_t Spine  = -1;               // the middle of SpineChain; -1 if it is empty
        int32_t Chest  = -1;               // where the arms meet
        int32_t Neck   = -1;               // the first bone of the chest's chain that is not an arm
        int32_t Head   = -1;               // the bone named "head" in that chain, or its last that is not an end bone
        int32_t HeadEnd = -1;              // the chain's last bone
        std::vector<int32_t> SpineChain;   // between Pelvis and Chest, both left out, pelvis first
        std::vector<int32_t> NeckChain;    // Neck to HeadEnd

        int32_t Clavicle[2] = { -1, -1 };  // the upper arm's parent, below the chest
        int32_t UpperArm[2] = { -1, -1 };
        int32_t Forearm[2]  = { -1, -1 };
        int32_t Hand[2]     = { -1, -1 };
        int32_t Thigh[2]    = { -1, -1 };
        int32_t Shin[2]     = { -1, -1 };
        int32_t Foot[2]     = { -1, -1 };
        int32_t Toe[2]      = { -1, -1 };

        /* The body's axes at rest, model space: Up pelvis to chest, Lateral
         * toward [0], Forward the way the toes point. */
        AnimVec3 Up, Lateral, Forward;
    };

    /* OutRoles for InSkeleton; false, with why in OutError, if it is not a
     * humanoid (no limbs found, or the arms do not meet above the legs). */
    RENDERCORE_API bool FindHumanoidRoles(const SkeletonData& InSkeleton, HumanoidRoles& OutRoles, std::string& OutError);

    /*
     * InOutMap (its Skeleton kept) filled with which bone of InSource drives
     * each bone of InTarget, each target bone the first way it matches:
     *
     *   1. by humanoid role, when both are humanoids: pelvis (its translation
     *      AnimationScaled), spine chain by position (equal counts one to
     *      one; otherwise the ends, the middle left at its reference), chest,
     *      neck, head, clavicles, arms, legs, toes, fingers by order under the
     *      hand; a root above the pelvis from the source's, scaled too;
     *   2. by exact name;
     *   3. by NormalizeBoneName.
     *
     * Each source bone drives at most one target bone; twist bones ("twist",
     * "roll", Rigify's ".001") are matched by name only, so they usually stay
     * unmapped and follow their parents. Translations are Skeleton but for
     * the pelvis and root; RootScale is left 0 (automatic) and pose offsets
     * identity (AlignRetargetArms). Returns the number of target bones mapped.
     */
    RENDERCORE_API size_t AutoMapRetarget(const SkeletonData& InSource, const SkeletonData& InTarget, RetargetSource& InOutMap);

    /* The editor's Align Arms: each mapped clavicle, upper arm, forearm and
     * hand of InTarget given ComputeRetargetPoseOffset, so the source's
     * reference arms lie along the target's (a T pose onto an A pose). Other
     * bones keep theirs. False if InTarget is not a humanoid. */
    RENDERCORE_API bool AlignRetargetArms(const SkeletonData& InSource, const SkeletonData& InTarget, RetargetSource& InOutMap);
}

#endif //RENDERCORE_HUMANOIDRIG_H
