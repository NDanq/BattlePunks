#ifndef NIMBLEEDITOR_EDITORCAMERA_H
#define NIMBLEEDITOR_EDITORCAMERA_H

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/RenderView.h"

/*
 * The viewport's fly camera: a position plus yaw and pitch, turned into a
 * RenderView::MakePerspective every frame. Owned by the viewport, never an
 * entity -- looking around must not edit the World.
 *
 * AXES follow RenderView's conventions, i.e. the game's: +X right, +Y DOWN the
 * screen, +Z into it. Yaw 0 / pitch 0 looks down +Z at the XY plane with +X to
 * the right, which is exactly the orthographic game view, in perspective.
 * Positive pitch looks toward screen-up, which is world -Y.
 */
class EditorCamera
{
public:
    EngineCore::Vector3 Position{400.0, 300.0, -800.0};
    float Yaw   = 0.0f;   // degrees about world Y; positive turns toward +X
    float Pitch = 0.0f;   // degrees; clamped short of straight up/down
    float FovY  = 60.0f;
    float ZNear = 1.0f;
    float ZFar  = 20000.0f;

    EngineCore::Vector3 GetForward() const;
    EngineCore::Vector3 GetRight() const;
    /* Screen-up, in world space (world -Y at zero pitch). */
    EngineCore::Vector3 GetUp() const;

    void AddYawPitch(float InYawDegrees, float InPitchDegrees);

    RenderCore::RenderView BuildView(EngineCore::Size InViewportPixels) const;

    /* The world-space ray through a pixel (logical or physical, as long as
     * InViewport uses the same units). OutDirection is normalised. */
    void ScreenRay(double InX, double InY, EngineCore::Size InViewport,
                   EngineCore::Vector3& OutOrigin, EngineCore::Vector3& OutDirection) const;
};

#endif //NIMBLEEDITOR_EDITORCAMERA_H
