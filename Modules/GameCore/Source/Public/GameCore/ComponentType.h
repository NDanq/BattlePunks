#ifndef GAMECORE_COMPONENTTYPE_H
#define GAMECORE_COMPONENTTYPE_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/EntityID.h"

/*
 * Forward-declared rather than #include "World.h": ComponentOwner below
 * holds a World*, and World.h needs ComponentTypeID from this file -- a
 * mutual dependency a forward declaration breaks cleanly. ComponentOwner's
 * inline helpers that need a complete World/Entity are defined at the bottom
 * of World.h and in Entity.h/Components.h instead -- see DESIGN.md.
 */
namespace GameCore { class World; class Entity; }

/*
 * Component identity: a small integer per type, assigned once, looked up by
 * name for Lua and by id everywhere else. StaticType() is an out-of-line
 * function-local static, never a template -- see DESIGN.md for why a
 * template here would give every image its own id for the same type.
 */
namespace GameCore
{
    using ComponentTypeID = uint16_t;
    enum : ComponentTypeID
    {
        COMPONENT_TYPE_INVALID = 0xFFFF,
        COMPONENT_TYPE_MAX     = 256
    };

    /* FROZEN LAYOUT, read directly by every image that includes this header
     * -- see DESIGN.md. */
    struct ComponentTypeInfo
    {
        ComponentTypeID ID        = COMPONENT_TYPE_INVALID;
        uint16_t        Alignment = 0;
        uint32_t        SizeBytes = 0;
        char            Name[48]  = {};   // fixed buffer, never std::string -- see DESIGN.md
    };

    /* Registers InName if unseen and returns its info. Called exactly once
     * per type, by that type's own StaticType() the first time anything asks
     * for it. A name collision with mismatched size/alignment is logged
     * loudly and keeps the first registration. */
    GAMECORE_API const ComponentTypeInfo& RegisterComponentType(const char* InName, uint32_t InSizeBytes, uint32_t InAlignment);

    /* nullptr if InName was never registered. */
    GAMECORE_API const ComponentTypeInfo* FindComponentType(const char* InName);

    /* nullptr for COMPONENT_TYPE_INVALID or an id nothing has registered yet. */
    GAMECORE_API const ComponentTypeInfo* GetComponentType(ComponentTypeID InID);

    GAMECORE_API size_t GetComponentTypeCount();

    /*
     * The first 16 bytes of EVERY component: who it belongs to. Written by
     * World::AddComponentRaw immediately after copying the caller's init
     * bytes -- the ONE place this is ever set. Composition, not inheritance,
     * so a component stays standard-layout. See DESIGN.md for why this is a
     * member rather than a lookup, and why storing a World* here is safe.
     */
    struct ComponentOwner
    {
        /* Named OwningWorld, not World, to avoid shadowing the type name
         * World for later uses in this class -- see DESIGN.md. */
        GameCore::World* OwningWorld = nullptr;
        EntityID         Entity;

        /* Declared here, defined in Entity.h (after both World and Entity
         * are complete) -- the return type is qualified because this struct
         * already has a member named Entity; see DESIGN.md. */
        inline ::GameCore::Entity GetEntity() const;

        /* T* is invalidated by any Add or Remove of T on this entity's
         * pool -- do not hold the result past the next mutation. */
        template<typename T> inline T*   GetSibling() const;
        template<typename T> inline bool HasSibling() const;

        /* "" if there is no NameComponent, never null. Defined in
         * Components.h, after NameComponent exists. */
        inline const char* GetName() const;
    };

    /* Every component struct begins with this macro's expansion -- declares
     * the Owner header field and the StaticType() this type registers
     * itself through. */
    #define GAMECORE_COMPONENT_BODY()                                        \
        ::GameCore::ComponentOwner Owner;                                    \
        static const ::GameCore::ComponentTypeInfo& StaticType();

    /* Defines StaticType() out-of-line -- exactly once, in exactly one .cpp.
     * static_assert on trivial copyability is ComponentPool.h's
     * memcpy-on-swap-remove requirement; static_assert on Owner's offset is
     * what lets the pool write the header blindly without knowing TypeName. */
    #define GAMECORE_DEFINE_COMPONENT(TypeName, NameLiteral)                                            \
        static_assert(std::is_trivially_copyable<TypeName>::value,                                      \
                      "components must be trivially copyable -- see ComponentPool.h's memcpy contract"); \
        static_assert(offsetof(TypeName, Owner) == 0,                                                    \
                      "GAMECORE_COMPONENT_BODY() must be the first thing declared in the struct");       \
        const ::GameCore::ComponentTypeInfo& TypeName::StaticType()                                      \
        {                                                                                                \
            static const ::GameCore::ComponentTypeInfo& Info =                                          \
                ::GameCore::RegisterComponentType(NameLiteral, sizeof(TypeName), alignof(TypeName));     \
            return Info;                                                                                 \
        }
}

#endif //GAMECORE_COMPONENTTYPE_H
