#ifndef GAMECORE_SPLINE_H
#define GAMECORE_SPLINE_H

#include <cstdint>
#include <vector>

#include "Public/GameCore/GameCoreAPI.h"
#include "Public/Math/Matrix.h"
#include "Public/Math/Vector.h"

/*
 * A curve through a list of points: what a Spline component draws, what the
 * Procedural Spawner places along or inside, and what a game samples to
 * follow a path (a rail camera, a patrol, a road).
 *
 * SHAPES. ST_Curve is a centripetal Catmull-Rom curve: it passes through
 * every point, never loops or overshoots between close ones, and its ends
 * leave straight toward their neighbours. ST_Linear joins the points with
 * straight segments. A closed curve joins the last point back to the first.
 *
 * DISTANCE. A built curve keeps an arc-length table (SPLINE_STEPS samples a
 * segment), so sampling by distance along it is even: equal steps of
 * distance are equal steps along the curve, whatever the spacing of its
 * points.
 *
 * Plain value type; build it from whatever space the points are in (an
 * entity's local space, or the world's through ToWorld) and the answers are
 * in that space.
 */
namespace GameCore
{
    enum ESplineType : uint8_t
    {
        ST_Curve  = 0,   // centripetal Catmull-Rom, through every point
        ST_Linear = 1,   // straight segments
    };

    enum : uint32_t
    {
        SPLINE_STEPS = 16,   // arc-length samples per segment (ST_Curve)
    };

    /* Where a sample along a curve is, and which way the curve goes there (unit length). */
    struct SplineSample
    {
        EngineCore::Vector3 Position;
        EngineCore::Vector3 Tangent{ 0.0f, 0.0f, 1.0f };
        float               Distance = 0.0f;   // along the curve from its start
    };

    class GAMECORE_API SplineCurve
    {
    public:
        SplineCurve() = default;
        SplineCurve(const EngineCore::Vector3* InPoints, uint32_t InCount, bool bInClosed, uint8_t InType = ST_Curve)
        {
            Build(InPoints, InCount, bInClosed, InType);
        }

        /* Rebuilds from InCount points. A closed curve needs three; fewer stay open. */
        void Build(const EngineCore::Vector3* InPoints, uint32_t InCount, bool bInClosed, uint8_t InType = ST_Curve);

        /* The same curve with every point through InMatrix (row-major, column
         * vectors: an entity's world matrix). */
        SplineCurve Transformed(const EngineCore::Matrix4x4& InMatrix) const;

        bool     IsValid()       const { return Points.size() >= 2; }
        bool     IsClosed()      const { return bClosed; }
        uint8_t  GetType()       const { return Type; }
        uint32_t GetPointCount() const { return uint32_t(Points.size()); }
        uint32_t GetSegmentCount() const;
        const std::vector<EngineCore::Vector3>& GetPoints() const { return Points; }

        /* Its whole length; 0 with fewer than two points. */
        float GetLength() const { return Lengths.empty() ? 0.0f : Lengths.back(); }

        /* Distance along the curve where point InIndex is. */
        float GetDistanceAtPoint(uint32_t InIndex) const;

        /* The sample InDistance along it, clamped to the ends (wrapped, when closed). */
        SplineSample SampleAtDistance(float InDistance) const;

        /* Segment InSegment (point i to i+1) at InT 0..1. */
        EngineCore::Vector3 Evaluate(uint32_t InSegment, float InT) const;

        /* The polyline the arc-length table was made from: what an editor
         * draws and what NearestTo and ContainsXZ test. A closed curve's ends
         * coincide. */
        const std::vector<EngineCore::Vector3>& GetPolyline() const { return Polyline; }

        /* The nearest point on the curve to InPoint: its sample, and the
         * straight-line distance to it (OutDistance). False when not valid. */
        bool NearestTo(const EngineCore::Vector3& InPoint, SplineSample& OutSample, float& OutDistance) const;

        /* The same, measured on X and Z only (from above): what "within this
         * far of a road" means on uneven ground. */
        bool NearestToXZ(const EngineCore::Vector3& InPoint, SplineSample& OutSample, float& OutDistance) const;

        /* Whether (InX, InZ) is inside the curve seen from above. Closed curves only. */
        bool ContainsXZ(float InX, float InZ) const;

        /* Its polyline's box. False when not valid. */
        bool GetBounds(EngineCore::Vector3& OutMin, EngineCore::Vector3& OutMax) const;

    private:
        /* Point InIndex, wrapped when closed, clamped otherwise -- phantom
         * ends are made by mirroring, so an open curve's ends are straight. */
        EngineCore::Vector3 ControlPoint(int64_t InIndex) const;

        std::vector<EngineCore::Vector3> Points;
        std::vector<EngineCore::Vector3> Polyline;   // SPLINE_STEPS per segment, plus the end
        std::vector<float>               Lengths;    // arc length at each polyline point
        bool                             bClosed = false;
        uint8_t                          Type    = ST_Curve;
    };
}

#endif //GAMECORE_SPLINE_H
