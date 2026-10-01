#ifndef NIMBLEEDITOR_TRANSFORMGIZMO_H
#define NIMBLEEDITOR_TRANSFORMGIZMO_H

#if WITH_EDITOR

#include <QString>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/TransformMath.h"
#include "Public/Math/Vector.h"

class EditorCamera;
class EditorContext;

/*
 * Move / rotate / scale handles: drawing (debug lines), hit testing and drag
 * math. No Qt input or World access; the viewport feeds it and applies the
 * result.
 *
 * Handles keep a fixed screen size. World / Local space applies to move and
 * rotate; scale is always local (applied before rotation).
 *
 * Drags are computed from the BeginDrag snapshot, so they never drift and
 * can be cancelled exactly. Positions are LOGICAL pixels.
 */
class TransformGizmo
{
public:
    enum class EMode   { None, Translate, Rotate, Scale };
    enum class ESpace  { World, Local };

    /* Center: the screen-plane move, the view-axis ring, or uniform scale. */
    enum class EHandle { None, X, Y, Z, XY, YZ, XZ, Center };

    /* TransformComponent's position, rotation (degrees) and scale. */
    struct Pose
    {
        EngineCore::Vector3 Position;
        EngineCore::Vector3 Rotation;
        EngineCore::Vector3 Scale{1.0, 1.0, 1.0};
    };

    struct Snap
    {
        bool   bEnabled      = false;
        double Translate     = 10.0;    // world units; a world grid in World space
        double RotateDegrees = 15.0;
        double Scale         = 0.1;
    };

    void   SetMode(EMode InMode)    { Mode = InMode; }
    EMode  GetMode() const          { return Mode; }
    void   SetSpace(ESpace InSpace) { Space = InSpace; }
    ESpace GetSpace() const         { return Space; }

    /* Call whenever the camera or viewport may have changed. */
    void SetView(const EditorCamera& InCamera, EngineCore::Size InLogicalViewport, float InDevicePixelRatio);

    /* Queues the handles, highlighting InHover or the dragged one. */
    void Queue(EditorContext& InContext, const Pose& InPose, EHandle InHover) const;

    /* The handle under a logical pixel, or None. */
    EHandle Hit(const Pose& InPose, double InX, double InY) const;

    /* False if InHandle can't be dragged from this view. */
    bool BeginDrag(EHandle InHandle, const Pose& InPose, double InX, double InY);

    /* The dragged pose; keeps the last good one when the math degenerates. */
    Pose UpdateDrag(double InX, double InY, const Snap& InSnap);

    /* The pose BeginDrag started from -- what a cancelled drag restores. */
    const Pose& GetDragStart() const { return Start; }

    void EndDrag() { Active = EHandle::None; }
    bool IsDragging() const { return Active != EHandle::None; }

    /* For the status bar; empty when not dragging. */
    QString DescribeDrag() const;

private:
    /* Where the handles are this frame. */
    struct Frame
    {
        EngineCore::Vector3 Origin;
        EngineCore::Vector3 Axes[3];
        EngineCore::Vector3 ToCamera;       // unit, origin to eye
        double              Size = 0.0;     // world length of a full axis
        bool                bValid = false;
    };

    Frame MakeFrame(const Pose& InPose) const;

    bool Project(const EngineCore::Vector3& InPoint, double& OutX, double& OutY) const;
    void Ray(double InX, double InY, EngineCore::Vector3& OutOrigin, EngineCore::Vector3& OutDir) const;

    /* Ring InRing (0..2 axes, 3 view) as normal, in-plane basis and radius. */
    void RingBasis(const Frame& InFrame, int InRing, EngineCore::Vector3& OutNormal,
                   EngineCore::Vector3& OutU, EngineCore::Vector3& OutV, double& OutRadius) const;

    /* False when the axis is too edge-on to use. */
    bool IsAxisUsable(const Frame& InFrame, int InAxis) const;
    bool IsPlaneUsable(const Frame& InFrame, int InNormalAxis) const;

    void QueueTranslate(EditorContext& InContext, const Frame& InFrame, EHandle InHighlight) const;
    void QueueRotate(EditorContext& InContext, const Frame& InFrame, EHandle InHighlight) const;
    void QueueScale(EditorContext& InContext, const Frame& InFrame, EHandle InHighlight) const;

    EHandle HitTranslate(const Frame& InFrame, double InX, double InY) const;
    EHandle HitRotate(const Frame& InFrame, double InX, double InY) const;
    EHandle HitScale(const Frame& InFrame, double InX, double InY) const;

    /* Screen distance to segment InA-InB; large if either end is behind. */
    double ScreenDistance(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, double InX, double InY) const;

    /* Nearest front-facing ring point; false if beyond the hit distance. */
    bool NearestRingPoint(const Frame& InFrame, int InRing, double InX, double InY,
                          EngineCore::Vector3& OutPoint, double& OutPixels) const;

    Pose UpdateTranslate(double InX, double InY, const Snap& InSnap);
    Pose UpdateRotate(double InX, double InY, const Snap& InSnap);
    Pose UpdateScale(double InX, double InY, const Snap& InSnap);

    EMode  Mode  = EMode::Translate;
    ESpace Space = ESpace::World;

    const EditorCamera* Camera = nullptr;
    EngineCore::Size    Viewport;
    float               PixelRatio = 1.0f;

    // === The drag ============================================================
    EHandle             Active = EHandle::None;
    Pose                Start;
    Pose                Current;
    Frame               StartFrame;
    double              StartX = 0.0, StartY = 0.0;
    double              StartParam = 0.0;          // along the axis, for axis moves and scales
    EngineCore::Vector3 StartHit;                  // on the plane, for plane and screen moves

    /* Rotate tracks the cursor's angle on the ring plane, or for edge-on
     * rings the on-screen tangent at the grab point. */
    EngineCore::Vector3 RotateAxis;
    EngineCore::Vector3 RotateStartDir;            // unit, origin to the grabbed point
    bool                bRotateAlongTangent = false;
    double              TangentX = 0.0, TangentY = 0.0;
    double              RingPixels = 1.0;          // the ring's radius on screen
    double              LastPlaneAngle = 0.0;
    double              RotateDegrees = 0.0;       // accumulated, unsnapped
    double              ShownDegrees = 0.0;        // snapped, as applied
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_TRANSFORMGIZMO_H
