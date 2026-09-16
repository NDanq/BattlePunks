#ifndef GAMECORE_ENTITY_H
#define GAMECORE_ENTITY_H

#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/World.h"

/*
 * The ergonomic handle: a World* and an EntityID, copied freely, forwarding
 * every call straight to World -- so Entity.GetComponent<TransformComponent>()
 * reads as a method on the entity but does no more than
 * World::GetComponent<T>(EntityID) already does. Header-only, not
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

        template<typename T> T*   GetComponent() const { return OwningWorld ? OwningWorld->GetComponent<T>(ID) : nullptr; }
        template<typename T> T*   AddComponent(const T& InInit = T()) const { return OwningWorld ? OwningWorld->AddComponent<T>(ID, InInit) : nullptr; }
        template<typename T> bool HasComponent() const { return OwningWorld && OwningWorld->HasComponent<T>(ID); }
        template<typename T> bool RemoveComponent() const { return OwningWorld && OwningWorld->RemoveComponent<T>(ID); }

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
}

#endif //GAMECORE_ENTITY_H
