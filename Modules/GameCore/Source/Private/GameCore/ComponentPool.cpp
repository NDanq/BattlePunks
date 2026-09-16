#include "Public/GameCore/ComponentPool.h"

#include <cstring>
#include <string>
#include <vector>

#include "Public/Core/CoreLog.h"
#include "Public/Core/CoreMemoryPool.h"
#include "Public/GameCore/GameCoreLog.h"

/*
 * The sparse set. See ComponentPool.h and DESIGN.md for the shape and the
 * reasoning; this is the mechanics.
 */

namespace GameCore
{
    struct ComponentPool::Impl
    {
        struct SparseEntry
        {
            uint32_t DenseIndex = 0xFFFFFFFFu;   // index into Dense/DenseEntities, or "absent"
            uint32_t Generation = 0;             // the entity generation this slot was added under
        };

        ComponentTypeInfo Type;

        uint8_t*  Dense         = nullptr;       // SystemAlignedAlloc'd, Type.Alignment aligned
        size_t    DenseCapacity = 0;              // elements
        size_t    DenseCount    = 0;

        std::vector<EntityID>    DenseEntities;   // parallel to Dense; DenseEntities[i] owns Dense + i*Type.SizeBytes
        std::vector<SparseEntry> Sparse;          // indexed by EntityID::Index

        ComponentRemovedFn RemovedFn       = nullptr;
        void*              RemovedUserData = nullptr;

        explicit Impl(const ComponentTypeInfo& InType) : Type(InType) {}

        ~Impl()
        {
            if(Dense)
            {
                EngineCore::SystemAlignedFree(Dense);
            }
        }

        uint8_t* SlotAt(const size_t InDenseIndex) const
        {
            return Dense + InDenseIndex * size_t(Type.SizeBytes);
        }

        void EnsureSparseCovers(const uint32_t InIndex)
        {
            if(InIndex >= Sparse.size())
            {
                Sparse.resize(size_t(InIndex) + 1);
            }
        }

        void EnsureDenseCapacity(const size_t InRequired)
        {
            if(InRequired <= DenseCapacity)
            {
                return;
            }

            size_t NewCapacity = DenseCapacity == 0 ? 16 : DenseCapacity * 2;
            while(NewCapacity < InRequired)
            {
                NewCapacity *= 2;
            }

            uint8_t* NewDense = static_cast<uint8_t*>(
                EngineCore::SystemAlignedAlloc(NewCapacity * size_t(Type.SizeBytes), Type.Alignment));

            if(!NewDense)
            {
                GAMECORE_LOG_ERROR(std::string("ComponentPool(\"") + Type.Name + "\"): allocation failed growing to "
                                    + std::to_string(NewCapacity) + " elements")
                return;
            }

            if(Dense && DenseCount > 0)
            {
                std::memcpy(NewDense, Dense, DenseCount * size_t(Type.SizeBytes));
            }
            if(Dense)
            {
                EngineCore::SystemAlignedFree(Dense);
            }

            Dense         = NewDense;
            DenseCapacity = NewCapacity;
        }

        bool TryResolve(const EntityID InEntity, uint32_t& OutDenseIndex) const
        {
            if(!InEntity.IsSet() || InEntity.Index >= Sparse.size())
            {
                return false;
            }
            const SparseEntry& Entry = Sparse[InEntity.Index];
            if(Entry.DenseIndex == 0xFFFFFFFFu || Entry.Generation != InEntity.Generation)
            {
                return false;
            }
            OutDenseIndex = Entry.DenseIndex;
            return true;
        }
    };

    ComponentPool::ComponentPool(const ComponentTypeInfo& InType) : P(new Impl(InType)) {}

    ComponentPool::~ComponentPool()
    {
        delete P;
    }

    void* ComponentPool::Add(const EntityID InEntity, const void* InInitBytes)
    {
        uint32_t Existing;
        if(P->TryResolve(InEntity, Existing))
        {
            GAMECORE_LOG_WARN(std::string("ComponentPool(\"") + P->Type.Name + "\"): Add() called for an entity that "
                               "already has this component; ignoring")
            return nullptr;
        }

        P->EnsureDenseCapacity(P->DenseCount + 1);
        if(P->DenseCount >= P->DenseCapacity)
        {
            /* EnsureDenseCapacity already logged the allocation failure. */
            return nullptr;
        }

        const size_t NewSlot = P->DenseCount;
        uint8_t* Payload = P->SlotAt(NewSlot);
        std::memcpy(Payload, InInitBytes, P->Type.SizeBytes);

        if(P->DenseEntities.size() <= NewSlot)
        {
            P->DenseEntities.resize(NewSlot + 1);
        }
        P->DenseEntities[NewSlot] = InEntity;

        P->EnsureSparseCovers(InEntity.Index);
        P->Sparse[InEntity.Index].DenseIndex = uint32_t(NewSlot);
        P->Sparse[InEntity.Index].Generation = InEntity.Generation;

        ++P->DenseCount;
        return Payload;
    }

    void* ComponentPool::Get(const EntityID InEntity) const
    {
        uint32_t DenseIndex;
        if(!P->TryResolve(InEntity, DenseIndex))
        {
            return nullptr;
        }
        return P->SlotAt(DenseIndex);
    }

    bool ComponentPool::Has(const EntityID InEntity) const
    {
        uint32_t Unused;
        return P->TryResolve(InEntity, Unused);
    }

    bool ComponentPool::Remove(const EntityID InEntity)
    {
        uint32_t DenseIndex;
        if(!P->TryResolve(InEntity, DenseIndex))
        {
            return false;
        }

        uint8_t* Payload = P->SlotAt(DenseIndex);
        if(P->RemovedFn)
        {
            /* Observer sees the final live values, before the swap-and-pop
             * below overwrites this slot -- see the class comment. */
            P->RemovedFn(InEntity, Payload, P->RemovedUserData);
        }

        const size_t LastSlot = P->DenseCount - 1;
        if(DenseIndex != LastSlot)
        {
            /* POD swap-and-pop: move the last live element into the hole. Safe
             * ONLY because GAMECORE_DEFINE_COMPONENT enforces trivial
             * copyability on every component -- see ComponentPool.h. */
            std::memcpy(Payload, P->SlotAt(LastSlot), P->Type.SizeBytes);

            const EntityID MovedEntity = P->DenseEntities[LastSlot];
            P->DenseEntities[DenseIndex] = MovedEntity;
            P->Sparse[MovedEntity.Index].DenseIndex = uint32_t(DenseIndex);
        }

        P->Sparse[InEntity.Index].DenseIndex = 0xFFFFFFFFu;
        --P->DenseCount;
        return true;
    }

    size_t ComponentPool::GetCount() const
    {
        return P->DenseCount;
    }

    void* ComponentPool::GetDenseData() const
    {
        return P->Dense;
    }

    EntityID ComponentPool::GetEntityAt(const size_t InDenseIndex) const
    {
        if(InDenseIndex >= P->DenseCount)
        {
            return EntityID();
        }
        return P->DenseEntities[InDenseIndex];
    }

    const ComponentTypeInfo& ComponentPool::GetType() const
    {
        return P->Type;
    }

    void ComponentPool::Reorder(const EntityID* InOrder, const size_t InCount)
    {
        if(InCount != P->DenseCount)
        {
            GAMECORE_LOG_ERROR(std::string("ComponentPool(\"") + P->Type.Name + "\"): Reorder() count ("
                                + std::to_string(InCount) + ") does not match live count ("
                                + std::to_string(P->DenseCount) + "); ignoring")
            return;
        }

        /* One extra allocation rather than permuting in place -- see
         * DESIGN.md for why. */
        uint8_t* Scratch = static_cast<uint8_t*>(
            EngineCore::SystemAlignedAlloc(InCount * size_t(P->Type.SizeBytes), P->Type.Alignment));
        if(!Scratch)
        {
            GAMECORE_LOG_ERROR(std::string("ComponentPool(\"") + P->Type.Name + "\"): Reorder() allocation failed")
            return;
        }

        std::vector<EntityID> NewDenseEntities(InCount);

        for(size_t NewIndex = 0; NewIndex < InCount; ++NewIndex)
        {
            uint32_t OldIndex;
            if(!P->TryResolve(InOrder[NewIndex], OldIndex))
            {
                GAMECORE_LOG_ERROR(std::string("ComponentPool(\"") + P->Type.Name
                                    + "\"): Reorder() given an entity that does not have this component; aborting")
                EngineCore::SystemAlignedFree(Scratch);
                return;
            }
            std::memcpy(Scratch + NewIndex * size_t(P->Type.SizeBytes), P->SlotAt(OldIndex), P->Type.SizeBytes);
            NewDenseEntities[NewIndex] = InOrder[NewIndex];
        }

        std::memcpy(P->Dense, Scratch, InCount * size_t(P->Type.SizeBytes));
        EngineCore::SystemAlignedFree(Scratch);

        P->DenseEntities = std::move(NewDenseEntities);
        for(size_t NewIndex = 0; NewIndex < InCount; ++NewIndex)
        {
            const EntityID& E = P->DenseEntities[NewIndex];
            P->Sparse[E.Index].DenseIndex = uint32_t(NewIndex);
        }
    }

    void ComponentPool::SetRemovedObserver(const ComponentRemovedFn InFn, void* InUserData)
    {
        P->RemovedFn       = InFn;
        P->RemovedUserData = InUserData;
    }
}
