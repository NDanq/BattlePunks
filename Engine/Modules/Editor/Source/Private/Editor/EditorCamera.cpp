#if WITH_EDITOR

#include "Public/Editor/EditorCamera.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float DegToRad = 3.14159265f / 180.0f;

    /* Just short of straight along world Y, where LookAt's (0,1,0) up
     * reference would be parallel to the view and the basis would vanish. */
    constexpr float MaxPitch = 89.0f;

    /* Closest a dolly may bring the camera to its pivot. */
    constexpr double MinFocusDistance = 5.0;

    /* LookAt's basis (Matrix.h): R = Up ^ D, U = D ^ R, Up = (0,1,0). */
    EngineCore::Vector3 ViewRight(const EngineCore::Vector3& InForward)
    {
        return (EngineCore::Vector3(0.0, 1.0, 0.0) ^ InForward).GetNormalize();
    }
}

EngineCore::Vector3 EditorCamera::GetForward() const
{
    const float Y = Yaw * DegToRad;
    const float P = Pitch * DegToRad;
    return EngineCore::Vector3(std::sin(Y) * std::cos(P), std::sin(P), std::cos(Y) * std::cos(P));
}

EngineCore::Vector3 EditorCamera::GetRight() const
{
    return ViewRight(GetForward());
}

EngineCore::Vector3 EditorCamera::GetUp() const
{
    /* The view's U axis: the projection keeps it (world +Y runs up the screen). */
    const EngineCore::Vector3 Forward = GetForward();
    return Forward ^ ViewRight(Forward);
}

EngineCore::Vector3 EditorCamera::GetFocusPoint() const
{
    return Position + GetForward() * double(FocusDistance);
}

void EditorCamera::AddYawPitch(const float InYawDegrees, const float InPitchDegrees)
{
    Yaw   = std::fmod(Yaw + InYawDegrees, 360.0f);
    Pitch = std::clamp(Pitch + InPitchDegrees, -MaxPitch, MaxPitch);
}

void EditorCamera::Orbit(const EngineCore::Vector3& InPivot, const float InYawDegrees, const float InPitchDegrees)
{
    const double Distance = std::max((InPivot - Position).GetLength(), MinFocusDistance);
    AddYawPitch(InYawDegrees, InPitchDegrees);
    Position      = InPivot - GetForward() * Distance;
    FocusDistance = float(Distance);
}

void EditorCamera::Dolly(const EngineCore::Vector3& InPivot, const double InDistance)
{
    const double Current = std::max((InPivot - Position).GetLength(), MinFocusDistance);
    const double Target  = std::max(Current - InDistance, MinFocusDistance);
    Position      = Position + GetForward() * (Current - Target);
    FocusDistance = float(Target);
}

void EditorCamera::Frame(const GameCore::AABB& InBox)
{
    if(InBox.IsEmpty())
    {
        return;
    }

    const EngineCore::Vector3 Center = (InBox.Min + InBox.Max) * 0.5;
    const double Radius = std::max((InBox.Max - InBox.Min).GetLength() * 0.5, 25.0);

    /* A sphere of that radius fits the narrower half-angle; a little margin
     * so the box does not touch the edges. */
    const double HalfFov  = double(FovY) * 0.5 * double(DegToRad);
    const double Distance = 1.2 * Radius / std::sin(HalfFov);

    Position      = Center - GetForward() * Distance;
    FocusDistance = float(Distance);
}

void EditorCamera::ApplyPreset(const EPreset InPreset, const EngineCore::Vector3& InPivot)
{
    /* Forward = (sin yaw cos pitch, sin pitch, cos yaw cos pitch). */
    float NewYaw = 0.0f, NewPitch = 0.0f;
    switch(InPreset)
    {
        case EPreset::Perspective: NewYaw =  35.0f; NewPitch = -25.0f;    break;   // above and to the left, looking down (-Y)
        case EPreset::Front:       NewYaw =   0.0f; NewPitch =   0.0f;    break;   // along +Z -- the game's own view
        case EPreset::Back:        NewYaw = 180.0f; NewPitch =   0.0f;    break;
        case EPreset::Top:         NewYaw =   0.0f; NewPitch = -MaxPitch; break;   // along -Y, from above
        case EPreset::Bottom:      NewYaw =   0.0f; NewPitch =  MaxPitch; break;
        case EPreset::Right:       NewYaw = -90.0f; NewPitch =   0.0f;    break;   // along -X, from +X
        case EPreset::Left:        NewYaw =  90.0f; NewPitch =   0.0f;    break;
    }

    const double Distance = std::max((InPivot - Position).GetLength(), MinFocusDistance);
    Yaw   = NewYaw;
    Pitch = NewPitch;
    Position      = InPivot - GetForward() * Distance;
    FocusDistance = float(Distance);
}

RenderCore::RenderView EditorCamera::BuildView(const EngineCore::Size InViewportPixels) const
{
    return RenderCore::RenderView::MakePerspective(InViewportPixels, Position, GetForward(), FovY, ZNear, ZFar);
}

double EditorCamera::GetWorldPerPixel(const double InDepth, const int InViewportHeight) const
{
    const double HalfFov = double(FovY) * 0.5 * double(DegToRad);
    return 2.0 * InDepth * std::tan(HalfFov) / double(std::max(1, InViewportHeight));
}

void EditorCamera::ScreenRay(const double InX, const double InY, const EngineCore::Size InViewport,
                             EngineCore::Vector3& OutOrigin, EngineCore::Vector3& OutDirection) const
{
    const double Width  = std::max(1, InViewport.Width);
    const double Height = std::max(1, InViewport.Height);

    /* Pixel -> NDC, top of the viewport at +1 (the GL convention every GDI
     * backend renders in). */
    const double NdcX = 2.0 * InX / Width - 1.0;
    const double NdcY = 1.0 - 2.0 * InY / Height;

    /* Inverting MakePerspective's projection at view depth 1. */
    const double FocalY = 1.0 / std::tan(double(FovY) * 0.5 * double(DegToRad));
    const double FocalX = FocalY / (Width / Height);
    const double ViewX  = NdcX / FocalX;
    const double ViewY  = NdcY / FocalY;

    const EngineCore::Vector3 Forward = GetForward();
    const EngineCore::Vector3 Right   = ViewRight(Forward);
    const EngineCore::Vector3 ViewUp  = Forward ^ Right;

    OutOrigin    = Position;
    OutDirection = (Right * ViewX + ViewUp * ViewY + Forward).GetNormalize();
}

bool EditorCamera::WorldToScreen(const EngineCore::Vector3& InPoint, const EngineCore::Size InViewport,
                                 double& OutX, double& OutY) const
{
    const EngineCore::Vector3 Forward = GetForward();
    const EngineCore::Vector3 Right   = ViewRight(Forward);
    const EngineCore::Vector3 ViewUp  = Forward ^ Right;

    const EngineCore::Vector3 Offset = InPoint - Position;
    const double Depth = Offset.Dot(Forward);
    if(Depth <= double(ZNear))
    {
        return false;
    }

    /* ScreenRay, run forwards. */
    const double Width  = std::max(1, InViewport.Width);
    const double Height = std::max(1, InViewport.Height);
    const double FocalY = 1.0 / std::tan(double(FovY) * 0.5 * double(DegToRad));
    const double FocalX = FocalY / (Width / Height);

    const double NdcX = FocalX * Offset.Dot(Right) / Depth;
    const double NdcY = FocalY * Offset.Dot(ViewUp) / Depth;

    OutX = (NdcX + 1.0) * 0.5 * Width;
    OutY = (1.0 - NdcY) * 0.5 * Height;
    return true;
}

#endif // WITH_EDITOR
