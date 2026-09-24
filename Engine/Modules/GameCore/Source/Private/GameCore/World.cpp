#include "Public/GameCore/World.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * World's storage: an entity free-list plus a generation array, a
 * ComponentPool*[COMPONENT_TYPE_MAX] table created lazily on first use, one
 * system vector per phase, and the deferred destroy/remove queues. See
 * DESIGN.md's "World.h / World.cpp" section for the full reasoning behind
 * each piece below.
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

            /* Cannot enforce that every subscriber unbound itself, but can
             * detect and warn -- see DESIGN.md. */
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

        /* Slot allocation ONLY -- no broadcast. Split out of CreateEntity so
         * the name variant can add NameComponent before broadcasting -- see
         * DESIGN.md. A member of Impl, not a free function, since Impl is
         * private to World -- see DESIGN.md. */
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

        /* Removes InEntity's every component IMMEDIATELY, bypassing
         * RemoveComponentRaw's own deferral -- see DESIGN.md for why calling
         * that here would be wrong, not merely redundant. Only ever runs
         * from TickPhase()'s flush. */
        void DestroyEntityImmediate(const EntityID InEntity)
        {
            if(!IsAliveIndex(InEntity))
            {
                return;
            }

            /* BEFORE any component is removed -- a subscriber can still read
             * what the entity had. */
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

    Entity World::CreateEntity()
    {
        const EntityID NewEntity = P->AllocateEntitySlot();
        P->EntityCreated.Broadcast(NewEntity);
        return Entity(this, NewEntity);
    }

    Entity World::CreateEntity(const char* InName)
    {
        const EntityID NewEntity = P->AllocateEntitySlot();

        /* Added BEFORE the OnEntityCreated broadcast below -- see DESIGN.md
         * for the bug this ordering fixes. */
        NameComponent Init;
        if(InName)
        {
            std::strncpy(Init.Name, InName, sizeof(Init.Name) - 1);
            Init.Name[sizeof(Init.Name) - 1] = '\0';
        }
        AddComponentRaw(NewEntity, NameComponent::StaticType().ID, &Init);

        P->EntityCreated.Broadcast(NewEntity);
        return Entity(this, NewEntity);
    }

    void World::DestroyEntity(const EntityID InEntity)
    {
        if(!P->IsAliveIndex(InEntity))
        {
            return;
        }

        if(P->bTicking)
        {
            /* Deferred to the end of the current phase. A duplicate
             * DestroyEntity within one phase is harmless -- the flush
             * re-checks IsAliveIndex per entry. */
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

    void World::ForEachEntity(void (*InFn)(EntityID InEntity, void* InUserData), void* InUserData) const
    {
        if(!InFn) { return; }

        /* Bounded by the slot table, not LiveEntityCount -- slots have holes.
         * Re-read per iteration so a rule-breaking InFn stays in bounds. */
        for(uint32_t Index = 0; Index < P->Alive.size(); ++Index)
        {
            if(P->Alive[Index])
            {
                InFn(EntityID(Index, P->Generations[Index]), InUserData);
            }
        }
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

        /* The ONE place a component's owner header is written -- after the
         * pool copies the caller's init bytes, so it always wins. */
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
            /* Deferred exactly like DestroyEntity. */
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
        /* BoundsComponent's removal observer: pulls the entity's leaf out of
         * the spatial index the moment the component is removed -- see
         * DESIGN.md for the bug this fixes. */
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

            /* Wired here, at first-use, rather than in World's constructor --
             * see DESIGN.md. */
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

            /* Refit at the END of TP_Physics -- see TickPhase.h. */
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

        /* Snapshot the count: a system registered mid-walk is picked up next
         * tick, not this one -- see DESIGN.md. */
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

        /* Swapped out, not iterated in place, so a re-entrant request during
         * the flush lands in a fresh queue -- see DESIGN.md. */
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

            /* bStatic skips RE-FITTING, never the FIRST insert -- see
             * DESIGN.md for the bug this fixes. */
            if(Bounds.bStatic && Bounds.ProxyID >= 0)
            {
                continue;
            }

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
