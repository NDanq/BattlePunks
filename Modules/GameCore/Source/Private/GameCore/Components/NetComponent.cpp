#include "Public/GameCore/Components/NetComponent.h"

#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"

/* NetComponent: registration and what it exposes. NetCore does the rest. */

namespace GameCore
{
    GAMECORE_DEFINE_COMPONENT(NetComponent, "Net")

    /* Only the authored switch: the ids are a session's. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetComponent, 1)
    {
        Ar.Flag("Replicate", C.bReplicate);
    }

    /* A client learns which entity is its own from the owner. */
    GAMECORE_DEFINE_COMPONENT_REPLICATION(NetComponent)
    {
        Rep.Field("OwnerClient", &NetComponent::OwnerClient);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(NetComponent)
    {
        Bind.ReadOnly("NetId",       &NetComponent::NetId);
        Bind.ReadOnly("OwnerClient", &NetComponent::OwnerClient);
        Bind.Flag("Replicate",       &NetComponent::bReplicate);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(NetComponent)
    {
        Props.Flag("Replicate", &NetComponent::bReplicate);
    }
#endif
}
