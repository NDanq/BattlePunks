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

        // === Members ========================================================
        // In World's slot tables, not component pools: one each per entity.

        /* Persistent, unique within a World. Unset if not alive. */
        EntityGuid GetGuid() const { return OwningWorld ? OwningWorld->GetEntityGuid(ID) : EntityGuid(); }

        /* Never null. Valid until the next SetName or destruction. */
        const char* GetName() const { return OwningWorld ? OwningWorld->GetEntityName(ID) : ""; }
        /* nullptr clears. False if not alive. Names need not be unique. */
        bool SetName(const char* InName) const { return OwningWorld && OwningWorld->SetEntityName(ID, InName); }


        // === Hierarchy ======================================================
        // Parent and children are members too. TransformComponent is LOCAL:
        // relative to the parent, or the world for an entity without one.
        // An entity with no Transform passes its parent's straight through.

        /* InvalidEntityID() detaches. With bInKeepWorldTransform the local
         * Transform is rewritten so the entity stays where it is; without
         * it the local values are kept and the entity moves with its new
         * parent. False if either is not alive, or InParent is this entity
         * or one of its descendants. Immediate, even inside a tick.
         * Destroying an entity destroys its children first. */
        bool SetParent(EntityID InParent, bool bInKeepWorldTransform = true) const { return OwningWorld && OwningWorld->SetEntityParent(ID, InParent, bInKeepWorldTransform); }
        bool ClearParent(bool bInKeepWorldTransform = true) const { return SetParent(InvalidEntityID(), bInKeepWorldTransform); }
        EntityID GetParent() const { return OwningWorld ? OwningWorld->GetEntityParent(ID) : EntityID(); }
        bool     HasParent() const { return GetParent().IsSet(); }

        /* Children in the order they were parented:
         *     for(EntityID C = E.GetFirstChild(); C.IsSet(); C = World.GetEntity(C).GetNextSibling())
         * Reparenting or destroying the current child ends the walk. */
        EntityID GetFirstChild() const { return OwningWorld ? OwningWorld->GetEntityFirstChild(ID) : EntityID(); }
        EntityID GetNextSibling() const { return OwningWorld ? OwningWorld->GetEntityNextSibling(ID) : EntityID(); }
        size_t   GetChildCount() const { return OwningWorld ? OwningWorld->GetEntityChildCount(ID) : 0; }

        /* Where this entity is in the world: its Transform composed under
         * every ancestor's (TransformMath.h's TransformPose). Exactly the
         * local Transform without a parent. Computed on each call. */
        TransformPose         GetWorldPose() const { return OwningWorld ? OwningWorld->GetEntityWorldPose(ID) : TransformPose(); }
        EngineCore::Matrix4x4 GetWorldMatrix() const { return OwningWorld ? OwningWorld->GetEntityWorldMatrix(ID) : EngineCore::Matrix4x4(); }
        /* Writes the local Transform that puts this entity at InPose. False
         * without a Transform. */
        bool SetWorldPose(const TransformPose& InPose) const { return OwningWorld && OwningWorld->SetEntityWorldPose(ID, InPose); }

        // === Prefab =========================================================

        /* The prefab instance this entity was spawned as part of; unset for
         * a loose entity. */
        PrefabInstanceID GetInstance() const { return OwningWorld ? OwningWorld->GetEntityInstance(ID) : PrefabInstanceID(); }
        bool             IsInInstance() const { return GetInstance().IsSet(); }
        /* Its template's id in that prefab; PREFAB_NO_LOCAL_ID (0) if loose. */
        uint32_t         GetPrefabLocalId() const { return OwningWorld ? OwningWorld->GetEntityPrefabLocalId(ID) : 0; }

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

    inline const char* ComponentOwner::GetName() const
    {
        return GetEntity().GetName();
    }

    /* Out of line for the same reason: GetEntity() returns an Entity
     * whatever T is, so the call is checked where the template is defined,
     * and World.h only has the forward declaration. */
    template<typename T, typename F>
    void World::Each(F&& InFn)
    {
        ComponentPool* Pool = FindPool(T::StaticType().ID);
        if(!Pool) { return; }
        T* Base = static_cast<T*>(Pool->GetDenseData());
        const size_t Count = Pool->GetCount();
        for(size_t Index = 0; Index < Count; ++Index)
        {
            InFn(GetEntity(Pool->GetEntityAt(Index)), Base[Index]);
        }
    }
}

#endif //GAMECORE_ENTITY_H
