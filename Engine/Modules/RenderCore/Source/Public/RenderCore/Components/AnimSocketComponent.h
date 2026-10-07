#ifndef RENDERCORE_ANIMSOCKETCOMPONENT_H
#define RENDERCORE_ANIMSOCKETCOMPONENT_H

#include <cstddef>

#include "Public/GameCore/ComponentType.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/RenderCoreAPI.h"

namespace RenderCore
{
    enum : size_t { ANIM_SOCKET_BONE_MAX = 64 };   // including the NUL

    /*
     * "Anim Socket". Holds its entity on a bone of its parent's animation
     * (ANIMATION.md): each animation tick, the entity's Transform is set to
     * where Bone is in its parent's model space, then moved by the offset in
     * the bone's own frame -- a sword in a hand, or on a back while
     * holstered (swap Bone to move it). The entity's Scale is its own.
     *
     * Needs a parent with an Animation component; until it has one (or the
     * bone), the Transform is left alone.
     */
    struct RENDERCORE_API AnimSocketComponent
    {
        GAMECORE_COMPONENT_BODY()

        char                Bone[ANIM_SOCKET_BONE_MAX] = {};
        EngineCore::Vector3 PositionOffset;   // in the bone's frame
        EngineCore::Vector3 RotationOffset;   // Euler degrees, in the bone's frame
    };
}

#endif //RENDERCORE_ANIMSOCKETCOMPONENT_H
