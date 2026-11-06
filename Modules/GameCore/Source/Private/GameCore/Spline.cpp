#include "Public/GameCore/Spline.h"

#include <algorithm>
#include <cmath>

namespace GameCore
{
    namespace
    {
        using EngineCore::Vector3;

        float DistanceBetween(const Vector3& InA, const Vector3& InB)
        {
            const float X = InA.GetX() - InB.GetX(), Y = InA.GetY() - InB.GetY(), Z = InA.GetZ() - InB.GetZ();
            return std::sqrt(X * X + Y * Y + Z * Z);
        }

        Vector3 Lerp(const Vector3& InA, const Vector3& InB, const float InT)
        {
            return Vector3(InA.GetX() + (InB.GetX() - InA.GetX()) * InT,
                           InA.GetY() + (InB.GetY() - InA.GetY()) * InT,
                           InA.GetZ() + (InB.GetZ() - InA.GetZ()) * InT);
        }

        Vector3 Direction(const Vector3& InFrom, const Vector3& InTo, const Vector3& InFallback)
        {
            const float Length = DistanceBetween(InFrom, InTo);
            if(Length < 1e-6f) { return InFallback; }
            return Vector3((InTo.GetX() - InFrom.GetX()) / Length, (InTo.GetY() - InFrom.GetY()) / Length,
                           (InTo.GetZ() - InFrom.GetZ()) / Length);
        }

        Vector3 TransformPoint(const EngineCore::Matrix4x4& InMatrix, const Vector3& InPoint)
        {
            float Out[3];
            for(int Row = 0; Row < 3; ++Row)
            {
                Out[Row] = float(InMatrix.Data[Row][0] * InPoint.GetX() + InMatrix.Data[Row][1] * InPoint.GetY()
                               + InMatrix.Data[Row][2] * InPoint.GetZ() + InMatrix.Data[Row][3]);
            }
            return Vector3(Out[0], Out[1], Out[2]);
        }

        /* The parameter step between two points of a centripetal curve: the square root of their distance. */
        float Knot(const Vector3& InA, const Vector3& InB)
        {
            return std::max(std::sqrt(DistanceBetween(InA, InB)), 1e-4f);
        }

        /* Squared distance from (InX, InZ) to segment A-B on X and Z, and where along it (0..1). */
        float DistanceToSegmentXZ2(const float InX, const float InZ, const Vector3& InA, const Vector3& InB, float& OutT)
        {
            const float DX = InB.GetX() - InA.GetX(), DZ = InB.GetZ() - InA.GetZ();
            const float Length2 = DX * DX + DZ * DZ;
            OutT = Length2 > 1e-12f ? std::clamp<float>(((InX - InA.GetX()) * DX + (InZ - InA.GetZ()) * DZ) / Length2, 0.0f, 1.0f) : 0.0f;
            const float PX = InA.GetX() + DX * OutT - InX, PZ = InA.GetZ() + DZ * OutT - InZ;
            return PX * PX + PZ * PZ;
        }

        float DistanceToSegment2(const Vector3& InP, const Vector3& InA, const Vector3& InB, float& OutT)
        {
            const float DX = InB.GetX() - InA.GetX(), DY = InB.GetY() - InA.GetY(), DZ = InB.GetZ() - InA.GetZ();
            const float Length2 = DX * DX + DY * DY + DZ * DZ;
            OutT = Length2 > 1e-12f
                 ? std::clamp<float>(((InP.GetX() - InA.GetX()) * DX + (InP.GetY() - InA.GetY()) * DY + (InP.GetZ() - InA.GetZ()) * DZ) / Length2,
                              0.0f, 1.0f)
                 : 0.0f;
            const float PX = InA.GetX() + DX * OutT - InP.GetX();
            const float PY = InA.GetY() + DY * OutT - InP.GetY();
            const float PZ = InA.GetZ() + DZ * OutT - InP.GetZ();
            return PX * PX + PY * PY + PZ * PZ;
        }
    }

    void SplineCurve::Build(const Vector3* InPoints, const uint32_t InCount, const bool bInClosed, const uint8_t InType)
    {
        Points.assign(InPoints, InPoints + (InPoints ? InCount : 0u));
        bClosed = bInClosed && Points.size() >= 3;
        Type    = InType == ST_Linear ? uint8_t(ST_Linear) : uint8_t(ST_Curve);
        Polyline.clear();
        Lengths.clear();
        if(Points.size() < 2) { return; }

        const uint32_t Segments = GetSegmentCount();
        const uint32_t Steps    = Type == ST_Linear ? 1u : uint32_t(SPLINE_STEPS);
        Polyline.reserve(size_t(Segments) * Steps + 1);
        Lengths.reserve(size_t(Segments) * Steps + 1);
        for(uint32_t Segment = 0; Segment < Segments; ++Segment)
        {
            for(uint32_t Step = 0; Step < Steps; ++Step)
            {
                Polyline.push_back(Evaluate(Segment, float(Step) / float(Steps)));
            }
        }
        Polyline.push_back(bClosed ? Points.front() : Points.back());

        float Total = 0.0f;
        Lengths.push_back(0.0f);
        for(size_t Index = 1; Index < Polyline.size(); ++Index)
        {
            Total += DistanceBetween(Polyline[Index - 1], Polyline[Index]);
            Lengths.push_back(Total);
        }
    }

    SplineCurve SplineCurve::Transformed(const EngineCore::Matrix4x4& InMatrix) const
    {
        std::vector<Vector3> Moved;
        Moved.reserve(Points.size());
        for(const Vector3& Each : Points) { Moved.push_back(TransformPoint(InMatrix, Each)); }
        return SplineCurve(Moved.data(), uint32_t(Moved.size()), bClosed, Type);
    }

    uint32_t SplineCurve::GetSegmentCount() const
    {
        if(Points.size() < 2) { return 0; }
        return bClosed ? uint32_t(Points.size()) : uint32_t(Points.size() - 1);
    }

    Vector3 SplineCurve::ControlPoint(const int64_t InIndex) const
    {
        const int64_t Count = int64_t(Points.size());
        if(bClosed)
        {
            return Points[size_t(((InIndex % Count) + Count) % Count)];
        }
        if(InIndex < 0)
        {
            /* Mirrored through the first point: the curve leaves it toward the second. */
            const Vector3& A = Points[0];
            const Vector3& B = Points[1];
            return Vector3(2.0f * A.GetX() - B.GetX(), 2.0f * A.GetY() - B.GetY(), 2.0f * A.GetZ() - B.GetZ());
        }
        if(InIndex >= Count)
        {
            const Vector3& A = Points[size_t(Count - 1)];
            const Vector3& B = Points[size_t(Count - 2)];
            return Vector3(2.0f * A.GetX() - B.GetX(), 2.0f * A.GetY() - B.GetY(), 2.0f * A.GetZ() - B.GetZ());
        }
        return Points[size_t(InIndex)];
    }

    Vector3 SplineCurve::Evaluate(const uint32_t InSegment, const float InT) const
    {
        if(Points.empty()) { return Vector3(); }
        if(Points.size() == 1) { return Points[0]; }
        const uint32_t Segment = std::min(InSegment, GetSegmentCount() - 1);
        const float    T       = std::clamp(InT, 0.0f, 1.0f);
        const Vector3 P1 = ControlPoint(int64_t(Segment));
        const Vector3 P2 = ControlPoint(int64_t(Segment) + 1);
        if(Type == ST_Linear) { return Lerp(P1, P2, T); }
        const Vector3 P0 = ControlPoint(int64_t(Segment) - 1);
        const Vector3 P3 = ControlPoint(int64_t(Segment) + 2);

        /* Barry and Goldman's pyramid, with centripetal knots. */
        const float T0 = 0.0f;
        const float T1 = T0 + Knot(P0, P1);
        const float T2 = T1 + Knot(P1, P2);
        const float T3 = T2 + Knot(P2, P3);
        const float U  = T1 + (T2 - T1) * T;
        const Vector3 A1 = Lerp(P0, P1, (U - T0) / (T1 - T0));
        const Vector3 A2 = Lerp(P1, P2, (U - T1) / (T2 - T1));
        const Vector3 A3 = Lerp(P2, P3, (U - T2) / (T3 - T2));
        const Vector3 B1 = Lerp(A1, A2, (U - T0) / (T2 - T0));
        const Vector3 B2 = Lerp(A2, A3, (U - T1) / (T3 - T1));
        return Lerp(B1, B2, (U - T1) / (T2 - T1));
    }

    float SplineCurve::GetDistanceAtPoint(const uint32_t InIndex) const
    {
        if(Lengths.empty()) { return 0.0f; }
        const size_t Steps = Type == ST_Linear ? 1u : size_t(SPLINE_STEPS);
        return Lengths[std::min(size_t(InIndex) * Steps, Lengths.size() - 1)];
    }

    SplineSample SplineCurve::SampleAtDistance(const float InDistance) const
    {
        SplineSample Out;
        if(Points.empty()) { return Out; }
        if(Lengths.size() < 2)
        {
            Out.Position = Points[0];
            return Out;
        }
        const float Total = Lengths.back();
        float Distance = InDistance;
        if(bClosed && Total > 0.0f)
        {
            Distance = std::fmod(Distance, Total);
            if(Distance < 0.0f) { Distance += Total; }
        }
        Distance = std::clamp(Distance, 0.0f, Total);

        /* The polyline step it falls in, then exactly on the curve there. */
        const size_t Upper = size_t(std::upper_bound(Lengths.begin(), Lengths.end(), Distance) - Lengths.begin());
        const size_t Index = std::min(Upper == 0 ? size_t(0) : Upper - 1, Lengths.size() - 2);
        const float  Span  = Lengths[Index + 1] - Lengths[Index];
        const float  Local = Span > 1e-6f ? (Distance - Lengths[Index]) / Span : 0.0f;
        const uint32_t Steps   = Type == ST_Linear ? 1u : uint32_t(SPLINE_STEPS);
        const uint32_t Segment = uint32_t(Index / Steps);
        const float    T       = (float(Index % Steps) + Local) / float(Steps);
        Out.Position = Evaluate(Segment, T);
        Out.Distance = Distance;

        /* Which way: across a small step of the curve itself, so a curve's tangent turns smoothly. */
        const Vector3 Fallback = Direction(Polyline[Index], Polyline[Index + 1], Vector3(0.0f, 0.0f, 1.0f));
        if(Type == ST_Linear)
        {
            Out.Tangent = Fallback;
        }
        else
        {
            const float Delta = 0.5f / float(Steps);
            const Vector3 Before = Evaluate(Segment, std::max(T - Delta, 0.0f));
            const Vector3 After  = Evaluate(Segment, std::min(T + Delta, 1.0f));
            Out.Tangent = Direction(Before, After, Fallback);
        }
        return Out;
    }

    bool SplineCurve::NearestTo(const Vector3& InPoint, SplineSample& OutSample, float& OutDistance) const
    {
        if(Polyline.size() < 2) { return false; }
        float Best = 1e30f, BestAlong = 0.0f;
        for(size_t Index = 0; Index + 1 < Polyline.size(); ++Index)
        {
            float T = 0.0f;
            const float D2 = DistanceToSegment2(InPoint, Polyline[Index], Polyline[Index + 1], T);
            if(D2 < Best)
            {
                Best      = D2;
                BestAlong = Lengths[Index] + (Lengths[Index + 1] - Lengths[Index]) * T;
            }
        }
        OutSample   = SampleAtDistance(BestAlong);
        OutDistance = std::sqrt(Best);
        return true;
    }

    bool SplineCurve::NearestToXZ(const Vector3& InPoint, SplineSample& OutSample, float& OutDistance) const
    {
        if(Polyline.size() < 2) { return false; }
        float Best = 1e30f, BestAlong = 0.0f;
        for(size_t Index = 0; Index + 1 < Polyline.size(); ++Index)
        {
            float T = 0.0f;
            const float D2 = DistanceToSegmentXZ2(InPoint.GetX(), InPoint.GetZ(), Polyline[Index], Polyline[Index + 1], T);
            if(D2 < Best)
            {
                Best      = D2;
                BestAlong = Lengths[Index] + (Lengths[Index + 1] - Lengths[Index]) * T;
            }
        }
        OutSample   = SampleAtDistance(BestAlong);
        OutDistance = std::sqrt(Best);
        return true;
    }

    bool SplineCurve::ContainsXZ(const float InX, const float InZ) const
    {
        if(!bClosed || Polyline.size() < 4) { return false; }
        /* Even-odd crossings of a ray toward +X. */
        bool bInside = false;
        for(size_t Index = 0, Prev = Polyline.size() - 1; Index < Polyline.size(); Prev = Index++)
        {
            const float AX = Polyline[Index].GetX(), AZ = Polyline[Index].GetZ();
            const float BX = Polyline[Prev].GetX(),  BZ = Polyline[Prev].GetZ();
            if((AZ > InZ) != (BZ > InZ))
            {
                const float Cross = AX + (InZ - AZ) * (BX - AX) / (BZ - AZ);
                if(InX < Cross) { bInside = !bInside; }
            }
        }
        return bInside;
    }

    bool SplineCurve::GetBounds(Vector3& OutMin, Vector3& OutMax) const
    {
        if(Polyline.empty()) { return false; }
        float Min[3] = { 1e30f, 1e30f, 1e30f }, Max[3] = { -1e30f, -1e30f, -1e30f };
        for(const Vector3& Each : Polyline)
        {
            const float P[3] = { float(Each.GetX()), float(Each.GetY()), float(Each.GetZ()) };
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                Min[Axis] = std::min(Min[Axis], P[Axis]);
                Max[Axis] = std::max(Max[Axis], P[Axis]);
            }
        }
        OutMin = Vector3(Min[0], Min[1], Min[2]);
        OutMax = Vector3(Max[0], Max[1], Max[2]);
        return true;
    }
}
