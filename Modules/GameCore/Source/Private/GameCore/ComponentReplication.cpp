#include "Public/GameCore/ComponentReplication.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/Math/Vector.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

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

namespace GameCore
{
bool QuantizeReplicatedFields(const ComponentTypeID InType, void* InComponent)
{
    size_t Count = 0;
    const ReplicatedFieldEntry* Fields = FindComponentReplication(InType, Count);
    bool bAny = false;
    for(size_t Index = 0; Index < Count && InComponent; ++Index)
    {
        const ReplicatedFieldEntry& Field = Fields[Index];
        if(Field.Quantize == RQ_None) { continue; }
        bAny = true;
        char* At = static_cast<char*>(InComponent) + Field.Offset;
        const auto Snap = [&Field](const float InValue)
        {
            return Field.Quantize == RQ_Angle ? ReplicationQuantize::SnapAngle(InValue) : ReplicationQuantize::SnapStep(InValue, Field.Step);
        };
        if(Field.FieldType == EScriptFieldType::Float)
        {
            float Value;
            std::memcpy(&Value, At, sizeof(Value));
            Value = Snap(Value);
            std::memcpy(At, &Value, sizeof(Value));
        }
        else if(Field.FieldType == EScriptFieldType::Vector3)
        {
            EngineCore::Vector3 Value;
            std::memcpy(&Value, At, sizeof(Value));
            Value = EngineCore::Vector3(Snap(float(Value.GetX())), Snap(float(Value.GetY())), Snap(float(Value.GetZ())));
            std::memcpy(At, &Value, sizeof(Value));
        }
    }
    return bAny;
}
}

namespace GameCore
{
namespace
{
    /* The types with a quantized field, as of a table generation. */
    struct QuantizedTypes
    {
        uint32_t                     Generation = 0xFFFFFFFFu;
        std::vector<ComponentTypeID> Types;
        std::vector<uint8_t>         ByType;
    };
    QuantizedTypes& GetQuantizedTypes()
    {
        static QuantizedTypes Cache;
        if(Cache.Generation == GetNetTablesGeneration()) { return Cache; }
        Cache.Types.clear();
        Cache.ByType.assign(GetComponentTypeCount(), 0);
        ForEachReplicatedType([](const ComponentTypeID InType, void* InUser)
        {
            QuantizedTypes& Out = *static_cast<QuantizedTypes*>(InUser);
            size_t Count = 0;
            const ReplicatedFieldEntry* Fields = FindComponentReplication(InType, Count);
            for(size_t Index = 0; Index < Count; ++Index)
            {
                if(Fields[Index].Quantize == RQ_None) { continue; }
                Out.Types.push_back(InType);
                if(size_t(InType) < Out.ByType.size()) { Out.ByType[InType] = 1; }
                break;
            }
        }, &Cache);
        Cache.Generation = GetNetTablesGeneration();
        return Cache;
    }
}

bool HasQuantizedFields(const ComponentTypeID InType)
{
    const QuantizedTypes& Cache = GetQuantizedTypes();
    return size_t(InType) < Cache.ByType.size() && Cache.ByType[InType] != 0;
}

void QuantizeEntityReplicatedFields(World& InWorld, const EntityID InEntity)
{
    const Entity Handle(&InWorld, InEntity);
    for(const ComponentTypeID Type : GetQuantizedTypes().Types)
    {
        if(void* Component = Handle.GetComponentByType(Type)) { QuantizeReplicatedFields(Type, Component); }
    }
}
}
