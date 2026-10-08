#ifndef RENDERCORE_ANIMATIONCOMPONENT_H
#define RENDERCORE_ANIMATIONCOMPONENT_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    enum : size_t { ANIMATION_PATH_MAX           = 128 };   // including the NUL
    enum : size_t { ANIMATION_PARAMETERS_MAX     = 12 };
    enum : size_t { ANIMATION_PARAMETER_NAME_MAX = 32 };    // including the NUL

    /* One parameter its anim state reads: a float, or a bool as 0 / 1, or a
     * trigger -- 1 until a transition uses it. */
    struct AnimationParameter
    {
        char  Name[ANIMATION_PARAMETER_NAME_MAX] = {};   // "" = unused
        float Value = 0.0f;
    };

    /*
     * "Animation". Animates the skinned mesh of the Render component on the
     * same entity (ANIMATION.md): its skeleton (the mesh's .nmskel) posed by
     * Animation each game tick (TP_PostUpdate), and drawn in that pose.
     * Animation is an .nmanim -- one clip, played from the start, looping as
     * the clip says -- or an .nmanimstate, an anim state: states, blends and
     * layers driven by Parameters. In the editor, outside Play, it shows the
     * first frame.
     *
     * Parameters are what gameplay tells an anim state (Speed, Armed,
     * Jump...); set them from Lua (animation:SetFloat("Speed", 300)) or C++.
     * The pose itself lives with the world's animation system, not here --
     * GetBoneLocation and the editor ask it.
     */
    struct RENDERCORE_API AnimationComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    Animation[ANIMATION_PATH_MAX] = {};   // .nmanim or .nmanimstate
        float   PlayRate = 1.0f;
        uint8_t bPaused  = 0;
        AnimationParameter Parameters[ANIMATION_PARAMETERS_MAX] = {};

        /* InName's value; InDefault if it has none. */
        float GetFloat(const char* InName, float InDefault = 0.0f) const;
        /* False if InName is empty or too long, or every slot is in use. */
        bool  SetFloat(const char* InName, float InValue);
        bool  GetBool(const char* InName) const       { return GetFloat(InName) != 0.0f; }
        bool  SetBool(const char* InName, bool bInValue) { return SetFloat(InName, bInValue ? 1.0f : 0.0f); }
        /* A trigger: set until a transition consumes it. */
        bool  SetTrigger(const char* InName)          { return SetFloat(InName, 1.0f); }
        /* The slot InName is in; -1 if none. */
        int32_t FindParameter(const char* InName) const;
    };
}

#endif //RENDERCORE_ANIMATIONCOMPONENT_H
