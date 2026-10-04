#if WITH_EDITOR

#include "Private/Editor/Lighting/LightBakeBVH.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace LightBake
{
    namespace
    {
        constexpr uint32_t LEAF_SIZE = 4;
        constexpr int      BINS      = 12;
        constexpr int      STACK     = 64;

        float SurfaceArea(const Vec3& InMin, const Vec3& InMax)
        {
            const Vec3 D = InMax - InMin;
            return D.X < 0.0f ? 0.0f : 2.0f * (D.X * D.Y + D.Y * D.Z + D.Z * D.X);
        }

        /* The slab test: the entry distance, or +inf for a miss. */
        float RayBox(const Vec3& InOrigin, const Vec3& InInvDir, const Vec3& InMin, const Vec3& InMax, const float InMaxT)
        {
            float Near = 0.0f, Far = InMaxT;
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                float T0 = (InMin[Axis] - InOrigin[Axis]) * InInvDir[Axis];
                float T1 = (InMax[Axis] - InOrigin[Axis]) * InInvDir[Axis];
                if(T0 > T1) { std::swap(T0, T1); }
                Near = T0 > Near ? T0 : Near;
                Far  = T1 < Far ? T1 : Far;
                if(Near > Far) { return std::numeric_limits<float>::infinity(); }
            }
            return Near;
        }
    }

    void BVH::Build(const std::vector<Triangle>& InTriangles)
    {
        Nodes.clear();
        Order.clear();
        Triangles.clear();
        const uint32_t Count = uint32_t(InTriangles.size());
        if(Count == 0) { return; }

        std::vector<Vec3> BoxMin(Count), BoxMax(Count), Centre(Count);
        Triangles.resize(Count);
        Order.resize(Count);
        for(uint32_t Index = 0; Index < Count; ++Index)
        {
            const Triangle& T = InTriangles[Index];
            Triangles[Index] = { T.P[0], T.P[1] - T.P[0], T.P[2] - T.P[0] };
            BoxMin[Index] = Min(Min(T.P[0], T.P[1]), T.P[2]);
            BoxMax[Index] = Max(Max(T.P[0], T.P[1]), T.P[2]);
            Centre[Index] = (BoxMin[Index] + BoxMax[Index]) * 0.5f;
            Order[Index]  = Index;
        }

        Nodes.reserve(size_t(Count) * 2);
        Nodes.push_back(Node{ Vec3(), Vec3(), 0, Count });

        /* Nodes to split: (node, first, count). */
        struct Pending { uint32_t Node, First, Count; };
        std::vector<Pending> Work{ { 0, 0, Count } };
        while(!Work.empty())
        {
            const Pending Job = Work.back();
            Work.pop_back();

            Vec3 Lo( std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max());
            Vec3 Hi(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
            Vec3 CLo = Lo, CHi = Hi;
            for(uint32_t At = Job.First; At < Job.First + Job.Count; ++At)
            {
                const uint32_t Tri = Order[At];
                Lo  = Min(Lo, BoxMin[Tri]);
                Hi  = Max(Hi, BoxMax[Tri]);
                CLo = Min(CLo, Centre[Tri]);
                CHi = Max(CHi, Centre[Tri]);
            }
            Nodes[Job.Node].Min = Lo;
            Nodes[Job.Node].Max = Hi;
            Nodes[Job.Node].First = Job.First;
            Nodes[Job.Node].Count = Job.Count;
            if(Job.Count <= LEAF_SIZE) { continue; }

            /* The cheapest split over every axis's bins. */
            const float LeafCost = float(Job.Count) * SurfaceArea(Lo, Hi);
            float BestCost = LeafCost;
            int   BestAxis = -1, BestBin = 0;
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                const float Extent = CHi[Axis] - CLo[Axis];
                if(Extent <= 1e-12f) { continue; }

                struct Bin { Vec3 Min, Max; uint32_t Count = 0; };
                Bin Bins[BINS];
                for(Bin& Each : Bins)
                {
                    Each.Min = Vec3( std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max());
                    Each.Max = Vec3(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
                }
                for(uint32_t At = Job.First; At < Job.First + Job.Count; ++At)
                {
                    const uint32_t Tri = Order[At];
                    const int Slot = std::min(BINS - 1, int((Centre[Tri][Axis] - CLo[Axis]) / Extent * BINS));
                    Bins[Slot].Min = Min(Bins[Slot].Min, BoxMin[Tri]);
                    Bins[Slot].Max = Max(Bins[Slot].Max, BoxMax[Tri]);
                    ++Bins[Slot].Count;
                }

                /* Left sweep, then right sweep, adding the costs. */
                float    LeftArea[BINS - 1];
                uint32_t LeftCount[BINS - 1];
                Vec3 SMin = Bins[0].Min, SMax = Bins[0].Max;
                uint32_t SCount = 0;
                for(int Split = 0; Split < BINS - 1; ++Split)
                {
                    SMin = Min(SMin, Bins[Split].Min);
                    SMax = Max(SMax, Bins[Split].Max);
                    SCount += Bins[Split].Count;
                    LeftArea[Split]  = SCount ? SurfaceArea(SMin, SMax) : 0.0f;
                    LeftCount[Split] = SCount;
                }
                SMin = Bins[BINS - 1].Min;
                SMax = Bins[BINS - 1].Max;
                SCount = 0;
                for(int Split = BINS - 2; Split >= 0; --Split)
                {
                    SMin = Min(SMin, Bins[Split + 1].Min);
                    SMax = Max(SMax, Bins[Split + 1].Max);
                    SCount += Bins[Split + 1].Count;
                    if(LeftCount[Split] == 0 || SCount == 0) { continue; }
                    const float Cost = float(LeftCount[Split]) * LeftArea[Split] + float(SCount) * SurfaceArea(SMin, SMax);
                    if(Cost < BestCost)
                    {
                        BestCost = Cost;
                        BestAxis = Axis;
                        BestBin  = Split;
                    }
                }
            }

            uint32_t Middle = Job.First;
            if(BestAxis >= 0)
            {
                const float Extent = CHi[BestAxis] - CLo[BestAxis];
                const auto Mid = std::partition(Order.begin() + Job.First, Order.begin() + Job.First + Job.Count,
                                                [&](const uint32_t InTri)
                                                {
                                                    const int Slot = std::min(BINS - 1, int((Centre[InTri][BestAxis] - CLo[BestAxis]) / Extent * BINS));
                                                    return Slot <= BestBin;
                                                });
                Middle = uint32_t(Mid - Order.begin());
            }
            if(BestAxis < 0 || Middle == Job.First || Middle == Job.First + Job.Count)
            {
                /* No split pays, or every centre fell one side: halve along the
                 * widest axis if it is still too big to be one leaf. */
                if(Job.Count <= LEAF_SIZE * 4) { continue; }
                int Axis = 0;
                const Vec3 Extent = CHi - CLo;
                if(Extent.Y > Extent[Axis]) { Axis = 1; }
                if(Extent.Z > Extent[Axis]) { Axis = 2; }
                Middle = Job.First + Job.Count / 2;
                std::nth_element(Order.begin() + Job.First, Order.begin() + Middle, Order.begin() + Job.First + Job.Count,
                                 [&](const uint32_t A, const uint32_t B) { return Centre[A][Axis] < Centre[B][Axis]; });
            }

            const uint32_t Left = uint32_t(Nodes.size());
            Nodes.push_back(Node());
            Nodes.push_back(Node());
            Nodes[Job.Node].First = Left;
            Nodes[Job.Node].Count = 0;
            Work.push_back({ Left,     Job.First, Middle - Job.First });
            Work.push_back({ Left + 1, Middle,    Job.First + Job.Count - Middle });
        }
    }

    template<bool bAnyHit>
    bool BVH::Traverse(const Vec3& InOrigin, const Vec3& InDirection, const float InMinT, float InMaxT, Hit* OutHit) const
    {
        if(Nodes.empty()) { return false; }

        const Vec3 InvDir(1.0f / InDirection.X, 1.0f / InDirection.Y, 1.0f / InDirection.Z);
        uint32_t Stack[STACK];
        int      Top = 0;
        Stack[Top++] = 0;
        bool bHit = false;

        while(Top > 0)
        {
            const Node& N = Nodes[Stack[--Top]];
            if(RayBox(InOrigin, InvDir, N.Min, N.Max, InMaxT) == std::numeric_limits<float>::infinity()) { continue; }

            if(N.Count > 0)
            {
                for(uint32_t At = N.First; At < N.First + N.Count; ++At)
                {
                    const Prepared& T = Triangles[Order[At]];
                    const Vec3  PVec = Cross(InDirection, T.E2);
                    const float Det  = Dot(T.E1, PVec);
                    if(std::fabs(Det) < 1e-12f) { continue; }
                    const float InvDet = 1.0f / Det;
                    const Vec3  TVec   = InOrigin - T.P0;
                    const float U      = Dot(TVec, PVec) * InvDet;
                    if(U < 0.0f || U > 1.0f) { continue; }
                    const Vec3  QVec = Cross(TVec, T.E1);
                    const float V    = Dot(InDirection, QVec) * InvDet;
                    if(V < 0.0f || U + V > 1.0f) { continue; }
                    const float Dist = Dot(T.E2, QVec) * InvDet;
                    if(Dist <= InMinT || Dist >= InMaxT) { continue; }
                    if(bAnyHit) { return true; }
                    InMaxT = Dist;
                    OutHit->T = Dist;
                    OutHit->Triangle = Order[At];
                    OutHit->U = U;
                    OutHit->V = V;
                    bHit = true;
                }
                continue;
            }

            /* The nearer child last, so it is visited first. */
            const Node& A = Nodes[N.First];
            const Node& B = Nodes[N.First + 1];
            const float DA = RayBox(InOrigin, InvDir, A.Min, A.Max, InMaxT);
            const float DB = RayBox(InOrigin, InvDir, B.Min, B.Max, InMaxT);
            if(Top + 2 > STACK) { continue; }   // deeper than any balanced build; skip rather than overflow
            if(DA < DB)
            {
                if(DB != std::numeric_limits<float>::infinity()) { Stack[Top++] = N.First + 1; }
                Stack[Top++] = N.First;
            }
            else
            {
                if(DA != std::numeric_limits<float>::infinity()) { Stack[Top++] = N.First; }
                if(DB != std::numeric_limits<float>::infinity()) { Stack[Top++] = N.First + 1; }
            }
        }
        return bHit;
    }

    bool BVH::Intersect(const Vec3& InOrigin, const Vec3& InDirection, const float InMinT, const float InMaxT, Hit& OutHit) const
    {
        return Traverse<false>(InOrigin, InDirection, InMinT, InMaxT, &OutHit);
    }

    bool BVH::Occluded(const Vec3& InOrigin, const Vec3& InDirection, const float InMinT, const float InMaxT) const
    {
        return Traverse<true>(InOrigin, InDirection, InMinT, InMaxT, nullptr);
    }
}

#endif // WITH_EDITOR
