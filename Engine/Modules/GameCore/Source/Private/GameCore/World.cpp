#include "Public/GameCore/World.h"

#include <cctype>
#include <algorithm>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/GameCoreLog.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/Scalability.h"
#include "Private/GameCore/PrefabInternal.h"

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
            EMobility         Mobility = EMobility::Dynamic;
            EDetailMode       DetailMode = EDetailMode::Low;   // the scalability bucket
            std::string       Folder;   // the editor Outliner's, "" for none

            /* The hierarchy, as slot indices: an intrusive, ordered child
             * list. Slots, not EntityIDs -- a child is destroyed with its
             * parent, so a link never outlives the slot it names. */
            uint32_t Parent      = NO_SLOT;
            uint32_t FirstChild  = NO_SLOT;
            uint32_t LastChild   = NO_SLOT;
            uint32_t NextSibling = NO_SLOT;
            uint32_t PrevSibling = NO_SLOT;
            uint32_t ChildCount  = 0;

            /* The prefab instance it was spawned as part of, and its
             * template's id there; NO_SLOT / 0 for a loose entity. */
            uint32_t InstanceSlot = NO_SLOT;
            uint32_t LocalId      = PREFAB_NO_LOCAL_ID;
        };

        /* A prefab instance. Its entities are the root's subtree; each
         * member records the instance, so nothing here lists them. */
        struct InstanceMembers
        {
            EntityGuid      Guid;
            std::string     Name;
            std::string     PrefabPath;   // "" for an in-memory asset
            PrefabAsset     Prefab;       // shares the spawned version's data
            uint32_t        RootSlot = NO_SLOT;
            EPrefabTickMode TickMode = EPrefabTickMode::Tick;

            /* The prefab's script and native behaviour, and the driver's state for them. */
            std::string         ScriptPath;
            std::string         NativeBehavior;
            InstanceScriptState Script;

            bool HasScript() const { return !ScriptPath.empty() || !NativeBehavior.empty(); }

            /* Bit P: phase P may run. All set at spawn. */
            uint8_t PhaseEnabled = uint8_t((1u << TP_MAX) - 1u);
        };

        /* SetTickMode / SetPhaseEnabled inside a tick, applied at the flush. */
        struct PendingInstanceChange
        {
            PrefabInstanceID Instance;
            bool             bMode    = true;   // else a phase
            EPrefabTickMode  Mode     = EPrefabTickMode::Tick;
            uint8_t          Phase    = 0;
            bool             bEnabled = true;
        };

        bool IsLiveScriptRef(const int32_t InRef) { return InRef != -2 && InRef != -1; }   // LUA_NOREF, LUA_REFNIL

    }

    struct World::Impl
    {
        WorldSettings Settings;

        /* SetPreviewDetailMode's, -1 for none; DetailSerial bumps on any
         * change that can move an entity in or out of its detail bucket. */
        int      PreviewDetailMode = -1;
        uint32_t DetailSerial      = 0;

        // --- entities --------------------------------------------------------

        std::vector<uint32_t> Generations;     // indexed by EntityID::Index; bumped on every destroy
        std::vector<bool>     Alive;            // parallel to Generations
        std::vector<uint32_t> FreeIndices;
        size_t                LiveEntityCount = 0;

        /* Parallel to Generations. A deque so GetName()
         * pointers survive growth. */
        std::deque<EntityMembers> Members;

        /* GUID -> slot, for lookup and uniqueness. */
        std::unordered_map<EntityGuid, uint32_t, EntityGuidHash> GuidToSlot;

        /* Scripted slots in the order paths were set (tick order). */

        ScriptDriver Driver;
        bool         bHasDriver = false;

        // --- prefab instances -------------------------------------------------
        // The entity tables' shape again: generations, a free list, and a
        // deque so GetInstanceName() pointers survive growth.

        std::vector<uint32_t>       InstanceGenerations;
        std::vector<bool>           InstanceAlive;
        std::vector<uint32_t>       FreeInstances;
        std::deque<InstanceMembers> Instances;
        size_t                      LiveInstanceCount = 0;
        std::unordered_map<EntityGuid, uint32_t, EntityGuidHash> InstanceGuidToSlot;

        /* Per phase, the instance slots whose function runs there, in the
         * order they started. Changed only outside a phase's walk. */
        std::vector<uint32_t>                InstanceBuckets[TP_MAX];
        /* Spawned (or re-queued) instances whose script the driver has not
         * taken yet; stale entries are skipped when popped. */
        std::deque<PrefabInstanceID>         PendingInstanceScripts;
        std::vector<PendingInstanceChange>   PendingInstanceChanges;

        TMulticastDelegate<PrefabInstanceID> InstanceSpawned;
        TMulticastDelegate<PrefabInstanceID> InstanceDestroyed;

        // --- components --------------------------------------------------------

        ComponentPool* Pools[COMPONENT_TYPE_MAX] = {};

        // --- systems, by phase ---------------------------------------------

        std::vector<SystemEntry> Systems[TP_MAX];
        std::vector<SystemEntry> EarlySystems[TP_MAX];   // SO_BeforeScripts

        bool       bTicking      = false;
        ETickPhase CurrentPhase  = TP_PrePhysics;

        std::vector<EntityID>               PendingDestroys;
        std::vector<PendingComponentRemoval> PendingComponentRemovals;

        // --- spatial -----------------------------------------------------------

        SpatialIndex Spatial;

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
               || OverlapBegin.GetCount() > 0 || OverlapEnd.GetCount() > 0
               || InstanceSpawned.GetCount() > 0 || InstanceDestroyed.GetCount() > 0)
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

            Members[Index].Guid     = InGuid;
            Members[Index].Name     = InName ? InName : "";
            Members[Index].Mobility = EMobility::Dynamic;
            Members[Index].DetailMode = EDetailMode::Low;
            Members[Index].Folder.clear();
            GuidToSlot[InGuid]  = Index;

            return EntityID(Index, Generations[Index]);
        }

        EntityID SlotID(const uint32_t InSlot) const
        {
            return InSlot == NO_SLOT ? EntityID() : EntityID(InSlot, Generations[InSlot]);
        }

        bool IsInstanceAliveIndex(const PrefabInstanceID InInstance) const
        {
            return InInstance.IsSet() && InInstance.Index < InstanceAlive.size()
                && InstanceAlive[InInstance.Index] && InstanceGenerations[InInstance.Index] == InInstance.Generation;
        }

        PrefabInstanceID InstanceSlotID(const uint32_t InSlot) const
        {
            return InSlot == NO_SLOT ? PrefabInstanceID() : PrefabInstanceID(InSlot, InstanceGenerations[InSlot]);
        }

        /* InSlot is its instance's root. */
        bool IsInstanceRoot(const uint32_t InSlot) const
        {
            const uint32_t Instance = Members[InSlot].InstanceSlot;
            return Instance != NO_SLOT && Instances[Instance].RootSlot == InSlot;
        }

        /* An unset or used InGuid gets a fresh one (warned if used). */
        uint32_t AllocateInstanceSlot(EntityGuid InGuid)
        {
            if(InGuid.IsSet() && InstanceGuidToSlot.count(InGuid) != 0)
            {
                char Text[EntityGuid::STRING_BUFFER];
                InGuid.ToString(Text);
                GAMECORE_LOG_WARN(std::string("SpawnPrefab: instance GUID ") + Text
                                  + " is already in use in this World; giving the new instance a fresh one")
                InGuid = EntityGuid();
            }
            while(!InGuid.IsSet() || InstanceGuidToSlot.count(InGuid) != 0)
            {
                InGuid = EntityGuid::Generate();
            }

            uint32_t Index;
            if(!FreeInstances.empty())
            {
                Index = FreeInstances.back();
                FreeInstances.pop_back();
            }
            else
            {
                Index = uint32_t(InstanceGenerations.size());
                InstanceGenerations.push_back(0);
                InstanceAlive.push_back(false);
                Instances.emplace_back();
            }
            InstanceAlive[Index] = true;
            ++LiveInstanceCount;
            Instances[Index].Guid = InGuid;
            InstanceGuidToSlot[InGuid] = Index;
            return Index;
        }

        // --- instance scripts ---------------------------------------------------

        void RemoveFromBuckets(const uint32_t InSlot)
        {
            for(std::vector<uint32_t>& Bucket : InstanceBuckets)
            {
                const auto Found = std::find(Bucket.begin(), Bucket.end(), InSlot);
                if(Found != Bucket.end()) { Bucket.erase(Found); }   // ordered: tick order is start order
            }
        }

        /* Files InSlot under every phase it ticks in now: tick mode Tick, a
         * started healthy script (or native behaviour), the phase defined and
         * enabled. */
        void Rebucket(const uint32_t InSlot)
        {
            RemoveFromBuckets(InSlot);
            const InstanceMembers& Instance = Instances[InSlot];
            const bool bRunning = IsLiveScriptRef(Instance.Script.InstanceRef) || Instance.Script.bStarted;
            if(!InstanceAlive[InSlot] || Instance.TickMode != EPrefabTickMode::Tick || !bRunning || Instance.Script.bErrored)
            {
                return;
            }
            /* A static root ticks no phase: its events still arrive (EMobility). */
            if(Instance.RootSlot != NO_SLOT && Members[Instance.RootSlot].Mobility == EMobility::Static)
            {
                return;
            }
            const uint8_t Runs = Instance.Script.PhaseMask & Instance.PhaseEnabled;
            for(int Phase = 0; Phase < TP_MAX; ++Phase)
            {
                if(Runs & (1u << Phase)) { InstanceBuckets[Phase].push_back(InSlot); }
            }
        }

        /* Fresh state, queued for the driver -- if there is a script to run. */
        void QueueInstanceScript(const uint32_t InSlot)
        {
            InstanceMembers& Instance = Instances[InSlot];
            Instance.Script = InstanceScriptState();
            if(!Instance.HasScript() || Instance.TickMode == EPrefabTickMode::Off) { return; }
            Instance.Script.bPendingInstantiate = 1;
            PendingInstanceScripts.push_back(InstanceSlotID(InSlot));
        }

        /* The driver's ReleaseInstance (OnDestroy), then a clean state. Out
         * of every list either way. */
        void ReleaseInstanceScript(World& InWorld, const uint32_t InSlot)
        {
            const PrefabInstanceID ID = InstanceSlotID(InSlot);
            RemoveFromBuckets(InSlot);
            const InstanceScriptState& Script = Instances[InSlot].Script;
            if(bHasDriver && Driver.ReleaseInstance && (IsLiveScriptRef(Script.InstanceRef) || Script.bStarted))
            {
                Driver.ReleaseInstance(InWorld, ID, Driver.UserData);
            }
            /* OnDestroy may have ended the instance; then its slot is reset already. */
            if(IsInstanceAliveIndex(ID))
            {
                Instances[InSlot].Script = InstanceScriptState();
                RemoveFromBuckets(InSlot);
            }
        }

        void ApplyInstanceMode(World& InWorld, const uint32_t InSlot, const EPrefabTickMode InMode)
        {
            InstanceMembers& Instance = Instances[InSlot];
            const EPrefabTickMode Old = Instance.TickMode;
            if(Old == InMode) { return; }
            Instance.TickMode = InMode;
            if(InMode == EPrefabTickMode::Off)
            {
                ReleaseInstanceScript(InWorld, InSlot);
            }
            else if(Old == EPrefabTickMode::Off)
            {
                QueueInstanceScript(InSlot);   // starts afresh, OnStart and all
            }
            else
            {
                Rebucket(InSlot);
            }
        }

        void ApplyPhaseEnabled(const uint32_t InSlot, const uint8_t InPhase, const bool bInEnabled)
        {
            uint8_t& Bits = Instances[InSlot].PhaseEnabled;
            const uint8_t Bit = uint8_t(1u << InPhase);
            const uint8_t Next = bInEnabled ? uint8_t(Bits | Bit) : uint8_t(Bits & ~Bit);
            if(Next == Bits) { return; }
            Bits = Next;
            Rebucket(InSlot);
        }

        void FreeInstanceSlot(const uint32_t InSlot)
        {
            RemoveFromBuckets(InSlot);
            InstanceGuidToSlot.erase(Instances[InSlot].Guid);
            Instances[InSlot] = InstanceMembers();
            InstanceAlive[InSlot] = false;
            ++InstanceGenerations[InSlot];
            FreeInstances.push_back(InSlot);
            --LiveInstanceCount;
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

            /* An instance goes with its root, and is announced first, while
             * every one of its entities is still whole. */
            const uint32_t RootOfInstance = IsInstanceRoot(InEntity.Index) ? Members[InEntity.Index].InstanceSlot : NO_SLOT;
            if(RootOfInstance != NO_SLOT)
            {
                InstanceDestroyed.Broadcast(InstanceSlotID(RootOfInstance));
                if(!IsAliveIndex(InEntity)) { return; }   // a subscriber destroyed it

                /* Its script next (OnDestroy), with every entity still whole. */
                ReleaseInstanceScript(InWorld, RootOfInstance);
                if(!IsAliveIndex(InEntity)) { return; }
            }

            /* BEFORE any component is removed -- a subscriber can still read
             * what the entity had. */
            EntityDestroyed.Broadcast(InEntity);

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

            if(IsInstanceRoot(InEntity.Index))
            {
                FreeInstanceSlot(Members[InEntity.Index].InstanceSlot);
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

    // === Mobility ================================================================

    const char* MobilityName(const EMobility InMobility)
    {
        switch(InMobility)
        {
            case EMobility::Static:     return "Static";
            case EMobility::Stationary: return "Stationary";
            default:                    return "Dynamic";
        }
    }

    bool ParseMobility(const char* InText, EMobility& OutMobility)
    {
        const auto Equals = [InText](const char* InName)
        {
            const char* A = InText;
            const char* B = InName;
            for(; *A && *B; ++A, ++B)
            {
                if(std::tolower(static_cast<unsigned char>(*A)) != std::tolower(static_cast<unsigned char>(*B))) { return false; }
            }
            return *A == *B;
        };
        if(!InText) { return false; }
        if(Equals("Static"))  { OutMobility = EMobility::Static;  return true; }
        if(Equals("Stationary")) { OutMobility = EMobility::Stationary; return true; }
        if(Equals("Dynamic")) { OutMobility = EMobility::Dynamic; return true; }
        return false;
    }

    // === Detail mode =============================================================

    const char* DetailModeName(const EDetailMode InMode)
    {
        switch(InMode)
        {
            case EDetailMode::Medium: return "Medium";
            case EDetailMode::High:   return "High";
            case EDetailMode::Ultra:  return "Ultra";
            default:                  return "Low";
        }
    }

    bool ParseDetailMode(const char* InText, EDetailMode& OutMode)
    {
        if(!InText) { return false; }
        const auto Equals = [InText](const char* InName)
        {
            const char* A = InText;
            const char* B = InName;
            for(; *A && *B; ++A, ++B)
            {
                if(std::tolower(static_cast<unsigned char>(*A)) != std::tolower(static_cast<unsigned char>(*B))) { return false; }
            }
            return *A == *B;
        };
        if(Equals("Low")    || Equals("0")) { OutMode = EDetailMode::Low;    return true; }
        if(Equals("Medium") || Equals("Med") || Equals("1")) { OutMode = EDetailMode::Medium; return true; }
        if(Equals("High")   || Equals("2")) { OutMode = EDetailMode::High;   return true; }
        if(Equals("Ultra")  || Equals("3")) { OutMode = EDetailMode::Ultra;  return true; }
        return false;
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

    const char* World::GetEntityFolder(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->Members[InEntity.Index].Folder.c_str() : "";
    }

    bool World::SetEntityFolder(const EntityID InEntity, const char* InFolder)
    {
        if(!P->IsAliveIndex(InEntity)) { return false; }
        P->Members[InEntity.Index].Folder = InFolder ? InFolder : "";
        return true;
    }

    EMobility World::GetEntityMobility(const EntityID InEntity) const
    {
        if(!P->IsAliveIndex(InEntity)) { return EMobility::Dynamic; }
        const EntityMembers& Member = P->Members[InEntity.Index];

        /* A static root makes its instance's dynamic members static too. */
        if(Member.Mobility == EMobility::Dynamic && Member.InstanceSlot != NO_SLOT)
        {
            const uint32_t RootSlot = P->Instances[Member.InstanceSlot].RootSlot;
            if(RootSlot != NO_SLOT && P->Members[RootSlot].Mobility == EMobility::Static) { return EMobility::Static; }
        }
        return Member.Mobility;
    }

    EMobility World::GetEntityAuthoredMobility(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->Members[InEntity.Index].Mobility : EMobility::Dynamic;
    }

    bool World::SetEntityMobility(const EntityID InEntity, const EMobility InMobility)
    {
        if(!P->IsAliveIndex(InEntity)) { return false; }
        EntityMembers& Member = P->Members[InEntity.Index];
        if(Member.Mobility == InMobility) { return true; }
        Member.Mobility = InMobility;

        /* An instance's root decides whether it ticks. */
        if(Member.InstanceSlot != NO_SLOT && P->Instances[Member.InstanceSlot].RootSlot == InEntity.Index)
        {
            P->Rebucket(Member.InstanceSlot);
        }
        return true;
    }

    EDetailMode World::GetEntityDetailMode(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->Members[InEntity.Index].DetailMode : EDetailMode::Low;
    }

    bool World::SetEntityDetailMode(const EntityID InEntity, const EDetailMode InMode)
    {
        if(!P->IsAliveIndex(InEntity) || uint8_t(InMode) >= DETAIL_MODE_COUNT) { return false; }
        EntityMembers& Member = P->Members[InEntity.Index];
        if(Member.DetailMode != InMode)
        {
            Member.DetailMode = InMode;
            ++P->DetailSerial;
        }
        return true;
    }

    bool World::IsEntityDetailVisible(const EntityID InEntity) const
    {
        return IsDetailVisible(GetEntityDetailMode(InEntity), GetEffectiveDetailMode());
    }

    EDetailMode World::GetEffectiveDetailMode() const
    {
        if(P->PreviewDetailMode >= 0) { return EDetailMode(P->PreviewDetailMode); }
        const uint8_t Override = GetSettings().DetailMode;
        if(Override != WORLD_DETAIL_USE_GLOBAL && Override < DETAIL_MODE_COUNT) { return EDetailMode(Override); }
        return Scalability::GetDetailMode();
    }

    void World::SetPreviewDetailMode(const int InMode)
    {
        const int Clamped = InMode >= 0 && InMode < int(DETAIL_MODE_COUNT) ? InMode : -1;
        if(P->PreviewDetailMode != Clamped)
        {
            P->PreviewDetailMode = Clamped;
            ++P->DetailSerial;
        }
    }

    int World::GetPreviewDetailMode() const { return P->PreviewDetailMode; }

    uint32_t World::GetDetailSerial() const { return P->DetailSerial; }

    // === Hierarchy ===============================================================

    bool World::SetEntityParent(const EntityID InEntity, const EntityID InParent, const bool bInKeepWorldTransform)
    {
        if(!P->IsAliveIndex(InEntity)) { return false; }
        if(InParent.IsSet() && !P->IsAliveIndex(InParent)) { return false; }

        const uint32_t NewParent = InParent.IsSet() ? InParent.Index : NO_SLOT;
        if(P->Members[InEntity.Index].Parent == NewParent) { return true; }

        /* An instance's entities stay inside it: only the root may move to
         * another parent (an instance attached to something). */
        const uint32_t Instance = P->Members[InEntity.Index].InstanceSlot;
        if(Instance != NO_SLOT && !P->IsInstanceRoot(InEntity.Index)
           && (NewParent == NO_SLOT || P->Members[NewParent].InstanceSlot != Instance))
        {
            GAMECORE_LOG_WARN(std::string("SetParent: '") + GetEntityName(InEntity) + "' is part of prefab instance '"
                              + P->Instances[Instance].Name + "' and must stay under one of its entities; ignoring")
            return false;
        }

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

    // === Prefab instances ========================================================

    PrefabInstance World::SpawnPrefab(const PrefabAsset& InPrefab, const TransformPose& InAt, const char* InName,
                                      const EntityGuid& InGuid)
    {
        const PrefabAsset::Data& Data = PrefabAccess::Read(InPrefab);
        if(Data.Entities.empty())
        {
            GAMECORE_LOG_WARN(std::string("SpawnPrefab: '") + InPrefab.GetName() + "' has no entities; nothing spawned")
            return PrefabInstance();
        }

        const uint32_t Slot = P->AllocateInstanceSlot(InGuid);
        const PrefabInstanceID ID = P->InstanceSlotID(Slot);
        {
            InstanceMembers& Instance = P->Instances[Slot];
            Instance.Name     = InName ? InName : Data.Name;
            Instance.Prefab   = InPrefab;
            Instance.TickMode   = Data.TickMode;
            Instance.ScriptPath = Data.ScriptPath;
            Instance.NativeBehavior = Data.NativeBehavior;
        }
        const EntityGuid InstanceGuid = P->Instances[Slot].Guid;

        /* Every entity first, so references between them resolve; each is a
         * member before OnEntityCreated fires. */
        LocalIdMap ToLive;
        std::vector<EntityID> Spawned;
        Spawned.reserve(Data.Entities.size());
        for(const PrefabEntityData& Template : Data.Entities)
        {
            const EntityID New = P->AllocateEntitySlot(Template.Name.c_str(),
                                                       DerivePrefabEntityGuid(InstanceGuid, Template.LocalId));
            P->Members[New.Index].InstanceSlot = Slot;
            P->Members[New.Index].LocalId      = Template.LocalId;
            P->Members[New.Index].Mobility     = Template.Mobility;
            P->Members[New.Index].DetailMode   = Template.DetailMode;
            ToLive[Template.LocalId] = New;
            Spawned.push_back(New);
        }
        P->Instances[Slot].RootSlot = Spawned.front().Index;
        for(const EntityID New : Spawned)
        {
            P->EntityCreated.Broadcast(New);
        }

        /* Then components, from their defaults through the serializer. */
        std::vector<std::string> Warned;
        for(size_t Index = 0; Index < Data.Entities.size(); ++Index)
        {
            for(const PrefabComponentData& Component : Data.Entities[Index].Components)
            {
                const ComponentTypeInfo* Info = FindComponentType(Component.TypeName.c_str());
                std::vector<uint8_t> Defaults;
                if(!Info || !ComponentDefaults(Info->ID, Defaults))
                {
                    if(std::find(Warned.begin(), Warned.end(), Component.TypeName) == Warned.end())
                    {
                        Warned.push_back(Component.TypeName);
                        GAMECORE_LOG_WARN("SpawnPrefab: '" + Data.Name + "' has '" + Component.TypeName + "' components, which "
                                          + (Info ? "have no serializer" : "this build does not register") + "; skipping them")
                    }
                    continue;
                }
                void* Added = AddComponentRaw(Spawned[Index], Info->ID, Defaults.data());
                if(Added && !ReadComponentBlob(Info->ID, Component.Blob, ToLive, Added))
                {
                    GAMECORE_LOG_WARN("SpawnPrefab: '" + Data.Name + "': a '" + Component.TypeName
                                      + "' component could not be read; it keeps its defaults")
                }
            }
        }

        /* The tree; local Transforms are already relative to these parents. */
        for(size_t Index = 1; Index < Data.Entities.size(); ++Index)
        {
            const auto Parent = ToLive.find(Data.Entities[Index].ParentId);
            if(Parent != ToLive.end())
            {
                SetEntityParent(Spawned[Index], Parent->second, false);
            }
        }

        /* Placement: InAt over the template root's own Transform. A root
         * without one gets one, unless InAt would leave it where it is. */
        const EntityID Root = Spawned.front();
        const bool bMoves = InAt.Position.GetX() != 0.0 || InAt.Position.GetY() != 0.0 || InAt.Position.GetZ() != 0.0
                         || InAt.Rotation.GetX() != 0.0 || InAt.Rotation.GetY() != 0.0 || InAt.Rotation.GetZ() != 0.0
                         || InAt.Scale.GetX() != 1.0 || InAt.Scale.GetY() != 1.0 || InAt.Scale.GetZ() != 1.0;
        if(auto* Transform = static_cast<TransformComponent*>(GetComponentRaw(Root, TransformComponent::StaticType().ID)))
        {
            /* Not composed when it changes nothing: Euler angles through a
             * matrix and back are not bit-exact. */
            if(bMoves) { Transform->SetPose(TransformMath::Compose(InAt, Transform->GetPose())); }
        }
        else if(bMoves && IsAlive(Root))
        {
            TransformComponent Placed;
            Placed.SetPose(InAt);
            AddComponentRaw(Root, TransformComponent::StaticType().ID, &Placed);
        }
        /* Static geometry: fitted once, below, and never again. */
        if(Data.TickMode == EPrefabTickMode::Off)
        {
            for(const EntityID Member : Spawned)
            {
                if(auto* Bounds = static_cast<BoundsComponent*>(GetComponentRaw(Member, BoundsComponent::StaticType().ID)))
                {
                    Bounds->bStatic = 1;
                }
            }
        }
        RefreshEntityBounds(Root);

        if(P->IsInstanceAliveIndex(ID))
        {
            /* The driver starts it at the next phase, with every component in place. */
            P->QueueInstanceScript(Slot);
            P->InstanceSpawned.Broadcast(ID);
        }
        return PrefabInstance(this, ID);
    }

    PrefabInstance World::SpawnPrefab(const char* InPath, const TransformPose& InAt, const char* InName, const EntityGuid& InGuid)
    {
        const PrefabAsset* Prefab = FindOrLoadPrefab(InPath);
        if(!Prefab)
        {
            GAMECORE_LOG_WARN(std::string("SpawnPrefab: could not load '") + (InPath ? InPath : "") + "'")
            return PrefabInstance();
        }
        const PrefabInstance Spawned = SpawnPrefab(*Prefab, InAt, InName, InGuid);
        if(P->IsInstanceAliveIndex(Spawned.GetID()))
        {
            P->Instances[Spawned.GetID().Index].PrefabPath = InPath;
        }
        return Spawned;
    }

    PrefabInstance World::RestorePrefabInstance(const PrefabInstanceRecord& InRecord)
    {
        if(InRecord.Count == 0 || !InRecord.Entities || !InRecord.LocalIds)
        {
            GAMECORE_LOG_WARN("RestorePrefabInstance: no entities; nothing restored")
            return PrefabInstance();
        }
        const EntityID Root = InRecord.Entities[0];
        for(size_t Index = 0; Index < InRecord.Count; ++Index)
        {
            const EntityID Member = InRecord.Entities[Index];
            bool bUnderRoot = false;
            if(P->IsAliveIndex(Member))
            {
                for(uint32_t Slot = Member.Index; Slot != NO_SLOT; Slot = P->Members[Slot].Parent)
                {
                    if(Slot == Root.Index) { bUnderRoot = true; break; }
                }
            }
            if(!bUnderRoot || P->Members[Member.Index].InstanceSlot != NO_SLOT)
            {
                GAMECORE_LOG_WARN(std::string("RestorePrefabInstance: '") + InRecord.Name
                                  + "' names an entity that is gone, outside its root, or already in an instance; "
                                    "its entities stay loose")
                return PrefabInstance();
            }
        }

        const uint32_t Slot = P->AllocateInstanceSlot(InRecord.Guid);
        const PrefabInstanceID ID = P->InstanceSlotID(Slot);
        InstanceMembers& Instance = P->Instances[Slot];
        Instance.Name         = InRecord.Name ? InRecord.Name : "";
        Instance.PrefabPath   = InRecord.PrefabPath ? InRecord.PrefabPath : "";
        Instance.ScriptPath   = InRecord.ScriptPath ? InRecord.ScriptPath : "";
        Instance.TickMode     = InRecord.TickMode;
        Instance.PhaseEnabled = uint8_t(InRecord.PhaseEnabled & ((1u << TP_MAX) - 1u));
        Instance.RootSlot     = Root.Index;
        if(InRecord.Prefab)
        {
            Instance.Prefab         = *InRecord.Prefab;
            Instance.NativeBehavior = InRecord.Prefab->GetNativeBehavior();
        }
        for(size_t Index = 0; Index < InRecord.Count; ++Index)
        {
            P->Members[InRecord.Entities[Index].Index].InstanceSlot = Slot;
            P->Members[InRecord.Entities[Index].Index].LocalId      = InRecord.LocalIds[Index];
        }

        P->QueueInstanceScript(Slot);
        P->InstanceSpawned.Broadcast(ID);
        return PrefabInstance(this, ID);
    }

    bool World::AdoptIntoInstance(const PrefabInstanceID InInstance, const EntityID InEntity, const uint32_t InLocalId)
    {
        if(!P->IsInstanceAliveIndex(InInstance) || !P->IsAliveIndex(InEntity) || InLocalId == PREFAB_NO_LOCAL_ID
           || GetEntityInstance(InEntity).IsSet() || FindInstanceEntity(InInstance, InLocalId).IsSet())
        {
            return false;
        }

        /* Members are the root's subtree: InEntity must be under the root. */
        const EntityID Root = GetInstanceRoot(InInstance);
        EntityID Ancestor = GetEntity(InEntity).GetParent();
        while(Ancestor.IsSet() && Ancestor != Root)
        {
            Ancestor = GetEntity(Ancestor).GetParent();
        }
        if(Ancestor != Root)
        {
            return false;
        }

        P->Members[InEntity.Index].InstanceSlot = InInstance.Index;
        P->Members[InEntity.Index].LocalId      = InLocalId;
        return true;
    }

    void World::DestroyPrefabInstance(const PrefabInstanceID InInstance)
    {
        if(P->IsInstanceAliveIndex(InInstance))
        {
            DestroyEntity(P->SlotID(P->Instances[InInstance.Index].RootSlot));
        }
    }

    bool World::IsInstanceAlive(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance);
    }

    size_t World::GetInstanceCount() const
    {
        return P->LiveInstanceCount;
    }

    PrefabInstance World::GetInstance(const PrefabInstanceID InInstance)
    {
        return PrefabInstance(this, InInstance);
    }

    PrefabInstanceID World::FindInstanceByGuid(const EntityGuid& InGuid) const
    {
        const auto Found = P->InstanceGuidToSlot.find(InGuid);
        return Found == P->InstanceGuidToSlot.end() ? PrefabInstanceID() : P->InstanceSlotID(Found->second);
    }

    void World::ForEachInstance(void (*InFn)(PrefabInstanceID InInstance, void* InUserData), void* InUserData) const
    {
        if(!InFn) { return; }
        for(uint32_t Index = 0; Index < P->InstanceAlive.size(); ++Index)
        {
            if(P->InstanceAlive[Index])
            {
                InFn(P->InstanceSlotID(Index), InUserData);
            }
        }
    }

    TMulticastDelegate<PrefabInstanceID>& World::OnInstanceSpawned()   { return P->InstanceSpawned; }
    TMulticastDelegate<PrefabInstanceID>& World::OnInstanceDestroyed() { return P->InstanceDestroyed; }

    PrefabInstanceID World::GetEntityInstance(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->InstanceSlotID(P->Members[InEntity.Index].InstanceSlot) : PrefabInstanceID();
    }

    uint32_t World::GetEntityPrefabLocalId(const EntityID InEntity) const
    {
        return P->IsAliveIndex(InEntity) ? P->Members[InEntity.Index].LocalId : PREFAB_NO_LOCAL_ID;
    }

    EntityID World::GetInstanceRoot(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance) ? P->SlotID(P->Instances[InInstance.Index].RootSlot) : EntityID();
    }

    EntityID World::FindInstanceEntity(const PrefabInstanceID InInstance, const uint32_t InLocalId) const
    {
        if(!P->IsInstanceAliveIndex(InInstance) || InLocalId == PREFAB_NO_LOCAL_ID) { return EntityID(); }

        /* Its members are the root's subtree (loose children may sit among them). */
        std::vector<uint32_t> Stack{ P->Instances[InInstance.Index].RootSlot };
        while(!Stack.empty())
        {
            const uint32_t Slot = Stack.back();
            Stack.pop_back();
            const EntityMembers& Member = P->Members[Slot];
            if(Member.InstanceSlot == InInstance.Index && Member.LocalId == InLocalId)
            {
                return P->SlotID(Slot);
            }
            for(uint32_t Child = Member.FirstChild; Child != NO_SLOT; Child = P->Members[Child].NextSibling)
            {
                Stack.push_back(Child);
            }
        }
        return EntityID();
    }

    const PrefabAsset* World::GetInstancePrefab(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance) ? &P->Instances[InInstance.Index].Prefab : nullptr;
    }

    const char* World::GetInstancePrefabPath(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance) ? P->Instances[InInstance.Index].PrefabPath.c_str() : "";
    }

    EntityGuid World::GetInstanceGuid(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance) ? P->Instances[InInstance.Index].Guid : EntityGuid();
    }

    const char* World::GetInstanceName(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance) ? P->Instances[InInstance.Index].Name.c_str() : "";
    }

    bool World::SetInstanceName(const PrefabInstanceID InInstance, const char* InName)
    {
        if(!P->IsInstanceAliveIndex(InInstance)) { return false; }
        P->Instances[InInstance.Index].Name = InName ? InName : "";
        return true;
    }

    EPrefabTickMode World::GetInstanceTickMode(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance) ? P->Instances[InInstance.Index].TickMode : EPrefabTickMode::Tick;
    }

    bool World::SetInstanceTickMode(const PrefabInstanceID InInstance, const EPrefabTickMode InMode)
    {
        if(!P->IsInstanceAliveIndex(InInstance)) { return false; }
        if(P->bTicking)
        {
            /* Deferred: the driver may be walking a phase's list. */
            PendingInstanceChange Change;
            Change.Instance = InInstance;
            Change.bMode    = true;
            Change.Mode     = InMode;
            P->PendingInstanceChanges.push_back(Change);
            return true;
        }
        P->ApplyInstanceMode(*this, InInstance.Index, InMode);
        return true;
    }

    bool World::IsInstancePhaseEnabled(const PrefabInstanceID InInstance, const ETickPhase InPhase) const
    {
        return P->IsInstanceAliveIndex(InInstance) && InPhase < TP_MAX
            && (P->Instances[InInstance.Index].PhaseEnabled & (1u << InPhase)) != 0;
    }

    bool World::SetInstancePhaseEnabled(const PrefabInstanceID InInstance, const ETickPhase InPhase, const bool bInEnabled)
    {
        if(!P->IsInstanceAliveIndex(InInstance) || InPhase >= TP_MAX) { return false; }
        if(P->bTicking)
        {
            PendingInstanceChange Change;
            Change.Instance = InInstance;
            Change.bMode    = false;
            Change.Phase    = uint8_t(InPhase);
            Change.bEnabled = bInEnabled;
            P->PendingInstanceChanges.push_back(Change);
            return true;
        }
        P->ApplyPhaseEnabled(InInstance.Index, uint8_t(InPhase), bInEnabled);
        return true;
    }

    // === Instance scripts: the driver side ======================================

    bool World::PopPendingInstanceScript(PrefabInstanceID& OutInstance)
    {
        while(!P->PendingInstanceScripts.empty())
        {
            const PrefabInstanceID Next = P->PendingInstanceScripts.front();
            P->PendingInstanceScripts.pop_front();
            if(!P->IsInstanceAliveIndex(Next)) { continue; }
            InstanceScriptState& Script = P->Instances[Next.Index].Script;
            if(!Script.bPendingInstantiate) { continue; }   // released or re-queued since
            Script.bPendingInstantiate = 0;
            OutInstance = Next;
            return true;
        }
        return false;
    }

    InstanceScriptState* World::GetInstanceScriptState(const PrefabInstanceID InInstance) const
    {
        if(!P->IsInstanceAliveIndex(InInstance) || !P->Instances[InInstance.Index].HasScript()) { return nullptr; }
        return &P->Instances[InInstance.Index].Script;
    }

    const char* World::GetInstanceNativeBehavior(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance) ? P->Instances[InInstance.Index].NativeBehavior.c_str() : "";
    }

    const char* World::GetInstanceScriptPath(const PrefabInstanceID InInstance) const
    {
        return P->IsInstanceAliveIndex(InInstance) ? P->Instances[InInstance.Index].ScriptPath.c_str() : "";
    }

    void World::SetInstanceScriptPhaseMask(const PrefabInstanceID InInstance, const uint8_t InMask)
    {
        if(!P->IsInstanceAliveIndex(InInstance)) { return; }
        P->Instances[InInstance.Index].Script.PhaseMask = InMask;
        P->Rebucket(InInstance.Index);
    }

    size_t World::GetTickingInstanceCount(const ETickPhase InPhase) const
    {
        return InPhase < TP_MAX ? P->InstanceBuckets[InPhase].size() : 0;
    }

    PrefabInstanceID World::GetTickingInstanceAt(const ETickPhase InPhase, const size_t InIndex) const
    {
        if(InPhase >= TP_MAX || InIndex >= P->InstanceBuckets[InPhase].size()) { return PrefabInstanceID(); }
        return P->InstanceSlotID(P->InstanceBuckets[InPhase][InIndex]);
    }

    void World::RequeueInstanceScript(const PrefabInstanceID InInstance)
    {
        if(!P->IsInstanceAliveIndex(InInstance)) { return; }
        P->RemoveFromBuckets(InInstance.Index);
        P->QueueInstanceScript(InInstance.Index);
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

        /* Instance tick modes and phases, before destroys: an Off releases
         * through ReleaseInstanceScript, once. */
        std::vector<PendingInstanceChange> InstanceChangesThisFlush;
        std::swap(InstanceChangesThisFlush, P->PendingInstanceChanges);
        for(const PendingInstanceChange& Change : InstanceChangesThisFlush)
        {
            if(!P->IsInstanceAliveIndex(Change.Instance)) { continue; }
            if(Change.bMode) { P->ApplyInstanceMode(*this, Change.Instance.Index, Change.Mode); }
            else             { P->ApplyPhaseEnabled(Change.Instance.Index, Change.Phase, Change.bEnabled); }
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

            /* bStatic (or a static entity) skips RE-FITTING, never the FIRST
             * insert -- see DESIGN.md for the bug this fixes. */
            if((Bounds.bStatic || GetEntityMobility(Bounds.Owner.Entity) == EMobility::Static) && Bounds.ProxyID >= 0)
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
        if(!P->bHasDriver || !IsAlive(InEntity)) { return false; }
        const ScriptDriver Driver = P->Driver;

        /* The instance's script hears about its entities -- but not about
         * one of its own entities touching another. */
        const PrefabInstanceID Instance = GetEntityInstance(InEntity);
        if(Driver.CallInstanceMethod && P->IsInstanceAliveIndex(Instance) && GetEntityInstance(InArg) != Instance)
        {
            return Driver.CallInstanceMethod(*this, Instance, InMethod, InArg, InEntity, Driver.UserData);
        }
        return false;
    }

    EventBus& World::GetEvents() { return P->Events; }

    WorldSettings&       World::GetSettings()       { return P->Settings; }
    const WorldSettings& World::GetSettings() const { return P->Settings; }
}
