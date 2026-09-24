#ifndef GAMECORE_COMPONENTPROPERTIES_H
#define GAMECORE_COMPONENTPROPERTIES_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/ComponentScript.h"   // EScriptFieldType / ScriptFieldTypeOf, shared field vocabulary
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"

/*
 * Editor-visible properties: which fields of a component a tool (the editor's
 * Details panel) may show and edit. Opt-in, exactly like the Lua bindings in
 * ComponentScript.h -- and deliberately a SEPARATE list from them, so a field
 * can be editable without being scriptable and the other way round.
 *
 *     GAMECORE_DEFINE_COMPONENT_PROPERTIES(PrimitiveComponent)
 *     {
 *         Props.Field("Width", &PrimitiveComponent::Width);
 *         Props.Flag("Visible", &PrimitiveComponent::bVisible);
 *         Props.Field("World", &BoundsComponent::World, PF_ReadOnly);
 *     }
 *
 * Anything not listed is invisible to the editor -- above all the per-run
 * state (Owner, ProxyID, InstanceRef, a RenderProxyHandle).
 *
 * The description is plain data (name, type, offset, size, flags), so a tool
 * reads and writes a component through GetComponentByType() with no knowledge
 * of its C++ type. Registration happens at static initialisation (the macro
 * does it); lookups are game thread only, like the rest of GameCore.
 */

namespace GameCore
{
    /* The same field vocabulary as the Lua bindings -- one set of supported
     * types, one set of conversions for a tool to implement. */
    using EPropertyType = EScriptFieldType;

    enum EPropertyFlags : uint8_t
    {
        PF_None     = 0,
        PF_ReadOnly = 1 << 0,   // shown, never written by a tool
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

    /* Called once per type by the macro. InRejected counts properties the
     * binder could not record (too many, or an over-long name) and is logged
     * as an error. False on a duplicate registration (the first is kept) or a
     * full table. */
    GAMECORE_API bool RegisterComponentProperties(ComponentTypeID InType, const ComponentPropertyEntry* InEntries,
                                                  size_t InCount, size_t InRejected);

    /* nullptr/0 if InType has no editor-visible properties -- a normal answer. */
    GAMECORE_API const ComponentPropertyEntry* FindComponentProperties(ComponentTypeID InType, size_t& OutCount);

    /*
     * What `Props` is inside GAMECORE_DEFINE_COMPONENT_PROPERTIES. Header-only;
     * everything it records is plain data, registered at Commit().
     */
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

        void Commit() { RegisterComponentProperties(T::StaticType().ID, Entries, Count, Rejected); }

    private:
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

            /* Measured on a default-constructed instance, as ComponentScriptBinder
             * does -- every component is default-constructible and trivially
             * copyable. */
            const T Probe{};
            const char* Base   = reinterpret_cast<const char*>(&Probe);
            const char* Member = reinterpret_cast<const char*>(&(Probe.*InMember));

            Entry.Type   = InType;
            Entry.Flags  = InFlags;
            Entry.Offset = uint32_t(Member - Base);
            Entry.Size   = uint32_t(sizeof(TField));
        }

        ComponentPropertyEntry Entries[COMPONENT_PROPERTIES_PER_TYPE_MAX];
        size_t                 Count    = 0;
        size_t                 Rejected = 0;
    };
}

/*
 * One per component type, at file scope, in exactly one .cpp -- beside the
 * type's other registrations. The body that follows has `Props` (a
 * ComponentPropertyBinder<TypeName>&) in scope.
 */
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

#endif //GAMECORE_COMPONENTPROPERTIES_H
