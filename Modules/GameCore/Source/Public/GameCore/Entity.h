#ifndef GAMECORE_ENTITY_H
#define GAMECORE_ENTITY_H

#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/World.h"

/*
 * The ergonomic handle: a World* and an EntityID, copied freely, forwarding
 * every call straight to World. This is what GAMECORE_DESIGN.md's
 * "Entity.GetComponent<TransformComponent>()" asked for -- the template call
 * reads as a method on the entity, but does no more than
 * World::GetComponent<T>(EntityID) already does; Entity adds no state and no
 * behaviour of its own.
 *
 * NOT GAMECORE_API. Header-only, no out-of-line members, instantiates in the
 * consumer -- the same rule GameCoreAPI.h states for EntityID.h and
 * TickPhase.h's enum. World.h is the thing actually exported; this is a thin
 * wrapper over it.
 *
 * WHY THIS IS NOT WHAT ComponentOwner STORES. ComponentOwner keeps a raw
 * World* and EntityID rather than an Entity, because Entity.h depends on
 * World.h (for the template forwards below) and ComponentType.h -- where
 * ComponentOwner lives -- is included BY World.h. An Entity member there would
 * be the same cycle GetEntity()'s placement already works around; two raw
 * fields cost nothing extra and Entity.GetEntity() reconstructs the same
 * value on demand.
 */
namespace GameCore
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(World* InWorld, EntityID InID) : OwningWorld(InWorld), ID(InID) {}

        /* True if this handle was ever given a World and a set EntityID --
         * NOT a liveness check. Use World::IsAlive(GetID()) (or the entity's
         * own IsAlive() convenience below) for "does this still exist". */
        bool IsValid() const { return OwningWorld != nullptr && ID.IsSet(); }
        bool IsAlive() const { return OwningWorld != nullptr && OwningWorld->IsAlive(ID); }

        EntityID GetID()    const { return ID; }
        World*   GetWorld() const { return OwningWorld; }

        template<typename T> T*   GetComponent() const { return OwningWorld ? OwningWorld->GetComponent<T>(ID) : nullptr; }
        template<typename T> T*   AddComponent(const T& InInit = T()) const { return OwningWorld ? OwningWorld->AddComponent<T>(ID, InInit) : nullptr; }
        template<typename T> bool HasComponent() const { return OwningWorld && OwningWorld->HasComponent<T>(ID); }
        template<typename T> bool RemoveComponent() const { return OwningWorld && OwningWorld->RemoveComponent<T>(ID); }

        /* "" if there is no NameComponent -- see ComponentOwner::GetName(),
         * which this forwards to via a GetComponent<NameComponent>() lookup
         * defined in Components.h, never null either way. */
        const char* GetName() const;

        void Destroy() const { if(OwningWorld) { OwningWorld->DestroyEntity(ID); } }

        friend bool operator==(const Entity& L, const Entity& R) { return L.OwningWorld == R.OwningWorld && L.ID == R.ID; }
        friend bool operator!=(const Entity& L, const Entity& R) { return !(L == R); }

    private:
        World*   OwningWorld = nullptr;
        EntityID ID;
    };

    // ============================================================================
    // ComponentOwner::GetEntity() -- needs a complete Entity, which is why it
    // could not be defined in ComponentType.h or World.h. See the placement
    // note on the declaration in ComponentType.h.
    //
    // ComponentOwner's OWN member is named "Entity", of type EntityID -- inside
    // this function body the bare identifier Entity resolves to THAT member,
    // not the class GameCore::Entity being constructed. Both uses below are
    // qualified (::GameCore::Entity for the type, this->Entity for the member)
    // for exactly that reason -- an unqualified `Entity(OwningWorld, Entity)`
    // would be the same shadowing trap the OwningWorld rename in
    // ComponentType.h exists to avoid, and here the class scope makes it
    // unavoidable without qualification rather than merely a risk.
    // ============================================================================
    inline ::GameCore::Entity ComponentOwner::GetEntity() const
    {
        return ::GameCore::Entity(OwningWorld, this->Entity);
    }
}

#endif //GAMECORE_ENTITY_H
