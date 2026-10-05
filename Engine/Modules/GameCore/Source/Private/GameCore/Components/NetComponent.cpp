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

    /* Only the authored switches: the ids are a session's. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(NetComponent, 2)
    {
        Ar.Flag("Replicate", C.bReplicate);
        if(Ar.GetVersion() >= 2) { Ar.Flag("Predicted", C.bPredicted); }
    }

    /* A client learns which entity is its own from the owner, and how far the
     * server has got through its inputs from the ack. */
    GAMECORE_DEFINE_COMPONENT_REPLICATION(NetComponent)
    {
        Rep.Field("OwnerClient", &NetComponent::OwnerClient);
        Rep.Field("InputAck",    &NetComponent::InputAck);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(NetComponent)
    {
        Bind.ReadOnly("NetId",       &NetComponent::NetId);
        Bind.ReadOnly("OwnerClient", &NetComponent::OwnerClient);
        Bind.Flag("Replicate",       &NetComponent::bReplicate);
        Bind.Flag("Predicted",       &NetComponent::bPredicted);
        Bind.ReadOnly("InputAck",    &NetComponent::InputAck);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(NetComponent)
    {
        Props.Flag("Replicate", &NetComponent::bReplicate);
        Props.Flag("Predicted", &NetComponent::bPredicted);
    }
#endif
}
