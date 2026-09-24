#include "Public/GameCore/ComponentProperties.h"

#include <string>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * The editor-visible property registry, and the built-in components' own
 * property lists. See ComponentProperties.h.
 */

namespace GameCore
{
namespace
{
    /* Constant-initialised, so registrars running during static init find a
     * valid table -- the same argument as ComponentScript.cpp's tables. */
    enum : size_t { PROPERTY_ENTRY_MAX = 1024 };

    struct PropertyTypeRange
    {
        uint16_t First       = 0;
        uint16_t Count       = 0;
        bool     bRegistered = false;
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
                                 const size_t InCount, const size_t InRejected)
{
    if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
    {
        GAMECORE_LOG_ERROR("RegisterComponentProperties: component type id " + std::to_string(InType) + " is out of range")
        return false;
    }

    if(InRejected > 0)
    {
        GAMECORE_LOG_ERROR(std::string("RegisterComponentProperties: '") + TypeNameOf(InType) + "' had "
                           + std::to_string(InRejected) + " property(ies) rejected -- more than "
                           + std::to_string(size_t(COMPONENT_PROPERTIES_PER_TYPE_MAX))
                           + " properties, or a name of " + std::to_string(size_t(COMPONENT_PROPERTY_NAME_MAX))
                           + "+ characters. Those will not appear in the editor.")
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

// === Built-in components ====================================================

GAMECORE_DEFINE_COMPONENT_PROPERTIES(TransformComponent)
{
    Props.Field("Position", &TransformComponent::Position);
    Props.Field("Rotation", &TransformComponent::Rotation);
    Props.Field("Scale",    &TransformComponent::Scale);
}

GAMECORE_DEFINE_COMPONENT_PROPERTIES(NameComponent)
{
    Props.Field("Name", &NameComponent::Name);
}

/* World is the refit's output, so read-only. ProxyID stays hidden. */
GAMECORE_DEFINE_COMPONENT_PROPERTIES(BoundsComponent)
{
    Props.Field("Local", &BoundsComponent::Local);
    Props.Field("World", &BoundsComponent::World, PF_ReadOnly);
    Props.Flag("Static", &BoundsComponent::bStatic);
}

/* The class is fixed at AddScript time -- renaming it here would not rebind
 * the Lua instance -- so everything but the culling hint is read-only.
 * InstanceRef stays hidden. */
GAMECORE_DEFINE_COMPONENT_PROPERTIES(ScriptComponent)
{
    Props.Field("ClassName", &ScriptComponent::ClassName, PF_ReadOnly);
    Props.Field("Phase",     &ScriptComponent::Phase,     PF_ReadOnly);
    Props.Flag("Started",    &ScriptComponent::bStarted,  PF_ReadOnly);
    Props.Flag("Errored",    &ScriptComponent::bErrored,  PF_ReadOnly);
    Props.Flag("CullOutsideActiveRegion", &ScriptComponent::bCullOutsideActiveRegion);
}
}
