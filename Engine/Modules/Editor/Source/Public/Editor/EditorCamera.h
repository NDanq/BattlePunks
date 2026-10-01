#ifndef NIMBLEEDITOR_EDITORCAMERA_H
#define NIMBLEEDITOR_EDITORCAMERA_H

#if WITH_EDITOR

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/RenderView.h"

/*
 * The viewport's camera (position, yaw, pitch), not an entity, so looking
 * around never edits the World.
 *
 * AXES as RenderView: +X right, +Y DOWN, +Z into the screen. Yaw/pitch 0
 * matches the game's ortho view; positive pitch looks toward world -Y.
 *
 * FOCUS. FocusDistance places the default pivot for orbits and presets.
 */
class EditorCamera
{
public:
    /* Named for where the camera looks FROM: Top looks from -Y toward +Y. */
    enum class EPreset
    {
        Perspective,
        Front, Back,     // from -Z / from +Z
        Top, Bottom,     // from -Y / from +Y
        Right, Left,     // from +X / from -X
    };

    EngineCore::Vector3 Position{400.0, 300.0, -800.0};
    float Yaw   = 0.0f;   // degrees about world Y; positive turns toward +X
    float Pitch = 0.0f;   // degrees; clamped short of straight up/down
    float FovY  = 60.0f;
    float ZNear = 1.0f;
    float ZFar  = 20000.0f;

    float FocusDistance = 800.0f;
    /* Multiplies fly, pan and dolly speeds -- the toolbar's speed control. */
    float SpeedScale    = 1.0f;

    EngineCore::Vector3 GetForward() const;
    EngineCore::Vector3 GetRight() const;
    /* Screen-up, in world space (world -Y at zero pitch). */
    EngineCore::Vector3 GetUp() const;

    /* Position + Forward * FocusDistance. */
    EngineCore::Vector3 GetFocusPoint() const;

    void AddYawPitch(float InYawDegrees, float InPitchDegrees);

    /* Keeps the distance to InPivot, which becomes FocusDistance. */
    void Orbit(const EngineCore::Vector3& InPivot, float InYawDegrees, float InPitchDegrees);

    /* Toward InPivot (positive) or away; never closer than a few units. */
    void Dolly(const EngineCore::Vector3& InPivot, double InDistance);

    /* Keeps the orientation, backs off until InBox fits the view. */
    void Frame(const GameCore::AABB& InBox);

    /* Snaps the orientation to InPreset and orbits there about InPivot. */
    void ApplyPreset(EPreset InPreset, const EngineCore::Vector3& InPivot);

    RenderCore::RenderView BuildView(EngineCore::Size InViewportPixels) const;

    /* World units per pixel at InDepth (pixels in the caller's units). */
    double GetWorldPerPixel(double InDepth, int InViewportHeight) const;

    /* World ray through a pixel (same units as InViewport); normalised. */
    void ScreenRay(double InX, double InY, EngineCore::Size InViewport,
                   EngineCore::Vector3& OutOrigin, EngineCore::Vector3& OutDirection) const;

    /* False if InPoint is behind the near plane. */
    bool WorldToScreen(const EngineCore::Vector3& InPoint, EngineCore::Size InViewport,
                       double& OutX, double& OutY) const;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORCAMERA_H
