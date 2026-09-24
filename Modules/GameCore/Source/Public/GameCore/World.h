#ifndef GAMECORE_WORLD_H
#define GAMECORE_WORLD_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/SpatialIndex.h"
#include "Public/GameCore/TickPhase.h"

/* Forward-declared; Entity.h includes World.h itself, so this header cannot
 * include Entity.h without a cycle. CreateEntity() and GetEntity() return one
 * and are defined out-of-line in World.cpp, where Entity.h is visible; a
 * caller needs Entity.h (or Components.h) to use the result. */
namespace GameCore { class Entity; }

/*
 * The scene: entities, their components, phased systems, the spatial index
 * and the event bus a Lua script or a decoupled module reaches through.
 *
 * Owned by NimbleEngine::Application (see Application::GetWorld()), ticked
 * once per frame after IGame::OnTick. Game thread only -- no locks anywhere
 * in this class. See DESIGN.md for the full ownership/threading rationale.
 */
namespace GameCore
{
    using SystemFn = void (*)(World& InWorld, float InDeltaSeconds, void* InUserData);

    class GAMECORE_API World
    {
    public:
        World();
        ~World();

        World(const World&)            = delete;
        World& operator=(const World&) = delete;

        // === Entities ========================================================

        /* Returns the handle, so components attach directly:
         *     Entity Marker = World.CreateEntity("Marker");
         *     Marker.AddComponent(Transform);
         * Entity converts implicitly to EntityID, so storing the result as an
         * EntityID still works. */
        Entity CreateEntity();
        /* Convenience: also adds a NameComponent set to InName. */
        Entity CreateEntity(const char* InName);

        /* DEFERRED to the end of the current phase when called during a
         * tick; immediate otherwise. Broadcasts OnEntityDestroyed() BEFORE
         * removing any component -- see DESIGN.md. */
        void DestroyEntity(EntityID InEntity);

        bool   IsAlive(EntityID InEntity) const;
        size_t GetEntityCount() const;

        /* The handle -- valid (Entity::IsValid()) regardless of IsAlive(). */
        Entity GetEntity(EntityID InEntity);

        /* Linear scan over the NameComponent pool. InvalidEntityID() if
         * nothing matches. */
        EntityID FindEntityByName(const char* InName) const;

        /* Every live entity in slot order, whatever it carries -- Each<T> only
         * sees entities with a T. InFn must not create or destroy an entity
         * (that mutates the slot tables being walked); components are fine. */
        void ForEachEntity(void (*InFn)(EntityID InEntity, void* InUserData), void* InUserData) const;

        // === Components ======================================================
        // Added, fetched and removed through Entity (Entity.h) only -- World
        // owns the pools but exposes no per-entity component calls.

        /* Visits every live T, in dense (pool) order. InFn must not Add or
         * Remove a T on ANY entity while this runs -- see DESIGN.md. */
        template<typename T, typename F>
        void Each(F&& InFn)
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

        /* Creates the pool on first use; nullptr for an unregistered id. */
        ComponentPool* GetPool(ComponentTypeID InType);
        /* nullptr if a pool for InType has never been created. */
        ComponentPool* FindPool(ComponentTypeID InType) const;

        // === Systems, by phase ===============================================
        // Function pointer + void*, not a virtual interface -- see DESIGN.md.
        // Registration order WITHIN a phase is execution order.

        void RegisterSystem(const char* InName, ETickPhase InPhase, SystemFn InFn, void* InUserData = nullptr);
        void UnregisterSystem(SystemFn InFn, void* InUserData);
        size_t UnregisterAllSystems(void* InUserData);

        /* Runs TP_PrePhysics..TP_PostUpdate via TickPhase() below, then the
         * spatial refit at the end of TP_Physics. */
        void Tick(float InDeltaSeconds);

        /* Runs InPhase's systems, then flushes THIS PHASE's deferred
         * destroy/remove queue. Public so a host can interleave work between
         * phases -- see DESIGN.md. */
        void TickPhase(ETickPhase InPhase, float InDeltaSeconds);

        bool       IsTicking() const;
        ETickPhase GetCurrentPhase() const;

        // === Spatial =========================================================
        // The tree is the World's; systems query it, they do not own it.

        SpatialIndex& GetSpatialIndex();

        /* Every entity whose WORLD box (not local) overlaps InBox. */
        void QueryBounds(const AABB& InBox, SpatialQueryFn InFn, void* InUserData) const;
        bool Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir,
                     float InMaxT, RaycastHit& OutHit) const;

        /* QueryBounds, then GetComponent<T> on each hit -- see DESIGN.md for
         * when this is the right tool versus Each<T>. */
        template<typename T, typename F>
        void EachInBounds(const AABB& InBox, F&& InFn)
        {
            struct Context { World* Self; F* Fn; } Ctx{ this, &InFn };
            QueryBounds(InBox, [](const EntityID InEntity, void* InUserData) -> bool
            {
                Context& C = *static_cast<Context*>(InUserData);
                if(void* Component = C.Self->GetComponentRaw(InEntity, T::StaticType().ID))
                {
                    (*C.Fn)(C.Self->GetEntity(InEntity), *static_cast<T*>(Component));
                }
                return true;
            }, &Ctx);
        }

        /* Recomputes every non-static Bounds's world box and pushes it into
         * the tree. Tick() calls this once per frame; a host may call it
         * again mid-frame. */
        void RefreshSpatialIndex();

        /* A shared "region gameplay currently cares about" hint -- see
         * DESIGN.md. ScriptComponent::CullOutsideActiveRegion is the one
         * built-in consumer. */
        void        SetActiveRegion(const AABB& InBox);
        bool        HasActiveRegion() const;
        const AABB& GetActiveRegion() const;
        void        ClearActiveRegion();

        // === Built-in delegates ==============================================
        // Typed, no name lookup -- ScriptEngine::BindWorld mirrors each onto
        // GetEvents() under a fixed name so Lua sees them through Events.On/Emit.

        TMulticastDelegate<EntityID>&                  OnEntityCreated();
        /* Fires BEFORE the entity's components are removed. */
        TMulticastDelegate<EntityID>&                  OnEntityDestroyed();
        TMulticastDelegate<EntityID, ComponentTypeID>& OnComponentAdded();
        TMulticastDelegate<EntityID, ComponentTypeID>& OnComponentRemoved();

        EventBus& GetEvents();

    private:
        /* The erased component path -- the by-name (Lua) and by-type (C++)
         * paths are THE SAME CODE from here. Private: Entity is the only
         * caller, so there is exactly one public way to touch a component. */
        friend class Entity;

        /* Copies InInitBytes, then writes ComponentOwner at offset 0 -- the
         * ONE place that happens. nullptr if InEntity is not alive, InType
         * is unregistered, or InEntity already has this component. */
        void* AddComponentRaw(EntityID InEntity, ComponentTypeID InType, const void* InInitBytes);
        void* GetComponentRaw(EntityID InEntity, ComponentTypeID InType) const;
        bool  HasComponentRaw(EntityID InEntity, ComponentTypeID InType) const;
        /* DEFERRED inside a tick -- see DestroyEntity. */
        bool  RemoveComponentRaw(EntityID InEntity, ComponentTypeID InType);

        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_WORLD_H
