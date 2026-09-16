#include "Public/GameCore/ComponentType.h"

#include <cstring>
#include <string>

#include "Public/GameCore/GameCoreLog.h"

/*
 * A fixed COMPONENT_TYPE_MAX-entry table, filled lazily as each component's
 * StaticType() first runs. Linear scan for name -> info, matching
 * CoreConfig.h's ConfigVar list. Entries are never removed and ids are never
 * reused -- see DESIGN.md for the registration-lifetime reasoning.
 */

namespace GameCore
{
    namespace
    {
        struct ComponentTypeRegistry
        {
            ComponentTypeInfo Entries[COMPONENT_TYPE_MAX];
            size_t            Count = 0;
        };

        /* Constant-initialised into .bss before any dynamic initialiser runs
         * -- see DESIGN.md for why this cannot be a function-local static or
         * a std::vector. */
        ComponentTypeRegistry GRegistry;
    }

    const ComponentTypeInfo& RegisterComponentType(const char* InName, const uint32_t InSizeBytes, const uint32_t InAlignment)
    {
        for(size_t Index = 0; Index < GRegistry.Count; ++Index)
        {
            ComponentTypeInfo& Existing = GRegistry.Entries[Index];
            if(std::strcmp(Existing.Name, InName) == 0)
            {
                if(Existing.SizeBytes != InSizeBytes || Existing.Alignment != InAlignment)
                {
                    GAMECORE_LOG_ERROR(std::string("RegisterComponentType: \"") + InName
                                        + "\" re-registered with a different size/alignment; "
                                          "keeping the first registration")
                }
                return Existing;
            }
        }

        if(GRegistry.Count >= COMPONENT_TYPE_MAX)
        {
            GAMECORE_LOG_ERROR(std::string("RegisterComponentType: COMPONENT_TYPE_MAX (")
                                + std::to_string(COMPONENT_TYPE_MAX) + ") reached; \""
                                + InName + "\" was not registered")
            return GRegistry.Entries[0];   // wrong data with a loud error, not a dangling reference -- see DESIGN.md
        }

        ComponentTypeInfo& NewEntry = GRegistry.Entries[GRegistry.Count];
        NewEntry.ID        = ComponentTypeID(GRegistry.Count);
        NewEntry.SizeBytes = InSizeBytes;
        NewEntry.Alignment = uint16_t(InAlignment);
        std::strncpy(NewEntry.Name, InName, sizeof(NewEntry.Name) - 1);
        NewEntry.Name[sizeof(NewEntry.Name) - 1] = '\0';

        ++GRegistry.Count;
        return NewEntry;
    }

    const ComponentTypeInfo* FindComponentType(const char* InName)
    {
        if(!InName) { return nullptr; }
        for(size_t Index = 0; Index < GRegistry.Count; ++Index)
        {
            if(std::strcmp(GRegistry.Entries[Index].Name, InName) == 0)
            {
                return &GRegistry.Entries[Index];
            }
        }
        return nullptr;
    }

    const ComponentTypeInfo* GetComponentType(const ComponentTypeID InID)
    {
        if(InID == COMPONENT_TYPE_INVALID || size_t(InID) >= GRegistry.Count)
        {
            return nullptr;
        }
        return &GRegistry.Entries[InID];
    }

    size_t GetComponentTypeCount()
    {
        return GRegistry.Count;
    }
}
