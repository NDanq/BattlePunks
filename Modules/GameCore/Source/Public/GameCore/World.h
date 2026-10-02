#ifndef GAMECORE_WORLD_H
#define GAMECORE_WORLD_H

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/Delegate.h"
#include "Public/GameCore/EntityGuid.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/SpatialIndex.h"
#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Math/Matrix.h"

/* Forward-declared; Entity.h includes World.h itself, so this header cannot
 * include Entity.h without a cycle. CreateEntity() and GetEntity() return one
 * and are defined out-of-line in World.cpp, where Entity.h is visible; a
 * caller needs Entity.h (or Components.h) to use the result. */
namespace GameCore { class Entity; }

/*
 * The scene: entities, their components, phased systems, the spatial index
 * and the event bus a Lua script or a decoupled module reaches through.
 *
 * Owned by NimbleEngine::GameSession (see GameSession::GetWorld()), ticked
 * once per frame after IGameMode::OnTick. Game thread only -- no locks anywhere
 * in this class. See DESIGN.md for the full ownership/threading rationale.
 */
namespace GameCore
{
    using SystemFn = void (*)(World& InWorld, float InDeltaSeconds, void* InUserData);

    /* Where a system runs within its phase, relative to the phase's scripts. */
    enum ESystemOrder : uint8_t
    {
        SO_AfterScripts = 0,   // the default: scripts first, then systems
        SO_BeforeScripts,      // ahead of the phase's scripts -- what they must see this frame (input)
    };

    /* Runtime state of an entity's script, owned by the ScriptDriver; World
     * only stores it. Reset when the path changes. */
    struct EntityScriptState
    {
        /* luaL_ref of the instance table; -2 is LUA_NOREF (no lua.h here). */
        int32_t InstanceRef = -2;

        /* From the class table; TP_PrePhysics if unspecified. */
        uint8_t Phase = TP_PrePhysics;

        uint8_t bStarted = 0;
        uint8_t bErrored = 0;

        /* Set by SetScript, cleared when the driver loads it. */
        uint8_t bPendingInstantiate = 0;

        /* Needs a BoundsComponent (DESIGN.md). Persisted; the class table's
         * CullOutsideActiveRegion overrides it. */
        uint8_t bCullOutsideActiveRegion = 0;
    };

    /*
     * What runs entity scripts. One per World (ScriptEngine::BindWorld installs
     * it); World calls it but knows nothing of Lua. See DESIGN.md.
     */
    struct ScriptDriver
    {
        /* At the start of every phase, before any system. */
        void (*TickPhase)(World& InWorld, ETickPhase InPhase, float InDeltaSeconds, void* InUserData) = nullptr;

        /* A script is going away (destroy or new path); path and state are
         * still set, and cleared afterwards. */
        void (*Release)(World& InWorld, EntityID InEntity, void* InUserData) = nullptr;

        /* InMethod(self, InArg) on a started script: World::CallScriptMethod. */
        bool (*CallMethod)(World& InWorld, EntityID InEntity, const char* InMethod, EntityID InArg, void* InUserData) = nullptr;

        void* UserData = nullptr;
    };

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
        /* Also sets the entity's name, before OnEntityCreated fires. */
        Entity CreateEntity(const char* InName);
        /* For loaders. An unset or already-used (warned) InGuid gets a fresh
         * one; read it back with Entity::GetGuid(). */
        Entity CreateEntity(const char* InName, const EntityGuid& InGuid);

        /* DEFERRED to the end of the current phase when called during a
         * tick; immediate otherwise. Broadcasts OnEntityDestroyed() BEFORE
         * removing any component -- see DESIGN.md. */
        void DestroyEntity(EntityID InEntity);

        bool   IsAlive(EntityID InEntity) const;
        size_t GetEntityCount() const;

        /* The handle -- valid (Entity::IsValid()) regardless of IsAlive(). */
        Entity GetEntity(EntityID InEntity);

        /* Linear scan over the live entities' names, in slot order.
         * InvalidEntityID() if nothing matches (or InName is ""). */
        EntityID FindEntityByName(const char* InName) const;

        /* A hash lookup. InvalidEntityID() if no live entity has InGuid. */
        EntityID FindEntityByGuid(const EntityGuid& InGuid) const;

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
        // Registration order WITHIN a phase and order is execution order.

        void RegisterSystem(const char* InName, ETickPhase InPhase, SystemFn InFn, void* InUserData = nullptr,
                            ESystemOrder InOrder = SO_AfterScripts);
        void UnregisterSystem(SystemFn InFn, void* InUserData);
        size_t UnregisterAllSystems(void* InUserData);

        /* Runs TP_PrePhysics..TP_PostUpdate via TickPhase() below, then the
         * spatial refit at the end of TP_Physics. */
        void Tick(float InDeltaSeconds);

        /* SO_BeforeScripts systems, scripts, then InPhase's other systems,
         * then THIS PHASE's deferred queue.
         * Public so a host can interleave work (DESIGN.md). */
        void TickPhase(ETickPhase InPhase, float InDeltaSeconds);

        bool       IsTicking() const;
        ETickPhase GetCurrentPhase() const;

        // === Entity scripts: the driver side ==================================
        // For ScriptEngine, not gameplay code (use Entity::SetScript).

        /* Copies InDriver; nullptr clears. Replacing releases nothing. */
        void SetScriptDriver(const ScriptDriver* InDriver);
        bool HasScriptDriver() const;

        /* Scripted entities in tick order. Stable during a tick. */
        size_t   GetScriptedEntityCount() const;
        EntityID GetScriptedEntityAt(size_t InIndex) const;

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

        /* Refits InEntity's Bounds now, and its descendants', even static
         * ones (which RefreshSpatialIndex skips), for a tool that moved it. */
        void RefreshEntityBounds(EntityID InEntity);

        /* The region gameplay cares about (DESIGN.md); used by
         * bCullOutsideActiveRegion. */
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

        // === Collision (Collision.cpp) =======================================
        // CollisionComponent pairs, from the start of TP_PostPhysics.

        /* (Self, Other), once for each side of a pair that began or ended.
         * Self is alive; Other may not be (it ended because it went away). */
        TMulticastDelegate<EntityID, EntityID>&        OnOverlapBegin();
        TMulticastDelegate<EntityID, EntityID>&        OnOverlapEnd();

        /* As of the last UpdateOverlaps. */
        bool   AreOverlapping(EntityID InA, EntityID InB) const;
        size_t GetOverlaps(EntityID InEntity, EntityID* OutOthers, size_t InMax) const;

        /* Intersection now, without events: every enabled collider whose box
         * overlaps InBox and whose Layer is in InMask. Up to InMax into
         * OutHits; returns how many there are in all. */
        size_t OverlapBox(const AABB& InBox, int32_t InMask, EntityID* OutHits, size_t InMax) const;

        /* The overlap pass: shapes, pairs, then the begin and end events.
         * Every World runs it at the start of TP_PostPhysics (before that
         * phase's scripts); public for a host or test that steps it. */
        void UpdateOverlaps();

        /* Calls InMethod(self, InArg) on InEntity's script if it has started
         * and defines it. False if there is no such method or it raised. */
        bool CallScriptMethod(EntityID InEntity, const char* InMethod, EntityID InArg);

        EventBus& GetEvents();

    private:
        /* Collision.cpp's state, kept in Impl. */
        std::vector<std::pair<uint64_t, uint64_t>>&       GetOverlapPairs();
        const std::vector<std::pair<uint64_t, uint64_t>>& GetOverlapPairs() const;

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

        /* Private: reached through Entity. */
        EntityGuid         GetEntityGuid(EntityID InEntity) const;
        const char*        GetEntityName(EntityID InEntity) const;
        bool               SetEntityName(EntityID InEntity, const char* InName);
        const char*        GetEntityScriptPath(EntityID InEntity) const;
        /* DEFERRED inside a tick -- see DestroyEntity. */
        bool               SetEntityScript(EntityID InEntity, const char* InPath);
        EntityScriptState* GetEntityScriptState(EntityID InEntity) const;

        /* The hierarchy -- see Entity.h's section for the contract. */
        bool                  SetEntityParent(EntityID InEntity, EntityID InParent, bool bInKeepWorldTransform);
        EntityID              GetEntityParent(EntityID InEntity) const;
        EntityID              GetEntityFirstChild(EntityID InEntity) const;
        EntityID              GetEntityNextSibling(EntityID InEntity) const;
        size_t                GetEntityChildCount(EntityID InEntity) const;
        TransformPose         GetEntityWorldPose(EntityID InEntity) const;
        EngineCore::Matrix4x4 GetEntityWorldMatrix(EntityID InEntity) const;
        bool                  SetEntityWorldPose(EntityID InEntity, const TransformPose& InPose);

        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_WORLD_H
