#include "Public/GameCore/ComponentSerializer.h"

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"

/*
 * The serializer registry and the shared field helpers. GSerializers is a
 * plain array so it is constant-initialised before any registrar runs --
 * ComponentType.cpp's GRegistry argument. Indexed by ComponentTypeID directly;
 * ids are dense and bounded by COMPONENT_TYPE_MAX.
 */

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

// === Shared field helpers ===================================================

void SerializeVector3(EngineCore::Archive& Ar, EngineCore::Vector3& InOutValue)
{
    /* Through the accessors, never GetData() -- see the header. Locals because
     * the accessors return by value. */
    float X = float(InOutValue.GetX());
    float Y = float(InOutValue.GetY());
    float Z = float(InOutValue.GetZ());

    Ar << X << Y << Z;

    if(Ar.IsLoading() && !Ar.HasError())
    {
        InOutValue.SetX(X);
        InOutValue.SetY(Y);
        InOutValue.SetZ(Z);
    }
}

void SerializeAABB(EngineCore::Archive& Ar, AABB& InOutValue)
{
    SerializeVector3(Ar, InOutValue.Min);
    SerializeVector3(Ar, InOutValue.Max);
}

void SerializeColor(EngineCore::Archive& Ar, EngineCore::Color& InOutValue)
{
    Ar << InOutValue.R << InOutValue.G << InOutValue.B << InOutValue.A;
}
}
