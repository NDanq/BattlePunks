#include "Public/GameCore/World.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * World's storage:
 *   - Entities: an index free-list plus a parallel Generation array -- the
 *     same shape EntityID.h's own comment describes, just the concrete arrays
 *     backing it.
 *   - Pools: ComponentPool*[COMPONENT_TYPE_MAX], created lazily on first
 *     GetPool() / AddComponentRaw for a given type -- most games use a small
 *     fraction of 256 possible component types, and a pool costs a real
 *     allocation (ComponentPool.h's dense buffer), so paying for one only when
 *     something actually adds that component is the right default.
 *   - Systems: one vector<SystemEntry> PER PHASE (TP_MAX buckets), each entry
 *     a function pointer + void*, in registration order.
 *   - The deferred queue: entities and (entity, type) component-removals
 *     requested while IsTicking(), applied once the CURRENT PHASE's systems
 *     have all run -- see TickPhase()'s comment for why per-phase and not
 *     per-frame.
 */

namespace GameCore
{
    namespace
    {
        struct SystemEntry
        {
            SystemFn Fn       = nullptr;
            void*    UserData = nullptr;
        };

        struct PendingComponentRemoval
        {
            EntityID        Entity;
            ComponentTypeID Type = COMPONENT_TYPE_INVALID;
        };
    }

    struct World::Impl
    {
        // --- entities --------------------------------------------------------

        std::vector<uint32_t> Generations;     // indexed by EntityID::Index; bumped on every destroy
        std::vector<bool>     Alive;            // parallel to Generations
        std::vector<uint32_t> FreeIndices;
        size_t                LiveEntityCount = 0;

        // --- components --------------------------------------------------------

        ComponentPool* Pools[COMPONENT_TYPE_MAX] = {};

        // --- systems, by phase ---------------------------------------------

        std::vector<SystemEntry> Systems[TP_MAX];

        bool       bTicking      = false;
        ETickPhase CurrentPhase  = TP_PrePhysics;

        std::vector<EntityID>               PendingDestroys;
        std::vector<PendingComponentRemoval> PendingComponentRemovals;

        // --- spatial -----------------------------------------------------------

        SpatialIndex Spatial;
        AABB         ActiveRegion;
        bool         bHasActiveRegion = false;

        // --- delegates / events ----------------------------------------------

        TMulticastDelegate<EntityID>                  EntityCreated;
        TMulticastDelegate<EntityID>                  EntityDestroyed;
        TMulticastDelegate<EntityID, ComponentTypeID>  ComponentAdded;
        TMulticastDelegate<EntityID, ComponentTypeID>  ComponentRemoved;
        EventBus                                       Events;

        ~Impl()
        {
            for(ComponentPool* Pool : Pools)
            {
                delete Pool;
            }

            /* The manager cannot enforce that every subscriber unbound itself,
             * but it CAN detect and warn -- CoreModuleManager's stance toward
             * GLogHandler/GFileProvider, applied to World's own delegates. */
            if(EntityCreated.GetCount() > 0 || EntityDestroyed.GetCount() > 0
               || ComponentAdded.GetCount() > 0 || ComponentRemoved.GetCount() > 0)
            {
                GAMECORE_LOG_WARN("World destroyed with bindings still on one of its built-in "
                                   "delegates. If any point into a hot-reloadable module, "
                                   "OnShutdown failed to unbind -- see Delegate.h's RELOAD note.")
            }
        }

        bool IsAliveIndex(const EntityID InEntity) const
        {
            return InEntity.IsSet() && InEntity.Index < Alive.size()
                && Alive[InEntity.Index] && Generations[InEntity.Index] == InEntity.Generation;
        }

        /*
         * Slot allocation ONLY -- no broadcast. A MEMBER OF Impl for the same
         * reason DestroyEntityImmediate below is: a free function taking
         * World::Impl& cannot access this PRIVATE nested type from outside
         * World's own member bodies. Split out of World::CreateEntity so that
         * CreateEntity(name) can add the NameComponent BEFORE OnEntityCreated
         * fires, not after -- see World::CreateEntity(const char*)'s own
         * comment for the bug this fixes (observed directly: Sandbox's
         * OnEntityCreatedLogger printed an empty name for an entity created
         * WITH a name).
         */
        EntityID AllocateEntitySlot()
        {
            uint32_t Index;
            if(!FreeIndices.empty())
            {
                Index = FreeIndices.back();
                FreeIndices.pop_back();
            }
            else
            {
                Index = uint32_t(Generations.size());
                Generations.push_back(0);
                Alive.push_back(false);
            }

            Alive[Index] = true;
            ++LiveEntityCount;

            return EntityID(Index, Generations[Index]);
        }

        /*
         * Removes InEntity's every component IMMEDIATELY, bypassing
         * World::RemoveComponentRaw's own deferral check entirely -- calling
         * that public entry point here would be WRONG, not merely redundant:
         * this only ever runs from inside World::TickPhase()'s own flush,
         * where bTicking is still true (it is restored only after BOTH flush
         * loops finish, so that anything the flush itself triggers still sees
         * a consistent "we are mid-tick" state elsewhere). RemoveComponentRaw
         * would see bTicking==true and defer AGAIN, into a queue that was just
         * cleared and will not be looked at again this phase -- the removal
         * would never actually happen, the entity's slot would be recycled
         * while its "removed" components still sit in their pools under a
         * generation that is about to be reused, and the next entity born into
         * that slot would silently inherit them. This IS the one safe place
         * to mutate pools directly: it never runs from inside a system's own
         * iteration, only from the phase flush that happens after every
         * system in that phase has already returned.
         *
         * A MEMBER OF Impl, NOT A FREE FUNCTION TAKING Impl&: Impl is a
         * PRIVATE nested type of World (the PIMPL idiom every *.cpp in this
         * module uses), and a free function outside World's own member bodies
         * has no access to name that type as a parameter -- only World's own
         * members, and Impl's own members, can. This function touches nothing
         * outside Impl's own fields, so it belongs here rather than needing a
         * friend declaration on World for a function that does not actually
         * need World itself.
         */
        void DestroyEntityImmediate(const EntityID InEntity)
        {
            if(!IsAliveIndex(InEntity))
            {
                return;
            }

            /* BEFORE any component is removed -- a subscriber can still read
             * what the entity had. See World.h's DestroyEntity contract. */
            EntityDestroyed.Broadcast(InEntity);

            for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
            {
                if(Pools[Type] && Pools[Type]->Has(InEntity))
                {
                    ComponentRemoved.Broadcast(InEntity, Type);
                    Pools[Type]->Remove(InEntity);
                }
            }

            Alive[InEntity.Index] = false;
            ++Generations[InEntity.Index];
            FreeIndices.push_back(InEntity.Index);
            --LiveEntityCount;
        }
    };

    World::World() : P(new Impl()) {}

    World::~World()
    {
        delete P;
    }

    // === Entities ===============================================================

    EntityID World::CreateEntity()
    {
        const EntityID NewEntity = P->AllocateEntitySlot();
        P->EntityCreated.Broadcast(NewEntity);
        return NewEntity;
    }

    EntityID World::CreateEntity(const char* InName)
    {
        const EntityID NewEntity = P->AllocateEntitySlot();

        /* AddComponentRaw itself broadcasts OnComponentAdded for the
         * NameComponent -- that one BEFORE OnEntityCreated is a separate,
         * smaller ordering question (a component-added subscriber sees an
         * entity not yet announced as created) that existing code does not
         * depend on either order for, unlike the name-visibility gap this
         * fix targets. Left as is rather than reordered speculatively. */
        NameComponent Init;
        if(InName)
        {
            std::strncpy(Init.Name, InName, sizeof(Init.Name) - 1);
            Init.Name[sizeof(Init.Name) - 1] = '\0';
        }
        AddComponent<NameComponent>(NewEntity, Init);

        P->EntityCreated.Broadcast(NewEntity);
        return NewEntity;
    }

    void World::DestroyEntity(const EntityID InEntity)
    {
        if(!P->IsAliveIndex(InEntity))
        {
            return;
        }

        if(P->bTicking)
        {
            /* Deferred to the end of the CURRENT phase -- see TickPhase(). A
             * duplicate DestroyEntity for the same entity within one phase is
             * harmless: the flush below re-checks IsAliveIndex per entry. */
            P->PendingDestroys.push_back(InEntity);
            return;
        }

        P->DestroyEntityImmediate(InEntity);
    }

    bool World::IsAlive(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity);
    }

    size_t World::GetEntityCount() const
    {
        return P->LiveEntityCount;
    }

    Entity World::GetEntity(const EntityID InEntity)
    {
        return Entity(this, InEntity);
    }

    EntityID World::FindEntityByName(const char* InName) const
    {
        if(!InName) { return EntityID(); }

        ComponentPool* Pool = FindPool(NameComponent::StaticType().ID);
        if(!Pool) { return EntityID(); }

        const NameComponent* Base = static_cast<const NameComponent*>(Pool->GetDenseData());
        const size_t Count = Pool->GetCount();
        for(size_t Index = 0; Index < Count; ++Index)
        {
            if(std::strcmp(Base[Index].Name, InName) == 0)
            {
                return Pool->GetEntityAt(Index);
            }
        }
        return EntityID();
    }

    // === Components: erased =====================================================

    void* World::AddComponentRaw(const EntityID InEntity, const ComponentTypeID InType, const void* InInitBytes)
    {
        if(!P->IsAliveIndex(InEntity))
        {
            GAMECORE_LOG_WARN("AddComponentRaw called for an entity that is not alive; ignoring")
            return nullptr;
        }

        ComponentPool* Pool = GetPool(InType);
        if(!Pool)
        {
            return nullptr;
        }

        void* Payload = Pool->Add(InEntity, InInitBytes);
        if(!Payload)
        {
            return nullptr;
        }

        /* The ONE place a component's owner header is written -- see
         * ComponentType.h's ComponentOwner comment. Written AFTER the pool
         * copies the caller's init bytes, so it always wins over whatever (if
         * anything) the caller's InInitBytes happened to have at offset 0. */
        ComponentOwner* Owner = static_cast<ComponentOwner*>(Payload);
        Owner->OwningWorld = this;
        Owner->Entity      = InEntity;

        P->ComponentAdded.Broadcast(InEntity, InType);
        return Payload;
    }

    void* World::GetComponentRaw(const EntityID InEntity, const ComponentTypeID InType) const
    {
        ComponentPool* Pool = FindPool(InType);
        return Pool ? Pool->Get(InEntity) : nullptr;
    }

    bool World::HasComponentRaw(const EntityID InEntity, const ComponentTypeID InType) const
    {
        ComponentPool* Pool = FindPool(InType);
        return Pool && Pool->Has(InEntity);
    }

    bool World::RemoveComponentRaw(const EntityID InEntity, const ComponentTypeID InType)
    {
        ComponentPool* Pool = FindPool(InType);
        if(!Pool || !Pool->Has(InEntity))
        {
            return false;
        }

        if(P->bTicking)
        {
            /* Deferred exactly like DestroyEntity -- a system iterating this
             * pool right now must not have it swap-removed out from under the
             * dense array it is walking. */
            PendingComponentRemoval Removal;
            Removal.Entity = InEntity;
            Removal.Type   = InType;
            P->PendingComponentRemovals.push_back(Removal);
            return true;
        }

        P->ComponentRemoved.Broadcast(InEntity, InType);
        return Pool->Remove(InEntity);
    }

    namespace
    {
        /*
         * BoundsComponent's removal observer: pulls the entity's leaf out of
         * the spatial index the moment the component itself is removed --
         * whether that is because the whole entity was destroyed
         * (World::DestroyEntityImmediate removes every pool's component,
         * Bounds included) or because a caller removed just the Bounds off a
         * still-live entity. Without this, a leaf outlives the component that
         * created it and every later query keeps returning an entity that no
         * longer has the Bounds it was found by -- exactly the "a destroyed
         * entity is never returned by a query" contract World.h states for
         * DestroyEntity, which cannot hold if nothing ever calls Remove() on
         * the tree side.
         *
         * The World* is closed over via InUserData: SetRemovedObserver is
         * (function pointer, void*), the same shape every other seam in this
         * module takes, so there is nowhere else to carry it.
         */
        void BoundsComponent_OnRemoved(EntityID /*InEntity*/, void* InComponent, void* InUserData)
        {
            const BoundsComponent& Bounds = *static_cast<const BoundsComponent*>(InComponent);
            World* Owner = static_cast<World*>(InUserData);
            if(Bounds.ProxyID >= 0)
            {
                Owner->GetSpatialIndex().Remove(Bounds.ProxyID);
            }
        }
    }

    ComponentPool* World::GetPool(const ComponentTypeID InType)
    {
        if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
        {
            return nullptr;
        }
        if(!P->Pools[InType])
        {
            const ComponentTypeInfo* Info = GetComponentType(InType);
            if(!Info)
            {
                GAMECORE_LOG_ERROR("GetPool given an id nothing has registered; refusing to create a pool for it")
                return nullptr;
            }
            P->Pools[InType] = new ComponentPool(*Info);

            /* Wired here, at first-use pool creation, rather than in World's
             * constructor: the constructor cannot name BoundsComponent
             * without including Components.h, which would make World.cpp
             * (and therefore every consumer of World.h) depend on the whole
             * built-in component set purely for this one wire-up. GetPool
             * already includes Components.h (via the CreateEntity(name) path
             * needing NameComponent), so the dependency is already paid for. */
            if(InType == BoundsComponent::StaticType().ID)
            {
                P->Pools[InType]->SetRemovedObserver(&BoundsComponent_OnRemoved, this);
            }
        }
        return P->Pools[InType];
    }

    ComponentPool* World::FindPool(const ComponentTypeID InType) const
    {
        if(InType == COMPONENT_TYPE_INVALID || InType >= COMPONENT_TYPE_MAX)
        {
            return nullptr;
        }
        return P->Pools[InType];
    }

    // === Systems, by phase =======================================================

    void World::RegisterSystem(const char* InName, const ETickPhase InPhase, const SystemFn InFn, void* InUserData)
    {
        if(InPhase >= TP_MAX || !InFn)
        {
            GAMECORE_LOG_ERROR(std::string("RegisterSystem(\"") + (InName ? InName : "?") + "\") given a bad phase or a null function")
            return;
        }
        SystemEntry Entry;
        Entry.Fn       = InFn;
        Entry.UserData = InUserData;
        P->Systems[InPhase].push_back(Entry);
    }

    void World::UnregisterSystem(const SystemFn InFn, void* InUserData)
    {
        for(auto& Bucket : P->Systems)
        {
            for(size_t Index = 0; Index < Bucket.size(); )
            {
                if(Bucket[Index].Fn == InFn && Bucket[Index].UserData == InUserData)
                {
                    Bucket.erase(Bucket.begin() + long(Index));
                }
                else
                {
                    ++Index;
                }
            }
        }
    }

    size_t World::UnregisterAllSystems(void* InUserData)
    {
        size_t RemovedCount = 0;
        for(auto& Bucket : P->Systems)
        {
            for(size_t Index = 0; Index < Bucket.size(); )
            {
                if(Bucket[Index].UserData == InUserData)
                {
                    Bucket.erase(Bucket.begin() + long(Index));
                    ++RemovedCount;
                }
                else
                {
                    ++Index;
                }
            }
        }
        return RemovedCount;
    }

    void World::Tick(const float InDeltaSeconds)
    {
        for(int PhaseIndex = 0; PhaseIndex < TP_MAX; ++PhaseIndex)
        {
            const ETickPhase Phase = ETickPhase(PhaseIndex);
            TickPhase(Phase, InDeltaSeconds);

            /* The spatial refit runs at the END of TP_Physics -- after every
             * system registered there, before TP_PostPhysics begins -- so a
             * physics step's own movement is reflected before anything
             * downstream queries the tree. See TickPhase.h's TP_Physics note. */
            if(Phase == TP_Physics)
            {
                RefreshSpatialIndex();
            }
        }
    }

    void World::TickPhase(const ETickPhase InPhase, const float InDeltaSeconds)
    {
        if(InPhase >= TP_MAX)
        {
            GAMECORE_LOG_ERROR("TickPhase given an out-of-range phase; ignoring")
            return;
        }

        const bool bWasTicking = P->bTicking;
        const ETickPhase PreviousPhase = P->CurrentPhase;
        P->bTicking     = true;
        P->CurrentPhase = InPhase;

        /* Snapshot the count: a system registering ANOTHER system for this same
         * phase mid-walk (RegisterSystem called from inside a system) is picked
         * up next tick, not this one -- the same "an Add during a broadcast is
         * not called this broadcast" rule Delegate.h states, applied to systems
         * instead of delegate bindings. */
        const std::vector<SystemEntry>& Bucket = P->Systems[InPhase];
        const size_t SystemCount = Bucket.size();
        for(size_t Index = 0; Index < SystemCount; ++Index)
        {
            const SystemEntry Entry = Bucket[Index];   // copied: Bucket may reallocate if the system itself registers one
            if(Entry.Fn)
            {
                Entry.Fn(*this, InDeltaSeconds, Entry.UserData);
            }
        }

        /*
         * Flush THIS PHASE's deferred work now, while bTicking is still true
         * for anything the flush itself triggers (a removal observer, or a
         * ComponentRemoved subscriber, that itself calls DestroyEntity or
         * RemoveComponent), then restore the outer ticking state. TickPhase
         * calls nest exactly once in practice (Tick() calling TickPhase() four
         * times, never a system calling TickPhase() itself), so bWasTicking is
         * always false today -- restoring it rather than hardcoding false is
         * what makes a host's own manual TickPhase() call (see World.h's
         * comment on why TickPhase is public) compose correctly if it is ever
         * nested.
         *
         * SWAPPED OUT, NOT ITERATED IN PLACE: a ComponentRemoved subscriber (or
         * a pool's removal observer) called from inside this very loop may
         * itself request another deferred removal or destroy -- bTicking is
         * still true here, so RemoveComponentRaw/DestroyEntity still defer
         * rather than mutate immediately. That new entry lands in a FRESH
         * PendingComponentRemovals/PendingDestroys (the member was swapped to
         * an empty vector below before this loop started), so it is picked up
         * by the loop below it or by the same call one phase from now, and
         * this loop is walking a snapshot that nothing can reallocate under it.
         */
        std::vector<PendingComponentRemoval> ComponentRemovalsThisFlush;
        std::swap(ComponentRemovalsThisFlush, P->PendingComponentRemovals);
        for(const PendingComponentRemoval& Removal : ComponentRemovalsThisFlush)
        {
            ComponentPool* Pool = FindPool(Removal.Type);
            if(Pool && Pool->Has(Removal.Entity))
            {
                P->ComponentRemoved.Broadcast(Removal.Entity, Removal.Type);
                Pool->Remove(Removal.Entity);
            }
        }

        std::vector<EntityID> DestroysThisFlush;
        std::swap(DestroysThisFlush, P->PendingDestroys);
        for(const EntityID& Entity : DestroysThisFlush)
        {
            P->DestroyEntityImmediate(Entity);
        }

        P->bTicking     = bWasTicking;
        P->CurrentPhase = PreviousPhase;
    }

    bool World::IsTicking() const
    {
        return P->bTicking;
    }

    ETickPhase World::GetCurrentPhase() const
    {
        return P->CurrentPhase;
    }

    // === Spatial =================================================================

    SpatialIndex& World::GetSpatialIndex()
    {
        return P->Spatial;
    }

    void World::QueryBounds(const AABB& InBox, const SpatialQueryFn InFn, void* InUserData) const
    {
        P->Spatial.QueryBounds(InBox, InFn, InUserData);
    }

    bool World::Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir,
                         const float InMaxT, RaycastHit& OutHit) const
    {
        return P->Spatial.Raycast(InOrigin, InDir, InMaxT, OutHit);
    }

    void World::RefreshSpatialIndex()
    {
        ComponentPool* Pool = FindPool(BoundsComponent::StaticType().ID);
        if(!Pool) { return; }

        BoundsComponent* Base = static_cast<BoundsComponent*>(Pool->GetDenseData());
        const size_t Count = Pool->GetCount();

        for(size_t Index = 0; Index < Count; ++Index)
        {
            BoundsComponent& Bounds = Base[Index];

            /*
             * bStatic skips RE-FITTING, never the FIRST insert -- a static
             * Bounds still needs exactly one leaf in the tree, or it is
             * simply invisible to every query forever (ProxyID stuck at -1).
             * "Never re-fit after insertion" in this struct's own doc comment
             * means what it says: AFTER insertion. An earlier version of this
             * loop skipped bStatic entities unconditionally and never gave
             * them a leaf at all -- caught by the smoke test's static-Bounds
             * case finding the entity missing from every query.
             */
            if(Bounds.bStatic && Bounds.ProxyID >= 0)
            {
                continue;
            }

            /*
             * The sibling read this component's owner header exists for: a
             * Bounds reaches its own entity's Transform (identity if there is
             * none) with no change to World or this pass. The day a
             * MeshComponent exists, Local can be derived the same way -- ask
             * the owner for the mesh, take its extents -- without touching
             * this function's shape.
             */
            const TransformComponent* Transform = Bounds.Owner.GetSibling<TransformComponent>();
            const EngineCore::Matrix4x4 LocalToWorld = Transform ? Transform->GetLocalMatrix() : EngineCore::Matrix4x4();

            Bounds.World = AABB::Transformed(Bounds.Local, LocalToWorld);

            if(Bounds.ProxyID < 0)
            {
                Bounds.ProxyID = P->Spatial.Insert(Bounds.Owner.Entity, Bounds.World);
            }
            else
            {
                P->Spatial.Move(Bounds.ProxyID, Bounds.World);
            }
        }
    }

    void World::SetActiveRegion(const AABB& InBox)
    {
        P->ActiveRegion     = InBox;
        P->bHasActiveRegion = true;
    }

    bool World::HasActiveRegion() const { return P->bHasActiveRegion; }
    const AABB& World::GetActiveRegion() const { return P->ActiveRegion; }
    void World::ClearActiveRegion() { P->bHasActiveRegion = false; }

    // === Built-in delegates ======================================================

    TMulticastDelegate<EntityID>&                  World::OnEntityCreated()     { return P->EntityCreated; }
    TMulticastDelegate<EntityID>&                  World::OnEntityDestroyed()   { return P->EntityDestroyed; }
    TMulticastDelegate<EntityID, ComponentTypeID>& World::OnComponentAdded()    { return P->ComponentAdded; }
    TMulticastDelegate<EntityID, ComponentTypeID>& World::OnComponentRemoved()  { return P->ComponentRemoved; }

    EventBus& World::GetEvents() { return P->Events; }
}
