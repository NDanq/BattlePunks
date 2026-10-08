#include "Public/GameCore/Components/AudioListenerComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* AudioListenerComponent: registration and what it exposes to saves, Lua and
 * the editor. The hearing is Engine/Modules/Audio. */

namespace GameCore
{
    GAMECORE_DEFINE_COMPONENT(AudioListenerComponent, "Audio Listener")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(AudioListenerComponent, 1)
    {
        Ar.Flag("Active", C.bActive);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(AudioListenerComponent)
    {
        Bind.Flag("Active", &AudioListenerComponent::bActive);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(AudioListenerComponent)
    {
        Props.Flag("Active", &AudioListenerComponent::bActive);
    }
#endif // WITH_EDITOR
}
