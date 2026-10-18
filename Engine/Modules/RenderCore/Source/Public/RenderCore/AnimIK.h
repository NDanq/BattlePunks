#ifndef RENDERCORE_ANIMIK_H
#define RENDERCORE_ANIMIK_H

#include <cstdint>

#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/AnimPose.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/SkeletonAsset.h"

/*
 * Inverse kinematics on a pose (ANIMATION.md, "IK"): a chain of three bones
 * -- hip, knee, ankle; shoulder, elbow, wrist -- turned so its end reaches a
 * point. Analytic (two bones, the law of cosines): the middle bends in the
 * plane the animation bends it in, toward Pole when the chain is straight,
 * and the bones keep their lengths. Bones between the three (twist bones)
 * ride along. What the anim state's IK chains do each frame
 * (AnimInstance) is built on this.
 */
namespace RenderCore
{
    struct AnimTwoBoneIK
    {
        int32_t  Root = -1, Mid = -1, End = -1;   // each below the one before
        AnimVec3 Target;                          // model space: where End goes
        bool     bRotation = false;               // End's model rotation set to Rotation; else kept as it was
        AnimQuat Rotation;
        AnimVec3 Pole{ 0.0f, 0.0f, 1.0f };        // model space: the bend's way when the chain is straight
        float    Weight = 1.0f;                   // 0: as it was; 1: at Target (and Rotation)
    };

    /* InOutPose with InSolve's chain turned (local rotations of Root, Mid
     * and, with bRotation, End). A target past the chain's reach is reached
     * for, straight; one nearer than it can fold is held off at that. */
    RENDERCORE_API void SolveTwoBoneIK(const SkeletonData& InSkeleton, AnimLocalPose& InOutPose, const AnimTwoBoneIK& InSolve);

    /* InBone's model matrix in InPose: its ancestors composed. */
    RENDERCORE_API AnimMatrix ComputeBoneModel(const SkeletonData& InSkeleton, const AnimLocalPose& InPose, int32_t InBone);
}

#endif //RENDERCORE_ANIMIK_H
