#include "Public/GameCore/ComponentRpc.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentScript.h"

/* The RPC registry (an array indexed by ComponentTypeID, as the serializer
 * and replication ones) and the local sink every process starts with. */

namespace GameCore
{
    /* ComponentReplication.cpp: the generation NetCore watches covers both tables. */
    void BumpNetTablesGeneration();
}

namespace GameCore
{
namespace
{
    struct RpcTable
    {
        RpcEntry Entries[RPCS_PER_TYPE_MAX];
        size_t   Count       = 0;
        bool     bRegistered = false;
    };

    RpcTable GTables[COMPONENT_TYPE_MAX];

    const char* TypeName(const ComponentTypeID InType)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        return Info ? Info->Name : "<unknown>";
    }

    /* Runs every call here and now: what a process that is not networked is,
     * its own server. */
    class LocalRpcSink final : public IRpcSink
    {
    public:
        void Route(World& InWorld, const EntityID InEntity, const ComponentTypeID InType, const RpcEntry& InRpc,
                   const std::vector<uint8_t>& InArgs) override
        {
            InvokeRpc(InWorld, InEntity, InType, InRpc, InArgs, NET_CLIENT_NONE, false);
        }
    };

    LocalRpcSink GLocalSink;
    IRpcSink*    GSink = &GLocalSink;
}

bool RegisterComponentRpcs(const ComponentTypeID InType, const RpcEntry* InEntries, const size_t InCount,
                           const size_t InRejected)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
    {
        CORE_LOG_ERROR("RegisterComponentRpcs: component type id " + std::to_string(InType) + " is out of range")
        return false;
    }

    if(InRejected > 0)
    {
        CORE_LOG_ERROR(std::string("RegisterComponentRpcs: '") + TypeName(InType) + "' has " + std::to_string(InRejected)
                       + " RPC(s) that could not be recorded (more than " + std::to_string(RPCS_PER_TYPE_MAX)
                       + ", or a name too long); calls to them will be refused")
    }

    RpcTable& Table = GTables[InType];
    if(Table.bRegistered)
    {
        CORE_LOG_WARN(std::string("RegisterComponentRpcs: '") + TypeName(InType) + "' already has RPCs; keeping the first ones")
        return false;
    }

    const size_t Count = InCount < size_t(RPCS_PER_TYPE_MAX) ? InCount : size_t(RPCS_PER_TYPE_MAX);
    for(size_t Index = 0; Index < Count; ++Index) { Table.Entries[Index] = InEntries[Index]; }

    /* Sorted by name, so an RPC's index is the same in every process built
     * from the same code, whatever order the body listed them in. */
    std::sort(Table.Entries, Table.Entries + Count, [](const RpcEntry& L, const RpcEntry& R)
    {
        return std::strcmp(L.Name, R.Name) < 0;
    });
    for(size_t Index = 0; Index < Count; ++Index)
    {
        if(Index > 0 && std::strcmp(Table.Entries[Index - 1].Name, Table.Entries[Index].Name) == 0)
        {
            CORE_LOG_ERROR(std::string("RegisterComponentRpcs: '") + TypeName(InType) + "' names two RPCs '"
                           + Table.Entries[Index].Name + "'; calls will reach the first")
        }
        Table.Entries[Index].Index = uint8_t(Index);
    }
    Table.Count       = Count;
    Table.bRegistered = true;
    BumpNetTablesGeneration();
    return true;
}

void UnregisterComponentRpcs(const ComponentTypeID InType)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX || !GTables[InType].bRegistered) { return; }
    GTables[InType] = RpcTable();
    BumpNetTablesGeneration();
}

const RpcEntry* FindComponentRpcs(const ComponentTypeID InType, size_t& OutCount)
{
    OutCount = 0;
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX || !GTables[InType].bRegistered) { return nullptr; }
    OutCount = GTables[InType].Count;
    return GTables[InType].Entries;
}

const RpcEntry* FindComponentRpc(const ComponentTypeID InType, const char* InName)
{
    size_t Count = 0;
    const RpcEntry* Entries = FindComponentRpcs(InType, Count);
    if(!Entries || !InName) { return nullptr; }
    for(size_t Index = 0; Index < Count; ++Index)
    {
        if(std::strcmp(Entries[Index].Name, InName) == 0) { return &Entries[Index]; }
    }
    return nullptr;
}

// === Sinks ===================================================================

void IRpcSink::WriteEntity(World&, const EntityID InEntity, EngineCore::Archive& Ar)
{
    uint64_t Packed = InEntity.Pack();
    Ar << Packed;
}

EntityID IRpcSink::ReadEntity(World&, EngineCore::Archive& Ar)
{
    uint64_t Packed = EntityID().Pack();
    Ar << Packed;
    return EntityID::Unpack(Packed);
}

void SetRpcSink(IRpcSink* InSink) { GSink = InSink ? InSink : &GLocalSink; }

IRpcSink& GetRpcSink() { return *GSink; }

bool InvokeRpc(World& InWorld, const EntityID InEntity, const ComponentTypeID InType, const RpcEntry& InRpc,
               const std::vector<uint8_t>& InArgs, const NetClientID InSender, const bool bInRemote)
{
    void* Component = Entity(&InWorld, InEntity).GetComponentByType(InType);
    if(!Component)
    {
        CORE_LOG_WARN(std::string("RPC ") + TypeName(InType) + "." + InRpc.Name
                      + ": the entity is gone or no longer has the component; dropped")
        return false;
    }
    if(!InRpc.Invoker) { return false; }

    RpcContext Context;
    Context.OwningWorld  = &InWorld;
    Context.Entity       = InEntity;
    Context.SenderClient = InSender;
    Context.bRemote      = bInRemote;

    EngineCore::Archive Ar = EngineCore::Archive::ForReading(InArgs);
    if(!InRpc.Invoker(Component, Ar, Context))
    {
        CORE_LOG_WARN(std::string("RPC ") + TypeName(InType) + "." + InRpc.Name + ": its arguments did not decode; dropped")
        return false;
    }
    return true;
}

void ReportRpcCallError(const ComponentTypeID InType, const char* InName, const char* InReason)
{
    CORE_LOG_WARN(std::string("RPC ") + TypeName(InType) + "." + (InName ? InName : "<null>") + " not called: " + InReason)
}

// === From scripts ============================================================

bool CallRpcFromScript(ScriptCall& InCall, const int InFirstArg, World& InWorld, const EntityID InEntity,
                       const ComponentTypeID InType, const RpcEntry& InRpc)
{
    if(!InWorld.IsAlive(InEntity))
    {
        InCall.Error("the entity is not alive");
        return false;
    }
    if(!Entity(&InWorld, InEntity).HasComponentByType(InType))
    {
        InCall.Error((std::string("the entity has no ") + TypeName(InType) + " component").c_str());
        return false;
    }

    /* The same encoding RpcArgs::Traits writes from C++, one letter at a time. */
    EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
    int Arg = InFirstArg;
    for(const char* Code = InRpc.ArgSignature; *Code != '\0' && !InCall.HasError(); ++Code)
    {
        switch(*Code)
        {
            case 'f': { float    Value = InCall.GetFloat(Arg++);  Ar << Value; break; }
            case 'd': { double   Value = InCall.GetNumber(Arg++); Ar << Value; break; }
            case 'i': { int32_t  Value = InCall.GetInt(Arg++);    Ar << Value; break; }
            case 'z': { bool     Value = InCall.GetBool(Arg++);   Ar << Value; break; }
            case 'u':
            {
                const double Number = InCall.GetNumber(Arg++);
                if(!InCall.HasError() && (Number < 0.0 || Number > 4294967295.0)) { InCall.Error("an argument is out of range for uint32"); }
                uint32_t Value = uint32_t(Number);
                Ar << Value;
                break;
            }
            case 'b':
            {
                const int32_t Number = InCall.GetInt(Arg++);
                if(!InCall.HasError() && (Number < 0 || Number > 255)) { InCall.Error("an argument is out of range for uint8 (0..255)"); }
                uint8_t Value = uint8_t(Number);
                Ar << Value;
                break;
            }
            case 'v':
            {
                const EngineCore::Vector3 Value = InCall.GetVector3(Arg);
                Arg += 3;
                float X = float(Value.GetX()), Y = float(Value.GetY()), Z = float(Value.GetZ());
                Ar << X << Y << Z;
                break;
            }
            case 'c':
            {
                EngineCore::Color Value = InCall.GetColor(Arg);
                Arg += 4;
                Ar << Value.R << Value.G << Value.B << Value.A;
                break;
            }
            case 'e':
            {
                const EntityID Value = InCall.GetEntity(Arg++);
                GetRpcSink().WriteEntity(InWorld, Value, Ar);
                break;
            }
            case 's':
            {
                /* What SerializeFixedString writes; the receiver clamps to its RpcString's size. */
                std::string Value = InCall.GetString(Arg++);
                Ar << Value;
                break;
            }
            default:
                InCall.Error("the RPC takes an argument type scripts cannot send");
                break;
        }
    }
    if(InCall.HasError()) { return false; }

    /* Arg is one past the last value read (a trailing Color's alpha may be left off). */
    if(InCall.GetArgCount() >= Arg)
    {
        InCall.Error((std::string("too many arguments: ") + TypeName(InType) + "." + InRpc.Name + " takes "
                      + std::to_string(std::strlen(InRpc.ArgSignature)) + " (a Vector3 is 3 values, a Color 4)").c_str());
        return false;
    }

    GetRpcSink().Route(InWorld, InEntity, InType, InRpc, Ar.GetBytes());
    return true;
}
}
