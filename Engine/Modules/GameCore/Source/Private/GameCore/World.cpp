#include "Public/GameCore/World.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * World's storage: entity free-list and generations, per-entity members,
 * lazily created component pools, per-phase systems, and the deferred
 * queues. See DESIGN.md's "World.h / World.cpp".
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

        /* "No slot" in the hierarchy links. */
        constexpr uint32_t NO_SLOT = 0xFFFFFFFFu;

        /* What an entity carries besides its components. */
        struct EntityMembers
        {
            EntityGuid        Guid;
            std::string       Name;
            std::string       ScriptPath;   // empty = no script
            EntityScriptState Script;

            /* The hierarchy, as slot indices: an intrusive, ordered child
             * list. Slots, not EntityIDs -- a child is destroyed with its
             * parent, so a link never outlives the slot it names. */
            uint32_t Parent      = NO_SLOT;
            uint32_t FirstChild  = NO_SLOT;
            uint32_t LastChild   = NO_SLOT;
            uint32_t NextSibling = NO_SLOT;
            uint32_t PrevSibling = NO_SLOT;
            uint32_t ChildCount  = 0;
        };

        struct PendingScriptChange
        {
            EntityID    Entity;
            std::string Path;   // empty = clear
        };
    }

    struct World::Impl
    {
        // --- entities --------------------------------------------------------

        std::vector<uint32_t> Generations;     // indexed by EntityID::Index; bumped on every destroy
        std::vector<bool>     Alive;            // parallel to Generations
        std::vector<uint32_t> FreeIndices;
        size_t                LiveEntityCount = 0;

        /* Parallel to Generations. A deque so GetName()/GetScriptState()
         * pointers survive growth. */
        std::deque<EntityMembers> Members;

        /* GUID -> slot, for lookup and uniqueness. */
        std::unordered_map<EntityGuid, uint32_t, EntityGuidHash> GuidToSlot;

        /* Scripted slots in the order paths were set (tick order). */
        std::vector<uint32_t> ScriptedSlots;

        ScriptDriver Driver;
        bool         bHasDriver = false;

        // --- components --------------------------------------------------------

        ComponentPool* Pools[COMPONENT_TYPE_MAX] = {};

        // --- systems, by phase ---------------------------------------------

        std::vector<SystemEntry> Systems[TP_MAX];
        std::vector<SystemEntry> EarlySystems[TP_MAX];   // SO_BeforeScripts

        bool       bTicking      = false;
        ETickPhase CurrentPhase  = TP_PrePhysics;

        std::vector<EntityID>               PendingDestroys;
        std::vector<PendingComponentRemoval> PendingComponentRemovals;
        std::vector<PendingScriptChange>     PendingScriptChanges;

        // --- spatial -----------------------------------------------------------

        SpatialIndex Spatial;
        AABB         ActiveRegion;
        bool         bHasActiveRegion = false;

        // --- delegates / events ----------------------------------------------

        TMulticastDelegate<EntityID>                  EntityCreated;
        TMulticastDelegate<EntityID>                  EntityDestroyed;
        TMulticastDelegate<EntityID, ComponentTypeID>  ComponentAdded;
        TMulticastDelegate<EntityID, ComponentTypeID>  ComponentRemoved;
        TMulticastDelegate<EntityID, EntityID>         OverlapBegin;
        TMulticastDelegate<EntityID, EntityID>         OverlapEnd;
        EventBus                                       Events;

        /* Overlapping pairs as of the last UpdateOverlaps: packed ids, lower
         * first, sorted (Collision.cpp). */
        std::vector<std::pair<uint64_t, uint64_t>>     Overlaps;

        ~Impl()
        {
            for(ComponentPool* Pool : Pools)
            {
                delete Pool;
            }

            /* Cannot enforce that every subscriber unbound itself, but can
             * detect and warn -- see DESIGN.md. */
            if(EntityCreated.GetCount() > 0 || EntityDestroyed.GetCount() > 0
               || ComponentAdded.GetCount() > 0 || ComponentRemoved.GetCount() > 0
               || OverlapBegin.GetCount() > 0 || OverlapEnd.GetCount() > 0)
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

        /* Slot and members only, no broadcast, so name and GUID are set
         * before OnEntityCreated (DESIGN.md). An unset or used InGuid gets a
         * fresh one (warned if used). */
        EntityID AllocateEntitySlot(const char* InName, EntityGuid InGuid)
        {
            if(InGuid.IsSet() && GuidToSlot.count(InGuid) != 0)
            {
                char Text[EntityGuid::STRING_BUFFER];
                InGuid.ToString(Text);
                GAMECORE_LOG_WARN(std::string("CreateEntity: GUID ") + Text
                                  + " is already in use in this World; giving the new entity a fresh one")
                InGuid = EntityGuid();
            }
            while(!InGuid.IsSet() || GuidToSlot.count(InGuid) != 0)
            {
                InGuid = EntityGuid::Generate();
            }

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
                Members.emplace_back();
            }

            Alive[Index] = true;
            ++LiveEntityCount;

            Members[Index].Guid = InGuid;
            Members[Index].Name = InName ? InName : "";
            GuidToSlot[InGuid]  = Index;

            return EntityID(Index, Generations[Index]);
        }

        /* Driver Release, then forget. Nothing is held across the Lua call. */
        void ReleaseScript(World& InWorld, const EntityID InEntity)
        {
            if(bHasDriver && Driver.Release)
            {
                Driver.Release(InWorld, InEntity, Driver.UserData);
            }

            EntityMembers& Slot = Members[InEntity.Index];
            Slot.ScriptPath.clear();
            Slot.Script = EntityScriptState();

            const auto Found = std::find(ScriptedSlots.begin(), ScriptedSlots.end(), InEntity.Index);
            if(Found != ScriptedSlots.end())
            {
                ScriptedSlots.erase(Found);   // ordered: tick order is set order
            }
        }

        /* SetScript's immediate half, also run by the phase flush. */
        void ApplyScriptChange(World& InWorld, const EntityID InEntity, const std::string& InPath)
        {
            if(!IsAliveIndex(InEntity) || Members[InEntity.Index].ScriptPath == InPath)
            {
                return;
            }

            if(!Members[InEntity.Index].ScriptPath.empty())
            {
                ReleaseScript(InWorld, InEntity);
                if(!IsAliveIndex(InEntity)) { return; }   // its OnDestroy destroyed it
            }

            if(InPath.empty())
            {
                return;
            }

            EntityMembers& Slot = Members[InEntity.Index];
            Slot.ScriptPath                 = InPath;
            Slot.Script                     = EntityScriptState();
            Slot.Script.bPendingInstantiate = 1;
            ScriptedSlots.push_back(InEntity.Index);
        }

        EntityID SlotID(const uint32_t InSlot) const
        {
            return InSlot == NO_SLOT ? EntityID() : EntityID(InSlot, Generations[InSlot]);
        }

        /* Takes InSlot out of its parent's child list; it becomes a root. */
        void Unlink(const uint32_t InSlot)
        {
            EntityMembers& Child = Members[InSlot];
            if(Child.Parent == NO_SLOT) { return; }

            EntityMembers& Parent = Members[Child.Parent];
            if(Child.PrevSibling != NO_SLOT) { Members[Child.PrevSibling].NextSibling = Child.NextSibling; }
            else                             { Parent.FirstChild = Child.NextSibling; }
            if(Child.NextSibling != NO_SLOT) { Members[Child.NextSibling].PrevSibling = Child.PrevSibling; }
            else                             { Parent.LastChild = Child.PrevSibling; }
            --Parent.ChildCount;

            Child.Parent = Child.NextSibling = Child.PrevSibling = NO_SLOT;
        }

        /* InSlot (a root) becomes InParentSlot's last child. */
        void LinkLast(const uint32_t InSlot, const uint32_t InParentSlot)
        {
            EntityMembers& Child  = Members[InSlot];
            EntityMembers& Parent = Members[InParentSlot];
            Child.Parent      = InParentSlot;
            Child.PrevSibling = Parent.LastChild;
            Child.NextSibling = NO_SLOT;
            if(Parent.LastChild != NO_SLOT) { Members[Parent.LastChild].NextSibling = InSlot; }
            else                            { Parent.FirstChild = InSlot; }
            Parent.LastChild = InSlot;
            ++Parent.ChildCount;
        }

        /* Destroys InSlot's children (and theirs) one at a time, re-reading
         * the list after each: a child's OnDestroy may reparent or destroy
         * a sibling, or even give this entity a new child. */
        void DestroyChildren(World& InWorld, const uint32_t InSlot)
        {
            uint32_t Guard = 0;
            while(Members[InSlot].FirstChild != NO_SLOT)
            {
                const uint32_t Child = Members[InSlot].FirstChild;
                DestroyEntityImmediate(InWorld, SlotID(Child));

                /* Still first: it could not be destroyed (it is mid-destroy
                 * higher up the stack). Detach it so the loop ends, and the
                 * outer destroy finishes it. Bounded by the guard in any case. */
                if(Members[InSlot].FirstChild == Child || ++Guard > 1000000u)
                {
                    GAMECORE_LOG_WARN("DestroyEntity: a child could not be destroyed with its parent; detaching it")
                    Unlink(Members[InSlot].FirstChild);
                }
            }
        }

        /* Removes InEntity's every component IMMEDIATELY, bypassing
         * RemoveComponentRaw's own deferral -- see DESIGN.md for why calling
         * that here would be wrong, not merely redundant. Only ever runs
         * from TickPhase()'s flush. */
        void DestroyEntityImmediate(World& InWorld, const EntityID InEntity)
        {
            if(!IsAliveIndex(InEntity))
            {
                return;
            }

            /* BEFORE any component is removed -- a subscriber can still read
             * what the entity had. */
            EntityDestroyed.Broadcast(InEntity);

            /* The script next, still before any component goes, so its
             * OnDestroy can read its siblings. */
            if(IsAliveIndex(InEntity) && !Members[InEntity.Index].ScriptPath.empty())
            {
                ReleaseScript(InWorld, InEntity);
            }

            /* Destroyed re-entrantly already; don't free the slot twice. */
            if(!IsAliveIndex(InEntity))
            {
                return;
            }

            /* Children before this entity's components go, so their
             * OnDestroy can still read the parent. */
            DestroyChildren(InWorld, InEntity.Index);
            if(!IsAliveIndex(InEntity))
            {
                return;
            }
            Unlink(InEntity.Index);

            for(ComponentTypeID Type = 0; Type < COMPONENT_TYPE_MAX; ++Type)
            {
                if(Pools[Type] && Pools[Type]->Has(InEntity))
                {
                    ComponentRemoved.Broadcast(InEntity, Type);
                    Pools[Type]->Remove(InEntity);
                }
            }

            GuidToSlot.erase(Members[InEntity.Index].Guid);
            Members[InEntity.Index] = EntityMembers();
            Alive[InEntity.Index] = false;
            ++Generations[InEntity.Index];
            FreeIndices.push_back(InEntity.Index);
            --LiveEntityCount;
        }
    };

    EntityGuid EntityGuid::Generate()
    {
        /* The OS generator on every supported platform. */
        static std::random_device Device;
        EntityGuid Result;
        do
        {
            Result.Hi = (uint64_t(Device()) << 32) | uint64_t(Device());
            Result.Lo = (uint64_t(Device()) << 32) | uint64_t(Device());
        }
        while(!Result.IsSet());
        return Result;
    }

    World::World() : P(new Impl())
    {
        /* Overlaps, first in TP_PostPhysics: the boxes were refit at the end
         * of TP_Physics, and that phase's scripts see the events' effects. */
        RegisterSystem("GameCore.Overlaps", TP_PostPhysics,
                       [](World& InWorld, float, void*) { InWorld.UpdateOverlaps(); }, P, SO_BeforeScripts);
    }

    World::~World()
    {
        delete P;
    }

    // === Entities ===============================================================

    Entity World::CreateEntity()
    {
        return CreateEntity(nullptr, EntityGuid());
    }

    Entity World::CreateEntity(const char* InName)
    {
        return CreateEntity(InName, EntityGuid());
    }

    Entity World::CreateEntity(const char* InName, const EntityGuid& InGuid)
    {
        /* Name and GUID before OnEntityCreated (DESIGN.md). */
        const EntityID NewEntity = P->AllocateEntitySlot(InName, InGuid);
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

        P->DestroyEntityImmediate(*this, InEntity);
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
        if(!InName || InName[0] == '\0') { return EntityID(); }

        for(uint32_t Index = 0; Index < P->Alive.size(); ++Index)
        {
            if(P->Alive[Index] && P->Members[Index].Name == InName)
            {
                return EntityID(Index, P->Generations[Index]);
            }
        }
        return EntityID();
    }

    EntityID World::FindEntityByGuid(const EntityGuid& InGuid) const
    {
        const auto Found = P->GuidToSlot.find(InGuid);
        if(Found == P->GuidToSlot.end()) { return EntityID(); }
        return EntityID(Found->second, P->Generations[Found->second]);
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

    // === Entity members ==========================================================

    EntityGuid World::GetEntityGuid(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->Members[InEntity.Index].Guid : EntityGuid();
    }

    const char* World::GetEntityName(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->Members[InEntity.Index].Name.c_str() : "";
    }

    bool World::SetEntityName(const EntityID InEntity, const char* InName)
    {
        if(!P->IsAliveIndex(InEntity)) { return false; }
        P->Members[InEntity.Index].Name = InName ? InName : "";
        return true;
    }

    const char* World::GetEntityScriptPath(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->Members[InEntity.Index].ScriptPath.c_str() : "";
    }

    bool World::SetEntityScript(const EntityID InEntity, const char* InPath)
    {
        if(!P->IsAliveIndex(InEntity)) { return false; }

        std::string Path(InPath ? InPath : "");
        if(P->bTicking)
        {
            /* Deferred like DestroyEntity: the driver may be mid-walk. */
            PendingScriptChange Change;
            Change.Entity = InEntity;
            Change.Path   = std::move(Path);
            P->PendingScriptChanges.push_back(std::move(Change));
            return true;
        }

        P->ApplyScriptChange(*this, InEntity, Path);
        return true;
    }

    EntityScriptState* World::GetEntityScriptState(const EntityID InEntity) const
    {
        if(!P->IsAliveIndex(InEntity) || P->Members[InEntity.Index].ScriptPath.empty())
        {
            return nullptr;
        }
        return &P->Members[InEntity.Index].Script;
    }

    // === Hierarchy ===============================================================

    bool World::SetEntityParent(const EntityID InEntity, const EntityID InParent, const bool bInKeepWorldTransform)
    {
        if(!P->IsAliveIndex(InEntity)) { return false; }
        if(InParent.IsSet() && !P->IsAliveIndex(InParent)) { return false; }

        const uint32_t NewParent = InParent.IsSet() ? InParent.Index : NO_SLOT;
        if(P->Members[InEntity.Index].Parent == NewParent) { return true; }

        /* No cycles: InParent may not be InEntity or below it. */
        for(uint32_t Slot = NewParent; Slot != NO_SLOT; Slot = P->Members[Slot].Parent)
        {
            if(Slot == InEntity.Index)
            {
                GAMECORE_LOG_WARN(std::string("SetParent: '") + GetEntityName(InParent) + "' is '"
                                  + GetEntityName(InEntity) + "' or one of its descendants; ignoring")
                return false;
            }
        }

        const TransformPose WorldPose = GetEntityWorldPose(InEntity);
        P->Unlink(InEntity.Index);
        if(NewParent != NO_SLOT)
        {
            P->LinkLast(InEntity.Index, NewParent);
        }

        if(bInKeepWorldTransform)
        {
            SetEntityWorldPose(InEntity, WorldPose);
        }
        return true;
    }

    EntityID World::GetEntityParent(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->SlotID(P->Members[InEntity.Index].Parent) : EntityID();
    }

    EntityID World::GetEntityFirstChild(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->SlotID(P->Members[InEntity.Index].FirstChild) : EntityID();
    }

    EntityID World::GetEntityNextSibling(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->SlotID(P->Members[InEntity.Index].NextSibling) : EntityID();
    }

    size_t World::GetEntityChildCount(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->Members[InEntity.Index].ChildCount : 0;
    }

    TransformPose World::GetEntityWorldPose(const EntityID InEntity) const
    {
        if(!P->IsAliveIndex(InEntity)) { return TransformPose(); }

        const auto* Transform = static_cast<const TransformComponent*>(GetComponentRaw(InEntity, TransformComponent::StaticType().ID));
        const TransformPose Local = Transform ? Transform->GetPose() : TransformPose();

        const uint32_t Parent = P->Members[InEntity.Index].Parent;
        if(Parent == NO_SLOT)
        {
            return Local;
        }
        /* The parent's world pose first, then this one under it: TRS
         * composition is not associative once a scale is non-uniform, and
         * root-down is the definition. Recursion depth is hierarchy depth. */
        return TransformMath::Compose(GetEntityWorldPose(P->SlotID(Parent)), Local);
    }

    EngineCore::Matrix4x4 World::GetEntityWorldMatrix(const EntityID InEntity) const
    {
        if(!P->IsAliveIndex(InEntity)) { return EngineCore::Matrix4x4(); }

        /* Unparented: exactly the local matrix, as before parenting existed. */
        if(P->Members[InEntity.Index].Parent == NO_SLOT)
        {
            const auto* Transform = static_cast<const TransformComponent*>(GetComponentRaw(InEntity, TransformComponent::StaticType().ID));
            return Transform ? Transform->GetLocalMatrix() : EngineCore::Matrix4x4();
        }
        return PoseToMatrix(GetEntityWorldPose(InEntity));
    }

    bool World::SetEntityWorldPose(const EntityID InEntity, const TransformPose& InPose)
    {
        auto* Transform = static_cast<TransformComponent*>(GetComponentRaw(InEntity, TransformComponent::StaticType().ID));
        if(!Transform) { return false; }

        const EntityID Parent = GetEntityParent(InEntity);
        if(!Parent.IsSet())
        {
            Transform->SetPose(InPose);
            return true;
        }
        /* Near the current local angles, so a round trip does not flip them. */
        Transform->SetPose(TransformMath::Relative(GetEntityWorldPose(Parent), InPose, Transform->Rotation));
        return true;
    }

    void World::SetScriptDriver(const ScriptDriver* InDriver)
    {
        P->Driver     = InDriver ? *InDriver : ScriptDriver();
        P->bHasDriver = InDriver != nullptr;
    }

    bool World::HasScriptDriver() const
    {
        return P->bHasDriver;
    }

    size_t World::GetScriptedEntityCount() const
    {
        return P->ScriptedSlots.size();
    }

    EntityID World::GetScriptedEntityAt(const size_t InIndex) const
    {
        if(InIndex >= P->ScriptedSlots.size()) { return EntityID(); }
        const uint32_t Slot = P->ScriptedSlots[InIndex];
        return EntityID(Slot, P->Generations[Slot]);
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

    void World::RegisterSystem(const char* InName, const ETickPhase InPhase, const SystemFn InFn, void* InUserData,
                               const ESystemOrder InOrder)
    {
        if(InPhase >= TP_MAX || !InFn)
        {
            GAMECORE_LOG_ERROR(std::string("RegisterSystem(\"") + (InName ? InName : "?") + "\") given a bad phase or a null function")
            return;
        }
        SystemEntry Entry;
        Entry.Fn       = InFn;
        Entry.UserData = InUserData;
        (InOrder == SO_BeforeScripts ? P->EarlySystems : P->Systems)[InPhase].push_back(Entry);
    }

    void World::UnregisterSystem(const SystemFn InFn, void* InUserData)
    {
        for(int BucketIndex = 0; BucketIndex < 2 * TP_MAX; ++BucketIndex)
        {
            std::vector<SystemEntry>& Bucket = BucketIndex < TP_MAX ? P->Systems[BucketIndex] : P->EarlySystems[BucketIndex - TP_MAX];
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
        for(int BucketIndex = 0; BucketIndex < 2 * TP_MAX; ++BucketIndex)
        {
            std::vector<SystemEntry>& Bucket = BucketIndex < TP_MAX ? P->Systems[BucketIndex] : P->EarlySystems[BucketIndex - TP_MAX];
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
         * tick, not this one -- see DESIGN.md. Copied: a bucket may reallocate
         * if the system itself registers one. */
        const auto RunSystems = [&](const std::vector<SystemEntry>& InBucket)
        {
            const size_t SystemCount = InBucket.size();
            for(size_t Index = 0; Index < SystemCount; ++Index)
            {
                const SystemEntry Entry = InBucket[Index];
                if(Entry.Fn)
                {
                    Entry.Fn(*this, InDeltaSeconds, Entry.UserData);
                }
            }
        };

        /* What this phase's scripts must already see -- input, at PrePhysics. */
        RunSystems(P->EarlySystems[InPhase]);

        /* Scripts before every other system (DESIGN.md). */
        if(P->bHasDriver && P->Driver.TickPhase)
        {
            const ScriptDriver Driver = P->Driver;
            Driver.TickPhase(*this, InPhase, InDeltaSeconds, Driver.UserData);
        }

        RunSystems(P->Systems[InPhase]);

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

        /* Before destroys, so each releases through ReleaseScript once. */
        std::vector<PendingScriptChange> ScriptChangesThisFlush;
        std::swap(ScriptChangesThisFlush, P->PendingScriptChanges);
        for(const PendingScriptChange& Change : ScriptChangesThisFlush)
        {
            P->ApplyScriptChange(*this, Change.Entity, Change.Path);
        }

        std::vector<EntityID> DestroysThisFlush;
        std::swap(DestroysThisFlush, P->PendingDestroys);
        for(const EntityID& Entity : DestroysThisFlush)
        {
            P->DestroyEntityImmediate(*this, Entity);
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

            Bounds.World = AABB::Transformed(Bounds.Local, GetEntityWorldMatrix(Bounds.Owner.Entity));

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

    void World::RefreshEntityBounds(const EntityID InEntity)
    {
        if(!P->IsAliveIndex(InEntity))
        {
            return;
        }

        if(auto* Bounds = static_cast<BoundsComponent*>(GetComponentRaw(InEntity, BoundsComponent::StaticType().ID)))
        {
            Bounds->World = AABB::Transformed(Bounds->Local, GetEntityWorldMatrix(InEntity));

            if(Bounds->ProxyID < 0)
            {
                Bounds->ProxyID = P->Spatial.Insert(InEntity, Bounds->World);
            }
            else
            {
                P->Spatial.Move(Bounds->ProxyID, Bounds->World);
            }
        }

        /* The children moved with it. */
        for(uint32_t Child = P->Members[InEntity.Index].FirstChild; Child != NO_SLOT; Child = P->Members[Child].NextSibling)
        {
            RefreshEntityBounds(P->SlotID(Child));
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
    TMulticastDelegate<EntityID, EntityID>&        World::OnOverlapBegin()      { return P->OverlapBegin; }
    TMulticastDelegate<EntityID, EntityID>&        World::OnOverlapEnd()        { return P->OverlapEnd; }

    std::vector<std::pair<uint64_t, uint64_t>>& World::GetOverlapPairs()             { return P->Overlaps; }
    const std::vector<std::pair<uint64_t, uint64_t>>& World::GetOverlapPairs() const { return P->Overlaps; }

    bool World::CallScriptMethod(const EntityID InEntity, const char* InMethod, const EntityID InArg)
    {
        if(!P->bHasDriver || !P->Driver.CallMethod || !IsAlive(InEntity)) { return false; }
        const ScriptDriver Driver = P->Driver;
        return Driver.CallMethod(*this, InEntity, InMethod, InArg, Driver.UserData);
    }

    EventBus& World::GetEvents() { return P->Events; }
}
