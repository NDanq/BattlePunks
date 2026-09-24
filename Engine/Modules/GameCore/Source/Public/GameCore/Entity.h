#ifndef GAMECORE_ENTITY_H
#define GAMECORE_ENTITY_H

#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/World.h"

/*
 * The handle: a World* and an EntityID, copied freely. THE way to add, get,
 * query or remove a component -- World has no per-entity component calls of
 * its own, only the private erased path Entity forwards to. Header-only, not
 * GAMECORE_API -- see DESIGN.md for why, and for why ComponentOwner does not
 * store an Entity directly.
 */
namespace GameCore
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(World* InWorld, EntityID InID) : OwningWorld(InWorld), ID(InID) {}

        /* True if this handle was ever given a World and a set EntityID --
         * NOT a liveness check. Use IsAlive() for "does this still exist". */
        bool IsValid() const { return OwningWorld != nullptr && ID.IsSet(); }
        bool IsAlive() const { return OwningWorld != nullptr && OwningWorld->IsAlive(ID); }

        EntityID GetID()    const { return ID; }
        World*   GetWorld() const { return OwningWorld; }

        /* Implicit, so World::CreateEntity() can return the handle while code
         * that stores or passes an EntityID keeps compiling. One-way: nothing
         * converts an EntityID back, since that would need a World. */
        operator EntityID() const { return ID; }

        // === Components: typed ==============================================
        // Cast-and-forward only; every one calls the by-type-id sibling below.

        /* T is deduced from InInit -- Marker.AddComponent(Transform) -- or
         * given explicitly for a default-initialised component:
         * Marker.AddComponent<TransformComponent>(). */
        template<typename T> T*   AddComponent(const T& InInit = T()) const { return static_cast<T*>(AddComponentByType(T::StaticType().ID, &InInit)); }
        template<typename T> T*   GetComponent() const { return static_cast<T*>(GetComponentByType(T::StaticType().ID)); }
        template<typename T> bool HasComponent() const { return HasComponentByType(T::StaticType().ID); }
        /* DEFERRED inside a tick, exactly like Destroy(). */
        template<typename T> bool RemoveComponent() const { return RemoveComponentByType(T::StaticType().ID); }

        // === Components: by type id =========================================
        // For callers holding only a ComponentTypeID (Lua, serialization).
        // The by-name and by-type paths are THE SAME CODE from here.

        /* Copies InInitBytes (InType's full SizeBytes), then writes the
         * ComponentOwner header. nullptr if not alive, InType is unregistered,
         * or this entity already has one. */
        void* AddComponentByType(ComponentTypeID InType, const void* InInitBytes) const { return OwningWorld ? OwningWorld->AddComponentRaw(ID, InType, InInitBytes) : nullptr; }
        void* GetComponentByType(ComponentTypeID InType) const { return OwningWorld ? OwningWorld->GetComponentRaw(ID, InType) : nullptr; }
        bool  HasComponentByType(ComponentTypeID InType) const { return OwningWorld && OwningWorld->HasComponentRaw(ID, InType); }
        bool  RemoveComponentByType(ComponentTypeID InType) const { return OwningWorld && OwningWorld->RemoveComponentRaw(ID, InType); }

        /* "" if there is no NameComponent, never null. Defined in
         * Components.h, after NameComponent exists. */
        const char* GetName() const;

        void Destroy() const { if(OwningWorld) { OwningWorld->DestroyEntity(ID); } }

        friend bool operator==(const Entity& L, const Entity& R) { return L.OwningWorld == R.OwningWorld && L.ID == R.ID; }
        friend bool operator!=(const Entity& L, const Entity& R) { return !(L == R); }

    private:
        World*   OwningWorld = nullptr;
        EntityID ID;
    };

    /* Defined here (not in ComponentType.h or World.h) since it needs a
     * complete Entity. Qualified on both sides -- see DESIGN.md for the
     * shadowing trap this avoids (ComponentOwner's own Entity member would
     * otherwise hide the class name inside this function body). */
    inline ::GameCore::Entity ComponentOwner::GetEntity() const
    {
        return ::GameCore::Entity(OwningWorld, this->Entity);
    }

    template<typename T>
    T* ComponentOwner::GetSibling() const
    {
        return GetEntity().template GetComponent<T>();
    }

    template<typename T>
    bool ComponentOwner::HasSibling() const
    {
        return GetEntity().template HasComponent<T>();
    }
}

#endif //GAMECORE_ENTITY_H
