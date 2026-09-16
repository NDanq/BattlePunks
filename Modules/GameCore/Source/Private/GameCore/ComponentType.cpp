#include "Public/GameCore/ComponentType.h"

#include <cstring>
#include <string>

#include "Public/GameCore/GameCoreLog.h"

/*
 * A fixed COMPONENT_TYPE_MAX-entry table, filled lazily as each component's
 * StaticType() first runs. Linear scan for name -> info, matching
 * CoreConfig.h's ConfigVar list and EventBus's name table: this runs once per
 * distinct type (the very first StaticType() call anywhere in the process
 * caches the result in a function-local static, see ComponentType.h) plus
 * whenever Lua looks a name up, never in a per-entity loop.
 *
 * REGISTRATION LIFETIME. Entries are never removed and ids are never reused --
 * unlike EntityID's generation-recycled slots, a component type is assumed to
 * exist for the life of the process. This engine has no hot-reloadable
 * component types yet (see ComponentType.h's ABI note), so there is no unload
 * case to handle today.
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

        /* Constant-initialised into .bss before any dynamic initialiser runs --
         * the same CoreConfig.h argument for GConfigHead, applied here: the
         * first StaticType() call anywhere in the process (which may itself run
         * during another type's dynamic initialisation, if a component is ever
         * a file-scope object) must find a valid, already-usable table. A
         * function-local static registry would have exactly the ordering
         * problem CoreConfig.h rejects a std::vector for. */
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
                    /* Two different types registering under the same name --
                     * genuinely a programming error (a name collision between a
                     * game's component and an engine one, most likely), logged
                     * loudly rather than silently keeping whichever won the
                     * race to register first. */
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
            /* Returns slot 0 rather than a dangling reference -- every caller
             * treats the return as a valid ComponentTypeInfo&, so there is no
             * "returned a null reference" escape hatch available here. Slot 0 is
             * always populated by the time this can be reached (something had
             * to register first to fill the table), so this is wrong data
             * rather than undefined behaviour, and the error above is what a
             * developer actually sees. */
            return GRegistry.Entries[0];
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
