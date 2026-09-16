#ifndef GAMECORE_COMPONENTTYPE_H
#define GAMECORE_COMPONENTTYPE_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/EntityID.h"

/*
 * Forward-declared here rather than #include "World.h": ComponentOwner below
 * holds a World*, and World.h in turn needs ComponentTypeID from this file for
 * ComponentPool*[COMPONENT_TYPE_MAX] -- a genuine mutual dependency that a
 * forward declaration breaks cleanly. Nothing in this header calls a World
 * method; ComponentOwner's inline helpers do, and they live at the bottom of
 * World.h instead (see the note there), once World is a complete type.
 */
namespace GameCore { class World; class Entity; }

/*
 * Component identity: a small integer per type, assigned once, looked up by
 * name for Lua and by id everywhere else.
 *
 * WHY StaticType() IS AN OUT-OF-LINE FUNCTION-LOCAL STATIC AND NOT A TEMPLATE
 *
 * A template<typename T> ComponentTypeID GetTypeID() would instantiate in every
 * image that calls it -- NimbleEngine.dll, Sandbox.exe, a future hot module --
 * and CoreMemory.h's structural rule applies again: a function-local static
 * inside a TEMPLATE is one instance PER IMAGE, not one per process, so
 * TransformComponent would register itself once per image and get a different
 * id each time, with nothing to say so. GAMECORE_DEFINE_COMPONENT expands to an
 * ORDINARY out-of-line function, defined exactly once, in exactly one .cpp, so
 * there is exactly one function-local static for that type in the whole
 * process -- the same reasoning CoreConfig.h gives for putting ConfigVar's
 * registration in EngineCore.dll rather than inline in the header.
 */
namespace GameCore
{
    using ComponentTypeID = uint16_t;
    enum : ComponentTypeID
    {
        COMPONENT_TYPE_INVALID = 0xFFFF,
        COMPONENT_TYPE_MAX     = 256
    };

    /*
     * FROZEN LAYOUT, read directly by every image that includes this header --
     * the same rule CoreMemory.h states for AllocHeader and CoreConfig.h for
     * ConfigVar. This engine has no hot-reloadable component types yet (see
     * ComponentType.cpp's registration-lifetime note), so there is no
     * MODULE_ABI_REVISION to bump today; the comment is here so that the day a
     * component type IS declared inside a hot module, whoever adds that reads
     * this first.
     */
    struct ComponentTypeInfo
    {
        ComponentTypeID ID        = COMPONENT_TYPE_INVALID;
        uint16_t        Alignment = 0;
        uint32_t        SizeBytes = 0;

        /* Fixed buffer, never std::string -- CoreConfig.h's ConfigVar::Name
         * argument, applied here: a std::string member would allocate in
         * whichever image registered the type. */
        char Name[48] = {};
    };

    /*
     * Registers InName if it has not been seen before and returns its info.
     * Called exactly once per type, by that type's own StaticType() the first
     * time anything asks for it -- lazy, and always after main() (or after
     * LoadLibrary for a module), which is what CoreModule.h's "do not allocate
     * from a file-scope constructor" rule wants: nothing here runs during
     * dynamic initialisation.
     *
     * Calling this twice with the SAME name and DIFFERENT size/alignment is a
     * programming error (two types fighting over one name) and is logged loudly
     * rather than silently keeping whichever registered first.
     */
    GAMECORE_API const ComponentTypeInfo& RegisterComponentType(const char* InName, uint32_t InSizeBytes, uint32_t InAlignment);

    /* nullptr if InName was never registered. The Lua by-name path
     * (e:AddComponent("Transform")) is the only caller that runs this on
     * anything resembling a hot path, and even there it is once per distinct
     * name a script uses, not once per call -- ScriptBindings.cpp caches the
     * result the first time a proxy metatable needs it. */
    GAMECORE_API const ComponentTypeInfo* FindComponentType(const char* InName);

    /* nullptr for COMPONENT_TYPE_INVALID or an id nothing has registered yet. */
    GAMECORE_API const ComponentTypeInfo* GetComponentType(ComponentTypeID InID);

    GAMECORE_API size_t GetComponentTypeCount();

    /*
     * The first 16 bytes of EVERY component: who it belongs to.
     *
     * Written by World::AddComponentRaw immediately after copying the caller's
     * init bytes -- the ONE place this is ever set -- and read by everything
     * else. Composition, not inheritance: a component stays standard-layout (no
     * vtable, no base-class slicing question) and the erased pool can write this
     * header at a fixed offset without knowing the derived type, the same way it
     * already knows SizeBytes and Alignment without knowing the type.
     *
     * WHY A MEMBER AND NOT A LOOKUP
     *
     * A system iterating a ComponentPool has a T& in hand and NOTHING ELSE in
     * scope -- no World*, no EntityID, unless it threads both through every
     * function signature in the module. That friction is exactly what makes
     * people reach for a cached raw pointer instead of asking properly, which is
     * the failure CorePointer.h's "a raw T* is fine to use, not to store"
     * warning already exists for. Sixteen bytes bought once, on every
     * component, buys `Bounds.Owner.GetSibling<TransformComponent>()` from
     * anywhere a component is in hand -- which is exactly the "a bounds
     * component wants the entity's mesh component" case this was asked for.
     *
     * WHY STORING A World* HERE IS SAFE, UNLIKE STORING ONE ANYWHERE ELSE
     *
     * A component only ever EXISTS inside one World's ComponentPool. It cannot
     * outlive that World (the pool is destroyed with it) and the World cannot
     * be destroyed while this component is still alive to read Owner.World --
     * so there is no dangling case to guard against, unlike a TWeakRef or a
     * cached IModule*, both of which exist because their target's lifetime is
     * genuinely independent of the holder's.
     */
    struct ComponentOwner
    {
        /*
         * Named OwningWorld, not World -- a member named World of type World*
         * would be LEGAL C++ (a pointer may share its pointee type's name) but
         * shadows the type name World for every use of the bare identifier
         * later in this class and in every inline method defined against it,
         * which makes GetSibling<T>()'s "World->GetComponent<T>()" a trap for
         * the next person editing this struct even though it compiles today.
         * Not worth the risk for saving four characters.
         */
        GameCore::World* OwningWorld = nullptr;
        EntityID         Entity;

        /*
         * Declared here, DEFINED in Entity.h (at the bottom, after both World
         * and Entity are complete types) -- neither this header nor World.h can
         * see a complete Entity without creating an include cycle. Header-only
         * either way; there is no ComponentType.cpp definition for it.
         *
         * QUALIFIED RETURN TYPE, NOT JUST STYLE: this struct already declares a
         * member named Entity (of type EntityID, right above) BEFORE this
         * point, so an unqualified `Entity GetEntity() const;` would have the
         * class-scope member hide the namespace-scope type and fail to compile
         * as "EntityID is not callable" -- the same shadowing risk OwningWorld's
         * naming above sidesteps, except here it is not avoidable by renaming
         * (Owner.Entity is the name every other file in this plan already uses)
         * so it is fixed by qualification instead.
         */
        inline ::GameCore::Entity GetEntity() const;

        /* T* is invalidated by any Add or Remove of T -- the same rule
         * World::GetComponent<T> and ComponentPool::Get already state. Do not
         * hold the result past the next mutation of T's pool. */
        template<typename T> inline T*   GetSibling() const;
        template<typename T> inline bool HasSibling() const;

        /* The most common sibling read, spelled out: "" if there is no
         * NameComponent, never null. Defined in Components.h, after
         * NameComponent exists -- ComponentType.h cannot name it without
         * creating the same cycle GetEntity() avoids. */
        inline const char* GetName() const;
    };

    /*
     * Every component struct begins with this macro's expansion -- see
     * GAMECORE_DEFINE_COMPONENT's static_assert, which refuses to compile a
     * component where it is not literally the first member. Declares both the
     * Owner header field and the StaticType() this type registers itself
     * through.
     */
    #define GAMECORE_COMPONENT_BODY()                                        \
        ::GameCore::ComponentOwner Owner;                                    \
        static const ::GameCore::ComponentTypeInfo& StaticType();

    /*
     * Defines StaticType() out-of-line -- exactly once, in exactly one .cpp,
     * for exactly one image. See the class comment above for why this cannot be
     * a template. static_assert on trivial copyability is ComponentPool.h's
     * memcpy-on-swap-remove requirement; static_assert on Owner's offset is
     * what lets the pool write the header blindly without knowing TypeName.
     */
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
