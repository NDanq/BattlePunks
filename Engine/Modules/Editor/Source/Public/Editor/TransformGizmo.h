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
 * The move / rotate / scale handles on the selected entity: drawing, hit
 * testing and the drag math. No Qt input and no World access -- the viewport
 * feeds it the camera, the cursor and the entity's transform, and writes back
 * what UpdateDrag returns. Like every viewport overlay it is drawn with
 * Renderer debug lines, which sit over the scene regardless of depth.
 *
 * SIZE. The handles keep a fixed size on screen: their world length is
 * re-derived every frame from the entity's depth.
 *
 * SPACE. World space moves and rotates along the world axes; Local along the
 * entity's own rotated ones. Scale always uses local axes -- scale is applied
 * before rotation (TransformComponent::GetLocalMatrix), so there is no
 * world-axis scale to offer.
 *
 * DRAGS are computed from a snapshot taken at BeginDrag, never incrementally,
 * so a drag cannot drift, and CancelDrag has something exact to go back to.
 *
 * Positions are in LOGICAL pixels, as Qt reports the cursor; thicknesses are
 * scaled by the device pixel ratio before they reach the Renderer.
 */
class TransformGizmo
{
public:
    enum class EMode   { None, Translate, Rotate, Scale };
    enum class ESpace  { World, Local };

    /* Center: the screen-plane move, the view-axis ring, or uniform scale. */
    enum class EHandle { None, X, Y, Z, XY, YZ, XZ, Center };

    /* Position, rotation (Euler degrees) and scale -- the TransformComponent
     * fields the gizmo reads and writes, without its component header. */
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

    /* The view every call below works in. Call before them each time the
     * camera or viewport may have changed; InCamera must outlive the calls. */
    void SetView(const EditorCamera& InCamera, EngineCore::Size InLogicalViewport, float InDevicePixelRatio);

    /* Queues the handles for InPose as debug lines, InHover (or the dragged
     * handle) highlighted. Nothing for mode None or an entity behind the camera. */
    void Queue(EditorContext& InContext, const Pose& InPose, EHandle InHover) const;

    /* The handle under a logical pixel, or None. */
    EHandle Hit(const Pose& InPose, double InX, double InY) const;

    /* Starts dragging InHandle from the cursor at (InX, InY). False, with no
     * drag begun, if the handle cannot be dragged from this view. */
    bool BeginDrag(EHandle InHandle, const Pose& InPose, double InX, double InY);

    /* The pose the drag has reached with the cursor at (InX, InY). Where the
     * math has no answer (a ray parallel to the drag plane) the last good
     * pose stands. */
    Pose UpdateDrag(double InX, double InY, const Snap& InSnap);

    /* The pose BeginDrag started from -- what a cancelled drag restores. */
    const Pose& GetDragStart() const { return Start; }

    void EndDrag() { Active = EHandle::None; }
    bool IsDragging() const { return Active != EHandle::None; }

    /* "Move  X 120  Y 0  Z 0" and the like, for the status bar; empty when
     * not dragging. */
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

    /* The rotate rings: circle InRing (0..2 about an axis, 3 the view ring)
     * as a unit normal, two in-plane unit vectors and a radius. */
    void RingBasis(const Frame& InFrame, int InRing, EngineCore::Vector3& OutNormal,
                   EngineCore::Vector3& OutU, EngineCore::Vector3& OutV, double& OutRadius) const;

    /* The handle each axis / plane is drawn and hit as -- false when it is
     * too edge-on to the view to use. */
    bool IsAxisUsable(const Frame& InFrame, int InAxis) const;
    bool IsPlaneUsable(const Frame& InFrame, int InNormalAxis) const;

    void QueueTranslate(EditorContext& InContext, const Frame& InFrame, EHandle InHighlight) const;
    void QueueRotate(EditorContext& InContext, const Frame& InFrame, EHandle InHighlight) const;
    void QueueScale(EditorContext& InContext, const Frame& InFrame, EHandle InHighlight) const;

    EHandle HitTranslate(const Frame& InFrame, double InX, double InY) const;
    EHandle HitRotate(const Frame& InFrame, double InX, double InY) const;
    EHandle HitScale(const Frame& InFrame, double InX, double InY) const;

    /* Pixel distance from (InX, InY) to the segment InA-InB on screen;
     * a large value if either end is behind the camera. */
    double ScreenDistance(const EngineCore::Vector3& InA, const EngineCore::Vector3& InB, double InX, double InY) const;

    /* The ring point nearest the cursor on screen, front half only; false if
     * the cursor is farther than the hit distance from every segment. */
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

    /* Rotate: about RotateAxis through the origin. Plane drags track the
     * cursor's angle on the ring's plane; edge-on rings fall back to dragging
     * along the ring's on-screen tangent at the grabbed point. */
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
