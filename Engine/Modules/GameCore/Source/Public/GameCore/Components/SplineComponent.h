#ifndef GAMECORE_SPLINECOMPONENT_H
#define GAMECORE_SPLINECOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/GameCoreAPI.h"
#include "Public/GameCore/Spline.h"
#include "Public/Math/Vector.h"

namespace GameCore
{
    enum : uint32_t
    {
        SPLINE_POINTS_MAX = 64,
    };

    /*
     * "Spline": a curve through up to SPLINE_POINTS_MAX points in its
     * entity's space (Spline.h), placed by its Transform. Nothing is drawn
     * in game; the editor draws it and edits its points (World > Spline
     * Points, or the `spline` console command).
     *
     * What uses it: a Procedural Spawner on the same entity places meshes
     * along it (Spline Path) or inside it (Spline Area, closed), and clears
     * them from around it (a spawner's Exclusions). A game samples it from
     * Lua or C++ (GetLocationAtDistance, FindDistanceClosestTo).
     *
     * POINTS IN USE are those up to the last one not at the entity's origin
     * (the Details panel's list rule): the first point may sit on the
     * origin, the last may not. GetPointCount counts them; AddPoint,
     * InsertPoint and RemovePoint keep the list packed.
     *
     * Width is a band around the curve, half on each side: how wide a road
     * is when it clears grass, or how wide a Spline Path spawner scatters.
     */
    struct GAMECORE_API SplineComponent
    {
        GAMECORE_COMPONENT_BODY()

        EngineCore::Vector3 Points[SPLINE_POINTS_MAX];
        uint8_t bClosed = 0;
        uint8_t Type    = ST_Curve;   // ESplineType
        uint8_t Reserved0 = 0;
        uint8_t Reserved1 = 0;
        float   Width   = 0.0f;       // world units across, centred on the curve

        int32_t GetPointCount() const;

        /* Entity space. GetPoint is the origin past the count. */
        EngineCore::Vector3 GetPoint(int32_t InIndex) const;
        bool SetPoint(int32_t InIndex, const EngineCore::Vector3& InPoint);
        /* At the end; false when full. A point exactly on the origin is
         * nudged off it, or it would not count (see above). */
        bool AddPoint(const EngineCore::Vector3& InPoint);
        bool InsertPoint(int32_t InIndex, const EngineCore::Vector3& InPoint);
        bool RemovePoint(int32_t InIndex);
        void ClearPoints();

        /* The curve in entity space, and in world space (its entity's world matrix). */
        SplineCurve GetCurve() const;
        SplineCurve GetWorldCurve() const;

        // === World-space queries (Lua's too) ==================================
        float               GetLength() const;
        EngineCore::Vector3 GetLocationAtDistance(float InDistance) const;
        EngineCore::Vector3 GetDirectionAtDistance(float InDistance) const;
        /* The distance along the curve nearest InLocation. */
        float               FindDistanceClosestTo(const EngineCore::Vector3& InLocation) const;
        /* Inside the closed curve, seen from above. */
        bool                IsInside(const EngineCore::Vector3& InLocation) const;
    };
}

#endif //GAMECORE_SPLINECOMPONENT_H
