#include "Public/RenderCore/MeshFracture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <set>
#include <unordered_map>

#include "Public/RenderCore/RenderCoreLog.h"

/* MeshFracture: Voronoi cells by successive plane clips, with every cut
 * capped. See MeshFracture.h for what it promises. */

namespace RenderCore
{
    namespace
    {
        struct Vec { double X = 0.0, Y = 0.0, Z = 0.0; };

        Vec    operator+(const Vec& A, const Vec& B) { return { A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
        Vec    operator-(const Vec& A, const Vec& B) { return { A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }
        Vec    operator*(const Vec& A, const double S) { return { A.X * S, A.Y * S, A.Z * S }; }
        double Dot(const Vec& A, const Vec& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
        Vec    Cross(const Vec& A, const Vec& B) { return { A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X }; }
        double Length(const Vec& A) { return std::sqrt(Dot(A, A)); }
        Vec    ToVec(const float* InP) { return { InP[0], InP[1], InP[2] }; }

        /* One corner of a working triangle: everything a vertex carries but UV1. */
        struct Vert
        {
            float P[3]  = {};
            float N[3]  = {};
            float UV[2] = {};
            float C[4]  = {};
        };

        /* The pieces are built as triangle soups and welded at the end. */
        struct Tri
        {
            Vert     V[3];
            uint32_t Section = 0;
            int32_t  Cut     = -1;   // a cap: the seed whose plane made it; -1 for the source's surface
        };

        /* F(x) = N.x - D; a piece keeps F < 0. N is unit length. */
        struct Plane
        {
            Vec    N;
            double D = 0.0;

            double Eval(const float* InP) const { return N.X * InP[0] + N.Y * InP[1] + N.Z * InP[2] - D; }
        };

        /* A cut's outline, one edge per clipped triangle. */
        struct Segment
        {
            float A[3];
            float B[3];
        };

        /* Lengths and areas below these are the same point or nothing,
         * scaled to the mesh. */
        struct Tolerances
        {
            double Snap  = 0.0;   // |F| under this counts as outside, so nothing sits on a plane
            double Weld  = 0.0;   // outline points closer than this are one point
            double Area  = 0.0;   // a 2D turn or a cap's area under this is flat
        };

        bool PositionLess(const Vert& InA, const Vert& InB)
        {
            for(int I = 0; I < 3; ++I)
            {
                if(InA.P[I] != InB.P[I]) { return InA.P[I] < InB.P[I]; }
            }
            return false;
        }

        /* Where edge A-B crosses the plane. Always interpolated from the same
         * end (the lesser position), so the two triangles sharing an edge
         * make the same point, bit for bit, and the cap's outline closes. */
        Vert CrossingPoint(const Vert& InA, const double InFA, const Vert& InB, const double InFB)
        {
            const bool   bSwap = PositionLess(InB, InA);
            const Vert&  A     = bSwap ? InB : InA;
            const Vert&  B     = bSwap ? InA : InB;
            const double FA    = bSwap ? InFB : InFA;
            const double FB    = bSwap ? InFA : InFB;
            const double T     = std::clamp(FA / (FA - FB), 0.0, 1.0);

            Vert Out;
            for(int I = 0; I < 3; ++I) { Out.P[I]  = float(A.P[I]  + (B.P[I]  - A.P[I])  * T); }
            for(int I = 0; I < 3; ++I) { Out.N[I]  = float(A.N[I]  + (B.N[I]  - A.N[I])  * T); }
            for(int I = 0; I < 2; ++I) { Out.UV[I] = float(A.UV[I] + (B.UV[I] - A.UV[I]) * T); }
            for(int I = 0; I < 4; ++I) { Out.C[I]  = float(A.C[I]  + (B.C[I]  - A.C[I])  * T); }
            const double Len = std::sqrt(double(Out.N[0]) * Out.N[0] + double(Out.N[1]) * Out.N[1] + double(Out.N[2]) * Out.N[2]);
            if(Len > 0.0)
            {
                for(float& Each : Out.N) { Each = float(Each / Len); }
            }
            return Out;
        }

        /* Keeps the part of InOutTris where F < 0; each clipped triangle adds
         * one edge of the cut's outline to OutSegments. */
        void ClipByPlane(std::vector<Tri>& InOutTris, const Plane& InPlane, const Tolerances& InTol, std::vector<Segment>& OutSegments)
        {
            std::vector<Tri> Kept;
            Kept.reserve(InOutTris.size());
            for(const Tri& Each : InOutTris)
            {
                double F[3];
                bool   In[3];
                int    InCount = 0;
                for(int K = 0; K < 3; ++K)
                {
                    F[K] = InPlane.Eval(Each.V[K].P);
                    if(std::fabs(F[K]) < InTol.Snap) { F[K] = InTol.Snap; }
                    In[K] = F[K] < 0.0;
                    InCount += In[K] ? 1 : 0;
                }
                if(InCount == 3) { Kept.push_back(Each); continue; }
                if(InCount == 0) { continue; }

                /* Sutherland-Hodgman on one triangle: 3 or 4 corners, one
                 * edge leaving the kept side and one coming back. */
                Vert    Poly[4];
                int     Count = 0;
                Segment Seg{};
                for(int K = 0; K < 3; ++K)
                {
                    const int Next = (K + 1) % 3;
                    if(In[K]) { Poly[Count++] = Each.V[K]; }
                    if(In[K] != In[Next])
                    {
                        const Vert X = CrossingPoint(Each.V[K], F[K], Each.V[Next], F[Next]);
                        Poly[Count++] = X;
                        std::memcpy(In[K] ? Seg.A : Seg.B, X.P, sizeof(X.P));
                    }
                }
                Tri Out = Each;
                Out.V[0] = Poly[0]; Out.V[1] = Poly[1]; Out.V[2] = Poly[2];
                Kept.push_back(Out);
                if(Count == 4)
                {
                    Out.V[0] = Poly[0]; Out.V[1] = Poly[2]; Out.V[2] = Poly[3];
                    Kept.push_back(Out);
                }
                OutSegments.push_back(Seg);
            }
            InOutTris.swap(Kept);
        }

        // === Capping ==========================================================

        struct P2 { double X = 0.0, Y = 0.0; };

        double Turn(const P2& A, const P2& B, const P2& C) { return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X); }

        double SignedArea(const std::vector<int>& InLoop, const std::vector<P2>& InPts)
        {
            double Area = 0.0;
            for(size_t I = 0, N = InLoop.size(); I < N; ++I)
            {
                const P2& A = InPts[InLoop[I]];
                const P2& B = InPts[InLoop[(I + 1) % N]];
                Area += A.X * B.Y - B.X * A.Y;
            }
            return 0.5 * Area;
        }

        bool PointInLoop(const P2& InP, const std::vector<int>& InLoop, const std::vector<P2>& InPts)
        {
            bool bInside = false;
            for(size_t I = 0, N = InLoop.size(), J = N - 1; I < N; J = I++)
            {
                const P2& A = InPts[InLoop[I]];
                const P2& B = InPts[InLoop[J]];
                if((A.Y > InP.Y) != (B.Y > InP.Y) && InP.X < (B.X - A.X) * (InP.Y - A.Y) / (B.Y - A.Y) + A.X)
                {
                    bInside = !bInside;
                }
            }
            return bInside;
        }

        /* Segments P-Q and A-B cross at a point inside both. */
        bool SegmentsCross(const P2& P, const P2& Q, const P2& A, const P2& B, const double InEps)
        {
            const double D1 = Turn(P, Q, A), D2 = Turn(P, Q, B);
            const double D3 = Turn(A, B, P), D4 = Turn(A, B, Q);
            return ((D1 > InEps && D2 < -InEps) || (D1 < -InEps && D2 > InEps))
                && ((D3 > InEps && D4 < -InEps) || (D3 < -InEps && D4 > InEps));
        }

        bool SamePoint(const P2& A, const P2& B, const double InEps)
        {
            return std::fabs(A.X - B.X) <= InEps && std::fabs(A.Y - B.Y) <= InEps;
        }

        /* Splices each hole (clockwise) into InOutOuter (counter-clockwise)
         * through a bridge to a vertex it can see, so one ear clip covers all. */
        void BridgeHoles(std::vector<int>& InOutOuter, std::vector<std::vector<int>> InHoles, const std::vector<P2>& InPts, const double InEps)
        {
            auto MaxX = [&](const std::vector<int>& InLoop)
            {
                double Best = -1e300;
                for(const int Index : InLoop) { Best = std::max(Best, InPts[Index].X); }
                return Best;
            };
            std::sort(InHoles.begin(), InHoles.end(), [&](const std::vector<int>& A, const std::vector<int>& B) { return MaxX(A) > MaxX(B); });

            for(size_t H = 0; H < InHoles.size(); ++H)
            {
                const std::vector<int>& Hole = InHoles[H];
                size_t M = 0;
                for(size_t I = 1; I < Hole.size(); ++I)
                {
                    if(InPts[Hole[I]].X > InPts[Hole[M]].X) { M = I; }
                }
                const P2& From = InPts[Hole[M]];

                /* The nearest outer vertex whose bridge crosses no edge. */
                auto Blocked = [&](const P2& InTo, const std::vector<int>& InLoop)
                {
                    for(size_t I = 0, N = InLoop.size(); I < N; ++I)
                    {
                        const P2& A = InPts[InLoop[I]];
                        const P2& B = InPts[InLoop[(I + 1) % N]];
                        if(SamePoint(A, From, InEps) || SamePoint(B, From, InEps) || SamePoint(A, InTo, InEps) || SamePoint(B, InTo, InEps)) { continue; }
                        if(SegmentsCross(From, InTo, A, B, InEps)) { return true; }
                    }
                    return false;
                };
                size_t Best     = SIZE_MAX;
                double BestDist = 1e300;
                for(size_t I = 0; I < InOutOuter.size(); ++I)
                {
                    const P2&    To   = InPts[InOutOuter[I]];
                    const double Dist = (To.X - From.X) * (To.X - From.X) + (To.Y - From.Y) * (To.Y - From.Y);
                    if(Dist >= BestDist) { continue; }
                    bool bBlocked = Blocked(To, InOutOuter);
                    for(size_t Other = H; Other < InHoles.size() && !bBlocked; ++Other) { bBlocked = Blocked(To, InHoles[Other]); }
                    if(!bBlocked) { Best = I; BestDist = Dist; }
                }
                if(Best == SIZE_MAX) { continue; }   // nothing visible: the hole is filled over

                std::vector<int> Spliced(InOutOuter.begin(), InOutOuter.begin() + Best + 1);
                for(size_t I = 0; I <= Hole.size(); ++I) { Spliced.push_back(Hole[(M + I) % Hole.size()]); }
                Spliced.push_back(InOutOuter[Best]);
                Spliced.insert(Spliced.end(), InOutOuter.begin() + Best + 1, InOutOuter.end());
                InOutOuter.swap(Spliced);
            }
        }

        /* Triangulates a counter-clockwise polygon (bridged holes allowed);
         * index triples to OutTris, all counter-clockwise. */
        void EarClip(std::vector<int> InPoly, const std::vector<P2>& InPts, const double InAreaEps, const double InEps, std::vector<int>& OutTris)
        {
            auto StrictlyInside = [&](const P2& P, const P2& A, const P2& B, const P2& C)
            {
                return Turn(A, B, P) > InAreaEps && Turn(B, C, P) > InAreaEps && Turn(C, A, P) > InAreaEps;
            };
            while(InPoly.size() > 3)
            {
                const size_t N       = InPoly.size();
                size_t       Ear     = SIZE_MAX;
                size_t       Fallback = 0;
                double       BestTurn = -1e300;
                for(size_t I = 0; I < N && Ear == SIZE_MAX; ++I)
                {
                    const int    A = InPoly[(I + N - 1) % N], B = InPoly[I], C = InPoly[(I + 1) % N];
                    const double T = Turn(InPts[A], InPts[B], InPts[C]);
                    if(T > BestTurn) { BestTurn = T; Fallback = I; }
                    if(T <= InAreaEps) { continue; }
                    bool bEar = true;
                    for(size_t J = 0; J < N && bEar; ++J)
                    {
                        const int P = InPoly[J];
                        if(P == A || P == B || P == C) { continue; }
                        const P2& Q = InPts[P];
                        if(SamePoint(Q, InPts[A], InEps) || SamePoint(Q, InPts[B], InEps) || SamePoint(Q, InPts[C], InEps)) { continue; }
                        bEar = !StrictlyInside(Q, InPts[A], InPts[B], InPts[C]);
                    }
                    if(bEar) { Ear = I; }
                }
                if(Ear == SIZE_MAX) { Ear = Fallback; }   // only flat corners left: clip the sharpest
                OutTris.push_back(InPoly[(Ear + N - 1) % N]);
                OutTris.push_back(InPoly[Ear]);
                OutTris.push_back(InPoly[(Ear + 1) % N]);
                InPoly.erase(InPoly.begin() + Ear);
            }
            if(InPoly.size() == 3) { OutTris.insert(OutTris.end(), InPoly.begin(), InPoly.end()); }
        }

        struct WeldKey
        {
            int64_t X, Y, Z;
            bool operator==(const WeldKey& O) const { return X == O.X && Y == O.Y && Z == O.Z; }
        };
        struct WeldKeyHash
        {
            size_t operator()(const WeldKey& K) const
            {
                uint64_t H = 1469598103934665603ull;
                for(const int64_t Each : { K.X, K.Y, K.Z }) { H = (H ^ uint64_t(Each)) * 1099511628211ull; }
                return size_t(H);
            }
        };

        /* What a cap's triangles carry. */
        struct CapStyle
        {
            uint32_t Section   = 0;
            double   UVScale   = 0.01;
            bool     bColors   = false;
            double   Winding   = 1.0;   // +1: a triangle's (B-A)x(C-A) points out of the mesh; -1: in
        };

        /* Closes a cut's outline into loops and fills them, facing +N.
         * False if the outline does not close (those loops are left open). */
        bool CapCut(const std::vector<Segment>& InSegments, const Plane& InPlane, const int32_t InCut, const CapStyle& InStyle,
                    const Tolerances& InTol, std::vector<Tri>& OutTris)
        {
            if(InSegments.empty()) { return true; }

            /* Join the outline's ends. Exactly: CrossingPoint makes the same
             * bits for both triangles on an edge, and any tolerance would
             * merge real points a hair apart and leave pinholes beside them. */
            std::unordered_map<WeldKey, int, WeldKeyHash> Index;
            std::vector<Vec> Points;
            auto PointOf = [&](const float* InP)
            {
                auto Bits = [](const float InF)
                {
                    const float Normal = InF + 0.0f;   // -0 is 0
                    uint32_t Out;
                    std::memcpy(&Out, &Normal, sizeof(Out));
                    return int64_t(Out);
                };
                const WeldKey Key{ Bits(InP[0]), Bits(InP[1]), Bits(InP[2]) };
                const auto Found = Index.find(Key);
                if(Found != Index.end()) { return Found->second; }
                Points.push_back(ToVec(InP));
                Index.emplace(Key, int(Points.size() - 1));
                return int(Points.size() - 1);
            };
            std::vector<std::pair<int, int>>  Edges;
            for(const Segment& Each : InSegments)
            {
                const int A = PointOf(Each.A), B = PointOf(Each.B);
                if(A != B) { Edges.emplace_back(A, B); }
            }
            /* Each segment runs from where its triangle left the kept side
             * to where it came back, so the outline is directed: outer
             * loops all turn one way and holes the other, even where welding
             * pinches a loop into two. */
            std::vector<std::vector<int>> EdgesFrom(Points.size());
            for(size_t I = 0; I < Edges.size(); ++I) { EdgesFrom[Edges[I].first].push_back(int(I)); }

            /* Walk them into loops. */
            bool bClosed = true;
            std::vector<std::vector<int>> Loops;
            std::vector<uint8_t> Used(Edges.size(), 0);
            for(size_t Start = 0; Start < Edges.size(); ++Start)
            {
                if(Used[Start]) { continue; }
                Used[Start] = 1;
                std::vector<int> Loop{ Edges[Start].first };
                int  Current = Edges[Start].second;
                bool bOpen   = false;
                while(Current != Loop.front())
                {
                    Loop.push_back(Current);
                    int Next = -1;
                    for(const int Edge : EdgesFrom[Current])
                    {
                        if(!Used[Edge]) { Next = Edge; break; }
                    }
                    if(Next < 0) { bOpen = true; break; }
                    Used[Next] = 1;
                    Current = Edges[Next].second;
                }
                if(bOpen) { bClosed = false; continue; }
                if(Loop.size() >= 3) { Loops.push_back(std::move(Loop)); }
            }
            if(Loops.empty()) { return bClosed; }

            /* Into the plane: U x V = N, so counter-clockwise in 2D faces +N. */
            const Vec N = InPlane.N;
            const Vec U = std::fabs(N.X) < 0.9 ? Cross(Vec{1.0, 0.0, 0.0}, N) * (1.0 / Length(Cross(Vec{1.0, 0.0, 0.0}, N)))
                                               : Cross(Vec{0.0, 1.0, 0.0}, N) * (1.0 / Length(Cross(Vec{0.0, 1.0, 0.0}, N)));
            const Vec V = Cross(N, U);
            std::vector<P2> Pts(Points.size());
            for(size_t I = 0; I < Points.size(); ++I) { Pts[I] = P2{ Dot(Points[I], U), Dot(Points[I], V) }; }

            /* Outer loops turn the way the largest one does; the others are
             * holes, each in the smallest outer loop around it. */
            const size_t LoopCount = Loops.size();
            std::vector<double> Areas(LoopCount);
            size_t Largest = 0;
            for(size_t I = 0; I < LoopCount; ++I)
            {
                Areas[I] = SignedArea(Loops[I], Pts);
                if(std::fabs(Areas[I]) > std::fabs(Areas[Largest])) { Largest = I; }
            }
            const bool bOuterPositive = Areas[Largest] > 0.0;
            auto IsOuter = [&](const size_t I) { return (Areas[I] > 0.0) == bOuterPositive; };

            std::vector<std::vector<std::vector<int>>> HolesOf(LoopCount);
            for(size_t I = 0; I < LoopCount; ++I)
            {
                if(IsOuter(I) || Areas[I] == 0.0) { continue; }
                size_t Parent = SIZE_MAX;
                for(size_t J = 0; J < LoopCount; ++J)
                {
                    if(IsOuter(J) && PointInLoop(Pts[Loops[I][0]], Loops[J], Pts)
                       && (Parent == SIZE_MAX || std::fabs(Areas[J]) < std::fabs(Areas[Parent])))
                    {
                        Parent = J;
                    }
                }
                if(Parent == SIZE_MAX) { continue; }
                std::vector<int> Hole = Loops[I];
                if(Areas[I] > 0.0) { std::reverse(Hole.begin(), Hole.end()); }
                HolesOf[Parent].push_back(std::move(Hole));
            }

            const double PointEps = InTol.Weld;
            std::vector<int> Triangles;
            for(size_t I = 0; I < LoopCount; ++I)
            {
                /* A plane that only nicks a corner leaves a tiny loop: it is
                 * still a hole in the piece, so fill it however small. */
                if(!IsOuter(I) || Areas[I] == 0.0) { continue; }
                std::vector<int> Outer = Loops[I];
                if(Areas[I] < 0.0) { std::reverse(Outer.begin(), Outer.end()); }
                BridgeHoles(Outer, HolesOf[I], Pts, PointEps);
                EarClip(Outer, Pts, InTol.Area, PointEps, Triangles);
            }

            for(size_t I = 0; I + 2 < Triangles.size(); I += 3)
            {
                Tri Out;
                Out.Section = InStyle.Section;
                Out.Cut     = InCut;
                for(int K = 0; K < 3; ++K)
                {
                    /* Counter-clockwise faces +N; flip for a mesh wound the other way. */
                    const int  Corner = InStyle.Winding > 0.0 ? K : (K == 0 ? 0 : 3 - K);
                    const Vec& P      = Points[Triangles[I + Corner]];
                    Vert& Each = Out.V[K];
                    Each.P[0] = float(P.X); Each.P[1] = float(P.Y); Each.P[2] = float(P.Z);
                    Each.N[0] = float(N.X); Each.N[1] = float(N.Y); Each.N[2] = float(N.Z);
                    Each.UV[0] = float(Dot(P, U) * InStyle.UVScale);
                    Each.UV[1] = float(Dot(P, V) * InStyle.UVScale);
                    if(InStyle.bColors) { Each.C[0] = Each.C[1] = Each.C[2] = Each.C[3] = 1.0f; }
                }
                OutTris.push_back(Out);
            }
            return bClosed;
        }

        // === Seeds ============================================================

        /* Bit-exact on every platform, unlike std's distributions. */
        struct Random
        {
            std::mt19937 Engine;
            explicit Random(const uint32_t InSeed) : Engine(InSeed) {}
            double Unit() { return double(Engine() >> 8) * (1.0 / 16777216.0); }
            Vec Direction()
            {
                for(;;)
                {
                    const Vec D{ Unit() * 2.0 - 1.0, Unit() * 2.0 - 1.0, Unit() * 2.0 - 1.0 };
                    const double L = Length(D);
                    if(L > 1e-3 && L <= 1.0) { return D * (1.0 / L); }
                }
            }
        };

        void Bounds(const MeshData& InMesh, Vec& OutMin, Vec& OutMax)
        {
            OutMin = Vec{ 1e300, 1e300, 1e300 };
            OutMax = Vec{ -1e300, -1e300, -1e300 };
            for(size_t I = 0; I + 2 < InMesh.Positions.size(); I += 3)
            {
                OutMin = Vec{ std::min(OutMin.X, double(InMesh.Positions[I])), std::min(OutMin.Y, double(InMesh.Positions[I + 1])), std::min(OutMin.Z, double(InMesh.Positions[I + 2])) };
                OutMax = Vec{ std::max(OutMax.X, double(InMesh.Positions[I])), std::max(OutMax.Y, double(InMesh.Positions[I + 1])), std::max(OutMax.Z, double(InMesh.Positions[I + 2])) };
            }
        }

        /* FP_Splinter's squash: lengths along the grain shrink by GrainStretch,
         * so cells that are round in that space are long along the grain. */
        struct Squash
        {
            Vec    Axis{ 0.0, 1.0, 0.0 };
            double Scale = 1.0;   // 1: none

            Vec Apply(const Vec& InP) const { return InP + Axis * ((Scale - 1.0) * Dot(Axis, InP)); }
        };

        Squash SquashOf(const FractureSettings& InSettings)
        {
            Squash Out;
            if(InSettings.Pattern != FP_Splinter) { return Out; }
            const Vec Axis = ToVec(InSettings.GrainAxis);
            const double Len = Length(Axis);
            if(Len < 1e-6) { return Out; }
            Out.Axis  = Axis * (1.0 / Len);
            Out.Scale = 1.0 / std::max(1.0, double(InSettings.GrainStretch));
            return Out;
        }
    }

    void MakeFractureSeeds(const MeshData& InMesh, const FractureSettings& InSettings, std::vector<float>& OutSeeds)
    {
        OutSeeds.clear();
        if(InMesh.Positions.size() < 9 || InSettings.PieceCount == 0) { return; }

        Vec Min, Max;
        Bounds(InMesh, Min, Max);
        const Vec    Size   = Max - Min;
        const Vec    Centre = (Min + Max) * 0.5;
        const Vec    Hit    = InSettings.bHasImpact ? ToVec(InSettings.ImpactPoint) : Centre;
        const double Radius = InSettings.ImpactRadius > 0.0f ? double(InSettings.ImpactRadius) : 0.5 * Length(Size);

        Random R(InSettings.Seed);
        auto InBounds = [&](const Vec& P)
        {
            return P.X >= Min.X && P.X <= Max.X && P.Y >= Min.Y && P.Y <= Max.Y && P.Z >= Min.Z && P.Z <= Max.Z;
        };
        auto Anywhere = [&]() { return Vec{ Min.X + Size.X * R.Unit(), Min.Y + Size.Y * R.Unit(), Min.Z + Size.Z * R.Unit() }; };
        /* A point near the hit, tried a few times before settling for anywhere. */
        auto NearHit = [&](const double InMinR, const double InMaxR, const double InPower)
        {
            for(int Try = 0; Try < 16; ++Try)
            {
                const Vec P = Hit + R.Direction() * (InMinR + (InMaxR - InMinR) * std::pow(R.Unit(), InPower));
                if(InBounds(P)) { return P; }
            }
            return Anywhere();
        };

        const uint32_t Count = InSettings.PieceCount;
        std::vector<Vec> Seeds;
        Seeds.reserve(Count);
        for(uint32_t I = 0; I < Count; ++I)
        {
            switch(InSettings.Pattern)
            {
                case FP_Clustered:
                    Seeds.push_back(R.Unit() < 0.7 ? NearHit(0.0, Radius, 2.0) : Anywhere());
                    break;
                case FP_Radial:
                {
                    /* The hit, then shells further out with more seeds each. */
                    if(I == 0 && InBounds(Hit)) { Seeds.push_back(Hit); break; }
                    const double Shell = std::sqrt(double(I) / double(std::max(1u, Count - 1)));
                    Seeds.push_back(NearHit(Radius * Shell * 0.85, Radius * Shell * 1.15, 1.0));
                    break;
                }
                default:
                    Seeds.push_back(Anywhere());
                    break;
            }
        }
        OutSeeds.reserve(Seeds.size() * 3);
        for(const Vec& Each : Seeds)
        {
            OutSeeds.push_back(float(Each.X));
            OutSeeds.push_back(float(Each.Y));
            OutSeeds.push_back(float(Each.Z));
        }
    }

    bool FractureMesh(const MeshData& InMesh, const FractureSettings& InSettings, FractureResult& OutResult)
    {
        OutResult = FractureResult();
        if(InMesh.Positions.size() < 9 || InMesh.Indices.size() < 3)
        {
            RENDERCORE_LOG_WARN("fracture: the mesh has no triangles")
            return false;
        }
        if(InMesh.IsSkinned())
        {
            RENDERCORE_LOG_WARN("fracture: skinned meshes cannot be fractured")
            return false;
        }

        // === The source as a soup =============================================

        const uint32_t VertexCount = InMesh.GetVertexCount();
        const bool     bColors     = InMesh.Colors.size() == size_t(VertexCount) * 4;
        auto VertOf = [&](const uint32_t InIndex)
        {
            Vert Out;
            std::memcpy(Out.P, &InMesh.Positions[size_t(InIndex) * 3], sizeof(Out.P));
            if(InMesh.Normals.size() == size_t(VertexCount) * 3) { std::memcpy(Out.N, &InMesh.Normals[size_t(InIndex) * 3], sizeof(Out.N)); }
            if(InMesh.UV0.size() == size_t(VertexCount) * 2)     { std::memcpy(Out.UV, &InMesh.UV0[size_t(InIndex) * 2], sizeof(Out.UV)); }
            if(bColors)                                           { std::memcpy(Out.C, &InMesh.Colors[size_t(InIndex) * 4], sizeof(Out.C)); }
            return Out;
        };

        std::vector<MeshSection> Sections = InMesh.Sections;
        if(Sections.empty())
        {
            MeshSection All;
            All.IndexCount = uint32_t(InMesh.Indices.size());
            Sections.push_back(All);
        }
        std::vector<Tri> Source;
        Source.reserve(InMesh.Indices.size() / 3);
        for(uint32_t S = 0; S < uint32_t(Sections.size()); ++S)
        {
            const uint32_t First = Sections[S].FirstIndex;
            const uint32_t Last  = std::min<uint32_t>(First + Sections[S].IndexCount, uint32_t(InMesh.Indices.size()));
            for(uint32_t I = First; I + 3 <= Last; I += 3)
            {
                const uint32_t A = InMesh.Indices[I], B = InMesh.Indices[I + 1], C = InMesh.Indices[I + 2];
                if(A >= VertexCount || B >= VertexCount || C >= VertexCount) { continue; }
                Tri Each;
                Each.V[0] = VertOf(A); Each.V[1] = VertOf(B); Each.V[2] = VertOf(C);
                Each.Section = S;
                Source.push_back(Each);
            }
        }

        /* Which way the source is wound: its signed volume is positive when
         * (B-A)x(C-A) points out. */
        double SignedVolume = 0.0;
        for(const Tri& Each : Source)
        {
            SignedVolume += Dot(ToVec(Each.V[0].P), Cross(ToVec(Each.V[1].P), ToVec(Each.V[2].P))) / 6.0;
        }
        const double Winding = SignedVolume < 0.0 ? -1.0 : 1.0;

        Vec Min, Max;
        Bounds(InMesh, Min, Max);
        const double Extent = std::max(Length(Max - Min), 1e-6);
        Tolerances Tol;
        Tol.Snap = Extent * 1e-6;
        Tol.Weld = Extent * 1e-5;
        Tol.Area = Extent * Extent * 1e-14;

        CapStyle Style;
        Style.Section = uint32_t(Sections.size());   // Interior: after the source's own
        Style.UVScale = InSettings.InteriorUVScale;
        Style.bColors = bColors;
        Style.Winding = Winding;

        // === Seeds and their planes ===========================================

        std::vector<float> SeedData;
        MakeFractureSeeds(InMesh, InSettings, SeedData);
        const Squash   Space     = SquashOf(InSettings);
        const uint32_t SeedCount = uint32_t(SeedData.size() / 3);
        std::vector<Vec> Seeds(SeedCount);
        for(uint32_t I = 0; I < SeedCount; ++I) { Seeds[I] = Space.Apply(ToVec(&SeedData[size_t(I) * 3])); }

        /* The plane half way from seed I to seed J, in the squashed space,
         * as a plane in mesh space (the squash is symmetric). */
        auto PlaneBetween = [&](const uint32_t I, const uint32_t J, Plane& OutPlane)
        {
            const Vec    Delta = Seeds[J] - Seeds[I];
            const Vec    Normal = Space.Apply(Delta);
            const double Len   = Length(Normal);
            if(Len < Tol.Weld) { return false; }   // the same seed twice
            OutPlane.N = Normal * (1.0 / Len);
            OutPlane.D = Dot((Seeds[I] + Seeds[J]) * 0.5, Delta) / Len;
            return true;
        };

        // === One piece per seed ===============================================

        std::vector<int32_t> PieceOfSeed(SeedCount, -1);
        std::vector<std::vector<std::pair<int32_t, double>>> CutAreas;   // per piece: (seed, cap area)
        std::vector<Segment> Outline;
        for(uint32_t I = 0; I < SeedCount; ++I)
        {
            /* Nearest neighbours first: they cut the most, and once the
             * piece is small the far planes miss it and cost one test. */
            std::vector<uint32_t> Others;
            for(uint32_t J = 0; J < SeedCount; ++J) { if(J != I) { Others.push_back(J); } }
            std::sort(Others.begin(), Others.end(), [&](const uint32_t A, const uint32_t B)
            {
                const double DA = Length(Seeds[A] - Seeds[I]), DB = Length(Seeds[B] - Seeds[I]);
                return DA != DB ? DA < DB : A < B;
            });

            std::vector<Tri> Piece = Source;
            for(const uint32_t J : Others)
            {
                Plane Cut;
                if(!PlaneBetween(I, J, Cut)) { continue; }
                bool bAllIn = true;
                for(const Tri& Each : Piece)
                {
                    for(const Vert& Corner : Each.V) { bAllIn = bAllIn && Cut.Eval(Corner.P) < -Tol.Snap; }
                    if(!bAllIn) { break; }
                }
                if(bAllIn) { continue; }

                Outline.clear();
                ClipByPlane(Piece, Cut, Tol, Outline);
                if(Piece.empty()) { break; }
                if(!CapCut(Outline, Cut, int32_t(J), Style, Tol, Piece)) { ++OutResult.OpenCaps; }
            }
            if(Piece.empty()) { continue; }

            /* Volume and centre of mass, by tetrahedra from the origin. */
            double Volume = 0.0;
            Vec    Moment;
            std::vector<std::pair<int32_t, double>> Areas;
            for(const Tri& Each : Piece)
            {
                const Vec A = ToVec(Each.V[0].P), B = ToVec(Each.V[1].P), C = ToVec(Each.V[2].P);
                const double Tet = Winding * Dot(A, Cross(B, C)) / 6.0;
                Volume += Tet;
                Moment = Moment + (A + B + C) * (Tet * 0.25);
                if(Each.Cut >= 0)
                {
                    const double Area = 0.5 * Length(Cross(B - A, C - A));
                    auto Found = std::find_if(Areas.begin(), Areas.end(), [&](const auto& P) { return P.first == Each.Cut; });
                    if(Found == Areas.end()) { Areas.emplace_back(Each.Cut, Area); }
                    else                     { Found->second += Area; }
                }
            }
            if(Volume <= Extent * Extent * Extent * 1e-9) { continue; }
            const Vec Centre = Moment * (1.0 / Volume);

            // === Weld into a MeshData =========================================

            FracturePiece Out;
            Out.Center[0] = float(Centre.X); Out.Center[1] = float(Centre.Y); Out.Center[2] = float(Centre.Z);
            Out.Volume    = float(Volume);
            MeshData& Mesh = Out.Mesh;

            struct VertHash
            {
                size_t operator()(const Vert& V) const
                {
                    uint64_t H = 1469598103934665603ull;
                    const unsigned char* Bytes = reinterpret_cast<const unsigned char*>(&V);
                    for(size_t I = 0; I < sizeof(Vert); ++I) { H = (H ^ Bytes[I]) * 1099511628211ull; }
                    return size_t(H);
                }
            };
            struct VertEqual
            {
                bool operator()(const Vert& A, const Vert& B) const { return std::memcmp(&A, &B, sizeof(Vert)) == 0; }
            };
            std::unordered_map<Vert, uint32_t, VertHash, VertEqual> Welded;
            for(uint32_t S = 0; S <= uint32_t(Sections.size()); ++S)
            {
                MeshSection Section;
                if(S < Sections.size()) { Section = Sections[S]; }
                else                    { Section.Name = "Interior"; Section.Material = InSettings.InteriorMaterial; }
                Section.FirstIndex = uint32_t(Mesh.Indices.size());
                for(const Tri& Each : Piece)
                {
                    if(Each.Section != S) { continue; }
                    for(Vert Corner : Each.V)
                    {
                        Corner.P[0] = float(Corner.P[0] - Centre.X);
                        Corner.P[1] = float(Corner.P[1] - Centre.Y);
                        Corner.P[2] = float(Corner.P[2] - Centre.Z);
                        const auto Found = Welded.find(Corner);
                        if(Found != Welded.end()) { Mesh.Indices.push_back(Found->second); continue; }
                        const uint32_t Index = Mesh.GetVertexCount();
                        Welded.emplace(Corner, Index);
                        Mesh.Indices.push_back(Index);
                        Mesh.Positions.insert(Mesh.Positions.end(), Corner.P, Corner.P + 3);
                        Mesh.Normals.insert(Mesh.Normals.end(), Corner.N, Corner.N + 3);
                        Mesh.UV0.insert(Mesh.UV0.end(), Corner.UV, Corner.UV + 2);
                        if(bColors) { Mesh.Colors.insert(Mesh.Colors.end(), Corner.C, Corner.C + 4); }
                    }
                }
                Section.IndexCount = uint32_t(Mesh.Indices.size()) - Section.FirstIndex;
                Mesh.Sections.push_back(Section);
            }
            ComputeMeshBounds(Mesh);

            PieceOfSeed[I] = int32_t(OutResult.Pieces.size());
            OutResult.Pieces.push_back(std::move(Out));
            CutAreas.push_back(std::move(Areas));
        }

        /* Neighbours: a piece whose cap from seed J's plane survived the
         * later cuts shares that face with J's piece. */
        std::set<std::pair<uint32_t, uint32_t>> Pairs;
        for(uint32_t P = 0; P < uint32_t(CutAreas.size()); ++P)
        {
            for(const auto& Each : CutAreas[P])
            {
                const int32_t Other = PieceOfSeed[Each.first];
                if(Other < 0 || uint32_t(Other) == P || Each.second <= Extent * Extent * 1e-7) { continue; }
                Pairs.emplace(std::min(P, uint32_t(Other)), std::max(P, uint32_t(Other)));
            }
        }
        OutResult.Neighbours.assign(Pairs.begin(), Pairs.end());
        return true;
    }
}
