#ifndef AICORE_NAVQUERY_H
#define AICORE_NAVQUERY_H

#include <cstdint>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/NavTypes.h"
#include "Public/Math/Vector.h"

class dtNavMeshQuery;

/*
 * Questions asked of a navmesh: paths, straight-line reachability, the
 * nearest point on it, a random point near another. Wraps Detour's
 * dtNavMeshQuery (findNearestPoly, findPath, findStraightPath, raycast,
 * findRandomPointAroundCircle).
 *
 * Each call takes a filter; null uses the default NavFilter (walkable,
 * ground cost 1, High Cost 5). One NavQuery per thread: NavWorld keeps the
 * game thread's.
 */
namespace AICore
{
    class NavMesh;

    class AICORE_API NavQuery
    {
    public:
        NavQuery();
        ~NavQuery();
        NavQuery(const NavQuery&)            = delete;
        NavQuery& operator=(const NavQuery&) = delete;

        /* Bound to InMesh (which must outlive it, or Init again). False if
         * InMesh is not valid. InMaxNodes bounds a search's open list. */
        bool Init(const NavMesh& InMesh, int32_t InMaxNodes = 4096);
        bool IsValid() const;

        /* How far from a point (each axis) the nearest-polygon search looks. */
        void SetSearchExtents(const EngineCore::Vector3& InExtents);
        const EngineCore::Vector3& GetSearchExtents() const;

        /*
         * The straight path from InStart to InEnd, both snapped onto the
         * navmesh first: corner points, InStart's projection first.
         * Partial when InEnd cannot be reached (the path goes as near as it
         * can); Failed when either end has no polygon near.
         */
        ENavPathStatus FindPath(const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd,
                                std::vector<EngineCore::Vector3>& OutPoints, const NavFilter* InFilter = nullptr) const;

        /* Along the navmesh surface from InStart toward InEnd: true if a wall
         * (a polygon edge with nothing beyond, or a filtered one) stops it,
         * OutHit where. False: the way is clear (OutHit is InEnd). */
        bool Raycast(const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd,
                     EngineCore::Vector3& OutHit, const NavFilter* InFilter = nullptr) const;

        /* The nearest navmesh point within the search extents. */
        bool ProjectPoint(const EngineCore::Vector3& InPoint, EngineCore::Vector3& OutPoint,
                          const NavFilter* InFilter = nullptr) const;

        /* A random reachable point within InRadius (2D) of InCenter, connected
         * to InCenter's polygon. InSeed picks it: the same seed, the same point. */
        bool RandomPointInRadius(const EngineCore::Vector3& InCenter, float InRadius, uint32_t InSeed,
                                 EngineCore::Vector3& OutPoint, const NavFilter* InFilter = nullptr) const;

        /* The area id under a point (NAV_AREA_*), or NAV_AREA_BLOCKED off the mesh. */
        uint8_t GetAreaAt(const EngineCore::Vector3& InPoint) const;

        /* Summed segment lengths. */
        static float GetPathLength(const std::vector<EngineCore::Vector3>& InPoints);

        dtNavMeshQuery* GetDetourQuery() const;

    private:
        struct Impl;
        Impl* P = nullptr;
    };
}

#endif //AICORE_NAVQUERY_H
