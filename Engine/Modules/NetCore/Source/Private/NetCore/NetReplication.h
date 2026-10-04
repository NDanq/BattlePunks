#ifndef NETCORE_NETREPLICATION_H
#define NETCORE_NETREPLICATION_H

#include <cstdint>

#include "Public/Core/CoreArchive.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

/*
 * How component data goes on the wire. Two paths:
 *
 *   spawn   the whole component, through its serializer (ComponentSerializer.h)
 *           in a versioned DATA chunk, as a cooked world stores it: every
 *           authored field, at the version the sender has.
 *   state   one replicated field at a time (ComponentReplication.h), by type:
 *           Vector3 as three floats through its accessors, never its bytes.
 *
 * Both write an EntityID as the entity's NetId (0 for none), through the map.
 */

namespace NetCore
{
    /* NetId <-> the local EntityID, as the session knows them. */
    class INetEntityMap
    {
    public:
        virtual ~INetEntityMap() = default;
        virtual int32_t            ToNetId(GameCore::EntityID InEntity) const = 0;
        virtual GameCore::EntityID FromNetId(int32_t InNetId) const = 0;
    };

    /* InField of InComponent (the start of the component's bytes). */
    void WriteReplicatedField(EngineCore::Archive& Ar, const GameCore::ReplicatedFieldEntry& InField,
                              const void* InComponent, const INetEntityMap& InMap);
    void ReadReplicatedField(EngineCore::Archive& Ar, const GameCore::ReplicatedFieldEntry& InField,
                             void* InOutComponent, const INetEntityMap& InMap);

    /* Whether InField differs between two copies of the component. */
    bool ReplicatedFieldDiffers(const GameCore::ReplicatedFieldEntry& InField, const void* InA, const void* InB);

    /* The component through its serializer, as one DATA chunk. False if the
     * type has no serializer (it is not sent). */
    bool WriteComponentData(EngineCore::Archive& Ar, GameCore::ComponentTypeID InType, void* InComponent,
                            const INetEntityMap& InMap);

    /* Reads one DATA chunk into InOutComponent (live storage, or bytes that
     * start as the type's defaults). False if it did not decode. */
    bool ReadComponentData(EngineCore::Archive& Ar, GameCore::ComponentTypeID InType, void* InOutComponent,
                           const INetEntityMap& InMap);
}

#endif //NETCORE_NETREPLICATION_H
