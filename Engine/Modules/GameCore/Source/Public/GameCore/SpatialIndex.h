#ifndef GAMECORE_SPATIALINDEX_H
#define GAMECORE_SPATIALINDEX_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

/*
 * A dynamic AABB tree -- the same shape as Box2D's b2DynamicTree and Bullet's
 * btDbvt: a binary tree of fattened boxes, leaves are entities, internal nodes
 * are the union of their children, rebalanced incrementally as leaves move.
 *
 * WHAT IS IN THE TREE AND WHAT IS NOT
 *
 * World keeps three things about an entity in three places, each chosen for
 * how it is accessed: identity in the entity free-list, component DATA in the
 * contiguous ComponentPools (iterated linearly, by a system that wants every
 * instance), and POSITION IN SPACE here (searched by region, by a system that
 * wants only what is nearby). A leaf is an EntityID and a fattened world box --
 * no component bytes -- so Insert/Remove/Move never touch a pool, and a system
 * iterating a pool never pays for the tree existing. See World.h's spatial
 * section and SpatialSystem.cpp for how a BoundsComponent's Transform-derived
 * box gets INTO a leaf every frame.
 *
 * WHY FAT BOXES
 *
 * A leaf's stored box is its true box grown by GetFatMargin() on every axis.
 * Move() is then a cheap common case: if the new true box still fits inside the
 * stored fat box, NOTHING in the tree changes -- no reinsertion, no rebalance,
 * just a compare. Only an escape past the margin costs a real remove+insert.
 * For an entity that jitters or moves slowly relative to its own size (which is
 * most of them, most frames), this turns "update every entity's tree position"
 * into "touch the handful that actually crossed their margin".
 *
 * ERASED, GAMECORE_API, EXPORTED -- same reason DelegateList and ComponentPool
 * are: the node array, free list and root all live in GameCore.dll, so World's
 * inline query helpers (EachInBounds<T>, see World.h) do nothing but forward
 * into here and then look results up in a ComponentPool.
 */

namespace GameCore
{
    /* Return false to stop the query early -- matches ComponentRemovedFn's and
     * SystemFn's "raw function pointer + void*" shape, for the reload reasons
     * Delegate.h's header comment already gives at length. */
    using SpatialQueryFn = bool (*)(EntityID InEntity, void* InUserData);

    struct RaycastHit
    {
        EntityID Entity;
        float    T = 0.0f;   // distance along the ray, in units of InDir's own length
    };

    class GAMECORE_API SpatialIndex
    {
    public:
        SpatialIndex();
        ~SpatialIndex();

        SpatialIndex(const SpatialIndex&)            = delete;
        SpatialIndex& operator=(const SpatialIndex&) = delete;

        /* Stores InWorldBox FATTENED by GetFatMargin(). Returns the proxy id --
         * an opaque handle into the node array, stable until Remove()d, that
         * the caller (BoundsComponent::ProxyID) holds so it never has to search
         * the tree for "which leaf is this entity". */
        int32_t Insert(EntityID InEntity, const AABB& InWorldBox);

        void Remove(int32_t InProxy);

        /*
         * True if the leaf was actually re-inserted (InWorldBox escaped its fat
         * box); false if it still fit and nothing in the tree changed -- the
         * cheap, common case. Either way, after this call GetFatBox(InProxy)
         * contains InWorldBox.
         */
        bool Move(int32_t InProxy, const AABB& InWorldBox);

        /* Visits every leaf whose FAT box overlaps InBox, in unspecified order,
         * calling InFn(Entity, InUserData) for each until InFn returns false or
         * every hit has been visited. Overlap is tested against the stored FAT
         * box, not a tight one -- a query near a leaf's margin may see it
         * slightly before or after its true box would justify, which is the
         * same conservative-over-exact trade the fat margin makes everywhere
         * else in this class. */
        void QueryBounds(const AABB& InBox, SpatialQueryFn InFn, void* InUserData) const;

        /* Nearest leaf box intersected by the ray (InOrigin, InDir), up to
         * InMaxT along InDir -- InDir need not be normalized; T is reported in
         * multiples of InDir's own length. False with OutHit untouched if
         * nothing is hit. Tests against fat boxes, same caveat as QueryBounds. */
        bool Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir,
                     float InMaxT, RaycastHit& OutHit) const;

        /* Discards the incremental tree and rebuilds top-down from every live
         * leaf's CURRENT fat box, splitting on the longest axis at the median
         * each level. Proxy ids are preserved (Insert/Remove/Move's contract to
         * the caller does not change), only the tree's internal shape does. For
         * after a level load inserts hundreds of static leaves in one burst --
         * incremental insertion into an empty tree in file order produces a
         * badly unbalanced tree; a rebuild afterwards fixes it in one pass
         * rather than depending on insertion order being sensible. */
        void Rebuild();

        /* Default 0.5 world units. Applies to future Insert/Move calls only --
         * changing this does not retroactively refit existing leaves. */
        void  SetFatMargin(float InMargin);
        float GetFatMargin() const;

        size_t  GetProxyCount() const;
        int32_t GetHeight() const;   // longest root-to-leaf path; O(log n) is the property Validate() checks for

        /* Walks the whole tree checking every invariant a dynamic AABB tree is
         * supposed to hold: every parent's box contains both children's boxes,
         * every leaf's stored box is at least its own true fat box, heights are
         * internally consistent. Expensive -- O(n) -- and meant for a test
         * oracle (GameCoreSmokeTest calls this after every mutating operation
         * in its spatial cases), not for a per-frame assertion. */
        bool Validate() const;

        const AABB& GetFatBox(int32_t InProxy) const;
        EntityID    GetEntity(int32_t InProxy) const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_SPATIALINDEX_H
