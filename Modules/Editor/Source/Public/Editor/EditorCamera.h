#ifndef NIMBLEEDITOR_EDITORCAMERA_H
#define NIMBLEEDITOR_EDITORCAMERA_H

#if WITH_EDITOR

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/Math/Vector.h"
#include "Public/RenderCore/RenderView.h"

/*
 * The viewport's camera: a position plus yaw and pitch, turned into a
 * RenderView::MakePerspective every frame. Owned by the viewport, never an
 * entity -- looking around must not edit the World.
 *
 * AXES follow RenderView's conventions, i.e. the game's: +X right, +Y DOWN the
 * screen, +Z into it. Yaw 0 / pitch 0 looks down +Z at the XY plane with +X to
 * the right, which is exactly the orthographic game view, in perspective.
 * Positive pitch looks toward screen-up, which is world -Y.
 *
 * FOCUS. FocusDistance is how far ahead the camera's point of interest sits
 * when nothing more specific is known -- the pivot an orbit or a view preset
 * turns about. Framing and orbiting keep it current.
 */
class EditorCamera
{
public:
    /* The snap views on the viewport toolbar and axis gizmo. Named for where
     * the camera looks FROM: Top looks from -Y (screen-up) toward +Y. */
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

    /* Turns about InPivot, keeping the distance to it; FocusDistance becomes
     * that distance. */
    void Orbit(const EngineCore::Vector3& InPivot, float InYawDegrees, float InPitchDegrees);

    /* Moves along the view toward InPivot (positive) or away, never closer
     * than a few units. */
    void Dolly(const EngineCore::Vector3& InPivot, double InDistance);

    /* Keeps the orientation, backs off until InBox fits the view. */
    void Frame(const GameCore::AABB& InBox);

    /* Snaps the orientation to InPreset and orbits there about InPivot. */
    void ApplyPreset(EPreset InPreset, const EngineCore::Vector3& InPivot);

    RenderCore::RenderView BuildView(EngineCore::Size InViewportPixels) const;

    /* World units covered by one pixel at InDepth along the view, for a
     * viewport InViewportHeight pixels tall (logical or physical, as the
     * caller measures). */
    double GetWorldPerPixel(double InDepth, int InViewportHeight) const;

    /* The world-space ray through a pixel (logical or physical, as long as
     * InViewport uses the same units). OutDirection is normalised. */
    void ScreenRay(double InX, double InY, EngineCore::Size InViewport,
                   EngineCore::Vector3& OutOrigin, EngineCore::Vector3& OutDirection) const;

    /* The pixel InPoint projects to, in InViewport's units. False if it is
     * behind the near plane. */
    bool WorldToScreen(const EngineCore::Vector3& InPoint, EngineCore::Size InViewport,
                       double& OutX, double& OutY) const;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITORCAMERA_H
