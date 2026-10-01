#include "Public/GameCore/ComponentSerializer.h"

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Vector.h"

/* The serializer registry (a constant-initialised array indexed by
 * ComponentTypeID) and the binary ComponentArchive. */

namespace GameCore
{
namespace
{
    ComponentSerializerInfo GSerializers[COMPONENT_TYPE_MAX];
    size_t                  GSerializerCount = 0;
}

bool RegisterComponentSerializer(const ComponentTypeID InType, const uint16_t InVersion,
                                 const ComponentSerializeFn InFn,
                                 const ComponentDefaultsFn InDefaultsFn)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
    {
        CORE_LOG_ERROR("RegisterComponentSerializer: component type id " + std::to_string(InType)
                       + " is out of range")
        return false;
    }

    if(InFn == nullptr)
    {
        CORE_LOG_ERROR("RegisterComponentSerializer: null serializer for component type id "
                       + std::to_string(InType))
        return false;
    }

    /* First registration wins: two serializers for one type are two field
     * orders, and letting the last one win would make the file format depend
     * on registration order. */
    if(GSerializers[InType].Fn != nullptr)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        CORE_LOG_WARN(std::string("RegisterComponentSerializer: a serializer is already registered for '")
                      + (Info ? Info->Name : "<unknown>") + "'; keeping the first one")
        return false;
    }

    /* Only a hand registration (not the macro) can pass null here. Allowed,
     * since a zeroed start is right for an all-zero-default type, but warned. */
    if(InDefaultsFn == nullptr)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        CORE_LOG_WARN(std::string("RegisterComponentSerializer: '")
                      + (Info ? Info->Name : "<unknown>")
                      + "' has no defaults function; restored instances will start from zeroed "
                        "bytes, which is wrong for any field whose default is not zero")
    }

    GSerializers[InType].Type       = InType;
    GSerializers[InType].Version    = InVersion;
    GSerializers[InType].Fn         = InFn;
    GSerializers[InType].DefaultsFn = InDefaultsFn;
    ++GSerializerCount;
    return true;
}

const ComponentSerializerInfo* FindComponentSerializer(const ComponentTypeID InType)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX) { return nullptr; }
    return GSerializers[InType].Fn ? &GSerializers[InType] : nullptr;
}

size_t GetComponentSerializerCount() { return GSerializerCount; }

// === The binary backend =====================================================
// Positional; names are ignored, so the bytes match plain `Ar << x`.

bool     BinaryComponentArchive::IsLoading() const  { return Ar.IsLoading(); }
uint16_t BinaryComponentArchive::GetVersion() const { return Ar.GetChunkVersion(); }

void BinaryComponentArchive::Field(const char*, float& InOut)    { Ar << InOut; }
void BinaryComponentArchive::Field(const char*, double& InOut)   { Ar << InOut; }
void BinaryComponentArchive::Field(const char*, int32_t& InOut)  { Ar << InOut; }
void BinaryComponentArchive::Field(const char*, uint32_t& InOut) { Ar << InOut; }
void BinaryComponentArchive::Field(const char*, uint8_t& InOut)  { Ar << InOut; }
void BinaryComponentArchive::Field(const char*, bool& InOut)     { Ar << InOut; }
void BinaryComponentArchive::Flag(const char*, uint8_t& InOut)   { Ar << InOut; }

void BinaryComponentArchive::Field(const char*, EngineCore::Vector3& InOut)
{
    /* Through the accessors, never GetData() -- see the header. Locals because
     * the accessors return by value. */
    float X = float(InOut.GetX());
    float Y = float(InOut.GetY());
    float Z = float(InOut.GetZ());

    Ar << X << Y << Z;

    if(Ar.IsLoading() && !Ar.HasError())
    {
        InOut.SetX(X);
        InOut.SetY(Y);
        InOut.SetZ(Z);
    }
}

void BinaryComponentArchive::Field(const char* InName, AABB& InOut)
{
    Field(InName, InOut.Min);
    Field(InName, InOut.Max);
}

void BinaryComponentArchive::Field(const char*, EngineCore::Color& InOut)
{
    Ar << InOut.R << InOut.G << InOut.B << InOut.A;
}

void BinaryComponentArchive::FixedString(const char*, char* InOutBuffer, const size_t InCapacity)
{
    Ar.SerializeFixedString(InOutBuffer, InCapacity);
}

void BinaryComponentArchive::Entity(const char*, EntityID& InOut)
{
    Remap.RemapEntity(Ar, InOut);
}
}
