#include "Public/RenderCore/Components/AnimSocketComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* AnimSocketComponent: registration and what it exposes. AnimationSystem
 * moves its entity (ANIMATION.md). */

namespace RenderCore
{
    GAMECORE_DEFINE_COMPONENT(AnimSocketComponent, "Anim Socket")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(AnimSocketComponent, 1)
    {
        Ar.Field("Bone",           C.Bone);
        Ar.Field("PositionOffset", C.PositionOffset);
        Ar.Field("RotationOffset", C.RotationOffset);
    }

    /* socket.Bone = "Spine_02" holsters; "Hand_R" draws. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(AnimSocketComponent)
    {
        Bind.Field("Bone",           &AnimSocketComponent::Bone);
        Bind.Field("PositionOffset", &AnimSocketComponent::PositionOffset);
        Bind.Field("RotationOffset", &AnimSocketComponent::RotationOffset);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(AnimSocketComponent)
    {
        Props.TypeCategory("Animation");
        Props.Category("Socket");
        Props.Field("Bone",            &AnimSocketComponent::Bone);
        Props.Field("Position Offset", &AnimSocketComponent::PositionOffset);
        Props.Field("Rotation Offset", &AnimSocketComponent::RotationOffset);
    }
#endif
}
