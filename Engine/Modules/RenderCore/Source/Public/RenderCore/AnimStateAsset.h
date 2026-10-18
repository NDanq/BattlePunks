#ifndef RENDERCORE_ANIMSTATEASSET_H
#define RENDERCORE_ANIMSTATEASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace EngineCore
{
    class Archive;
}

/*
 * .nmanimstate -- an anim state: what an Animation component plays when a
 * skeleton is more than one clip (ANIMATION.md). Parameters, set by gameplay,
 * drive it; it is made of layers, each a state machine:
 *
 *   - a STATE plays one clip, or blends several along a float parameter (a
 *     1D blend space: idle at Speed 0, walk at 150, run at 400), or two (a
 *     2D blend space: walks forward, back and to each side, by speed and
 *     direction);
 *   - a TRANSITION moves from one state (or any) to another when its
 *     conditions hold -- optionally only as the state's clip ends -- cross-
 *     fading over its Duration;
 *   - states in one SYNC GROUP keep in step: a later layer's state takes
 *     the phase of the first layer playing one of its group (an upper-body
 *     walk stepping with the legs), and a transition between two of a layer's
 *     states in one group starts the new one where the old one was;
 *   - a LAYER after the first is laid over the ones before through a bone
 *     mask, at its weight (scaled by a parameter, if it names one): an
 *     upper-body layer holding a weapon while the legs walk. An ADDITIVE
 *     layer adds its clips' movement instead -- each clip relative to its
 *     first frame (or the reference pose) -- so breathing or a flinch plays
 *     over whatever the layers before are doing.
 *
 * And, after the layers, an optional LOOK-AT: a float parameter (an angle in
 * degrees, clamped to a range) turns a chain of bones -- a spine, a neck --
 * about one model-space axis, each bone taking its share of the angle. The
 * upper body pitching with the camera: gameplay sets the camera's pitch, the
 * spine bends to it, and whatever rides the chest (arms, a socketed weapon)
 * follows.
 *
 * An EngineCore::Archive of chunks:
 *
 *   NAST v1      the file
 *     APRM v1    the parameters: name, type, default
 *     ALYR v5    the layers, each with its states and transitions
 *                (v4: no sync groups; v3: no editor positions; v2: nor Blend or AdditiveBase;
 *                v1: nor 2D blend spaces -- BlendParameterY, PositionY)
 *     ALOK v1    the look-at; optional (none: no look-at)
 *     ARTG v1    the default retarget, an .nmretarget path; optional (none: none)
 *     AIKS v1    IK: its chains and the pelvis and ground settings; optional (none: no IK)
 *
 * Clips are named by runtime path; skeletons are not -- a state plays on
 * whatever skeleton its entity's mesh has, its clips binding by bone name.
 */
namespace RenderCore
{
    enum class EAnimParameterType : uint8_t
    {
        Float   = 0,
        Bool    = 1,   // 0 or 1
        Trigger = 2    // set to 1; back to 0 when a transition uses it
    };

    struct AnimStateParameter
    {
        std::string        Name;
        EAnimParameterType Type    = EAnimParameterType::Float;
        float              Default = 0.0f;
    };

    /* One clip of a state: alone, or a point of its blend space. */
    struct AnimBlendSample
    {
        std::string Clip;             // an .nmanim, by runtime path
        float       Position  = 0.0f; // where it plays fully, along the blend parameter
        float       PositionY = 0.0f; // and along BlendParameterY, in a 2D blend space
    };

    enum class EAnimLoop : uint8_t { Clip = 0, Once = 1, Loop = 2 };   // Clip: as each clip says

    struct AnimStateNode
    {
        std::string                  Name;
        std::vector<AnimBlendSample> Samples;          // one: a clip; more: a blend space
        std::string                  BlendParameter;   // a Float parameter; empty: the first sample alone
        std::string                  BlendParameterY;  // a second: a 2D blend space; empty: 1D (or none)
        float                        Speed = 1.0f;
        EAnimLoop                    Loop  = EAnimLoop::Clip;
        float                        EditorX = 0.0f, EditorY = 0.0f;   // where the anim state editor draws it; not played
        std::string                  SyncGroup;        // states of one group keep in step, across layers and transitions; "": none
    };

    enum class EAnimCompare : uint8_t
    {
        Greater = 0, Less = 1, Equal = 2, NotEqual = 3,   // a Float against Value
        IsTrue  = 4, IsFalse = 5,                         // a Bool (or a Float, as non-zero)
        Triggered = 6                                     // a Trigger; consumed when the transition fires
    };

    struct AnimCondition
    {
        std::string  Parameter;
        EAnimCompare Compare = EAnimCompare::IsTrue;
        float        Value   = 0.0f;
    };

    struct AnimTransition
    {
        int32_t  From = -1;           // a state of the layer; -1: any but To
        int32_t  To   = 0;
        float    Duration = 0.2f;     // the crossfade, seconds
        bool     bAtEnd = false;      // only as From's clip ends: within Duration of it
        std::vector<AnimCondition> Conditions;   // all must hold; none: it fires (bAtEnd: at the end)
    };

    enum class EAnimLayerBlend : uint8_t
    {
        Override = 0,   // its pose replaces what is below, by weight and mask
        Additive = 1    // its movement is added to what is below
    };

    /* What an additive layer's clips are measured from. */
    enum class EAnimAdditiveBase : uint8_t
    {
        FirstFrame    = 0,   // each clip's own first frame: it adds how it moves from there
        ReferencePose = 1    // the skeleton's reference pose
    };

    struct AnimLayer
    {
        std::string                 Name;
        EAnimLayerBlend             Blend        = EAnimLayerBlend::Override;
        EAnimAdditiveBase           AdditiveBase = EAnimAdditiveBase::FirstFrame;
        float                       Weight = 1.0f;
        std::string                 WeightParameter;   // scales Weight (clamped 0..1); empty: none
        std::vector<std::string>    MaskBones;         // each with what is below it; empty: every bone
        int32_t                     EntryState = 0;
        std::vector<AnimStateNode>  States;
        std::vector<AnimTransition> Transitions;
        float                       AnyEditorX = 0.0f, AnyEditorY = 0.0f;   // the editor's Any State node
        bool                        bEditorPlaced = false;                  // false: the editor lays the states out
    };

    /* A bone a look-at turns, and its share of the angle. */
    struct AnimLookBone
    {
        std::string Bone;
        float       Share = 0.0f;   // of the angle; a chain's shares usually add to 1
    };

    struct AnimLookAt
    {
        std::string               Parameter;         // a Float: the angle, degrees; empty: no look-at
        std::string               WeightParameter;   // scales the angle (clamped 0..1): Armed, say; empty: none
        float                     MinAngle = -45.0f; // the parameter is clamped to these, degrees
        float                     MaxAngle =  45.0f;
        float                     Axis[3]  = { 1.0f, 0.0f, 0.0f };   // model space; a positive angle turns about it right-handed
        float                     Response = 12.0f;  // how fast it follows, per second (1 - e^(-Response t)); 0: at once
        std::vector<AnimLookBone> Bones;             // root first: each is turned after its parent

        bool IsSet() const { return !Parameter.empty() && !Bones.empty(); }
    };

    /* What an IK chain's end reaches for (AnimIK.h). */
    enum class EAnimIKGoal : uint8_t
    {
        Ground = 0,   // a foot: the ground under it, traced each tick (the world's), it and the pelvis lowered or raised to it
        Bone   = 1,   // a hand: a place fixed to another bone -- the off hand on a weapon held in the other
        Script = 2    // a point gameplay sets (animation:SetIKGoal); none set: it stays as animated
    };

    /* Three bones -- hip, knee, ankle; shoulder, elbow, wrist -- turned so the
     * last reaches its goal, bending the middle the way the animation bends it. */
    struct AnimIKChain
    {
        std::string   Name;                 // what script goals name it by: "LeftFoot", "OffHand"
        std::string   Root, Mid, End;       // bones, each below the one before (twist bones between are fine)
        EAnimIKGoal   Goal = EAnimIKGoal::Ground;
        std::string   GoalBone;             // a Bone goal's bone
        AnimTransform GoalOffset;           // where End goes, in GoalBone's space
        float         Weight = 1.0f;        // 0..1
        std::string   WeightParameter;      // scales Weight (clamped 0..1): Armed, Grounded; empty: none
        float         Pole[3] = { 0.0f, 0.0f, 1.0f };   // model space: where the middle bends when the chain is straight (knees forward)
        bool          bAlignEnd = true;     // the end turned with the ground's slope (Ground) or as the goal is (Bone)
    };

    struct AnimIK
    {
        std::vector<AnimIKChain> Chains;
        std::string PelvisBone;         // lowered so feet reach ground below where the entity stands; empty: not moved
        float TraceUp   = 50.0f;        // the ground is looked for from this far above each animated foot...
        float TraceDown = 75.0f;        // ...to this far below it
        float MaxStep   = 50.0f;        // no foot (nor the pelvis) moves farther than this up or down
        float Response  = 15.0f;        // how fast feet follow the ground, per second (1 - e^(-Response t)); 0: at once

        bool IsSet() const { return !Chains.empty(); }
    };

    struct AnimStateData
    {
        std::vector<AnimStateParameter> Parameters;
        std::vector<AnimLayer>          Layers;
        AnimLookAt                      LookAt;
        std::string                     Retarget;   // an .nmretarget its clips play through when made for another
                                                    // skeleton (AnimRetarget.h); the component's Retarget wins
        AnimIK                          IK;         // after the look-at: feet on the ground, hands on their goals

        /* -1 if there is none. */
        RENDERCORE_API int32_t FindParameter(const std::string& InName) const;

        /* Why it cannot play; null if it can. Checked on load and save. */
        RENDERCORE_API const char* FindDefect() const;
    };

    constexpr const char* ANIM_STATE_EXTENSION = ".nmanimstate";

    /* Both directions. False, logged, for an invalid anim state. */
    RENDERCORE_API bool SerializeAnimState(EngineCore::Archive& Ar, AnimStateData& InOutState);

    RENDERCORE_API bool LoadAnimStateFile(const std::string& InPath, AnimStateData& OutState);

#if WITH_EDITOR
    RENDERCORE_API bool SaveAnimStateFile(const std::string& InPath, AnimStateData& InState);
#endif

    /* Loaded anim states by path; MeshAssetCache's contract. */
    class RENDERCORE_API AnimStateAssetCache
    {
    public:
        static std::shared_ptr<const AnimStateData> Find(const std::string& InPath);
        static void     Invalidate(const std::string& InPath);
        static uint64_t GetGeneration();
    };
}

#endif //RENDERCORE_ANIMSTATEASSET_H
