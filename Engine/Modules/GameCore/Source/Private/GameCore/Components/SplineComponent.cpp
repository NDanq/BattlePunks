#include "Public/GameCore/Components/SplineComponent.h"

#include <cstdio>

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"

/* SplineComponent: registration, its point list, and what it exposes. The
 * curve itself is Spline.h. */

namespace GameCore
{
    namespace
    {
        bool IsOrigin(const EngineCore::Vector3& InPoint)
        {
            return InPoint.GetX() == 0.0f && InPoint.GetY() == 0.0f && InPoint.GetZ() == 0.0f;
        }

        /* Off the origin by a hundredth of a unit: still counted as a point. */
        EngineCore::Vector3 Counted(const EngineCore::Vector3& InPoint)
        {
            return IsOrigin(InPoint) ? EngineCore::Vector3(0.0f, 0.01f, 0.0f) : InPoint;
        }

        struct PointFieldNames
        {
            char Names[SPLINE_POINTS_MAX][16];
            PointFieldNames()
            {
                for(uint32_t Index = 0; Index < SPLINE_POINTS_MAX; ++Index)
                {
                    std::snprintf(Names[Index], sizeof(Names[Index]), "Point%u", Index);
                }
            }
        };

        const char* PointField(const uint32_t InIndex)
        {
            static const PointFieldNames Fields;
            return Fields.Names[InIndex];
        }
    }

    GAMECORE_DEFINE_COMPONENT(SplineComponent, "Spline")

    int32_t SplineComponent::GetPointCount() const
    {
        for(uint32_t Count = SPLINE_POINTS_MAX; Count > 0; --Count)
        {
            if(!IsOrigin(Points[Count - 1])) { return int32_t(Count); }
        }
        return 0;
    }

    EngineCore::Vector3 SplineComponent::GetPoint(const int32_t InIndex) const
    {
        if(InIndex < 0 || InIndex >= GetPointCount()) { return EngineCore::Vector3(); }
        return Points[InIndex];
    }

    bool SplineComponent::SetPoint(const int32_t InIndex, const EngineCore::Vector3& InPoint)
    {
        const uint32_t Count = uint32_t(GetPointCount());
        if(InIndex < 0 || uint32_t(InIndex) >= Count) { return false; }
        /* The last one may not sit on the origin; the others may. */
        Points[InIndex] = uint32_t(InIndex) + 1 == Count ? Counted(InPoint) : InPoint;
        return true;
    }

    bool SplineComponent::AddPoint(const EngineCore::Vector3& InPoint)
    {
        const uint32_t Count = uint32_t(GetPointCount());
        if(Count >= SPLINE_POINTS_MAX) { return false; }
        Points[Count] = Counted(InPoint);
        return true;
    }

    bool SplineComponent::InsertPoint(const int32_t InIndex, const EngineCore::Vector3& InPoint)
    {
        const uint32_t Count = uint32_t(GetPointCount());
        if(InIndex < 0 || uint32_t(InIndex) > Count || Count >= SPLINE_POINTS_MAX) { return false; }
        if(uint32_t(InIndex) == Count) { return AddPoint(InPoint); }
        for(uint32_t Index = Count; Index > uint32_t(InIndex); --Index) { Points[Index] = Points[Index - 1]; }
        Points[InIndex] = InPoint;
        return true;
    }

    bool SplineComponent::RemovePoint(const int32_t InIndex)
    {
        const uint32_t Count = uint32_t(GetPointCount());
        if(InIndex < 0 || uint32_t(InIndex) >= Count) { return false; }
        for(uint32_t Index = uint32_t(InIndex); Index + 1 < Count; ++Index) { Points[Index] = Points[Index + 1]; }
        Points[Count - 1] = EngineCore::Vector3();
        /* The new last point must not be on the origin, or the count would shrink further. */
        if(Count >= 2) { Points[Count - 2] = Counted(Points[Count - 2]); }
        return true;
    }

    void SplineComponent::ClearPoints()
    {
        for(EngineCore::Vector3& Each : Points) { Each = EngineCore::Vector3(); }
    }

    SplineCurve SplineComponent::GetCurve() const
    {
        return SplineCurve(Points, uint32_t(GetPointCount()), bClosed != 0, Type);
    }

    SplineCurve SplineComponent::GetWorldCurve() const
    {
        const Entity Self = Owner.GetEntity();
        return Self.IsAlive() ? GetCurve().Transformed(Self.GetWorldMatrix()) : GetCurve();
    }

    float SplineComponent::GetLength() const
    {
        return GetWorldCurve().GetLength();
    }

    EngineCore::Vector3 SplineComponent::GetLocationAtDistance(const float InDistance) const
    {
        return GetWorldCurve().SampleAtDistance(InDistance).Position;
    }

    EngineCore::Vector3 SplineComponent::GetDirectionAtDistance(const float InDistance) const
    {
        return GetWorldCurve().SampleAtDistance(InDistance).Tangent;
    }

    float SplineComponent::FindDistanceClosestTo(const EngineCore::Vector3& InLocation) const
    {
        SplineSample Sample;
        float Distance = 0.0f;
        return GetWorldCurve().NearestTo(InLocation, Sample, Distance) ? Sample.Distance : 0.0f;
    }

    bool SplineComponent::IsInside(const EngineCore::Vector3& InLocation) const
    {
        return GetWorldCurve().ContainsXZ(InLocation.GetX(), InLocation.GetZ());
    }

    /* The points in use, then nothing: a load clears the rest. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SplineComponent, 1)
    {
        Ar.Flag("Closed", C.bClosed);
        Ar.Field("Type",  C.Type);
        Ar.Field("Width", C.Width);
        uint32_t Count = uint32_t(C.GetPointCount());
        Ar.Field("PointCount", Count);
        if(Count > SPLINE_POINTS_MAX) { Count = SPLINE_POINTS_MAX; }
        for(uint32_t Index = 0; Index < Count; ++Index)
        {
            Ar.Field(PointField(Index), C.Points[Index]);
        }
        if(Ar.IsLoading())
        {
            for(uint32_t Index = Count; Index < SPLINE_POINTS_MAX; ++Index) { C.Points[Index] = EngineCore::Vector3(); }
        }
    }

    /*
     * Points are entity space; the queries are world space:
     *     local s = e:GetComponent("Spline")
     *     local p = s:GetLocationAtDistance(s:GetLength() * 0.5)
     */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(SplineComponent)
    {
        Bind.Flag("Closed", &SplineComponent::bClosed);
        Bind.Field("Type",  &SplineComponent::Type);
        Bind.Field("Width", &SplineComponent::Width);
        Bind.NativePure<&SplineComponent::GetPointCount>("GetPointCount", "-> Count");
        Bind.NativePure<&SplineComponent::GetPoint>("GetPoint", "Index -> Point");
        Bind.Native<&SplineComponent::SetPoint>("SetPoint", "Index, Point -> Set");
        Bind.Native<&SplineComponent::AddPoint>("AddPoint", "Point -> Added");
        Bind.Native<&SplineComponent::InsertPoint>("InsertPoint", "Index, Point -> Inserted");
        Bind.Native<&SplineComponent::RemovePoint>("RemovePoint", "Index -> Removed");
        Bind.Native<&SplineComponent::ClearPoints>("ClearPoints", "");
        Bind.NativePure<&SplineComponent::GetLength>("GetLength", "-> Length");
        Bind.NativePure<&SplineComponent::GetLocationAtDistance>("GetLocationAtDistance", "Distance -> Location");
        Bind.NativePure<&SplineComponent::GetDirectionAtDistance>("GetDirectionAtDistance", "Distance -> Direction");
        Bind.NativePure<&SplineComponent::FindDistanceClosestTo>("FindDistanceClosestTo", "Location -> Distance");
        Bind.NativePure<&SplineComponent::IsInside>("IsInside", "Location -> Inside");
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SplineComponent)
    {
        Props.TypeCategory("World");
        Props.Choice("Type", &SplineComponent::Type, "Curve|Linear");   // ESplineType
        Props.Flag("Closed", &SplineComponent::bClosed);
        Props.Field("Width", &SplineComponent::Width);
        /* Entity space; in use up to the last one off the origin. */
        Props.Array("Points", &SplineComponent::Points);
    }
#endif // WITH_EDITOR
}
