#ifndef NIMBLEEDITOR_LIGHTBAKEBVH_H
#define NIMBLEEDITOR_LIGHTBAKEBVH_H

#if WITH_EDITOR

#include <cstdint>
#include <vector>

#include "Private/Editor/Lighting/LightBakeScene.h"

/*
 * A bounding volume hierarchy over a bake scene's triangles, for the rays a
 * light bake traces: binned surface-area-heuristic build, stack traversal,
 * Moller-Trumbore triangles (as ViewportWidget's RayHitsMesh). Read-only once
 * built, so any number of threads may trace at once.
 */
namespace LightBake
{
    struct Hit
    {
        float    T = 0.0f;
        uint32_t Triangle = 0;
        float    U = 0.0f, V = 0.0f;   // barycentric: P = (1-U-V) P0 + U P1 + V P2
    };

    class BVH
    {
    public:
        void Build(const std::vector<Triangle>& InTriangles);

        /* The nearest hit in (InMinT, InMaxT); either side of a triangle. */
        bool Intersect(const Vec3& InOrigin, const Vec3& InDirection, float InMinT, float InMaxT, Hit& OutHit) const;

        /* Anything in (InMinT, InMaxT): a shadow ray. */
        bool Occluded(const Vec3& InOrigin, const Vec3& InDirection, float InMinT, float InMaxT) const;

        bool IsEmpty() const { return Nodes.empty(); }

    private:
        struct Node
        {
            Vec3     Min, Max;
            uint32_t First = 0;   // leaf: first in Order; inner: the left child (right is First + 1)
            uint32_t Count = 0;   // leaf: triangles; 0 for an inner node
        };

        /* Per triangle, its corners as an origin and two edges. */
        struct Prepared
        {
            Vec3 P0, E1, E2;
        };

        template<bool bAnyHit>
        bool Traverse(const Vec3& InOrigin, const Vec3& InDirection, float InMinT, float InMaxT, Hit* OutHit) const;

        std::vector<Node>     Nodes;
        std::vector<uint32_t> Order;      // triangle indices, leaves' ranges into it
        std::vector<Prepared> Triangles;  // by triangle index
    };
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_LIGHTBAKEBVH_H
