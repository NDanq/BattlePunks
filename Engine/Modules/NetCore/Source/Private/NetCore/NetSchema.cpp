#include "Private/NetCore/NetSchema.h"

#include <algorithm>
#include <cstring>

#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentRpc.h"

namespace NetCore
{
namespace
{
    /* FNV-1a, 64-bit: stable across compilers and runs, which std::hash is not. */
    struct Fnv
    {
        uint64_t Value = 0xcbf29ce484222325ull;

        void Bytes(const void* InData, const size_t InSize)
        {
            const uint8_t* Data = static_cast<const uint8_t*>(InData);
            for(size_t Index = 0; Index < InSize; ++Index)
            {
                Value ^= Data[Index];
                Value *= 0x100000001b3ull;
            }
        }
        void Text(const char* InText) { Bytes(InText, std::strlen(InText) + 1); }
        void Byte(const uint8_t InValue) { Bytes(&InValue, 1); }
    };

    bool HasNetTables(const GameCore::ComponentTypeID InType)
    {
        size_t Count = 0;
        return GameCore::FindComponentReplication(InType, Count) != nullptr
            || GameCore::FindComponentRpcs(InType, Count) != nullptr;
    }
}

/* Both kinds of table bump GameCore's generation; a type registering for the
 * first time shows in the type count. */
void NetSchema::Refresh()
{
    if(BuiltGeneration != GameCore::GetNetTablesGeneration() || BuiltTypeCount != GameCore::GetComponentTypeCount())
    {
        Rebuild();
    }
}

void NetSchema::Rebuild()
{
    BuiltGeneration = GameCore::GetNetTablesGeneration();
    BuiltTypeCount  = GameCore::GetComponentTypeCount();

    Types.clear();
    for(size_t Index = 0; Index < GameCore::GetComponentTypeCount(); ++Index)
    {
        const GameCore::ComponentTypeID Type = GameCore::ComponentTypeID(Index);
        if(GameCore::GetComponentType(Type) && HasNetTables(Type)) { Types.push_back(Type); }
    }
    std::sort(Types.begin(), Types.end(), [](const GameCore::ComponentTypeID L, const GameCore::ComponentTypeID R)
    {
        return std::strcmp(GameCore::GetComponentType(L)->Name, GameCore::GetComponentType(R)->Name) < 0;
    });

    ToWireMap.assign(GameCore::COMPONENT_TYPE_MAX, NET_WIRE_TYPE_INVALID);
    Fnv Hasher;
    for(size_t Wire = 0; Wire < Types.size(); ++Wire)
    {
        const GameCore::ComponentTypeID Type = Types[Wire];
        ToWireMap[Type] = uint16_t(Wire);
        Hasher.Text(GameCore::GetComponentType(Type)->Name);

        size_t FieldCount = 0;
        const GameCore::ReplicatedFieldEntry* Fields = GameCore::FindComponentReplication(Type, FieldCount);
        Hasher.Byte(uint8_t(FieldCount));
        for(size_t Field = 0; Field < FieldCount; ++Field)
        {
            Hasher.Text(Fields[Field].Name);
            Hasher.Byte(uint8_t(Fields[Field].Kind));
            Hasher.Byte(uint8_t(Fields[Field].FieldType));
            Hasher.Byte(Fields[Field].Flags);
        }

        size_t RpcCount = 0;
        const GameCore::RpcEntry* Rpcs = GameCore::FindComponentRpcs(Type, RpcCount);
        Hasher.Byte(uint8_t(RpcCount));
        for(size_t Rpc = 0; Rpc < RpcCount; ++Rpc)
        {
            Hasher.Text(Rpcs[Rpc].Name);
            Hasher.Byte(uint8_t(Rpcs[Rpc].Target));
            Hasher.Text(Rpcs[Rpc].ArgSignature);
        }
    }
    Hash = Hasher.Value;
}

uint16_t NetSchema::ToWire(const GameCore::ComponentTypeID InType) const
{
    return InType < ToWireMap.size() ? ToWireMap[InType] : uint16_t(NET_WIRE_TYPE_INVALID);
}

GameCore::ComponentTypeID NetSchema::FromWire(const uint16_t InWire) const
{
    return InWire < Types.size() ? Types[InWire] : GameCore::ComponentTypeID(GameCore::COMPONENT_TYPE_INVALID);
}
}
