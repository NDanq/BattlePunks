#if WITH_EDITOR

#include "Public/Editor/ViewportWidget.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Math/Matrix.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/Editor/EditorContext.h"

namespace
{
    constexpr float  LookDegreesPerPixel   = 0.2f;
    constexpr float  OrbitDegreesPerPixel  = 0.3f;
    constexpr float  FlyUnitsPerSecond     = 600.0f;
    constexpr float  FastMultiplier        = 4.0f;
    constexpr double DollyFractionPerNotch = 0.15;    // of the distance to the pivot
    constexpr double DollyFractionPerPixel = 0.005;

    /* A press-release that moved less than this is a click, not a drag. */
    constexpr int ClickSlopPixels = 4;

    /* Thicknesses and paddings in LOGICAL pixels; scaled by the device pixel
     * ratio before they reach the Renderer, which works in physical ones. */
    const EngineCore::Color SelectionColor(255, 165, 30);
    constexpr float         SelectionThickness  = 2.5f;
    constexpr double        SelectionPadPixels  = 4.0;   // outline sits just outside the shape
    const EngineCore::Color AllBoundsColor(120, 126, 140);
    constexpr float         AllBoundsThickness  = 1.0f;

    /* The axis gizmo: bottom-left, a fixed size on screen. */
    constexpr double GizmoMarginPixels = 60.0;    // from the left and bottom edges to its origin
    constexpr double GizmoAxisPixels   = 40.0;
    constexpr double GizmoDepth        = 100.0;   // world units in front of the camera; well past ZNear
    constexpr double GizmoHitPixels    = 12.0;
    const EngineCore::Color GizmoAxisColors[3] = { EngineCore::Color(230, 70, 70),
                                                   EngineCore::Color(90, 200, 80),
                                                   EngineCore::Color(70, 130, 240) };
    const EngineCore::Color GizmoNegativeColor(100, 104, 116);

    /* AABB::Fattened, except an axis with no extent stays flat -- padding a
     * quad's box in depth would outline it twice, front and back. */
    GameCore::AABB PadNonFlat(const GameCore::AABB& InBox, const double InPad)
    {
        const EngineCore::Vector3 Extent = InBox.Extent();
        const EngineCore::Vector3 Pad(Extent.GetX() > 0.0 ? InPad : 0.0,
                                      Extent.GetY() > 0.0 ? InPad : 0.0,
                                      Extent.GetZ() > 0.0 ? InPad : 0.0);
        return GameCore::AABB(InBox.Min - Pad, InBox.Max + Pad);
    }

    EngineCore::Vector3 UnitAxis(const int InAxis)
    {
        return EngineCore::Vector3(InAxis == 0 ? 1.0 : 0.0, InAxis == 1 ? 1.0 : 0.0, InAxis == 2 ? 1.0 : 0.0);
    }

    /* An affine transform's inverse, as a 3x4 (the bottom row is 0 0 0 1). */
    struct Affine3x4
    {
        double M[3][4] = {};

        /* p' = M * (p, InW), row-major with column vectors -- Matrix.h's layout. */
        EngineCore::Vector3 Apply(const EngineCore::Vector3& P, const double InW) const
        {
            const double X = P.GetX(), Y = P.GetY(), Z = P.GetZ();
            return EngineCore::Vector3(M[0][0] * X + M[0][1] * Y + M[0][2] * Z + M[0][3] * InW,
                                       M[1][0] * X + M[1][1] * Y + M[1][2] * Z + M[1][3] * InW,
                                       M[2][0] * X + M[2][1] * Y + M[2][2] * Z + M[2][3] * InW);
        }
    };

    /*
     * Not Matrix4x4::Inverse: that CORE_CHECKs (fatal) on a zero determinant,
     * and a degenerate transform -- a zero scale, or a rotation Core builds
     * singular -- is something a user can type into the Details panel. False
     * for anything near-singular, measured relative to the column lengths so
     * a small but honest scale still inverts.
     */
    bool InvertAffine(const EngineCore::Matrix4x4& InMatrix, Affine3x4& Out)
    {
        double A[3][3];
        for(int Row = 0; Row < 3; ++Row)
        {
            for(int Col = 0; Col < 3; ++Col) { A[Row][Col] = double(InMatrix.Data[Row][Col]); }
        }

        const double C00 = A[1][1] * A[2][2] - A[1][2] * A[2][1];
        const double C01 = A[1][2] * A[2][0] - A[1][0] * A[2][2];
        const double C02 = A[1][0] * A[2][1] - A[1][1] * A[2][0];
        const double Det = A[0][0] * C00 + A[0][1] * C01 + A[0][2] * C02;

        double Scale = 1.0;
        for(int Col = 0; Col < 3; ++Col)
        {
            Scale *= std::sqrt(A[0][Col] * A[0][Col] + A[1][Col] * A[1][Col] + A[2][Col] * A[2][Col]);
        }
        if(Scale <= 0.0 || std::abs(Det) < 1e-5 * Scale)
        {
            return false;
        }

        const double InvDet = 1.0 / Det;
        double Inv[3][3];
        Inv[0][0] = C00 * InvDet;
        Inv[1][0] = C01 * InvDet;
        Inv[2][0] = C02 * InvDet;
        Inv[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) * InvDet;
        Inv[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) * InvDet;
        Inv[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) * InvDet;
        Inv[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) * InvDet;
        Inv[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) * InvDet;
        Inv[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) * InvDet;

        for(int Row = 0; Row < 3; ++Row)
        {
            double Translation = 0.0;
            for(int Col = 0; Col < 3; ++Col)
            {
                Out.M[Row][Col] = Inv[Row][Col];
                Translation    -= Inv[Row][Col] * double(InMatrix.Data[Col][3]);
            }
            Out.M[Row][3] = Translation;
        }
        return true;
    }
}

ViewportWidget::ViewportWidget(EditorContext& InContext, QWidget* InParent)
    : QWidget(InParent)
    , Context(InContext)
{
    /* A real child HWND the backend owns outright: no Qt backing store, no
     * background erase to flicker over the rendered frame. */
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setMinimumSize(64, 64);

    FrameTimer.setTimerType(Qt::PreciseTimer);
    FrameTimer.setInterval(16);
    connect(&FrameTimer, &QTimer::timeout, this, &ViewportWidget::OnFrame);
}

EngineCore::WindowHandle ViewportWidget::GetWindowHandle()
{
    return EngineCore::WindowHandle(reinterpret_cast<void*>(winId()));
}

EngineCore::Size ViewportWidget::GetPixelSize() const
{
    const qreal Ratio = devicePixelRatioF();
    return EngineCore::Size(int(std::lround(width() * Ratio)), int(std::lround(height() * Ratio)));
}

void ViewportWidget::StartRendering()
{
    FrameClock.start();
    FrameTimer.start();
}

// === Navigation commands =====================================================

void ViewportWidget::FrameSelection()
{
    const GameCore::AABB Box = Context.GetEntityBounds(Context.GetSelection());
    if(!Box.IsEmpty())
    {
        Camera.Frame(Box);
    }
}

void ViewportWidget::FrameAll()
{
    Camera.Frame(Context.GetSceneBounds());
}

void ViewportWidget::ApplyPreset(const EditorCamera::EPreset InPreset)
{
    Camera.ApplyPreset(InPreset, GetPivot());
}

EngineCore::Vector3 ViewportWidget::GetPivot() const
{
    const GameCore::AABB Box = Context.GetEntityBounds(Context.GetSelection());
    return Box.IsEmpty() ? Camera.GetFocusPoint() : Box.Center();
}

// === Frame ===================================================================

void ViewportWidget::paintEvent(QPaintEvent* /*InEvent*/)
{
    /* Nothing: the frame timer presents. Overridden so Qt does not try to
     * paint a surface it does not own. */
}

void ViewportWidget::OnFrame()
{
    const float DeltaSeconds = float(FrameClock.restart()) / 1000.0f;
    ApplyFlyInput(DeltaSeconds);

    QueueOverlay();

    const EngineCore::Size Pixels = GetPixelSize();
    Context.RenderFrame(Camera.BuildView(Pixels), Pixels, GetWindowHandle());
}

void ViewportWidget::ApplyFlyInput(const float InDeltaSeconds)
{
    if(Drag != EDrag::Look || HeldKeys.isEmpty())
    {
        return;
    }

    double Forward = 0.0, Right = 0.0, Up = 0.0;
    if(HeldKeys.contains(Qt::Key_W)) { Forward += 1.0; }
    if(HeldKeys.contains(Qt::Key_S)) { Forward -= 1.0; }
    if(HeldKeys.contains(Qt::Key_D)) { Right   += 1.0; }
    if(HeldKeys.contains(Qt::Key_A)) { Right   -= 1.0; }
    if(HeldKeys.contains(Qt::Key_E)) { Up      += 1.0; }
    if(HeldKeys.contains(Qt::Key_Q)) { Up      -= 1.0; }

    double Speed = double(FlyUnitsPerSecond) * double(Camera.SpeedScale) * double(InDeltaSeconds);
    if(HeldKeys.contains(Qt::Key_Shift)) { Speed *= double(FastMultiplier); }

    Camera.Position = Camera.Position
                    + Camera.GetForward() * (Forward * Speed)
                    + Camera.GetRight()   * (Right * Speed)
                    + Camera.GetUp()      * (Up * Speed);
}

// === Overlay =================================================================

void ViewportWidget::QueueOverlay()
{
    const GameCore::EntityID Selected = Context.GetSelection();
    const float Ratio = float(devicePixelRatioF());

    if(bShowAllBounds)
    {
        struct Collect { EditorContext* Ctx; GameCore::EntityID Skip; float Thickness; } C{ &Context, Selected, AllBoundsThickness * Ratio };
        Context.GetWorld().ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
        {
            const auto& Data = *static_cast<const Collect*>(InUserData);
            if(InEntity != Data.Skip)
            {
                Data.Ctx->DrawDebugBox(Data.Ctx->GetEntityBounds(InEntity), AllBoundsColor, Data.Thickness);
            }
        }, &C);
    }

    /* Last, so it sits over the grey boxes where they overlap. Padded by a
     * few pixels' worth of world units at the box's depth, so the outline
     * frames the shape instead of covering its edge. An unset selection has
     * empty bounds, which draw nothing. */
    const GameCore::AABB Box = Context.GetEntityBounds(Selected);
    if(!Box.IsEmpty())
    {
        const double Depth = std::max(double((Box.Center() - Camera.Position).Dot(Camera.GetForward())), double(Camera.ZNear));
        const double Pad   = SelectionPadPixels * Camera.GetWorldPerPixel(Depth, height());
        Context.DrawDebugBox(PadNonFlat(Box, Pad), SelectionColor, SelectionThickness * Ratio);
    }

    QueueAxisGizmo();
}

void ViewportWidget::GetGizmoFrame(EngineCore::Vector3& OutOrigin, double& OutAxisLength) const
{
    /* Through the gizmo's pixel, at a fixed depth in front of the camera --
     * so it turns with the view but never moves on screen. */
    EngineCore::Vector3 RayOrigin, RayDir;
    Camera.ScreenRay(GizmoMarginPixels, double(height()) - GizmoMarginPixels, GetLogicalSize(), RayOrigin, RayDir);

    const double AlongForward = std::max(double(RayDir.Dot(Camera.GetForward())), 1e-3);
    OutOrigin     = RayOrigin + RayDir * (GizmoDepth / AlongForward);
    OutAxisLength = GizmoAxisPixels * Camera.GetWorldPerPixel(GizmoDepth, height());
}

void ViewportWidget::QueueAxisGizmo()
{
    EngineCore::Vector3 Origin;
    double AxisLength = 0.0;
    GetGizmoFrame(Origin, AxisLength);
    const float Ratio = float(devicePixelRatioF());

    for(int Axis = 0; Axis < 3; ++Axis)
    {
        const EngineCore::Vector3 Dir = UnitAxis(Axis);

        RenderCore::DebugLine Negative;
        Negative.Start           = Origin;
        Negative.End             = Origin - Dir * (AxisLength * 0.5);
        Negative.Color           = GizmoNegativeColor;
        Negative.ThicknessPixels = 1.5f * Ratio;
        Context.DrawDebugLine(Negative);

        RenderCore::DebugLine Shaft;
        Shaft.Start           = Origin;
        Shaft.End             = Origin + Dir * AxisLength;
        Shaft.Color           = GizmoAxisColors[Axis];
        Shaft.ThicknessPixels = 2.5f * Ratio;
        Context.DrawDebugLine(Shaft);

        /* A fat stub at the positive end marks direction and is the click target. */
        RenderCore::DebugLine Tip = Shaft;
        Tip.Start           = Origin + Dir * (AxisLength * 0.78);
        Tip.ThicknessPixels = 7.0f * Ratio;
        Context.DrawDebugLine(Tip);
    }
}

int ViewportWidget::HitGizmoAxis(const QPoint& InLogicalPos) const
{
    EngineCore::Vector3 Origin;
    double AxisLength = 0.0;
    GetGizmoFrame(Origin, AxisLength);

    int    Best     = -1;
    double BestDist = GizmoHitPixels;
    for(int Axis = 0; Axis < 3; ++Axis)
    {
        double X = 0.0, Y = 0.0;
        if(!Camera.WorldToScreen(Origin + UnitAxis(Axis) * (AxisLength * 0.9), GetLogicalSize(), X, Y))
        {
            continue;
        }
        const double Dist = std::hypot(X - double(InLogicalPos.x()), Y - double(InLogicalPos.y()));
        if(Dist < BestDist)
        {
            BestDist = Dist;
            Best     = Axis;
        }
    }
    return Best;
}

// === Input ===================================================================

void ViewportWidget::mousePressEvent(QMouseEvent* InEvent)
{
    setFocus(Qt::MouseFocusReason);
    LastMousePos = InEvent->position().toPoint();
    PressPos     = LastMousePos;

    if(Drag != EDrag::None)
    {
        return;   // one drag at a time; another button is ignored until release
    }

    const bool bAlt = (InEvent->modifiers() & Qt::AltModifier) != 0;
    switch(InEvent->button())
    {
        case Qt::LeftButton:   Drag = bAlt ? EDrag::Orbit : EDrag::None; break;
        case Qt::RightButton:  Drag = bAlt ? EDrag::Dolly : EDrag::Look; break;
        case Qt::MiddleButton: Drag = EDrag::Pan;                        break;
        default:               return;
    }
    DragButton = InEvent->button();
    DragPivot  = GetPivot();
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* InEvent)
{
    const QPoint Pos   = InEvent->position().toPoint();
    const QPoint Delta = Pos - LastMousePos;
    LastMousePos = Pos;

    switch(Drag)
    {
        case EDrag::Look:
            /* Drag right turns right; drag up looks up. */
            Camera.AddYawPitch(float(Delta.x()) * LookDegreesPerPixel, float(-Delta.y()) * LookDegreesPerPixel);
            break;

        case EDrag::Orbit:
            Camera.Orbit(DragPivot, float(Delta.x()) * OrbitDegreesPerPixel, float(-Delta.y()) * OrbitDegreesPerPixel);
            break;

        case EDrag::Dolly:
        {
            /* Right or down pulls in; proportional to the distance, so it
             * feels the same close up and far away. */
            const double Amount = double(Delta.x() + Delta.y()) * double(Camera.FocusDistance) * DollyFractionPerPixel;
            Camera.Dolly(DragPivot, Amount * double(Camera.SpeedScale));
            break;
        }

        case EDrag::Pan:
        {
            /* One pixel of drag moves the focus plane one pixel: the scene
             * at the focus distance stays under the cursor. */
            const double UnitsPerPixel = Camera.GetWorldPerPixel(Camera.FocusDistance, height());
            Camera.Position = Camera.Position
                            - Camera.GetRight() * (double(Delta.x()) * UnitsPerPixel)
                            + Camera.GetUp()    * (double(Delta.y()) * UnitsPerPixel);
            break;
        }

        case EDrag::None:
            break;
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* InEvent)
{
    const bool bWasDragging = Drag != EDrag::None;
    if(InEvent->button() == DragButton)
    {
        Drag       = EDrag::None;
        DragButton = Qt::NoButton;
    }

    /* Only a plain left click (no drag, no Alt-orbit) selects. */
    if(InEvent->button() != Qt::LeftButton || bWasDragging)
    {
        return;
    }
    const QPoint Pos = InEvent->position().toPoint();
    if((Pos - PressPos).manhattanLength() > ClickSlopPixels)
    {
        return;
    }

    /* The gizmo first: it floats over the scene. */
    const int Axis = HitGizmoAxis(Pos);
    if(Axis >= 0)
    {
        /* View FROM the clicked axis' positive side, toward the pivot. */
        using EPreset = EditorCamera::EPreset;
        static const EPreset FromPositive[3] = { EPreset::Right, EPreset::Bottom, EPreset::Back  };
        static const EPreset FromNegative[3] = { EPreset::Left,  EPreset::Top,    EPreset::Front };
        const bool bOpposite = (InEvent->modifiers() & Qt::ShiftModifier) != 0;
        ApplyPreset(bOpposite ? FromNegative[Axis] : FromPositive[Axis]);
        return;
    }

    Context.SetSelection(Pick(Pos));
}

void ViewportWidget::wheelEvent(QWheelEvent* InEvent)
{
    const double Notches = double(InEvent->angleDelta().y()) / 120.0;
    Camera.Dolly(GetPivot(), Notches * double(Camera.FocusDistance) * DollyFractionPerNotch * double(Camera.SpeedScale));
}

void ViewportWidget::keyPressEvent(QKeyEvent* InEvent)
{
    if(InEvent->isAutoRepeat())
    {
        return;
    }
    switch(InEvent->key())
    {
        case Qt::Key_F:    FrameSelection(); return;
        case Qt::Key_Home: FrameAll();       return;
        default:           break;
    }
    HeldKeys.insert(InEvent->key());
}

void ViewportWidget::keyReleaseEvent(QKeyEvent* InEvent)
{
    if(!InEvent->isAutoRepeat())
    {
        HeldKeys.remove(InEvent->key());
    }
}

void ViewportWidget::focusOutEvent(QFocusEvent* InEvent)
{
    /* A release that lands in another widget would otherwise leave the
     * camera flying, or a drag stuck on. */
    HeldKeys.clear();
    Drag       = EDrag::None;
    DragButton = Qt::NoButton;
    QWidget::focusOutEvent(InEvent);
}

// === Picking =================================================================

GameCore::EntityID ViewportWidget::Pick(const QPoint& InLogicalPos) const
{
    EngineCore::Vector3 RayOrigin, RayDir;
    Camera.ScreenRay(InLogicalPos.x(), InLogicalPos.y(), GetLogicalSize(), RayOrigin, RayDir);

    GameCore::EntityID Best;
    double BestT = std::numeric_limits<double>::max();

    /*
     * Ray against each primitive's quad, in the quad's own space: the inverse
     * of TransformComponent::GetLocalMatrix() -- the matrix the proxy draws
     * with -- takes the ray to where the quad is the unit square at z = 0,
     * scaled by Width x Height (GDI's half-unit quad, RenderContext::DrawQuad).
     * The hit parameter t is the same in both spaces, since the transform is
     * affine and the direction is carried with w = 0.
     */
    Context.GetWorld().Each<RenderCore::PrimitiveComponent>(
        [&](const GameCore::Entity& InEntity, RenderCore::PrimitiveComponent& InPrimitive)
    {
        if(!InPrimitive.bVisible)
        {
            return;
        }
        const GameCore::TransformComponent* Transform = InEntity.GetComponent<GameCore::TransformComponent>();
        if(!Transform)
        {
            return;
        }
        Affine3x4 Inverse;
        if(!InvertAffine(Transform->GetLocalMatrix(), Inverse))
        {
            return;   // degenerate -- drawn as a line at most, nothing to hit
        }
        const EngineCore::Vector3 LocalOrigin = Inverse.Apply(RayOrigin, 1.0);
        const EngineCore::Vector3 LocalDir    = Inverse.Apply(RayDir, 0.0);

        if(std::abs(LocalDir.GetZ()) < 1e-9)
        {
            return;   // parallel to the quad
        }
        const double T = -LocalOrigin.GetZ() / LocalDir.GetZ();
        if(T <= 0.0 || T >= BestT)
        {
            return;
        }

        const double HitX = LocalOrigin.GetX() + LocalDir.GetX() * T;
        const double HitY = LocalOrigin.GetY() + LocalDir.GetY() * T;
        if(std::abs(HitX) <= 0.5 * double(InPrimitive.Width) && std::abs(HitY) <= 0.5 * double(InPrimitive.Height))
        {
            BestT = T;
            Best  = InEntity.GetID();
        }
    });

    return Best;
}

#endif // WITH_EDITOR
