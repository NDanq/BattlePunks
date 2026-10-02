#include "Public/GameCore/ComponentProperties.h"

#include <string>

#include "Public/GameCore/GameCoreLog.h"

/* The editor property registry. Each built-in component's list is in its own
 * Components/ .cpp. Editor targets only. See ComponentProperties.h. */

#if WITH_EDITOR

namespace GameCore
{
namespace
{
    /* Constant-initialised, so static-init registrars find a valid table. */
    enum : size_t { PROPERTY_ENTRY_MAX = 1024 };

    struct PropertyTypeRange
    {
        uint16_t                    First       = 0;
        uint16_t                    Count       = 0;
        bool                        bRegistered = false;
        uint8_t                     TypeFlags   = CPF_None;
        ComponentPropertyDefaultsFn DefaultsFn  = nullptr;
    };

    ComponentPropertyEntry GPropertyEntries[PROPERTY_ENTRY_MAX];
    size_t                 GPropertyEntryCount = 0;
    PropertyTypeRange      GPropertyTypes[COMPONENT_TYPE_MAX];

    const char* TypeNameOf(const ComponentTypeID InType)
    {
        const ComponentTypeInfo* Info = GetComponentType(InType);
        return Info ? Info->Name : "?";
    }
}

bool RegisterComponentProperties(const ComponentTypeID InType, const ComponentPropertyEntry* InEntries,
                                 const size_t InCount, const size_t InRejected,
                                 const ComponentPropertyDefaultsFn InDefaultsFn, const uint8_t InTypeFlags)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
    {
        GAMECORE_LOG_ERROR("RegisterComponentProperties: component type id " + std::to_string(InType) + " is out of range")
        return false;
    }

    if(InRejected > 0)
    {
        GAMECORE_LOG_ERROR(std::string("RegisterComponentProperties: '") + TypeNameOf(InType) + "' had "
                           + std::to_string(InRejected) + " property(ies) or categories rejected -- more than "
                           + std::to_string(size_t(COMPONENT_PROPERTIES_PER_TYPE_MAX))
                           + " properties, a name of " + std::to_string(size_t(COMPONENT_PROPERTY_NAME_MAX))
                           + "+ characters, or a category of " + std::to_string(size_t(COMPONENT_PROPERTY_CATEGORY_MAX))
                           + "+. Those properties will not appear in the editor; a rejected category's are uncategorised.")
    }

    /* First wins, as RegisterComponentScript does. */
    if(GPropertyTypes[InType].bRegistered)
    {
        GAMECORE_LOG_WARN(std::string("RegisterComponentProperties: '") + TypeNameOf(InType)
                          + "' already has editor properties; keeping the first")
        return false;
    }

    if(GPropertyEntryCount + InCount > PROPERTY_ENTRY_MAX)
    {
        GAMECORE_LOG_ERROR(std::string("RegisterComponentProperties: the property table is full; '")
                           + TypeNameOf(InType) + "' will show no properties in the editor")
        return false;
    }

    GPropertyTypes[InType].First       = uint16_t(GPropertyEntryCount);
    GPropertyTypes[InType].Count       = uint16_t(InCount);
    GPropertyTypes[InType].bRegistered = true;
    GPropertyTypes[InType].TypeFlags   = InTypeFlags;
    GPropertyTypes[InType].DefaultsFn  = InDefaultsFn;
    for(size_t Index = 0; Index < InCount; ++Index)
    {
        GPropertyEntries[GPropertyEntryCount++] = InEntries[Index];
    }
    return true;
}

const ComponentPropertyEntry* FindComponentProperties(const ComponentTypeID InType, size_t& OutCount)
{
    OutCount = 0;
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX || !GPropertyTypes[InType].bRegistered)
    {
        return nullptr;
    }
    OutCount = GPropertyTypes[InType].Count;
    return &GPropertyEntries[GPropertyTypes[InType].First];
}

bool GetComponentPropertyDefaults(const ComponentTypeID InType, void* OutBytes)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX || OutBytes == nullptr)
    {
        return false;
    }
    const PropertyTypeRange& Range = GPropertyTypes[InType];
    if(!Range.bRegistered || Range.DefaultsFn == nullptr)
    {
        return false;
    }
    Range.DefaultsFn(OutBytes);
    return true;
}

bool IsComponentAddable(const ComponentTypeID InType)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
    {
        return false;
    }
    const PropertyTypeRange& Range = GPropertyTypes[InType];
    return Range.bRegistered && Range.DefaultsFn != nullptr && (Range.TypeFlags & CPF_NotAddable) == 0;
}
}
#endif // WITH_EDITOR
