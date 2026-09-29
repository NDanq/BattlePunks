#if WITH_EDITOR

#include "Public/Editor/TransformGizmo.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Public/RenderCore/RenderTypes.h"
#include "Public/Editor/EditorCamera.h"
#include "Public/Editor/EditorContext.h"

using EngineCore::Vector3;
using GameCore::Rotation3;
namespace TM = GameCore::TransformMath;

namespace
{
    /* Sizes in LOGICAL pixels on screen. An axis is GizmoPixels long; every
     * other length is a fraction of that (the Frame's Size in world units). */
    constexpr double GizmoPixels = 100.0;
    constexpr double HitPixels   = 8.0;

    constexpr double AxisStart      = 0.12;   // shafts leave the centre handle clear
    constexpr double TranslateTip   = 0.80;   // the fat arrow stub from here to the end
    constexpr double PlaneNear      = 0.22;   // plane squares span these fractions
    constexpr double PlaneFar       = 0.42;
    constexpr double MoveCenterHalf = 0.07;
    constexpr double ScaleCube      = 0.88;   // the scale stub from here to the end
    constexpr double ScaleCenterHalf = 0.10;
    constexpr double RingRadius     = 0.85;
    constexpr double ViewRingRadius = 1.05;
    constexpr int    RingSegments   = 64;

    /* Too edge-on to use: an axis nearly along the view, or a plane nearly
     * side-on to it. */
    constexpr double AxisAlongViewLimit = 0.985;
    constexpr double PlaneSideOnLimit   = 0.15;
    /* Below this, a rotate drag uses the ring's on-screen tangent instead of
     * the ring's plane. */
    constexpr double RingEdgeOnLimit    = 0.2;

    constexpr double MinScale = 1e-3;

    const EngineCore::Color AxisColors[3] = { EngineCore::Color(230, 70, 70),
                                              EngineCore::Color(90, 200, 80),
                                              EngineCore::Color(70, 130, 240) };
    const EngineCore::Color HighlightColor(255, 220, 40);
    const EngineCore::Color CenterColor(215, 215, 220);
    const EngineCore::Color BackRingColor(96, 100, 112);
    const EngineCore::Color TrailColor(150, 150, 160);

    using EHandle = TransformGizmo::EHandle;

    int AxisOf(const EHandle InHandle)
    {
        switch(InHandle)
        {
            case EHandle::X: return 0;
            case EHandle::Y: return 1;
            case EHandle::Z: return 2;
            default:         return -1;
        }
    }

    EHandle AxisHandle(const int InAxis)
    {
        static const EHandle Handles[3] = { EHandle::X, EHandle::Y, EHandle::Z };
        return Handles[InAxis];
    }

    /* The two in-plane axes and the normal axis of a plane handle. */
    bool PlaneAxes(const EHandle InHandle, int& OutA, int& OutB, int& OutNormal)
    {
        switch(InHandle)
        {
            case EHandle::XY: OutA = 0; OutB = 1; OutNormal = 2; return true;
            case EHandle::YZ: OutA = 1; OutB = 2; OutNormal = 0; return true;
            case EHandle::XZ: OutA = 0; OutB = 2; OutNormal = 1; return true;
            default:          return false;
        }
    }

    const EHandle PlaneHandles[3] = { EHandle::XY, EHandle::YZ, EHandle::XZ };

    double SnapTo(const double InValue, const double InStep)
    {
        return InStep > 0.0 ? std::round(InValue / InStep) * InStep : InValue;
    }

    double Component(const Vector3& InV, const int InAxis)
    {
        return InAxis == 0 ? InV.GetX() : (InAxis == 1 ? InV.GetY() : InV.GetZ());
    }

    Vector3 WithComponent(const Vector3& InV, const int InAxis, const double InValue)
    {
        return Vector3(InAxis == 0 ? InValue : InV.GetX(),
                       InAxis == 1 ? InValue : InV.GetY(),
                       InAxis == 2 ? InValue : InV.GetZ());
    }

    Vector3 Normalized(const Vector3& InV)
    {
        const double Length = InV.GetLength();
        return Length > 0.0 ? InV * (1.0 / Length) : InV;
    }

    /* The parameter along the line InOrigin + InAxis * t (InAxis unit) of its
     * closest approach to the ray. False when the two are nearly parallel. */
    bool ClosestOnAxis(const Vector3& InRayOrigin, const Vector3& InRayDir,
                       const Vector3& InOrigin, const Vector3& InAxis, double& OutT)
    {
        const Vector3 W = InOrigin - InRayOrigin;
        const double  B = InAxis.Dot(InRayDir);
        const double  Denom = 1.0 - B * B;
        if(Denom < 1e-6)
        {
            return false;
        }
        OutT = (B * InRayDir.Dot(W) - InAxis.Dot(W)) / Denom;
        return true;
    }

    /* Where the ray meets the plane through InOrigin with normal InNormal, in
     * front of the eye. */
    bool RayPlane(const Vector3& InRayOrigin, const Vector3& InRayDir,
                  const Vector3& InOrigin, const Vector3& InNormal, Vector3& OutHit)
    {
        const double Denom = InRayDir.Dot(InNormal);
        if(std::abs(Denom) < 1e-6)
        {
            return false;
        }
        const double S = (InOrigin - InRayOrigin).Dot(InNormal) / Denom;
        if(S <= 0.0)
        {
            return false;
        }
        OutHit = InRayOrigin + InRayDir * S;
        return true;
    }

    double PointSegmentDistance(const double InPx, const double InPy,
                                const double InAx, const double InAy, const double InBx, const double InBy,
                                double* OutParam = nullptr)
    {
        const double Dx = InBx - InAx, Dy = InBy - InAy;
        const double LengthSq = Dx * Dx + Dy * Dy;
        double T = LengthSq > 0.0 ? ((InPx - InAx) * Dx + (InPy - InAy) * Dy) / LengthSq : 0.0;
        T = std::clamp(T, 0.0, 1.0);
        if(OutParam)
        {
            *OutParam = T;
        }
        return std::hypot(InPx - (InAx + Dx * T), InPy - (InAy + Dy * T));
    }

    void Line(EditorContext& InContext, const Vector3& InA, const Vector3& InB,
              const EngineCore::Color InColor, const float InThickness, const float InPixelRatio)
    {
        RenderCore::DebugLine L;
        L.Start           = InA;
        L.End             = InB;
        L.Color           = InColor;
        L.ThicknessPixels = InThickness * InPixelRatio;
        InContext.DrawDebugLine(L);
    }

    void Quad(EditorContext& InContext, const Vector3 InCorners[4],
              const EngineCore::Color InColor, const float InThickness, const float InPixelRatio)
    {
        for(int Corner = 0; Corner < 4; ++Corner)
        {
            Line(InContext, InCorners[Corner], InCorners[(Corner + 1) % 4], InColor, InThickness, InPixelRatio);
        }
    }

    QString Signed(const double InValue, const int InDecimals)
    {
        return (InValue >= 0.0 ? QStringLiteral("+") : QString()) + QString::number(InValue, 'f', InDecimals);
    }
}

// === View ====================================================================

void TransformGizmo::SetView(const EditorCamera& InCamera, const EngineCore::Size InLogicalViewport, const float InDevicePixelRatio)
{
    Camera     = &InCamera;
    Viewport   = InLogicalViewport;
    PixelRatio = InDevicePixelRatio;
}

TransformGizmo::Frame TransformGizmo::MakeFrame(const Pose& InPose) const
{
    Frame F;
    if(!Camera || Mode == EMode::None)
    {
        return F;
    }

    F.Origin = InPose.Position;
    const Vector3 Offset = F.Origin - Camera->Position;
    const double  Depth  = Offset.Dot(Camera->GetForward());
    const double  Distance = Offset.GetLength();
    if(Depth <= 2.0 * double(Camera->ZNear) || Distance <= 0.0)
    {
        return F;   // behind the eye, or on it
    }

    F.Size     = GizmoPixels * Camera->GetWorldPerPixel(Depth, Viewport.Height);
    F.ToCamera = Offset * (-1.0 / Distance);

    const bool bLocal = Mode == EMode::Scale || Space == ESpace::Local;
    const Rotation3 R = bLocal ? TM::EulerToRotation(InPose.Rotation) : Rotation3();
    for(int Axis = 0; Axis < 3; ++Axis)
    {
        F.Axes[Axis] = Normalized(R.GetAxis(Axis));
    }
    F.bValid = true;
    return F;
}

bool TransformGizmo::Project(const Vector3& InPoint, double& OutX, double& OutY) const
{
    return Camera && Camera->WorldToScreen(InPoint, Viewport, OutX, OutY);
}

void TransformGizmo::Ray(const double InX, const double InY, Vector3& OutOrigin, Vector3& OutDir) const
{
    Camera->ScreenRay(InX, InY, Viewport, OutOrigin, OutDir);
}

void TransformGizmo::RingBasis(const Frame& InFrame, const int InRing, Vector3& OutNormal,
                               Vector3& OutU, Vector3& OutV, double& OutRadius) const
{
    if(InRing < 3)
    {
        OutNormal = InFrame.Axes[InRing];
        OutU      = InFrame.Axes[(InRing + 1) % 3];
        OutV      = InFrame.Axes[(InRing + 2) % 3];
        OutRadius = RingRadius * InFrame.Size;
        return;
    }

    /* Facing the eye: about the line of sight through the origin. */
    OutNormal = InFrame.ToCamera;
    OutU      = Normalized(Camera->GetRight() - OutNormal * Camera->GetRight().Dot(OutNormal));
    OutV      = OutNormal ^ OutU;
    OutRadius = ViewRingRadius * InFrame.Size;
}

bool TransformGizmo::IsAxisUsable(const Frame& InFrame, const int InAxis) const
{
    return std::abs(InFrame.Axes[InAxis].Dot(InFrame.ToCamera)) < AxisAlongViewLimit;
}

bool TransformGizmo::IsPlaneUsable(const Frame& InFrame, const int InNormalAxis) const
{
    return std::abs(InFrame.Axes[InNormalAxis].Dot(InFrame.ToCamera)) > PlaneSideOnLimit;
}

double TransformGizmo::ScreenDistance(const Vector3& InA, const Vector3& InB, const double InX, const double InY) const
{
    double Ax, Ay, Bx, By;
    if(!Project(InA, Ax, Ay) || !Project(InB, Bx, By))
    {
        return std::numeric_limits<double>::max();
    }
    return PointSegmentDistance(InX, InY, Ax, Ay, Bx, By);
}

bool TransformGizmo::NearestRingPoint(const Frame& InFrame, const int InRing, const double InX, const double InY,
                                      Vector3& OutPoint, double& OutPixels) const
{
    Vector3 Normal, U, V;
    double Radius = 0.0;
    RingBasis(InFrame, InRing, Normal, U, V, Radius);

    OutPixels = HitPixels;
    bool bFound = false;
    Vector3 Previous = InFrame.Origin + U * Radius;
    for(int Segment = 1; Segment <= RingSegments; ++Segment)
    {
        const double  Angle = 2.0 * TM::Pi * double(Segment) / double(RingSegments);
        const Vector3 Point = InFrame.Origin + (U * std::cos(Angle) + V * std::sin(Angle)) * Radius;
        const Vector3 Mid   = (Previous + Point) * 0.5;

        /* The far half is behind the gizmo; only the near half takes clicks. */
        const bool bFront = InRing == 3 || (Mid - InFrame.Origin).Dot(InFrame.ToCamera) >= -0.02 * Radius;
        double Ax, Ay, Bx, By;
        if(bFront && Project(Previous, Ax, Ay) && Project(Point, Bx, By))
        {
            double Param = 0.0;
            const double Distance = PointSegmentDistance(InX, InY, Ax, Ay, Bx, By, &Param);
            if(Distance < OutPixels)
            {
                OutPixels = Distance;
                OutPoint  = Previous + (Point - Previous) * Param;
                bFound    = true;
            }
        }
        Previous = Point;
    }
    return bFound;
}

// === Drawing =================================================================

void TransformGizmo::Queue(EditorContext& InContext, const Pose& InPose, const EHandle InHover) const
{
    const Frame F = MakeFrame(InPose);
    if(!F.bValid)
    {
        return;
    }
    const EHandle Highlight = IsDragging() ? Active : InHover;

    switch(Mode)
    {
        case EMode::Translate: QueueTranslate(InContext, F, Highlight); break;
        case EMode::Rotate:    QueueRotate(InContext, F, Highlight);    break;
        case EMode::Scale:     QueueScale(InContext, F, Highlight);     break;
        case EMode::None:      break;
    }
}

void TransformGizmo::QueueTranslate(EditorContext& InContext, const Frame& InFrame, const EHandle InHighlight) const
{
    const Vector3& O = InFrame.Origin;
    const double   S = InFrame.Size;

    /* Where the drag started, so the distance moved reads at a glance. */
    if(IsDragging())
    {
        Line(InContext, Start.Position, O, TrailColor, 1.0f, PixelRatio);
    }

    for(int Axis = 0; Axis < 3; ++Axis)
    {
        if(!IsAxisUsable(InFrame, Axis))
        {
            continue;
        }
        const Vector3 Dir = InFrame.Axes[Axis];
        const EngineCore::Color Color = InHighlight == AxisHandle(Axis) ? HighlightColor : AxisColors[Axis];
        Line(InContext, O + Dir * (AxisStart * S), O + Dir * S, Color, 2.5f, PixelRatio);
        Line(InContext, O + Dir * (TranslateTip * S), O + Dir * S, Color, 7.0f, PixelRatio);
    }

    for(const EHandle Plane : PlaneHandles)
    {
        int A, B, Normal;
        PlaneAxes(Plane, A, B, Normal);
        if(!IsPlaneUsable(InFrame, Normal))
        {
            continue;
        }
        const Vector3 Ea = InFrame.Axes[A] * S, Eb = InFrame.Axes[B] * S;
        const Vector3 Corners[4] = { O + Ea * PlaneNear + Eb * PlaneNear, O + Ea * PlaneFar + Eb * PlaneNear,
                                     O + Ea * PlaneFar  + Eb * PlaneFar,  O + Ea * PlaneNear + Eb * PlaneFar };
        Quad(InContext, Corners, InHighlight == Plane ? HighlightColor : AxisColors[Normal], 2.0f, PixelRatio);
    }

    const Vector3 R = Camera->GetRight() * (MoveCenterHalf * S), U = Camera->GetUp() * (MoveCenterHalf * S);
    const Vector3 Corners[4] = { O - R - U, O + R - U, O + R + U, O - R + U };
    Quad(InContext, Corners, InHighlight == EHandle::Center ? HighlightColor : CenterColor, 2.0f, PixelRatio);
}

void TransformGizmo::QueueRotate(EditorContext& InContext, const Frame& InFrame, const EHandle InHighlight) const
{
    const Vector3& O = InFrame.Origin;

    for(int Ring = 0; Ring < 4; ++Ring)
    {
        const EHandle Handle = Ring < 3 ? AxisHandle(Ring) : EHandle::Center;

        /* While dragging, only the ring in use -- the others would only
         * clutter the spokes. It is drawn where the drag began: the ring
         * about the rotation axis does not move as it turns. */
        if(IsDragging() && Handle != Active)
        {
            continue;
        }
        const Frame& Basis = IsDragging() ? StartFrame : InFrame;

        Vector3 Normal, U, V;
        double Radius = 0.0;
        RingBasis(Basis, Ring, Normal, U, V, Radius);

        const bool bLit = InHighlight == Handle;
        const EngineCore::Color FrontColor = bLit ? HighlightColor : (Ring < 3 ? AxisColors[Ring] : CenterColor);

        Vector3 Previous = O + U * Radius;
        for(int Segment = 1; Segment <= RingSegments; ++Segment)
        {
            const double  Angle = 2.0 * TM::Pi * double(Segment) / double(RingSegments);
            const Vector3 Point = O + (U * std::cos(Angle) + V * std::sin(Angle)) * Radius;
            const Vector3 Mid   = (Previous + Point) * 0.5;
            const bool bFront = Ring == 3 || (Mid - O).Dot(InFrame.ToCamera) >= -0.02 * Radius;

            if(bFront)
            {
                Line(InContext, Previous, Point, FrontColor, Ring < 3 ? 2.5f : 1.5f, PixelRatio);
            }
            else if(!IsDragging())
            {
                Line(InContext, Previous, Point, BackRingColor, 1.0f, PixelRatio);
            }
            Previous = Point;
        }

        if(IsDragging())
        {
            /* Spokes from the centre to where the drag began and to where it is. */
            const Vector3 Turned = TM::AxisAngle(RotateAxis, ShownDegrees) * RotateStartDir;
            Line(InContext, O, O + RotateStartDir * Radius, TrailColor, 1.5f, PixelRatio);
            Line(InContext, O, O + Turned * Radius, HighlightColor, 2.5f, PixelRatio);
        }
    }
}

void TransformGizmo::QueueScale(EditorContext& InContext, const Frame& InFrame, const EHandle InHighlight) const
{
    const Vector3& O = InFrame.Origin;
    const double   S = InFrame.Size;

    for(int Axis = 0; Axis < 3; ++Axis)
    {
        if(!IsAxisUsable(InFrame, Axis))
        {
            continue;
        }
        const Vector3 Dir = InFrame.Axes[Axis];
        const bool bLit = InHighlight == AxisHandle(Axis) || InHighlight == EHandle::Center;
        const EngineCore::Color Color = bLit ? HighlightColor : AxisColors[Axis];
        Line(InContext, O + Dir * (AxisStart * S), O + Dir * (ScaleCube * S), Color, 2.5f, PixelRatio);
        Line(InContext, O + Dir * (ScaleCube * S), O + Dir * S, Color, 10.0f, PixelRatio);
    }

    const Vector3 R = Camera->GetRight() * (ScaleCenterHalf * S), U = Camera->GetUp() * (ScaleCenterHalf * S);
    const Vector3 Corners[4] = { O - R - U, O + R - U, O + R + U, O - R + U };
    Quad(InContext, Corners, InHighlight == EHandle::Center ? HighlightColor : CenterColor, 2.5f, PixelRatio);
}

// === Hit testing =============================================================

TransformGizmo::EHandle TransformGizmo::Hit(const Pose& InPose, const double InX, const double InY) const
{
    const Frame F = MakeFrame(InPose);
    if(!F.bValid)
    {
        return EHandle::None;
    }
    switch(Mode)
    {
        case EMode::Translate: return HitTranslate(F, InX, InY);
        case EMode::Rotate:    return HitRotate(F, InX, InY);
        case EMode::Scale:     return HitScale(F, InX, InY);
        case EMode::None:      break;
    }
    return EHandle::None;
}

TransformGizmo::EHandle TransformGizmo::HitTranslate(const Frame& InFrame, const double InX, const double InY) const
{
    const Vector3& O = InFrame.Origin;
    const double   S = InFrame.Size;

    /* Smallest target first: the centre square sits where the axes meet. */
    double Cx, Cy;
    if(Project(O, Cx, Cy))
    {
        const double Half = MoveCenterHalf * GizmoPixels + 3.0;
        if(std::abs(InX - Cx) <= Half && std::abs(InY - Cy) <= Half)
        {
            return EHandle::Center;
        }
    }

    for(const EHandle Plane : PlaneHandles)
    {
        int A, B, Normal;
        PlaneAxes(Plane, A, B, Normal);
        if(!IsPlaneUsable(InFrame, Normal))
        {
            continue;
        }
        const Vector3 Ea = InFrame.Axes[A] * S, Eb = InFrame.Axes[B] * S;
        const Vector3 Corners[4] = { O + Ea * PlaneNear + Eb * PlaneNear, O + Ea * PlaneFar + Eb * PlaneNear,
                                     O + Ea * PlaneFar  + Eb * PlaneFar,  O + Ea * PlaneNear + Eb * PlaneFar };
        double Px[4], Py[4];
        bool bOnScreen = true;
        for(int Corner = 0; Corner < 4 && bOnScreen; ++Corner)
        {
            bOnScreen = Project(Corners[Corner], Px[Corner], Py[Corner]);
        }
        if(!bOnScreen)
        {
            continue;
        }
        /* Inside the projected (convex) quad: the same side of every edge. */
        int Positive = 0, Negative = 0;
        for(int Corner = 0; Corner < 4; ++Corner)
        {
            const int    Next  = (Corner + 1) % 4;
            const double Cross = (Px[Next] - Px[Corner]) * (InY - Py[Corner]) - (Py[Next] - Py[Corner]) * (InX - Px[Corner]);
            (Cross >= 0.0 ? Positive : Negative) += 1;
        }
        if(Positive == 4 || Negative == 4)
        {
            return Plane;
        }
    }

    EHandle Best = EHandle::None;
    double  BestDistance = HitPixels;
    for(int Axis = 0; Axis < 3; ++Axis)
    {
        if(!IsAxisUsable(InFrame, Axis))
        {
            continue;
        }
        const double Distance = ScreenDistance(O + InFrame.Axes[Axis] * (AxisStart * S), O + InFrame.Axes[Axis] * S, InX, InY);
        if(Distance < BestDistance)
        {
            BestDistance = Distance;
            Best         = AxisHandle(Axis);
        }
    }
    return Best;
}

TransformGizmo::EHandle TransformGizmo::HitRotate(const Frame& InFrame, const double InX, const double InY) const
{
    /* Edge-on rings draw as lines across the face-on one; where they cross,
     * the ring facing the eye wins -- it is the one being aimed at. */
    constexpr double TiePixels = 2.0;

    EHandle Best = EHandle::None;
    double  BestDistance = HitPixels;
    double  BestFacing   = -1.0;
    for(int Ring = 0; Ring < 4; ++Ring)
    {
        Vector3 Point;
        double  Distance = 0.0;
        if(!NearestRingPoint(InFrame, Ring, InX, InY, Point, Distance))
        {
            continue;
        }
        Vector3 Normal, U, V;
        double  Radius = 0.0;
        RingBasis(InFrame, Ring, Normal, U, V, Radius);
        const double Facing = std::abs(Normal.Dot(InFrame.ToCamera));

        const bool bCloser = Distance < BestDistance - TiePixels;
        const bool bTied   = std::abs(Distance - BestDistance) <= TiePixels && Facing > BestFacing;
        if(Best == EHandle::None ? Distance < BestDistance : (bCloser || bTied))
        {
            BestDistance = Distance;
            BestFacing   = Facing;
            Best         = Ring < 3 ? AxisHandle(Ring) : EHandle::Center;
        }
    }
    return Best;
}

TransformGizmo::EHandle TransformGizmo::HitScale(const Frame& InFrame, const double InX, const double InY) const
{
    const Vector3& O = InFrame.Origin;

    double Cx, Cy;
    if(Project(O, Cx, Cy))
    {
        const double Half = ScaleCenterHalf * GizmoPixels + 3.0;
        if(std::abs(InX - Cx) <= Half && std::abs(InY - Cy) <= Half)
        {
            return EHandle::Center;
        }
    }

    EHandle Best = EHandle::None;
    double  BestDistance = HitPixels;
    for(int Axis = 0; Axis < 3; ++Axis)
    {
        if(!IsAxisUsable(InFrame, Axis))
        {
            continue;
        }
        const double Distance = ScreenDistance(O + InFrame.Axes[Axis] * (AxisStart * InFrame.Size),
                                               O + InFrame.Axes[Axis] * InFrame.Size, InX, InY);
        if(Distance < BestDistance)
        {
            BestDistance = Distance;
            Best         = AxisHandle(Axis);
        }
    }
    return Best;
}

// === Dragging ================================================================

bool TransformGizmo::BeginDrag(const EHandle InHandle, const Pose& InPose, const double InX, const double InY)
{
    Active = EHandle::None;
    const Frame F = MakeFrame(InPose);
    if(!F.bValid || InHandle == EHandle::None)
    {
        return false;
    }

    Vector3 RayOrigin, RayDir;
    Ray(InX, InY, RayOrigin, RayDir);

    const int Axis = AxisOf(InHandle);
    int A = 0, B = 1, Normal = 2;

    switch(Mode)
    {
        case EMode::Translate:
            if(Axis >= 0)
            {
                if(!ClosestOnAxis(RayOrigin, RayDir, F.Origin, F.Axes[Axis], StartParam))
                {
                    return false;
                }
            }
            else if(PlaneAxes(InHandle, A, B, Normal))
            {
                if(!RayPlane(RayOrigin, RayDir, F.Origin, F.Axes[Normal], StartHit))
                {
                    return false;
                }
            }
            else if(!RayPlane(RayOrigin, RayDir, F.Origin, Camera->GetForward(), StartHit))
            {
                return false;
            }
            break;

        case EMode::Rotate:
        {
            const int Ring = Axis >= 0 ? Axis : 3;
            Vector3 U, V;
            double Radius = 0.0;
            RingBasis(F, Ring, RotateAxis, U, V, Radius);

            Vector3 Grab;
            double  Pixels = 0.0;
            if(!NearestRingPoint(F, Ring, InX, InY, Grab, Pixels))
            {
                return false;
            }
            RotateStartDir = Normalized(Grab - F.Origin);
            RingPixels     = std::max(1.0, Radius * GizmoPixels / F.Size);
            RotateDegrees  = 0.0;
            ShownDegrees   = 0.0;
            LastPlaneAngle = 0.0;

            bRotateAlongTangent = std::abs(RayDir.Dot(RotateAxis)) < RingEdgeOnLimit
                               || !RayPlane(RayOrigin, RayDir, F.Origin, RotateAxis, StartHit)
                               || (StartHit - F.Origin).GetLength() < 1e-6 * F.Size;
            if(bRotateAlongTangent)
            {
                /* Along the ring's on-screen tangent at its point nearest the
                 * eye: the near half is what is drawn and grabbed, and it
                 * moves that way as the ring turns. The grabbed point's own
                 * tangent would be useless at the ends of an edge-on ring,
                 * where it points straight at the eye. Positive turns move a
                 * ring point along Axis x radius. */
                Vector3 Nearest = F.ToCamera - RotateAxis * F.ToCamera.Dot(RotateAxis);
                Nearest = Nearest.GetLength() > 1e-3 ? Normalized(Nearest) : RotateStartDir;
                const Vector3 Anchor  = F.Origin + Nearest * Radius;
                const Vector3 Tangent = Normalized(RotateAxis ^ Nearest);
                double Gx, Gy, Tx, Ty;
                if(!Project(Anchor, Gx, Gy) || !Project(Anchor + Tangent * (0.1 * Radius), Tx, Ty))
                {
                    return false;
                }
                const double Length = std::hypot(Tx - Gx, Ty - Gy);
                if(Length < 1e-6)
                {
                    return false;
                }
                TangentX = (Tx - Gx) / Length;
                TangentY = (Ty - Gy) / Length;
            }
            break;
        }

        case EMode::Scale:
            if(Axis >= 0)
            {
                if(!ClosestOnAxis(RayOrigin, RayDir, F.Origin, F.Axes[Axis], StartParam))
                {
                    return false;
                }
                /* Grabbed right at the centre, a ratio of distances would jump. */
                if(std::abs(StartParam) < 0.05 * F.Size)
                {
                    StartParam = F.Size;
                }
            }
            break;

        case EMode::None:
            return false;
    }

    Start      = InPose;
    Current    = InPose;
    StartFrame = F;
    StartX     = InX;
    StartY     = InY;
    Active     = InHandle;
    return true;
}

TransformGizmo::Pose TransformGizmo::UpdateDrag(const double InX, const double InY, const Snap& InSnap)
{
    if(!IsDragging() || !Camera)
    {
        return Current;
    }
    switch(Mode)
    {
        case EMode::Translate: Current = UpdateTranslate(InX, InY, InSnap); break;
        case EMode::Rotate:    Current = UpdateRotate(InX, InY, InSnap);    break;
        case EMode::Scale:     Current = UpdateScale(InX, InY, InSnap);     break;
        case EMode::None:      break;
    }
    return Current;
}

TransformGizmo::Pose TransformGizmo::UpdateTranslate(const double InX, const double InY, const Snap& InSnap)
{
    Vector3 RayOrigin, RayDir;
    Ray(InX, InY, RayOrigin, RayDir);

    const Frame& F     = StartFrame;
    const bool   bGrid = InSnap.bEnabled && InSnap.Translate > 0.0;
    const bool   bWorldGrid = bGrid && Space == ESpace::World;
    Pose Out = Start;

    const int Axis = AxisOf(Active);
    int A = 0, B = 1, Normal = 2;
    if(Axis >= 0)
    {
        double T = 0.0;
        if(!ClosestOnAxis(RayOrigin, RayDir, F.Origin, F.Axes[Axis], T))
        {
            return Current;
        }
        double Distance = T - StartParam;
        if(bGrid && !bWorldGrid)
        {
            Distance = SnapTo(Distance, InSnap.Translate);
        }
        Out.Position = Start.Position + F.Axes[Axis] * Distance;
        if(bWorldGrid)
        {
            Out.Position = WithComponent(Out.Position, Axis, SnapTo(Component(Out.Position, Axis), InSnap.Translate));
        }
        return Out;
    }

    const Vector3 PlaneNormal = PlaneAxes(Active, A, B, Normal) ? F.Axes[Normal] : Camera->GetForward();
    Vector3 Hit;
    if(!RayPlane(RayOrigin, RayDir, F.Origin, PlaneNormal, Hit))
    {
        return Current;
    }
    Vector3 Delta = Hit - StartHit;

    if(bGrid && !bWorldGrid && Active != EHandle::Center)
    {
        Delta = F.Axes[A] * SnapTo(Delta.Dot(F.Axes[A]), InSnap.Translate)
              + F.Axes[B] * SnapTo(Delta.Dot(F.Axes[B]), InSnap.Translate);
    }
    Out.Position = Start.Position + Delta;

    if(bGrid && (bWorldGrid || Active == EHandle::Center))
    {
        /* Only the coordinates the drag moved: the screen plane often leaves
         * depth alone, and snapping it would pop the entity off its plane. */
        for(int C = 0; C < 3; ++C)
        {
            if(std::abs(Component(Delta, C)) > 1e-6 * F.Size)
            {
                Out.Position = WithComponent(Out.Position, C, SnapTo(Component(Out.Position, C), InSnap.Translate));
            }
        }
    }
    return Out;
}

TransformGizmo::Pose TransformGizmo::UpdateRotate(const double InX, const double InY, const Snap& InSnap)
{
    const Vector3& O = StartFrame.Origin;

    if(bRotateAlongTangent)
    {
        const double Along = (InX - StartX) * TangentX + (InY - StartY) * TangentY;
        RotateDegrees = TM::ToDegrees(Along / RingPixels);
    }
    else
    {
        Vector3 RayOrigin, RayDir;
        Ray(InX, InY, RayOrigin, RayDir);
        Vector3 Hit;
        if(RayPlane(RayOrigin, RayDir, O, RotateAxis, Hit))
        {
            const Vector3 From = StartHit - O;
            const Vector3 To   = Hit - O;
            if(To.GetLength() > 1e-6 * StartFrame.Size)
            {
                /* Accumulated turn by turn, so dragging round more than once
                 * keeps counting instead of wrapping at 180. */
                const double Angle = TM::ToDegrees(std::atan2((From ^ To).Dot(RotateAxis), From.Dot(To)));
                double Step = Angle - LastPlaneAngle;
                Step -= 360.0 * std::round(Step / 360.0);
                RotateDegrees += Step;
                LastPlaneAngle = Angle;
            }
        }
    }

    ShownDegrees = InSnap.bEnabled ? SnapTo(RotateDegrees, InSnap.RotateDegrees) : RotateDegrees;

    /* About a world-space axis, onto the starting rotation. In Local space
     * that axis is one of the entity's own, which is the same turn. */
    Pose Out = Start;
    const Rotation3 Turned = TM::AxisAngle(RotateAxis, ShownDegrees) * TM::EulerToRotation(Start.Rotation);
    Out.Rotation = TM::RotationToEuler(Turned, Current.Rotation);
    return Out;
}

TransformGizmo::Pose TransformGizmo::UpdateScale(const double InX, const double InY, const Snap& InSnap)
{
    Pose Out = Start;
    const bool bSnap = InSnap.bEnabled && InSnap.Scale > 0.0;

    const auto Clamped = [](const double InValue, const double InStartValue)
    {
        return std::abs(InValue) >= MinScale ? InValue : (InStartValue < 0.0 ? -MinScale : MinScale);
    };

    const int Axis = AxisOf(Active);
    if(Axis >= 0)
    {
        Vector3 RayOrigin, RayDir;
        Ray(InX, InY, RayOrigin, RayDir);
        double T = 0.0;
        if(!ClosestOnAxis(RayOrigin, RayDir, StartFrame.Origin, StartFrame.Axes[Axis], T))
        {
            return Current;
        }
        const double StartValue = Component(Start.Scale, Axis);
        double Value = StartValue * (T / StartParam);
        if(bSnap)
        {
            Value = SnapTo(Value, InSnap.Scale);
        }
        Out.Scale = WithComponent(Start.Scale, Axis, Clamped(Value, StartValue));
        return Out;
    }

    /* Uniform: right or up grows, exponentially so it feels the same at any size. */
    const double Pixels = (InX - StartX) - (InY - StartY);
    double Factor = std::exp(Pixels * 0.01);
    if(bSnap)
    {
        Factor = std::max(InSnap.Scale, SnapTo(Factor, InSnap.Scale));
    }
    for(int C = 0; C < 3; ++C)
    {
        const double StartValue = Component(Start.Scale, C);
        Out.Scale = WithComponent(Out.Scale, C, Clamped(StartValue * Factor, StartValue));
    }
    return Out;
}

QString TransformGizmo::DescribeDrag() const
{
    if(!IsDragging())
    {
        return QString();
    }

    switch(Mode)
    {
        case EMode::Translate:
        {
            const Vector3 Delta = Current.Position - Start.Position;
            return QStringLiteral("Move   X %1   Y %2   Z %3")
                   .arg(Signed(Delta.GetX(), 1), Signed(Delta.GetY(), 1), Signed(Delta.GetZ(), 1));
        }
        case EMode::Rotate:
        {
            static const char* const Axes[4] = { "X", "Y", "Z", "view" };
            const int Axis  = AxisOf(Active);
            const bool bView = Axis < 0;
            return QStringLiteral("Rotate   %1%2 about %3%4")
                   .arg(Signed(ShownDegrees, 1), QString(QChar(0x00B0)), QString::fromLatin1(Axes[bView ? 3 : Axis]),
                        bView ? QString() : (Space == ESpace::Local ? QStringLiteral(" (local)") : QStringLiteral(" (world)")));
        }
        case EMode::Scale:
            return QStringLiteral("Scale   X %1   Y %2   Z %3")
                   .arg(QString::number(Current.Scale.GetX(), 'f', 3),
                        QString::number(Current.Scale.GetY(), 'f', 3),
                        QString::number(Current.Scale.GetZ(), 'f', 3));
        case EMode::None:
            break;
    }
    return QString();
}

#endif // WITH_EDITOR
