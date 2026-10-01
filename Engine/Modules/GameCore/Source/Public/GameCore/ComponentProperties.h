#ifndef GAMECORE_COMPONENTPROPERTIES_H
#define GAMECORE_COMPONENTPROPERTIES_H

#include <cstddef>
#include <cstdint>
#include <cstring>     // std::memcpy

#include "Public/GameCore/ComponentScript.h"   // EScriptFieldType / ScriptFieldTypeOf
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"

/* EDITOR ONLY. Wrap every GAMECORE_DEFINE_COMPONENT_PROPERTIES in
 * `#if WITH_EDITOR`; a forgotten wrapper fails to compile in a game build. */
#if WITH_EDITOR

/*
 * Editor-visible properties: the fields a tool may show and edit. Opt-in,
 * and deliberately separate from the Lua bindings (ComponentScript.h).
 *
 *     GAMECORE_DEFINE_COMPONENT_PROPERTIES(RenderComponent)
 *     {
 *         Props.Field("Width", &RenderComponent::Width);
 *         Props.Flag("Visible", &RenderComponent::bVisible);
 *         Props.Field("World", &BoundsComponent::World, PF_ReadOnly);
 *     }
 *
 * Unlisted fields (per-run state above all) are invisible. Entries are plain
 * data (name, type, offset, size, flags), so a tool needs no C++ type.
 * Registered at static init; lookups are game thread only.
 */

namespace GameCore
{
    /* Same field types as the Lua bindings. */
    using EPropertyType = EScriptFieldType;

    enum EPropertyFlags : uint8_t
    {
        PF_None     = 0,
        PF_ReadOnly = 1 << 0,   // shown, never written by a tool

        /* Asset hints on a string field, for a picker / drag-drop. */
        PF_AssetMesh     = 1 << 1,   // a runtime-relative path to a mesh asset
        PF_AssetMaterial = 1 << 2,   // a material's name
    };

    enum : size_t
    {
        COMPONENT_PROPERTY_NAME_MAX       = 40,   // including the NUL
        COMPONENT_PROPERTIES_PER_TYPE_MAX = 32,
    };

    struct ComponentPropertyEntry
    {
        char          Name[COMPONENT_PROPERTY_NAME_MAX] = {};
        EPropertyType Type   = EPropertyType::Float;
        uint8_t       Flags  = PF_None;
        uint32_t      Offset = 0;   // from the start of the component, Owner header included
        uint32_t      Size   = 0;   // sizeof the member; a FixedString's buffer size including the NUL

        bool IsReadOnly() const { return (Flags & PF_ReadOnly) != 0; }
    };

    /* Writes a default-constructed instance into OutBytes (SizeBytes long). */
    using ComponentPropertyDefaultsFn = void (*)(void* OutBytes);

    enum EComponentPropertyTypeFlags : uint8_t
    {
        CPF_None         = 0,
        /* Hidden from "Add Component" (e.g. added and owned by a system). */
        CPF_NotAddable   = 1 << 0,
    };

    /* Called by the macro. InRejected (too many, or over-long names) is logged.
     * False on a duplicate (first kept) or a full table. */
    GAMECORE_API bool RegisterComponentProperties(ComponentTypeID InType, const ComponentPropertyEntry* InEntries,
                                                  size_t InCount, size_t InRejected,
                                                  ComponentPropertyDefaultsFn InDefaultsFn, uint8_t InTypeFlags);

    /* nullptr/0 if InType has no editor-visible properties. */
    GAMECORE_API const ComponentPropertyEntry* FindComponentProperties(ComponentTypeID InType, size_t& OutCount);

    /* InType's default-constructed bytes (SizeBytes long), for "changed"
     * markers, reset and Add Component. False if InType has no property list.
     * Never zero-fill instead: zero is not the default (see ProxyID). */
    GAMECORE_API bool GetComponentPropertyDefaults(ComponentTypeID InType, void* OutBytes);

    /* True if InType registered a property list without CPF_NotAddable. */
    GAMECORE_API bool IsComponentAddable(ComponentTypeID InType);

    /* `Props` inside GAMECORE_DEFINE_COMPONENT_PROPERTIES; registers at Commit(). */
    template<typename T>
    class ComponentPropertyBinder
    {
    public:
        template<typename TField>
        void Field(const char* InName, TField T::* InMember, const uint8_t InFlags = PF_None)
        {
            Add(InName, InMember, ScriptFieldTypeOf<TField>::Value, InFlags);
        }

        /* A uint8_t 0/1 flag, shown as a checkbox rather than a number. */
        void Flag(const char* InName, uint8_t T::* InMember, const uint8_t InFlags = PF_None)
        {
            Add(InName, InMember, EPropertyType::Flag, InFlags);
        }

        /* Keeps the type out of a tool's "Add Component" (CPF_NotAddable). */
        void NotAddable() { TypeFlags |= CPF_NotAddable; }

        void Commit() { RegisterComponentProperties(T::StaticType().ID, Entries, Count, Rejected, &Defaults, TypeFlags); }

    private:
        /* memcpy is sound: GAMECORE_DEFINE_COMPONENT asserts trivial copyability. */
        static void Defaults(void* OutBytes)
        {
            const T Fresh{};
            std::memcpy(OutBytes, &Fresh, sizeof(T));
        }

        template<typename TField>
        void Add(const char* InName, TField T::* InMember, const EPropertyType InType, const uint8_t InFlags)
        {
            size_t Length = 0;
            while(InName != nullptr && InName[Length] != '\0') { ++Length; }

            if(Count >= COMPONENT_PROPERTIES_PER_TYPE_MAX || Length == 0 || Length >= COMPONENT_PROPERTY_NAME_MAX)
            {
                ++Rejected;
                return;
            }

            ComponentPropertyEntry& Entry = Entries[Count++];
            for(size_t Index = 0; Index < Length; ++Index) { Entry.Name[Index] = InName[Index]; }

            /* Measured on a default-constructed instance. */
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&(Probe.*InMember));

            Entry.Type   = InType;
            Entry.Flags  = InFlags;
            Entry.Offset = uint32_t(Member - Base);
            Entry.Size   = uint32_t(sizeof(TField));
        }

        ComponentPropertyEntry Entries[COMPONENT_PROPERTIES_PER_TYPE_MAX];
        size_t                 Count     = 0;
        size_t                 Rejected  = 0;
        uint8_t                TypeFlags = CPF_None;
    };
}

/* Once per type, at file scope in one .cpp. The body has `Props` in scope. */
#define GAMECORE_DEFINE_COMPONENT_PROPERTIES(TypeName)                                               \
    static void TypeName##_PropertiesBody(::GameCore::ComponentPropertyBinder<TypeName>& Props);     \
    namespace                                                                                         \
    {                                                                                                 \
        struct TypeName##_PropertiesRegistrar                                                         \
        {                                                                                             \
            TypeName##_PropertiesRegistrar()                                                          \
            {                                                                                         \
                ::GameCore::ComponentPropertyBinder<TypeName> Props;                                  \
                TypeName##_PropertiesBody(Props);                                                     \
                Props.Commit();                                                                       \
            }                                                                                         \
        };                                                                                            \
        TypeName##_PropertiesRegistrar G##TypeName##_PropertiesRegistrar;                             \
    }                                                                                                 \
    static void TypeName##_PropertiesBody(::GameCore::ComponentPropertyBinder<TypeName>& Props)

#endif // WITH_EDITOR

#endif //GAMECORE_COMPONENTPROPERTIES_H
