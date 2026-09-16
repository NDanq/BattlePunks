#ifndef GAMECORE_COMPONENTPOOL_H
#define GAMECORE_COMPONENTPOOL_H

#include <cstddef>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

/*
 * Sparse-set storage for one component type: dense, contiguous, cache-friendly
 * iteration, O(1) add/get/has/remove by EntityID.
 *
 * ERASED, LIKE DelegateList -- SAME REASON. ComponentPool deals in bytes with a
 * stride; World::AddComponent<T> and Entity::GetComponent<T> are the typed
 * views that cast T* to and from what this class hands back. This is
 * CoreMemory.h's "templates compute, EngineCore allocates" rule again: a
 * type-erased pool means the Lua by-name path (e:AddComponent("Transform")) and
 * the C++ typed path are THE SAME CODE, rather than a duplicate untyped
 * implementation existing only for scripts.
 *
 * CLASSIC SPARSE SET. Sparse[entity.Index] is either NOT_PRESENT or a dense
 * slot; Dense[slot] is the component bytes; a parallel DenseEntities[slot]
 * array is the reverse map AND carries the entity's Generation, so a stale
 * EntityID (recycled index, old generation) reads back "absent" rather than
 * aliasing whatever now occupies that sparse slot.
 *
 * WHY THE DENSE BUFFER IS SystemAlignedAlloc, NOT std::vector<uint8_t>
 *
 * A component's alignment can be 16 (Vector3 under WITH_SIMD -- see Math/
 * Vector.h's ABI guard). std::vector<uint8_t> makes no alignment promise beyond
 * alignof(uint8_t), so a Transform pool backed by one would misalign every
 * other element the moment the stride is not a multiple of 16. Allocating
 * through EngineCore::SystemAlignedAlloc at the type's alignment is the
 * same fix CoreMemoryPool.h's FixedBlockPool applies for the same reason.
 *
 * SWAP-AND-POP, POD ONLY. Remove() memcpy's the last live element into the hole
 * left by the removed one -- correct ONLY because ComponentType.h's
 * GAMECORE_DEFINE_COMPONENT already enforces trivial copyability on every
 * component. This is also why "a T* is invalidated by any Add or Remove of that
 * type" is not a suggestion: the object behind a held pointer may have just
 * been relocated eight bytes to the left.
 */

namespace GameCore
{
    /* Fired from inside Remove(), BEFORE the swap-and-pop happens -- so the
     * observer sees the component's last live values, not the tombstone left
     * behind. World's own removal path (see World.h) and ScriptSystem's
     * ScriptComponent cleanup both hang off this. One observer per pool: a pool
     * that needs to fan out to several listeners does so from inside that one
     * function, the same "one seam, caller fans out" shape World's own
     * OnComponentRemoved delegate takes above this. */
    using ComponentRemovedFn = void (*)(EntityID InEntity, void* InComponent, void* InUserData);

    class GAMECORE_API ComponentPool
    {
    public:
        explicit ComponentPool(const ComponentTypeInfo& InType);
        ~ComponentPool();

        ComponentPool(const ComponentPool&)            = delete;
        ComponentPool& operator=(const ComponentPool&) = delete;

        /*
         * Copies InInitBytes (InType.SizeBytes long) into a new dense slot for
         * InEntity and returns the payload pointer -- NOT yet carrying a valid
         * ComponentOwner header; World::AddComponentRaw writes that
         * immediately after this returns, which is the one place it happens.
         * nullptr, with a warning logged, if InEntity already has this
         * component -- Add is not an upsert.
         */
        void* Add(EntityID InEntity, const void* InInitBytes);

        /* nullptr if InEntity has never had this component, or if its
         * Generation no longer matches what is on record (a stale handle to a
         * recycled slot) -- see the class comment's note on DenseEntities. */
        void* Get(EntityID InEntity) const;
        bool  Has(EntityID InEntity) const;

        /* Fires the removal observer (if any) with the component's final
         * values, THEN swap-and-pops. False if InEntity did not have this
         * component. */
        bool Remove(EntityID InEntity);

        size_t GetCount() const;

        /* The dense buffer's base, GetType().SizeBytes apart per element,
         * aligned to GetType().Alignment -- what World::Each<T> iterates
         * directly rather than calling Get() per entity. Undefined to read past
         * GetCount() * GetType().SizeBytes. */
        void* GetDenseData() const;

        EntityID GetEntityAt(size_t InDenseIndex) const;

        const ComponentTypeInfo& GetType() const;

        /*
         * Permutes dense storage into InOrder (InCount entries, every one of
         * them a live member of this pool, no duplicates) -- a full
         * re-layout, not an incremental sort. EntityID -> component identity is
         * unchanged; every T* obtained before this call is invalidated, exactly
         * as an Add or Remove would invalidate one.
         *
         * A TOOL a host calls ONCE, deliberately -- after a level load, sorting
         * a static pool into the SpatialIndex's leaf order so a region query
         * over mostly-static geometry walks close-to-sequential memory (see
         * SpatialIndex.h's "Locality across the two structures" note). Nothing
         * in World or the per-frame tick calls this on its own; the cost of a
         * full permute is exactly why not.
         */
        void Reorder(const EntityID* InOrder, size_t InCount);

        void SetRemovedObserver(ComponentRemovedFn InFn, void* InUserData);

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_COMPONENTPOOL_H
