#include "Editor/EditorCamera.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float DegToRad = 3.14159265f / 180.0f;

    /* LookAt's own basis (Matrix.h): R = Up ^ D with Up = (0,1,0), U = D ^ R.
     * Restated so a ray leaves the camera through the same axes the view
     * matrix projects along. */
    EngineCore::Vector3 ViewRight(const EngineCore::Vector3& InForward)
    {
        return (EngineCore::Vector3(0.0, 1.0, 0.0) ^ InForward).GetNormalize();
    }
}

EngineCore::Vector3 EditorCamera::GetForward() const
{
    const float Y = Yaw * DegToRad;
    const float P = Pitch * DegToRad;
    return EngineCore::Vector3(std::sin(Y) * std::cos(P), -std::sin(P), std::cos(Y) * std::cos(P));
}

EngineCore::Vector3 EditorCamera::GetRight() const
{
    return ViewRight(GetForward());
}

EngineCore::Vector3 EditorCamera::GetUp() const
{
    /* The view's U axis is flipped by the projection (world +Y runs down the
     * screen), so screen-up is -U. */
    const EngineCore::Vector3 Forward = GetForward();
    return (Forward ^ ViewRight(Forward)) * -1.0;
}

void EditorCamera::AddYawPitch(const float InYawDegrees, const float InPitchDegrees)
{
    Yaw   = std::fmod(Yaw + InYawDegrees, 360.0f);
    Pitch = std::clamp(Pitch + InPitchDegrees, -89.0f, 89.0f);
}

RenderCore::RenderView EditorCamera::BuildView(const EngineCore::Size InViewportPixels) const
{
    return RenderCore::RenderView::MakePerspective(InViewportPixels, Position, GetForward(), FovY, ZNear, ZFar);
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

    /* Inverting MakePerspective's projection at view depth 1: its Y row is
     * negated, hence the sign on ViewY. */
    const double FocalY = 1.0 / std::tan(double(FovY) * 0.5 * double(DegToRad));
    const double FocalX = FocalY / (Width / Height);
    const double ViewX  = NdcX / FocalX;
    const double ViewY  = -NdcY / FocalY;

    const EngineCore::Vector3 Forward = GetForward();
    const EngineCore::Vector3 Right   = ViewRight(Forward);
    const EngineCore::Vector3 ViewUp  = Forward ^ Right;

    OutOrigin    = Position;
    OutDirection = (Right * ViewX + ViewUp * ViewY + Forward).GetNormalize();
}
