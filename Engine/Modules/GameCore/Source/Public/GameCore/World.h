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

/*
 * Forward-declared; Entity.h is header-only and includes World.h itself for the
 * inline definitions of its own template methods, so this header cannot
 * include Entity.h without a cycle. World::GetEntity() returns Entity by value
 * and is defined out-of-line in World.cpp instead, where Entity.h IS visible.
 */
namespace GameCore { class Entity; }

/*
 * The scene: entities, their components, phased systems, the spatial index and
 * the event bus a Lua script or a decoupled module reaches through.
 *
 * OWNERSHIP. NimbleEngine::Application owns exactly one World (see
 * Application::GetWorld() in NimbleEngine.h) and ticks it once per frame, after
 * IGame::OnTick -- so a game acts first (it may spawn something this frame),
 * then the World's own phases run. A game reaches entities through
 * Application::GetWorld(), never by constructing its own World.
 *
 * GAME THREAD ONLY. No locks anywhere in this class, matching
 * GameWindowSystem's stated thread contract: every call happens on the thread
 * that runs IGame::OnTick, which is also the thread that pumps GameWindow.
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

        EntityID CreateEntity();
        /* Convenience: also adds a NameComponent set to InName. */
        EntityID CreateEntity(const char* InName);

        /*
         * DEFERRED to the end of the CURRENT PHASE when called from inside
         * Tick()/TickPhase() -- see "Phases and the deferred queue" below.
         * Outside a tick, this is immediate. Broadcasts OnEntityDestroyed()
         * BEFORE removing a single component, so a subscriber can still read
         * what the entity had; only after that does every pool's Remove() run
         * (firing ScriptComponent's removal observer among them), the
         * generation bump, and the free-list recycle.
         */
        void DestroyEntity(EntityID InEntity);

        bool   IsAlive(EntityID InEntity) const;
        size_t GetEntityCount() const;

        /* The handle -- World* plus EntityID, valid (in the sense of
         * Entity::IsValid()) regardless of whether InEntity itself IsAlive(). */
        Entity GetEntity(EntityID InEntity);

        /* Linear scan over the NameComponent pool -- see NameComponent's own
         * note on why this is not indexed. InvalidEntityID() (via a default
         * EntityID) if nothing matches. */
        EntityID FindEntityByName(const char* InName) const;

        // === Components: typed ==============================================
        // Inline, cast-and-forward only -- see ComponentPool.h's "templates
        // compute" note. Every one of these calls the *Raw sibling below.

        template<typename T>
        T* AddComponent(EntityID InEntity, const T& InInit = T())
        {
            return static_cast<T*>(AddComponentRaw(InEntity, T::StaticType().ID, &InInit));
        }

        template<typename T>
        T* GetComponent(EntityID InEntity) const
        {
            return static_cast<T*>(GetComponentRaw(InEntity, T::StaticType().ID));
        }

        template<typename T>
        bool HasComponent(EntityID InEntity) const
        {
            return HasComponentRaw(InEntity, T::StaticType().ID);
        }

        /* DEFERRED inside a tick, exactly like DestroyEntity -- see below. */
        template<typename T>
        bool RemoveComponent(EntityID InEntity)
        {
            return RemoveComponentRaw(InEntity, T::StaticType().ID);
        }

        /*
         * Visits every live T, in dense (pool) order -- InFn(Entity, T&).
         * InFn must not Add or Remove a T on ANY entity while this runs: doing
         * so would relocate or shrink the very buffer this is walking. Adding
         * or removing a DIFFERENT component type, or destroying the CURRENT
         * entity (which defers to end-of-phase if called during a tick, per
         * DestroyEntity's own contract), is fine.
         */
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

        // === Components: erased =============================================
        // What the templates above and the Lua bindings both call -- the by-name
        // (Lua) and by-type (C++) paths are THE SAME CODE from here down.

        /* Copies InInitBytes (ComponentTypeInfo::SizeBytes long), then writes
         * this component's ComponentOwner header at offset 0 -- the ONE place
         * that ever happens. nullptr if InEntity is not alive, or InType is not
         * a registered id, or InEntity already has this component (a pool
         * Add() is not an upsert -- see ComponentPool.h). */
        void* AddComponentRaw(EntityID InEntity, ComponentTypeID InType, const void* InInitBytes);
        void* GetComponentRaw(EntityID InEntity, ComponentTypeID InType) const;
        bool  HasComponentRaw(EntityID InEntity, ComponentTypeID InType) const;
        /* DEFERRED inside a tick -- see DestroyEntity. */
        bool  RemoveComponentRaw(EntityID InEntity, ComponentTypeID InType);

        /* Creates the pool on first use if InType is a registered id and no
         * pool exists yet; nullptr for COMPONENT_TYPE_INVALID or an id nothing
         * has registered. */
        ComponentPool* GetPool(ComponentTypeID InType);
        /* nullptr if a pool for InType has never been created -- unlike
         * GetPool, never creates one. */
        ComponentPool* FindPool(ComponentTypeID InType) const;

        // === Systems, by phase ===============================================
        // Function pointer + void*, not a virtual interface -- GameWindowDelegates.h's
        // reasoning: no allocation, no owner, no vtable into a possibly-unloaded
        // image. Registration order WITHIN a phase is execution order.

        void RegisterSystem(const char* InName, ETickPhase InPhase, SystemFn InFn, void* InUserData = nullptr);
        void UnregisterSystem(SystemFn InFn, void* InUserData);
        size_t UnregisterAllSystems(void* InUserData);

        /* Runs TP_PrePhysics..TP_PostUpdate in order via TickPhase() below,
         * then World's own spatial refit (see RefreshSpatialIndex) at the end
         * of TP_Physics -- see TickPhase.h's TP_Physics note. */
        void Tick(float InDeltaSeconds);

        /*
         * Runs every system registered to InPhase, in registration order, then
         * flushes THIS PHASE's deferred destroy/remove queue -- so a
         * TP_PrePhysics system that destroys an entity has it gone before
         * TP_Physics runs, not merely gone by next frame. Public so a host can
         * interleave its own work between phases without being a registered
         * system itself (a future physics module stepping between TP_PrePhysics
         * and TP_PostPhysics, say) -- Tick() is simply four calls to this in a
         * row plus the refit.
         */
        void TickPhase(ETickPhase InPhase, float InDeltaSeconds);

        bool       IsTicking() const;
        ETickPhase GetCurrentPhase() const;

        // === Spatial =========================================================
        // The tree is the World's; systems query it, they do not own it. See
        // SpatialIndex.h and Components.h's BoundsComponent for how a leaf gets
        // populated from a Transform every frame.

        SpatialIndex& GetSpatialIndex();

        /* Every entity whose WORLD box (not local) overlaps InBox. */
        void QueryBounds(const AABB& InBox, SpatialQueryFn InFn, void* InUserData) const;
        bool Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir,
                     float InMaxT, RaycastHit& OutHit) const;

        /* QueryBounds, then GetComponent<T> on each hit -- only entities that
         * have T are passed to InFn(Entity, T&). The extra fetch per hit is a
         * sparse-set lookup (random access), which is the cost of asking by
         * region instead of walking a pool in order -- see SpatialIndex.h's
         * "update only nearby components" note for when this is and is not the
         * right tool. */
        template<typename T, typename F>
        void EachInBounds(const AABB& InBox, F&& InFn)
        {
            struct Context { World* Self; F* Fn; } Ctx{ this, &InFn };
            QueryBounds(InBox, [](const EntityID InEntity, void* InUserData) -> bool
            {
                Context& C = *static_cast<Context*>(InUserData);
                if(T* Component = C.Self->GetComponent<T>(InEntity))
                {
                    (*C.Fn)(C.Self->GetEntity(InEntity), *Component);
                }
                return true;
            }, &Ctx);
        }

        /* Runs the refit pass: every non-static BoundsComponent's World box is
         * recomputed from its owner's Transform (identity if none) and pushed
         * into the SpatialIndex via Move(). Tick() calls this once, at the end
         * of TP_Physics; a host may call it again mid-frame (after moving
         * something outside the normal tick, say) at the cost of walking the
         * Bounds pool again. */
        void RefreshSpatialIndex();

        /*
         * A shared hint for "the region gameplay currently cares about" -- the
         * common "around the player" case. World does not enforce anything
         * from this itself; ScriptSystem's per-phase tick honours it for a
         * script class that opts in with CullOutsideActiveRegion (see
         * Components.h's ScriptComponent), skipping OnTick for an entity whose
         * World box misses this region. A World with no active region set ticks
         * everything, which is also what ClearActiveRegion() restores.
         */
        void        SetActiveRegion(const AABB& InBox);
        bool        HasActiveRegion() const;
        const AABB& GetActiveRegion() const;
        void        ClearActiveRegion();

        // === Built-in delegates ==============================================
        // Typed, no name lookup -- ScriptEngine::BindWorld mirrors each of these
        // onto GetEvents() under a fixed name ("EntityCreated" etc.) so Lua sees
        // them through the one Events.On/Emit mechanism.

        TMulticastDelegate<EntityID>&                  OnEntityCreated();
        /* Fires BEFORE the entity's components are removed -- a subscriber can
         * still read what it had. */
        TMulticastDelegate<EntityID>&                  OnEntityDestroyed();
        TMulticastDelegate<EntityID, ComponentTypeID>& OnComponentAdded();
        TMulticastDelegate<EntityID, ComponentTypeID>& OnComponentRemoved();

        EventBus& GetEvents();

    private:
        struct Impl;
        Impl* P;
    };

    // ============================================================================
    // ComponentOwner's inline methods that need only a complete World (not a
    // complete Entity) -- defined here, not in ComponentType.h, because World
    // is only a complete type below this point. ComponentType.h forward-declares
    // World for exactly this reason; see the note at the top of that file.
    // GetEntity() and GetName() need a complete Entity too and are defined in
    // Entity.h and Components.h respectively, further down the include chain.
    // ============================================================================

    template<typename T>
    T* ComponentOwner::GetSibling() const
    {
        return OwningWorld ? OwningWorld->GetComponent<T>(Entity) : nullptr;
    }

    template<typename T>
    bool ComponentOwner::HasSibling() const
    {
        return OwningWorld && OwningWorld->HasComponent<T>(Entity);
    }
}

#endif //GAMECORE_WORLD_H
