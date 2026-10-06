#include "Public/GameCore/ComponentReplication.h"

#include <string>

#include "Public/Core/CoreLog.h"

/* The replication registry: a constant-initialised array indexed by
 * ComponentTypeID, as ComponentSerializer.cpp's. */

namespace GameCore
{
namespace
{
    struct ReplicationTable
    {
        ReplicatedFieldEntry Entries[REPLICATED_FIELDS_PER_TYPE_MAX];
        size_t               Count       = 0;
        bool                 bRegistered = false;
    };

    ReplicationTable GTables[COMPONENT_TYPE_MAX];
    uint32_t         GGeneration = 0;

    const char* TypeName(const ComponentTypeID InType)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        return Info ? Info->Name : "<unknown>";
    }
}

bool RegisterComponentReplication(const ComponentTypeID InType, const ReplicatedFieldEntry* InEntries,
                                  const size_t InCount, const size_t InRejected)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
    {
        CORE_LOG_ERROR("RegisterComponentReplication: component type id " + std::to_string(InType) + " is out of range")
        return false;
    }

    if(InRejected > 0)
    {
        CORE_LOG_ERROR(std::string("RegisterComponentReplication: '") + TypeName(InType) + "' has "
                       + std::to_string(InRejected) + " field(s) that could not be recorded (more than "
                       + std::to_string(REPLICATED_FIELDS_PER_TYPE_MAX) + ", or a name too long); they will not replicate")
    }

    /* First registration wins, as for serializers: two tables are two wire
     * layouts, and the schema hash would depend on load order. */
    ReplicationTable& Table = GTables[InType];
    if(Table.bRegistered)
    {
        CORE_LOG_WARN(std::string("RegisterComponentReplication: '") + TypeName(InType)
                      + "' already has a replication table; keeping the first one")
        return false;
    }

    const size_t Count = InCount < size_t(REPLICATED_FIELDS_PER_TYPE_MAX) ? InCount : size_t(REPLICATED_FIELDS_PER_TYPE_MAX);
    for(size_t Index = 0; Index < Count; ++Index)
    {
        Table.Entries[Index] = InEntries[Index];
    }
    Table.Count       = Count;
    Table.bRegistered = true;
    ++GGeneration;
    return true;
}

void UnregisterComponentReplication(const ComponentTypeID InType)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX || !GTables[InType].bRegistered) { return; }
    GTables[InType] = ReplicationTable();
    ++GGeneration;
}

const ReplicatedFieldEntry* FindComponentReplication(const ComponentTypeID InType, size_t& OutCount)
{
    OutCount = 0;
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX || !GTables[InType].bRegistered) { return nullptr; }
    OutCount = GTables[InType].Count;
    return GTables[InType].Entries;
}

void ForEachReplicatedType(void (*InFn)(ComponentTypeID InType, void* InUserData), void* InUserData)
{
    if(!InFn) { return; }
    for(size_t Index = 0; Index < size_t(COMPONENT_TYPE_MAX); ++Index)
    {
        if(GTables[Index].bRegistered) { InFn(ComponentTypeID(Index), InUserData); }
    }
}

uint32_t GetNetTablesGeneration() { return GGeneration; }

/* Not exported: ComponentRpc.cpp, in this image, bumps it too. */
void BumpNetTablesGeneration() { ++GGeneration; }
}
