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
#include "Public/GameCore/PrefabInstanceID.h"
#include "Public/GameCore/SpatialIndex.h"
#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Math/Matrix.h"

/* Forward-declared; Entity.h includes World.h itself, so this header cannot
 * include Entity.h without a cycle. CreateEntity() and GetEntity() return one
 * and are defined out-of-line in World.cpp, where Entity.h is visible; a
 * caller needs Entity.h (or Components.h) to use the result. */
namespace GameCore { class Entity; }

/* Likewise Prefab.h's, which includes this header. */
namespace GameCore
{
    class PrefabAsset;
    struct NativePrefabBehavior;
    class PrefabInstance;
    struct PrefabInstanceRecord;
    enum class EPrefabTickMode : uint8_t;
}

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

    /* Runtime state of a prefab instance's script, owned by the ScriptDriver;
     * World stores it and keeps the per-phase lists from PhaseMask. */
    struct InstanceScriptState
    {
        /* luaL_ref of the instance table; -2 is LUA_NOREF. */
        int32_t InstanceRef = -2;

        /* Bit P: the instance ticks in phase P -- its script defines a function
         * for it (On<Phase>, or OnTick for its TickPhase) or its native
         * behaviour a callback. Set by the driver at instantiation. */
        uint8_t PhaseMask = 0;

        /* The script's share of PhaseMask: where the Lua class is called. */
        uint8_t LuaPhaseMask = 0;

        /* The driver's cache of the native behaviour (NativePrefab.h): good
         * while GetNativePrefabGeneration() still equals NativeGeneration. */
        const NativePrefabBehavior* Native = nullptr;
        uint32_t                    NativeGeneration = 0;

        /* The phase OnTick stands for, TP_MAX if the class has no OnTick. */
        uint8_t OnTickPhase = TP_MAX;

        uint8_t bStarted = 0;
        uint8_t bErrored = 0;

        /* Queued for instantiation; cleared when the driver takes it. */
        uint8_t bPendingInstantiate = 0;
    };

    /*
     * What runs prefab instances' scripts. One per World
     * (ScriptEngine::BindWorld installs it); World calls it but knows nothing
     * of Lua. See DESIGN.md.
     */
    struct ScriptDriver
    {
        /* At the start of every phase, before any system. */
        void (*TickPhase)(World& InWorld, ETickPhase InPhase, float InDeltaSeconds, void* InUserData) = nullptr;

        /* An instance's script is going away (destroyed, or tick mode Off);
         * its state is still set, and reset afterwards. */
        void (*ReleaseInstance)(World& InWorld, PrefabInstanceID InInstance, void* InUserData) = nullptr;

        /* InMethod(self, InArg, InMine) on a started instance script. */
        bool (*CallInstanceMethod)(World& InWorld, PrefabInstanceID InInstance, const char* InMethod, EntityID InArg,
                                   EntityID InMine, void* InUserData) = nullptr;

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

        // === Prefab instances (Prefab.h) ====================================

        /* InPrefab's entities, its root placed at InAt (composed over the
         * template root's own Transform, which a captured prefab stores as
         * identity). Parented and with every component in place before
         * OnInstanceSpawned fires. An unset or used InGuid gets a fresh one.
         * Invalid (and warned) if InPrefab is empty. Immediate, even in a
         * tick, like CreateEntity. */
        PrefabInstance SpawnPrefab(const PrefabAsset& InPrefab, const TransformPose& InAt = TransformPose(),
                                   const char* InName = nullptr, const EntityGuid& InGuid = EntityGuid());
        /* Through FindOrLoadPrefab. Invalid if it cannot be loaded. */
        PrefabInstance SpawnPrefab(const char* InPath, const TransformPose& InAt = TransformPose(),
                                   const char* InName = nullptr, const EntityGuid& InGuid = EntityGuid());

        /* For loaders: makes an instance of entities that already exist, as
         * a binary world stores them (Prefab.h's PrefabInstanceRecord). Every
         * entity must be alive, in no instance, and in the first one's (the
         * root's) subtree. Invalid, warned, otherwise. The script is queued
         * as for a spawn. */
        PrefabInstance RestorePrefabInstance(const PrefabInstanceRecord& InRecord);

        /* Authoring: InEntity, in no instance and inside InInstance's root
         * subtree, joins it as template InLocalId -- an entity an editor added
         * to the prefab it is editing (CapturePrefab's bInAdoptNew). False if
         * either is dead, InEntity is in an instance or outside the subtree,
         * or InLocalId is unset or taken. */
        bool AdoptIntoInstance(PrefabInstanceID InInstance, EntityID InEntity, uint32_t InLocalId);

        /* Destroys the root -- and so the whole instance. DEFERRED in a tick. */
        void DestroyPrefabInstance(PrefabInstanceID InInstance);

        bool             IsInstanceAlive(PrefabInstanceID InInstance) const;
        size_t           GetInstanceCount() const;
        PrefabInstance   GetInstance(PrefabInstanceID InInstance);
        PrefabInstanceID FindInstanceByGuid(const EntityGuid& InGuid) const;

        /* Every live instance in slot order. InFn must not spawn or destroy one. */
        void ForEachInstance(void (*InFn)(PrefabInstanceID InInstance, void* InUserData), void* InUserData) const;

        /* After every entity and component is in place. */
        TMulticastDelegate<PrefabInstanceID>& OnInstanceSpawned();
        /* When the root starts to go: every entity is still there. */
        TMulticastDelegate<PrefabInstanceID>& OnInstanceDestroyed();

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

        // === The script driver ================================================
        // For ScriptEngine, not gameplay code (give a prefab a script).

        /* Copies InDriver; nullptr clears. Replacing releases nothing. */
        void SetScriptDriver(const ScriptDriver* InDriver);
        bool HasScriptDriver() const;

        // === Instance scripts: the driver side ================================
        // For ScriptEngine. An instance with a script and a tick mode other
        // than Off is queued when spawned; the driver takes it, instantiates
        // its class and reports the phases it defines, and World keeps one
        // list per phase of the instances that tick there.

        /* The next queued instance, oldest first; false when none is left.
         * Skips what died or turned Off since it was queued. */
        bool PopPendingInstanceScript(PrefabInstanceID& OutInstance);

        /* nullptr if InInstance is not alive or has neither a script nor a
         * native behaviour. Stable while it lives. */
        InstanceScriptState* GetInstanceScriptState(PrefabInstanceID InInstance) const;
        /* "" if none. */
        const char*          GetInstanceScriptPath(PrefabInstanceID InInstance) const;
        /* The native prefab whose callbacks drive it (PrefabAsset::GetNativeBehavior); "" if none. */
        const char*          GetInstanceNativeBehavior(PrefabInstanceID InInstance) const;

        /* Records InMask and files the instance under each phase it ticks in.
         * Not while ticking a phase's instances (the driver calls it before). */
        void SetInstanceScriptPhaseMask(PrefabInstanceID InInstance, uint8_t InMask);

        /* InPhase's ticking instances in the order they started. Stable
         * during a tick: every change to the lists waits for the flush. */
        size_t           GetTickingInstanceCount(ETickPhase InPhase) const;
        PrefabInstanceID GetTickingInstanceAt(ETickPhase InPhase, size_t InIndex) const;

        /* Resets InInstance's script to pending and queues it again (unbind). */
        void RequeueInstanceScript(PrefabInstanceID InInstance);

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

        /* Calls InMethod(self, InArg, InEntity) on InEntity's prefab
         * instance's script -- InEntity is the third argument, "mine" -- if
         * it has started and defines it. An instance is not told about InArg
         * when InArg is one of its own. False if it did not run cleanly. */
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

        /* The hierarchy -- see Entity.h's section for the contract. */
        bool                  SetEntityParent(EntityID InEntity, EntityID InParent, bool bInKeepWorldTransform);
        EntityID              GetEntityParent(EntityID InEntity) const;
        EntityID              GetEntityFirstChild(EntityID InEntity) const;
        EntityID              GetEntityNextSibling(EntityID InEntity) const;
        size_t                GetEntityChildCount(EntityID InEntity) const;
        TransformPose         GetEntityWorldPose(EntityID InEntity) const;
        EngineCore::Matrix4x4 GetEntityWorldMatrix(EntityID InEntity) const;
        bool                  SetEntityWorldPose(EntityID InEntity, const TransformPose& InPose);

        /* Prefab membership -- see Entity.h and Prefab.h's PrefabInstance. */
        friend class PrefabInstance;
        PrefabInstanceID   GetEntityInstance(EntityID InEntity) const;
        uint32_t           GetEntityPrefabLocalId(EntityID InEntity) const;
        EntityID           GetInstanceRoot(PrefabInstanceID InInstance) const;
        EntityID           FindInstanceEntity(PrefabInstanceID InInstance, uint32_t InLocalId) const;
        const PrefabAsset* GetInstancePrefab(PrefabInstanceID InInstance) const;
        const char*        GetInstancePrefabPath(PrefabInstanceID InInstance) const;
        EntityGuid         GetInstanceGuid(PrefabInstanceID InInstance) const;
        const char*        GetInstanceName(PrefabInstanceID InInstance) const;
        bool               SetInstanceName(PrefabInstanceID InInstance, const char* InName);
        EPrefabTickMode    GetInstanceTickMode(PrefabInstanceID InInstance) const;
        /* DEFERRED inside a tick. */
        bool               SetInstanceTickMode(PrefabInstanceID InInstance, EPrefabTickMode InMode);
        bool               IsInstancePhaseEnabled(PrefabInstanceID InInstance, ETickPhase InPhase) const;
        /* DEFERRED inside a tick. */
        bool               SetInstancePhaseEnabled(PrefabInstanceID InInstance, ETickPhase InPhase, bool bInEnabled);

        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_WORLD_H
