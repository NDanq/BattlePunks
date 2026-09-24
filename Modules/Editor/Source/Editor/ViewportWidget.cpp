#include "Editor/ViewportWidget.h"

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
#include "Editor/EditorContext.h"

namespace
{
    constexpr float LookDegreesPerPixel = 0.2f;
    constexpr float FlyUnitsPerSecond   = 600.0f;
    constexpr float FastMultiplier      = 4.0f;
    constexpr float DollyUnitsPerNotch  = 80.0f;
    constexpr float FrameDistance       = 600.0f;

    /* A press-release that moved less than this is a click, not a drag. */
    constexpr int ClickSlopPixels = 4;

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

void ViewportWidget::FrameEntity(const GameCore::EntityID InEntity)
{
    GameCore::World& World = Context.GetWorld();
    if(!World.IsAlive(InEntity))
    {
        return;
    }
    const GameCore::TransformComponent* Transform = World.GetEntity(InEntity).GetComponent<GameCore::TransformComponent>();
    if(!Transform)
    {
        return;
    }
    Camera.Position = Transform->Position - Camera.GetForward() * double(FrameDistance);
}

void ViewportWidget::paintEvent(QPaintEvent* /*InEvent*/)
{
    /* Nothing: the frame timer presents. Overridden so Qt does not try to
     * paint a surface it does not own. */
}

void ViewportWidget::OnFrame()
{
    const float DeltaSeconds = float(FrameClock.restart()) / 1000.0f;
    ApplyFlyInput(DeltaSeconds);

    const EngineCore::Size Pixels = GetPixelSize();
    Context.RenderFrame(Camera.BuildView(Pixels), Pixels, GetWindowHandle());
}

void ViewportWidget::ApplyFlyInput(const float InDeltaSeconds)
{
    if(!bLooking || HeldKeys.isEmpty())
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

    double Speed = double(FlyUnitsPerSecond) * double(InDeltaSeconds);
    if(HeldKeys.contains(Qt::Key_Shift)) { Speed *= double(FastMultiplier); }

    Camera.Position = Camera.Position
                    + Camera.GetForward() * (Forward * Speed)
                    + Camera.GetRight()   * (Right * Speed)
                    + Camera.GetUp()      * (Up * Speed);
}

void ViewportWidget::mousePressEvent(QMouseEvent* InEvent)
{
    setFocus(Qt::MouseFocusReason);
    LastMousePos = InEvent->position().toPoint();
    PressPos     = LastMousePos;

    if(InEvent->button() == Qt::RightButton)
    {
        bLooking = true;
    }
    else if(InEvent->button() == Qt::MiddleButton)
    {
        bPanning = true;
    }
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* InEvent)
{
    const QPoint Pos   = InEvent->position().toPoint();
    const QPoint Delta = Pos - LastMousePos;
    LastMousePos = Pos;

    if(bLooking)
    {
        /* Drag right turns right; drag up looks up. */
        Camera.AddYawPitch(float(Delta.x()) * LookDegreesPerPixel, float(-Delta.y()) * LookDegreesPerPixel);
    }
    else if(bPanning)
    {
        /* Scaled so the scene roughly tracks the cursor at a typical distance. */
        const double UnitsPerPixel = 1.5;
        Camera.Position = Camera.Position
                        - Camera.GetRight() * (double(Delta.x()) * UnitsPerPixel)
                        + Camera.GetUp()    * (double(Delta.y()) * UnitsPerPixel);
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* InEvent)
{
    if(InEvent->button() == Qt::RightButton)
    {
        bLooking = false;
    }
    else if(InEvent->button() == Qt::MiddleButton)
    {
        bPanning = false;
    }
    else if(InEvent->button() == Qt::LeftButton)
    {
        const QPoint Pos = InEvent->position().toPoint();
        if((Pos - PressPos).manhattanLength() <= ClickSlopPixels)
        {
            Context.SetSelection(Pick(Pos));
        }
    }
}

void ViewportWidget::wheelEvent(QWheelEvent* InEvent)
{
    const double Notches = double(InEvent->angleDelta().y()) / 120.0;
    Camera.Position = Camera.Position + Camera.GetForward() * (Notches * double(DollyUnitsPerNotch));
}

void ViewportWidget::keyPressEvent(QKeyEvent* InEvent)
{
    if(InEvent->isAutoRepeat())
    {
        return;
    }
    if(InEvent->key() == Qt::Key_F)
    {
        FrameEntity(Context.GetSelection());
        return;
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
     * camera flying forever. */
    HeldKeys.clear();
    bLooking = false;
    bPanning = false;
    QWidget::focusOutEvent(InEvent);
}

GameCore::EntityID ViewportWidget::Pick(const QPoint& InLogicalPos) const
{
    EngineCore::Vector3 RayOrigin, RayDir;
    Camera.ScreenRay(InLogicalPos.x(), InLogicalPos.y(), EngineCore::Size(width(), height()), RayOrigin, RayDir);

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
