#ifndef GAMECORE_COMPONENTPOOL_H
#define GAMECORE_COMPONENTPOOL_H

#include <cstddef>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

/*
 * Sparse-set storage for one component type: dense, contiguous,
 * cache-friendly iteration, O(1) add/get/has/remove by EntityID. Erased, like
 * DelegateList -- see DESIGN.md for why, and for the sparse-set shape, the
 * alignment story, and the swap-and-pop/POD contract.
 *
 * A T* from Add/Get/Each is invalidated by any Add or Remove of that type --
 * the object behind a held pointer may have just been relocated.
 */

namespace GameCore
{
    /* Fired from inside Remove(), BEFORE the swap-and-pop, so the observer
     * sees the component's last live values. One observer per pool; fan out
     * from inside it if several listeners are needed. */
    using ComponentRemovedFn = void (*)(EntityID InEntity, void* InComponent, void* InUserData);

    class GAMECORE_API ComponentPool
    {
    public:
        explicit ComponentPool(const ComponentTypeInfo& InType);
        ~ComponentPool();

        ComponentPool(const ComponentPool&)            = delete;
        ComponentPool& operator=(const ComponentPool&) = delete;

        /* Copies InInitBytes into a new dense slot. Does NOT write
         * ComponentOwner -- World::AddComponentRaw does that right after.
         * nullptr, with a warning, if InEntity already has this component. */
        void* Add(EntityID InEntity, const void* InInitBytes);

        /* nullptr if InEntity never had this component, or its generation no
         * longer matches (a stale handle to a recycled slot). */
        void* Get(EntityID InEntity) const;
        bool  Has(EntityID InEntity) const;

        /* Fires the removal observer with the component's final values,
         * THEN swap-and-pops. False if InEntity did not have this component. */
        bool Remove(EntityID InEntity);

        size_t GetCount() const;

        /* The dense buffer's base, SizeBytes apart per element, aligned to
         * Alignment -- what World::Each<T> iterates directly. Undefined past
         * GetCount() * SizeBytes. */
        void* GetDenseData() const;

        EntityID GetEntityAt(size_t InDenseIndex) const;

        const ComponentTypeInfo& GetType() const;

        /* Permutes dense storage into InOrder (every entry a live member, no
         * duplicates) -- a full re-layout, not an incremental sort. Every T*
         * from before this call is invalidated. A tool a host calls once
         * (e.g. sorting a static pool into SpatialIndex leaf order after a
         * level load) -- never called by World's own per-frame tick. */
        void Reorder(const EntityID* InOrder, size_t InCount);

        void SetRemovedObserver(ComponentRemovedFn InFn, void* InUserData);

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_COMPONENTPOOL_H
