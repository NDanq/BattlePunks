#include "Public/AICore/NavQuery.h"

#include <cmath>

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include "Private/AICore/NavInternal.h"
#include "Public/AICore/NavMesh.h"

/* NavQuery: Detour's queries with world-space vectors. NavQuery.h has the
 * contract. */
namespace AICore
{
    namespace
    {
        constexpr int MAX_PATH_POLYS = 512;

        /* A seeded stream for findRandomPointAroundCircle, which asks for
         * numbers through a plain function pointer. */
        thread_local uint32_t GRandomState = 1;
        float RandomFloat()
        {
            GRandomState ^= GRandomState << 13;
            GRandomState ^= GRandomState >> 17;
            GRandomState ^= GRandomState << 5;
            return float(GRandomState & 0xFFFFFF) / float(0x1000000);
        }
    }

    struct NavQuery::Impl
    {
        dtNavMeshQuery* Query = nullptr;
        EngineCore::Vector3 Extents{ 200.0, 400.0, 200.0 };

        ~Impl() { dtFreeNavMeshQuery(Query); }

        bool Nearest(const EngineCore::Vector3& InPoint, const dtQueryFilter& InFilter, dtPolyRef& OutRef, float (&OutPoint)[3]) const
        {
            float Point[3], Half[3];
            NavDetail::ToFloat3(InPoint, Point);
            NavDetail::ToFloat3(Extents, Half);
            OutRef = 0;
            if(dtStatusFailed(Query->findNearestPoly(Point, Half, &InFilter, &OutRef, OutPoint))) { return false; }
            return OutRef != 0;
        }
    };

    NavQuery::NavQuery() : P(new Impl()) {}
    NavQuery::~NavQuery() { delete P; }

    bool NavQuery::Init(const NavMesh& InMesh, const int32_t InMaxNodes)
    {
        if(!InMesh.IsValid()) { return false; }
        if(!P->Query) { P->Query = dtAllocNavMeshQuery(); }
        return P->Query && dtStatusSucceed(P->Query->init(InMesh.GetDetourMesh(), InMaxNodes));
    }

    bool NavQuery::IsValid() const { return P->Query && P->Query->getAttachedNavMesh(); }

    void NavQuery::SetSearchExtents(const EngineCore::Vector3& InExtents) { P->Extents = InExtents; }
    const EngineCore::Vector3& NavQuery::GetSearchExtents() const { return P->Extents; }

    ENavPathStatus NavQuery::FindPath(const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd,
                                      std::vector<EngineCore::Vector3>& OutPoints, const NavFilter* InFilter) const
    {
        OutPoints.clear();
        if(!IsValid()) { return ENavPathStatus::Failed; }
        const NavDetail::NavQueryFilter Filter(InFilter);

        dtPolyRef StartRef = 0, EndRef = 0;
        float Start[3], End[3];
        if(!P->Nearest(InStart, Filter, StartRef, Start) || !P->Nearest(InEnd, Filter, EndRef, End))
        {
            return ENavPathStatus::Failed;
        }

        dtPolyRef Polys[MAX_PATH_POLYS];
        int PolyCount = 0;
        const dtStatus Status = P->Query->findPath(StartRef, EndRef, Start, End, &Filter, Polys, &PolyCount, MAX_PATH_POLYS);
        if(dtStatusFailed(Status) || PolyCount == 0) { return ENavPathStatus::Failed; }

        /* A partial path ends on the polygon nearest the goal: clamp the
         * end point onto it. */
        float Goal[3] = { End[0], End[1], End[2] };
        const bool bPartial = Polys[PolyCount - 1] != EndRef || dtStatusDetail(Status, DT_PARTIAL_RESULT);
        if(bPartial)
        {
            bool bOver = false;
            P->Query->closestPointOnPoly(Polys[PolyCount - 1], End, Goal, &bOver);
        }

        float Straight[MAX_PATH_POLYS * 3];
        unsigned char Flags[MAX_PATH_POLYS];
        dtPolyRef StraightRefs[MAX_PATH_POLYS];
        int StraightCount = 0;
        if(dtStatusFailed(P->Query->findStraightPath(Start, Goal, Polys, PolyCount, Straight, Flags, StraightRefs,
                                                     &StraightCount, MAX_PATH_POLYS)))
        {
            return ENavPathStatus::Failed;
        }
        OutPoints.reserve(size_t(StraightCount));
        for(int Index = 0; Index < StraightCount; ++Index)
        {
            OutPoints.push_back(NavDetail::FromFloat3(&Straight[Index * 3]));
        }
        return bPartial ? ENavPathStatus::Partial : ENavPathStatus::Complete;
    }

    bool NavQuery::Raycast(const EngineCore::Vector3& InStart, const EngineCore::Vector3& InEnd, EngineCore::Vector3& OutHit,
                           const NavFilter* InFilter) const
    {
        OutHit = InEnd;
        if(!IsValid()) { return true; }
        const NavDetail::NavQueryFilter Filter(InFilter);
        dtPolyRef StartRef = 0;
        float Start[3], End[3];
        if(!P->Nearest(InStart, Filter, StartRef, Start)) { OutHit = InStart; return true; }
        NavDetail::ToFloat3(InEnd, End);

        float T = 0.0f;
        float Normal[3];
        dtPolyRef Path[MAX_PATH_POLYS];
        int PathCount = 0;
        if(dtStatusFailed(P->Query->raycast(StartRef, Start, End, &Filter, &T, Normal, Path, &PathCount, MAX_PATH_POLYS)))
        {
            OutHit = InStart;
            return true;
        }
        if(T >= 1.0f) { return false; }   // FLT_MAX: reached the end
        OutHit = EngineCore::Vector3(Start[0] + (End[0] - Start[0]) * T, Start[1] + (End[1] - Start[1]) * T, Start[2] + (End[2] - Start[2]) * T);
        return true;
    }

    bool NavQuery::ProjectPoint(const EngineCore::Vector3& InPoint, EngineCore::Vector3& OutPoint, const NavFilter* InFilter) const
    {
        if(!IsValid()) { return false; }
        const NavDetail::NavQueryFilter Filter(InFilter);
        dtPolyRef Ref = 0;
        float Point[3];
        if(!P->Nearest(InPoint, Filter, Ref, Point)) { return false; }
        OutPoint = NavDetail::FromFloat3(Point);
        return true;
    }

    bool NavQuery::RandomPointInRadius(const EngineCore::Vector3& InCenter, const float InRadius, const uint32_t InSeed,
                                       EngineCore::Vector3& OutPoint, const NavFilter* InFilter) const
    {
        if(!IsValid() || InRadius <= 0.0f) { return false; }
        const NavDetail::NavQueryFilter Filter(InFilter);
        dtPolyRef StartRef = 0;
        float Start[3];
        if(!P->Nearest(InCenter, Filter, StartRef, Start)) { return false; }

        GRandomState = InSeed ? InSeed : 0x9E3779B9u;
        /* findRandomPointAroundCircle picks among the polygons that reach
         * into the circle, and a big one can put the point beyond it: try a
         * few, keep the first inside. */
        for(int Attempt = 0; Attempt < 8; ++Attempt)
        {
            dtPolyRef Ref = 0;
            float Point[3];
            if(dtStatusFailed(P->Query->findRandomPointAroundCircle(StartRef, Start, InRadius, &Filter, &RandomFloat, &Ref, Point)))
            {
                return false;
            }
            const EngineCore::Vector3 Candidate = NavDetail::FromFloat3(Point);
            if(NavDetail::Distance2D(Candidate, NavDetail::FromFloat3(Start)) <= InRadius || Attempt == 7)
            {
                OutPoint = Candidate;
                return true;
            }
        }
        return false;
    }

    uint8_t NavQuery::GetAreaAt(const EngineCore::Vector3& InPoint) const
    {
        if(!IsValid()) { return NAV_AREA_BLOCKED; }
        NavFilter All;
        All.IncludeFlags = NAV_FLAG_ALL;
        All.ExcludeFlags = 0;
        const NavDetail::NavQueryFilter Filter(&All);
        dtPolyRef Ref = 0;
        float Point[3];
        if(!P->Nearest(InPoint, Filter, Ref, Point)) { return NAV_AREA_BLOCKED; }
        unsigned char Area = NAV_AREA_BLOCKED;
        P->Query->getAttachedNavMesh()->getPolyArea(Ref, &Area);
        return Area;
    }

    float NavQuery::GetPathLength(const std::vector<EngineCore::Vector3>& InPoints)
    {
        float Length = 0.0f;
        for(size_t Index = 1; Index < InPoints.size(); ++Index)
        {
            Length += NavDetail::Distance3D(InPoints[Index - 1], InPoints[Index]);
        }
        return Length;
    }

    dtNavMeshQuery* NavQuery::GetDetourQuery() const { return P->Query; }
}
