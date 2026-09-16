#ifndef GAMECORE_SPATIALINDEX_H
#define GAMECORE_SPATIALINDEX_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/GameCore/GameCoreAPI.h"

/*
 * A dynamic AABB tree -- the same shape as Box2D's b2DynamicTree and
 * Bullet's btDbvt: a binary tree of fattened boxes, leaves are entities,
 * internal nodes are the union of their children, rebalanced incrementally
 * as leaves move. See DESIGN.md for what is (and is not) kept in the tree,
 * why boxes are fattened, and why this class is erased and exported.
 */

namespace GameCore
{
    /* Return false to stop the query early -- matches ComponentRemovedFn's
     * and SystemFn's "raw function pointer + void*" shape. */
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

        /* Stores InWorldBox fattened by GetFatMargin(). Returns a proxy id
         * stable until Remove()d, for the caller (BoundsComponent::ProxyID)
         * to hold rather than search the tree for. */
        int32_t Insert(EntityID InEntity, const AABB& InWorldBox);

        void Remove(int32_t InProxy);

        /* True if the leaf was actually re-inserted (escaped its fat box);
         * false if it still fit -- the cheap, common case. Either way,
         * GetFatBox(InProxy) contains InWorldBox afterward. */
        bool Move(int32_t InProxy, const AABB& InWorldBox);

        /* Visits every leaf whose FAT box overlaps InBox, calling
         * InFn(Entity, InUserData) until it returns false or every hit is
         * visited. Tested against the stored fat box, not a tight one. */
        void QueryBounds(const AABB& InBox, SpatialQueryFn InFn, void* InUserData) const;

        /* Nearest leaf box intersected by the ray, up to InMaxT along InDir
         * (need not be normalized). False with OutHit untouched if nothing
         * is hit. Tests against fat boxes, same caveat as QueryBounds. */
        bool Raycast(const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir,
                     float InMaxT, RaycastHit& OutHit) const;

        /* Discards the incremental tree and rebuilds top-down from every
         * live leaf's current fat box. Proxy ids are preserved -- only the
         * tree's internal shape changes. For after a level load inserts many
         * static leaves at once -- see DESIGN.md. */
        void Rebuild();

        /* Default 0.5 world units. Applies to future Insert/Move only. */
        void  SetFatMargin(float InMargin);
        float GetFatMargin() const;

        size_t  GetProxyCount() const;
        int32_t GetHeight() const;   // longest root-to-leaf path

        /* Walks the whole tree checking every dynamic-AABB-tree invariant.
         * O(n), meant as a test oracle -- see DESIGN.md. */
        bool Validate() const;

        const AABB& GetFatBox(int32_t InProxy) const;
        EntityID    GetEntity(int32_t InProxy) const;

    private:
        struct Impl;
        Impl* P;
    };
}

#endif //GAMECORE_SPATIALINDEX_H
