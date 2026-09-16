#ifndef GAMECORE_AABB_H
#define GAMECORE_AABB_H

#include <algorithm>
#include <cfloat>

#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

/*
 * An axis-aligned box, in whatever space its two callers agree on -- entity
 * space for BoundsComponent::Local, world space for BoundsComponent::World and
 * every SpatialIndex leaf.
 *
 * Header-only POD over EngineCore::Vector3, the same reuse-not-redeclare stance
 * the rest of GameCore takes toward Public/Math/*.h. Not annotated GAMECORE_API:
 * every member here is inline, so there is nothing out-of-line to export -- the
 * same rule GameCoreAPI.h states for EntityID.h and TickPhase.h's enum.
 */

namespace GameCore
{
    struct AABB
    {
        EngineCore::Vector3 Min;
        EngineCore::Vector3 Max;

        AABB() : Min(FLT_MAX, FLT_MAX, FLT_MAX), Max(-FLT_MAX, -FLT_MAX, -FLT_MAX) {}
        AABB(const EngineCore::Vector3& InMin, const EngineCore::Vector3& InMax) : Min(InMin), Max(InMax) {}

        /* True for a default-constructed AABB (Min > Max on every axis) -- the
         * "nothing has been unioned into this yet" state Union() below relies
         * on to seed an accumulation with no special-cased first iteration. */
        bool IsEmpty() const
        {
            return Min.GetX() > Max.GetX() || Min.GetY() > Max.GetY() || Min.GetZ() > Max.GetZ();
        }

        bool Overlaps(const AABB& InOther) const
        {
            if(IsEmpty() || InOther.IsEmpty()) { return false; }
            return Min.GetX() <= InOther.Max.GetX() && Max.GetX() >= InOther.Min.GetX()
                && Min.GetY() <= InOther.Max.GetY() && Max.GetY() >= InOther.Min.GetY()
                && Min.GetZ() <= InOther.Max.GetZ() && Max.GetZ() >= InOther.Min.GetZ();
        }

        bool Contains(const EngineCore::Vector3& InPoint) const
        {
            return InPoint.GetX() >= Min.GetX() && InPoint.GetX() <= Max.GetX()
                && InPoint.GetY() >= Min.GetY() && InPoint.GetY() <= Max.GetY()
                && InPoint.GetZ() >= Min.GetZ() && InPoint.GetZ() <= Max.GetZ();
        }

        bool Contains(const AABB& InOther) const
        {
            return Contains(InOther.Min) && Contains(InOther.Max);
        }

        EngineCore::Vector3 Center() const { return (Min + Max) * 0.5; }
        EngineCore::Vector3 Extent() const { return Max - Min; }

        /* Used by the spatial index's sibling-selection heuristic (see
         * SpatialIndex.h): the tree picks the insertion point that grows total
         * surface area the least, the standard AABB-tree cost function from
         * Bittner et al. / Box2D's b2DynamicTree. */
        float SurfaceArea() const
        {
            if(IsEmpty()) { return 0.0f; }
            const EngineCore::Vector3 E = Extent();
            return float(2.0 * (E.GetX() * E.GetY() + E.GetY() * E.GetZ() + E.GetZ() * E.GetX()));
        }

        AABB Fattened(const float InMargin) const
        {
            const EngineCore::Vector3 M(InMargin, InMargin, InMargin);
            return AABB(Min - M, Max + M);
        }

        static AABB Union(const AABB& InA, const AABB& InB)
        {
            if(InA.IsEmpty()) { return InB; }
            if(InB.IsEmpty()) { return InA; }
            return AABB(
                EngineCore::Vector3(std::min(InA.Min.GetX(), InB.Min.GetX()),
                                     std::min(InA.Min.GetY(), InB.Min.GetY()),
                                     std::min(InA.Min.GetZ(), InB.Min.GetZ())),
                EngineCore::Vector3(std::max(InA.Max.GetX(), InB.Max.GetX()),
                                     std::max(InA.Max.GetY(), InB.Max.GetY()),
                                     std::max(InA.Max.GetZ(), InB.Max.GetZ())));
        }

        /*
         * The box of InLocal's eight transformed corners -- a conservative
         * over-approximation under rotation, not a tight rotated box. Uses
         * Vector4 with w=1, NOT Matrix4x4::operator*(Vector3) -- that overload
         * is a direction transform (w=0) and would silently drop translation.
         * See DESIGN.md for the full reasoning.
         */
        static AABB Transformed(const AABB& InLocal, const EngineCore::Matrix4x4& InMatrix)
        {
            if(InLocal.IsEmpty()) { return AABB(); }

            AABB Result;
            for(int CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
            {
                const EngineCore::Vector4 Corner(
                    (CornerIndex & 1) ? InLocal.Max.GetX() : InLocal.Min.GetX(),
                    (CornerIndex & 2) ? InLocal.Max.GetY() : InLocal.Min.GetY(),
                    (CornerIndex & 4) ? InLocal.Max.GetZ() : InLocal.Min.GetZ(),
                    1.0);
                const EngineCore::Vector4 Transformed = InMatrix * Corner;
                const EngineCore::Vector3 Point(Transformed.GetX(), Transformed.GetY(), Transformed.GetZ());
                Result = Union(Result, AABB(Point, Point));
            }
            return Result;
        }
    };
}

#endif //GAMECORE_AABB_H
