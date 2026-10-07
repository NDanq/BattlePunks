#ifndef RENDERCORE_ANIMSTATEASSET_H
#define RENDERCORE_ANIMSTATEASSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
 *   - a LAYER after the first is laid over the ones before through a bone
 *     mask, at its weight (scaled by a parameter, if it names one): an
 *     upper-body layer holding a weapon while the legs walk. An ADDITIVE
 *     layer adds its clips' movement instead -- each clip relative to its
 *     first frame (or the reference pose) -- so breathing or a flinch plays
 *     over whatever the layers before are doing.
 *
 * An EngineCore::Archive of chunks:
 *
 *   NAST v1      the file
 *     APRM v1    the parameters: name, type, default
 *     ALYR v4    the layers, each with its states and transitions
 *                (v3: no editor positions; v2: nor Blend or AdditiveBase;
 *                v1: nor 2D blend spaces -- BlendParameterY, PositionY)
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

    struct AnimStateData
    {
        std::vector<AnimStateParameter> Parameters;
        std::vector<AnimLayer>          Layers;

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
