#if WITH_EDITOR

#include "Public/NimbleEngine/Input.h"
#include "Public/Editor/ViewportWidget.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <cstring>

#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/Components.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Math/Matrix.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderComponents.h"
#include "Public/RenderCore/RenderTypes.h"
#include "Public/Editor/EditorContext.h"
#include "Public/Editor/EditorDocument.h"
#include "Private/Editor/EditorEditActions.h"
#include "Private/Editor/Prefab/PrefabAssets.h"
#include "Private/Editor/Content/ContentPaths.h"

/* Handle snap steps. Translate is a world grid in World space, a step along
 * the handle in Local. */
NIMBLE_CONFIG_FLOAT(GEditorSnapTranslate, "Editor", "SnapTranslate", 10.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GEditorSnapRotate,    "Editor", "SnapRotate",    15.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GEditorSnapScale,     "Editor", "SnapScale",     0.1f,  nullptr);

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
    const EngineCore::Color SecondarySelectionColor(200, 120, 40);   // the rest of a multiple selection
    const EngineCore::Color AllBoundsColor(120, 126, 140);
    constexpr float         AllBoundsThickness  = 1.0f;

    /* The game camera's screen on z = 0: ortho, 1:1 units to pixels, at the
     * default 1280 x 720. */
    constexpr double            GameScreenWidth  = 1280.0;
    constexpr double            GameScreenHeight = 720.0;
    const EngineCore::Color     GameScreenColor(90, 150, 230);
    constexpr float             GameScreenThickness = 1.5f;
    constexpr double            GameFrustumDepth    = 200.0;   // how far a perspective camera's frustum is drawn

    /* The axis gizmo: bottom-left, a fixed size on screen. */
    constexpr double GizmoMarginPixels = 60.0;    // from the left and bottom edges to its origin
    constexpr double GizmoAxisPixels   = 40.0;
    constexpr double GizmoDepth        = 100.0;   // world units in front of the camera; well past ZNear
    constexpr double GizmoHitPixels    = 12.0;
    const EngineCore::Color GizmoAxisColors[3] = { EngineCore::Color(230, 70, 70),
                                                   EngineCore::Color(90, 200, 80),
                                                   EngineCore::Color(70, 130, 240) };
    const EngineCore::Color GizmoNegativeColor(100, 104, 116);

    /* Light gizmos: rings of LightRingSegments; the sun's arrow a fixed size
     * on screen; an unselected light's drawn at LightDimFactor of its colour. */
    constexpr int    LightRingSegments    = 48;
    constexpr float  LightThickness       = 1.5f;
    constexpr float  LightSelectedThickness = 2.0f;
    constexpr double LightDimFactor       = 0.45;
    constexpr double SunArrowPixels       = 70.0;
    constexpr double LightPickPixels      = 12.0;

    /* InColor scaled toward black, never so dark it vanishes on the clear. */
    EngineCore::Color GizmoColor(const EngineCore::Color InColor, const bool bInSelected)
    {
        const double Scale = bInSelected ? 1.0 : LightDimFactor;
        const auto Channel = [Scale](const uint8_t InValue) { return uint8_t(std::clamp(40.0 + double(InValue) * Scale * 0.85, 0.0, 255.0)); };
        return EngineCore::Color(Channel(InColor.R), Channel(InColor.G), Channel(InColor.B));
    }

    /* Two unit vectors perpendicular to InAxis and each other. */
    void Perpendiculars(const EngineCore::Vector3& InAxis, EngineCore::Vector3& OutU, EngineCore::Vector3& OutV)
    {
        const EngineCore::Vector3 Helper = std::abs(InAxis.GetY()) < 0.9 ? EngineCore::Vector3(0.0, 1.0, 0.0) : EngineCore::Vector3(1.0, 0.0, 0.0);
        OutU = (InAxis ^ Helper).GetNormalize();
        OutV = (InAxis ^ OutU).GetNormalize();
    }

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

    /* In the world: under a parent, the handles sit where the entity is,
     * not at its local values. */
    TransformGizmo::Pose PoseOf(const GameCore::TransformComponent& InTransform)
    {
        const GameCore::TransformPose World = InTransform.Owner.GetEntity().GetWorldPose();
        TransformGizmo::Pose Pose;
        Pose.Position = World.Position;
        Pose.Rotation = World.Rotation;
        Pose.Scale    = World.Scale;
        return Pose;
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

    /* Not Matrix4x4::Inverse, which is fatal on a singular matrix (a user can
     * type a zero scale). False if near-singular relative to column lengths. */
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

    /* The nearest t > 0 at which the ray InOrigin + t * InDir meets one of
     * InMesh's triangles, either face (Moller-Trumbore); 0 for a miss. */
    double RayHitsMesh(const RenderCore::MeshData& InMesh, const EngineCore::Vector3& InOrigin, const EngineCore::Vector3& InDir)
    {
        const double O[3] = { InOrigin.GetX(), InOrigin.GetY(), InOrigin.GetZ() };
        const double D[3] = { InDir.GetX(), InDir.GetY(), InDir.GetZ() };
        const auto Vertex = [&](const uint32_t InIndex, double* OutP)
        {
            for(int Axis = 0; Axis < 3; ++Axis) { OutP[Axis] = double(InMesh.Positions[size_t(InIndex) * 3 + size_t(Axis)]); }
        };

        double Best = 0.0;
        for(size_t At = 0; At + 2 < InMesh.Indices.size(); At += 3)
        {
            double P0[3], P1[3], P2[3];
            Vertex(InMesh.Indices[At], P0);
            Vertex(InMesh.Indices[At + 1], P1);
            Vertex(InMesh.Indices[At + 2], P2);

            const double E1[3] = { P1[0] - P0[0], P1[1] - P0[1], P1[2] - P0[2] };
            const double E2[3] = { P2[0] - P0[0], P2[1] - P0[1], P2[2] - P0[2] };
            const double H[3]  = { D[1] * E2[2] - D[2] * E2[1], D[2] * E2[0] - D[0] * E2[2], D[0] * E2[1] - D[1] * E2[0] };
            const double A     = E1[0] * H[0] + E1[1] * H[1] + E1[2] * H[2];
            if(std::abs(A) < 1e-12) { continue; }   // parallel to the triangle

            const double F = 1.0 / A;
            const double S[3] = { O[0] - P0[0], O[1] - P0[1], O[2] - P0[2] };
            const double U = F * (S[0] * H[0] + S[1] * H[1] + S[2] * H[2]);
            if(U < 0.0 || U > 1.0) { continue; }

            const double Q[3] = { S[1] * E1[2] - S[2] * E1[1], S[2] * E1[0] - S[0] * E1[2], S[0] * E1[1] - S[1] * E1[0] };
            const double V = F * (D[0] * Q[0] + D[1] * Q[1] + D[2] * Q[2]);
            if(V < 0.0 || U + V > 1.0) { continue; }

            const double T = F * (E2[0] * Q[0] + E2[1] * Q[1] + E2[2] * Q[2]);
            if(T > 1e-9 && (Best == 0.0 || T < Best)) { Best = T; }
        }
        return Best;
    }
}

ViewportWidget::ViewportWidget(EditorContext& InContext, EditorDocument& InDocument, QWidget* InParent)
    : QWidget(InParent)
    , Context(InContext)
    , Document(InDocument)
{
    /* A real child HWND the backend owns outright: no Qt backing store, no
     * background erase to flicker over the rendered frame. */
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);   // for the transform handles' hover highlight
    setMinimumSize(64, 64);
    setAcceptDrops(true);     // a mesh dragged from the Content Browser

    /* The context draws it every frame from now on (EditorContext::RunFrame). */
    Context.AddViewport(this);

    /* Handing the view to or from the game mid-drag would leave the camera
     * flying on keys whose release it never sees. */
    const auto ResetNavigation = [this]()
    {
        FinishGizmoDrag(false);
        HeldKeys.clear();
        Drag       = EDrag::None;
        DragButton = Qt::NoButton;
    };
    connect(&Context, &EditorContext::PlayStateChanged,       this, ResetNavigation);
    connect(&Context, &EditorContext::PlayViewEjectedChanged, this, ResetNavigation);

    /* A drag's entity is only good while it stays selected in the same World. */
    const auto DropGizmoDrag = [this]()
    {
        FinishGizmoDrag(false);
        GizmoHover = TransformGizmo::EHandle::None;
    };
    connect(&Document, &EditorDocument::SelectionChanged, this, DropGizmoDrag);
    connect(&Document, &EditorDocument::WorldReset,       this, DropGizmoDrag);

    EditorEditActions::Install(this, Document, [this]() { EditorEditActions::PromptRename(this, Document); });
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

ViewportWidget::~ViewportWidget()
{
    /* While the native window still exists: its swapchain goes with it. */
    Context.RemoveViewport(this);
}

// === Navigation commands =====================================================

void ViewportWidget::FrameSelection()
{
    const GameCore::AABB Box = GetSelectionBounds();
    if(!Box.IsEmpty())
    {
        Camera.Frame(Box);
    }
}

void ViewportWidget::FrameAll()
{
    Camera.Frame(Document.GetSceneBounds());
}

void ViewportWidget::ApplyPreset(const EditorCamera::EPreset InPreset)
{
    Camera.ApplyPreset(InPreset, GetPivot());
}

void ViewportWidget::ApplySandboxView()
{
    /* The game camera's screen from the origin -- see GameScreenWidth. */
    const GameCore::AABB GameScreen(EngineCore::Vector3(0.0, 0.0, 0.0), EngineCore::Vector3(GameScreenWidth, GameScreenHeight, 0.0));

    Camera.Yaw   = 0.0f;   // Front: along +Z, the game's own view direction
    Camera.Pitch = 0.0f;
    Camera.Frame(GameScreen);
}

void ViewportWidget::ApplyGameCameraView()
{
    bool bFound = false;
    Document.GetWorld().Each<RenderCore::CameraComponent>(
        [this, &bFound](const GameCore::Entity& InEntity, RenderCore::CameraComponent& InCamera)
    {
        if(bFound || !InCamera.bActive)
        {
            return;
        }
        bFound = true;
        if(InCamera.Projection != RenderCore::CP_Perspective)
        {
            ApplySandboxView();
            return;
        }
        /* The editor camera's yaw is the Transform's Y; its pitch is the
         * Transform's X negated -- a positive pitch looks up for the editor
         * camera, down for a rotation (LightForward). */
        const GameCore::TransformPose Pose = InEntity.GetWorldPose();
        Camera.Position = Pose.Position;
        Camera.Pitch    = std::clamp(-float(Pose.Rotation.GetX()), -89.0f, 89.0f);
        Camera.Yaw      = float(Pose.Rotation.GetY());
    });
    if(!bFound)
    {
        FrameAll();
    }
}

EngineCore::Vector3 ViewportWidget::GetPivot() const
{
    const GameCore::AABB Box = GetSelectionBounds();
    return Box.IsEmpty() ? Camera.GetFocusPoint() : Box.Center();
}

GameCore::AABB ViewportWidget::GetSelectionBounds() const
{
    GameCore::AABB Box;
    for(const GameCore::EntityID Each : Document.GetSelectedEntities())
    {
        Box = GameCore::AABB::Union(Box, Document.GetEntityBounds(Each));
    }
    return Box;
}

void ViewportWidget::ShowContextMenu(const QPoint& InLogicalPos)
{
    const GameCore::EntityID Under = Document.GetPickTarget(Pick(InLogicalPos));
    if(Under.IsSet() && !Document.IsSelected(Under))
    {
        Document.SetSelection(Under);
    }
    HeldKeys.clear();   // the menu takes the keyboard; releases would be missed
    QMenu Menu(this);
    EditorEditActions::AddToMenu(&Menu, Document, [this]() { EditorEditActions::PromptRename(this, Document); });
    Menu.exec(mapToGlobal(InLogicalPos));
}

bool ViewportWidget::event(QEvent* InEvent)
{
    if(InEvent->type() == QEvent::ShortcutOverride && Drag != EDrag::None)
    {
        InEvent->accept();   // delivered as a key press, not a shortcut
        return true;
    }
    return QWidget::event(InEvent);
}

// === Frame ===================================================================

void ViewportWidget::paintEvent(QPaintEvent* /*InEvent*/)
{
    /* Nothing: the frame timer presents. Overridden so Qt does not try to
     * paint a surface it does not own. */
}

bool ViewportWidget::PrepareFrame(const float InDeltaSeconds, const bool bInEditorView, Frame& OutFrame)
{
    UpdateMouseHold();
    if(!isVisible())
    {
        return false;
    }

    /* Navigation and overlays belong to the editor's view; while playing
     * through the game's camera the viewport shows the game alone. */
    if(bInEditorView)
    {
        ApplyFlyInput(InDeltaSeconds);
        Gizmo.SetView(Camera, GetLogicalSize(), float(devicePixelRatioF()));
        QueueOverlay();
    }

    OutFrame.Pixels = GetPixelSize();
    OutFrame.View   = Camera.BuildView(OutFrame.Pixels);
    OutFrame.Window = GetWindowHandle();
    return true;
}

void ViewportWidget::UpdateMouseHold()
{
    const bool bWant = isVisible() && hasFocus() && window()->isActiveWindow() && Context.IsPlayMouseHeldByViewport();
    if(bWant == bHoldingMouse)
    {
        return;
    }
    if(!bWant)
    {
        ReleaseMouseHold();
        return;
    }
    bHoldingMouse = true;
    setCursor(Qt::BlankCursor);
    grabMouse();
    QCursor::setPos(mapToGlobal(rect().center()));
}

void ViewportWidget::ReleaseMouseHold()
{
    if(!bHoldingMouse)
    {
        return;
    }
    bHoldingMouse = false;
    releaseMouse();
    unsetCursor();
}

void ViewportWidget::ApplyFlyInput(const float InDeltaSeconds)
{
    /* WASD/QE fly with the viewport focused: while looking (right drag), or
     * with no button held -- but not under Ctrl or Alt, which make them
     * shortcuts (Ctrl+S, Ctrl+D) rather than moves. */
    if(HeldKeys.isEmpty() || !hasFocus())
    {
        return;
    }
    if(Drag != EDrag::Look
       && (Drag != EDrag::None || HeldKeys.contains(Qt::Key_Control) || HeldKeys.contains(Qt::Key_Alt) || HeldKeys.contains(Qt::Key_Meta)))
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

    if(Forward != 0.0 || Right != 0.0 || Up != 0.0)
    {
        bContextClick = false;   // it flew: not a click
    }
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
    const GameCore::EntityID Selected = Document.GetSelection();
    const float Ratio = float(devicePixelRatioF());

    if(bShowAllBounds)
    {
        struct Collect { EditorContext* Ctx; EditorDocument* Doc; GameCore::EntityID Skip; float Thickness; } C{ &Context, &Document, Selected, AllBoundsThickness * Ratio };
        Document.GetWorld().ForEachEntity([](const GameCore::EntityID InEntity, void* InUserData)
        {
            const auto& Data = *static_cast<const Collect*>(InUserData);
            if(!Data.Doc->IsSelected(InEntity))
            {
                Data.Ctx->DrawDebugBox(Data.Doc->GetEntityBounds(InEntity), AllBoundsColor, Data.Thickness);
            }
        }, &C);
    }

    /* Where the game draws: each active camera's screen (orthographic) or
     * frustum (perspective). The editor's own view replaces the camera, so this
     * is the only sign of what the game shows. */
    Document.GetWorld().Each<RenderCore::CameraComponent>(
        [this, Ratio](const GameCore::Entity& InEntity, RenderCore::CameraComponent& InCamera)
    {
        if(!InCamera.bActive)
        {
            return;
        }
        const GameCore::TransformPose Pose = InEntity.GetWorldPose();
        const EngineCore::Vector3 Origin = Pose.Position;
        if(InCamera.Projection == RenderCore::CP_Perspective)
        {
            /* A perspective camera: its frustum, a short way out, at the game's aspect. */
            const EngineCore::Vector3 Forward = RenderCore::LightForward(Pose.Rotation);
            const EngineCore::Vector3 Right   = (EngineCore::Vector3(0.0, 1.0, 0.0) ^ Forward).GetNormalize();
            const EngineCore::Vector3 Up      = Forward ^ Right;
            const double Depth = std::min(double(InCamera.ZFar), GameFrustumDepth);
            const double HalfY = Depth * std::tan(double(std::clamp(InCamera.FovY, 1.0f, 170.0f)) * 0.5 * 3.14159265358979 / 180.0);
            const double HalfX = HalfY * GameScreenWidth / GameScreenHeight;
            EngineCore::Vector3 Corners[4];
            for(int Corner = 0; Corner < 4; ++Corner)
            {
                Corners[Corner] = Origin + Forward * Depth + Right * ((Corner & 1) ? HalfX : -HalfX)
                                + Up * ((Corner & 2) ? HalfY : -HalfY);
            }
            const int Rim[4] = { 0, 1, 3, 2 };
            for(int Edge = 0; Edge < 4; ++Edge)
            {
                RenderCore::DebugLine Line;
                Line.Color           = GameScreenColor;
                Line.ThicknessPixels = GameScreenThickness * Ratio;
                Line.Start = Origin;
                Line.End   = Corners[Edge];
                Context.DrawDebugLine(Line);
                Line.Start = Corners[Rim[Edge]];
                Line.End   = Corners[Rim[(Edge + 1) % 4]];
                Context.DrawDebugLine(Line);
            }
            return;
        }
        const GameCore::AABB Screen(EngineCore::Vector3(Origin.GetX(), Origin.GetY(), 0.0),
                                    EngineCore::Vector3(Origin.GetX() + GameScreenWidth, Origin.GetY() + GameScreenHeight, 0.0));
        Context.DrawDebugBox(Screen, GameScreenColor, GameScreenThickness * Ratio);
    });

    /* Last, over the grey boxes; padded a few pixels so it frames the shape.
     * Every selected entity; the primary (the gizmo's) the brightest. */
    for(const GameCore::EntityID Each : Document.GetSelectedEntities())
    {
        const GameCore::AABB Box = Document.GetEntityBounds(Each);
        if(Box.IsEmpty())
        {
            continue;
        }
        const double Depth = std::max(double((Box.Center() - Camera.Position).Dot(Camera.GetForward())), double(Camera.ZNear));
        const double Pad   = SelectionPadPixels * Camera.GetWorldPerPixel(Depth, height());
        Context.DrawDebugBox(PadNonFlat(Box, Pad), Each == Selected ? SelectionColor : SecondarySelectionColor,
                             SelectionThickness * Ratio);
    }

    QueueLightGizmos();

    if(const GameCore::TransformComponent* Target = GetGizmoTarget())
    {
        Gizmo.Queue(Context, PoseOf(*Target), GizmoHover);
    }

    QueueAxisGizmo();
}

void ViewportWidget::QueueLightGizmos()
{
    const GameCore::EntityID Selected = Document.GetSelection();
    const float Ratio = float(devicePixelRatioF());

    const auto Segment = [this](const EngineCore::Vector3& InA, const EngineCore::Vector3& InB,
                                const EngineCore::Color InColor, const float InThickness)
    {
        RenderCore::DebugLine Line;
        Line.Start           = InA;
        Line.End             = InB;
        Line.Color           = InColor;
        Line.ThicknessPixels = InThickness;
        Context.DrawDebugLine(Line);
    };
    /* A circle about InCentre in the plane of InU and InV. */
    const auto Ring = [&Segment](const EngineCore::Vector3& InCentre, const EngineCore::Vector3& InU, const EngineCore::Vector3& InV,
                                 const double InRadius, const EngineCore::Color InColor, const float InThickness)
    {
        EngineCore::Vector3 Previous = InCentre + InU * InRadius;
        for(int Step = 1; Step <= LightRingSegments; ++Step)
        {
            const double Angle = 2.0 * 3.14159265358979 * double(Step) / double(LightRingSegments);
            const EngineCore::Vector3 Point = InCentre + (InU * std::cos(Angle) + InV * std::sin(Angle)) * InRadius;
            Segment(Previous, Point, InColor, InThickness);
            Previous = Point;
        }
    };
    /* Which lights draw, and how. */
    const auto Style = [&](const GameCore::Entity& InEntity, const EngineCore::Color InLight, EngineCore::Color& OutColor, float& OutThickness)
    {
        const bool bSelected = Document.IsSelected(InEntity.GetID());
        if(!bSelected && !bShowLightGizmos)
        {
            return false;
        }
        OutColor     = GizmoColor(InLight, bSelected);
        OutThickness = (bSelected ? LightSelectedThickness : LightThickness) * Ratio;
        return true;
    };

    GameCore::World& World = Document.GetWorld();

    /* Point: three great circles at Radius. */
    World.Each<RenderCore::PointLightComponent>(
        [&](const GameCore::Entity& InEntity, RenderCore::PointLightComponent& InLight)
    {
        EngineCore::Color Color;
        float Thickness = 0.0f;
        if(!Style(InEntity, InLight.Color, Color, Thickness)) { return; }
        const EngineCore::Vector3 Centre = InEntity.GetWorldPose().Position;
        const double Radius = std::max(double(InLight.Radius), 0.0);
        const EngineCore::Vector3 X(1.0, 0.0, 0.0), Y(0.0, 1.0, 0.0), Z(0.0, 0.0, 1.0);
        Ring(Centre, X, Y, Radius, Color, Thickness);
        Ring(Centre, X, Z, Radius, Color, Thickness);
        Ring(Centre, Y, Z, Radius, Color, Thickness);
    });

    /* Spot: the outer cone to Range (its rim and four edges), the inner rim dimmer. */
    World.Each<RenderCore::SpotLightComponent>(
        [&](const GameCore::Entity& InEntity, RenderCore::SpotLightComponent& InLight)
    {
        EngineCore::Color Color;
        float Thickness = 0.0f;
        if(!Style(InEntity, InLight.Color, Color, Thickness)) { return; }
        const GameCore::TransformPose Pose = InEntity.GetWorldPose();
        const EngineCore::Vector3 Apex    = Pose.Position;
        const EngineCore::Vector3 Forward = RenderCore::LightForward(Pose.Rotation);
        EngineCore::Vector3 U, V;
        Perpendiculars(Forward, U, V);

        const double Degrees = 3.14159265358979 / 180.0;
        const double Range   = std::max(double(InLight.Range), 0.0);
        const double Outer   = std::clamp(double(InLight.OuterConeAngle), 0.5, 89.0) * Degrees;
        const double Inner   = std::clamp(double(InLight.InnerConeAngle), 0.0, std::clamp(double(InLight.OuterConeAngle), 0.5, 89.0)) * Degrees;

        /* The rim where the cone meets the sphere of Range. */
        const EngineCore::Vector3 OuterCentre = Apex + Forward * (Range * std::cos(Outer));
        const double              OuterRadius = Range * std::sin(Outer);
        Ring(OuterCentre, U, V, OuterRadius, Color, Thickness);
        for(const EngineCore::Vector3& Side : { U, V, U * -1.0, V * -1.0 })
        {
            Segment(Apex, OuterCentre + Side * OuterRadius, Color, Thickness);
        }
        if(Inner > 0.0)
        {
            Ring(Apex + Forward * (Range * std::cos(Inner)), U, V, Range * std::sin(Inner), GizmoColor(InLight.Color, false), Thickness * 0.75f);
        }
    });

    /* Directional: an arrow from the entity along the way it shines, a fixed size on screen. */
    World.Each<RenderCore::DirectionalLightComponent>(
        [&](const GameCore::Entity& InEntity, RenderCore::DirectionalLightComponent& InLight)
    {
        EngineCore::Color Color;
        float Thickness = 0.0f;
        if(!Style(InEntity, InLight.Color, Color, Thickness)) { return; }
        const GameCore::TransformPose Pose = InEntity.GetWorldPose();
        const EngineCore::Vector3 Origin  = Pose.Position;
        const EngineCore::Vector3 Forward = RenderCore::LightForward(Pose.Rotation);
        const double Depth  = std::max(double((Origin - Camera.Position).Dot(Camera.GetForward())), double(Camera.ZNear));
        const double Length = SunArrowPixels * Camera.GetWorldPerPixel(Depth, height());
        EngineCore::Vector3 U, V;
        Perpendiculars(Forward, U, V);

        /* Three parallel shafts, as rays, then a head on the middle one. */
        const EngineCore::Vector3 Tip = Origin + Forward * Length;
        Segment(Origin, Tip, Color, Thickness * 1.5f);
        for(const EngineCore::Vector3& Side : { U, U * -1.0 })
        {
            const EngineCore::Vector3 Start = Origin + Side * (Length * 0.25);
            Segment(Start, Start + Forward * (Length * 0.7), Color, Thickness);
        }
        for(const EngineCore::Vector3& Side : { U, V, U * -1.0, V * -1.0 })
        {
            Segment(Tip, Tip - Forward * (Length * 0.22) + Side * (Length * 0.1), Color, Thickness * 1.5f);
        }
    });

    /* Scene capture probes: a mirror's rectangle on its plane and the way it
     * faces; an omnidirectional probe's small cube, and (selected) its reach. */
    World.Each<RenderCore::SceneCaptureProbeComponent>(
        [&](const GameCore::Entity& InEntity, RenderCore::SceneCaptureProbeComponent& InProbe)
    {
        EngineCore::Color Color;
        float Thickness = 0.0f;
        if(!Style(InEntity, EngineCore::Color(120, 200, 255), Color, Thickness)) { return; }
        const GameCore::TransformPose Pose = InEntity.GetWorldPose();
        const GameCore::Rotation3 Axes = GameCore::TransformMath::EulerToRotation(Pose.Rotation);
        const EngineCore::Vector3 Centre = Pose.Position;
        if(InProbe.Mode == RenderCore::SCM_PlanarMirror)
        {
            const EngineCore::Vector3 X = Axes.GetAxis(0) * double(InProbe.ExtentX);
            const EngineCore::Vector3 Z = Axes.GetAxis(2) * double(InProbe.ExtentZ);
            const EngineCore::Vector3 Corners[4] = { Centre + X + Z, Centre - X + Z, Centre - X - Z, Centre + X - Z };
            for(int Edge = 0; Edge < 4; ++Edge) { Segment(Corners[Edge], Corners[(Edge + 1) % 4], Color, Thickness); }
            const double Arrow = std::max(std::min(double(InProbe.ExtentX), double(InProbe.ExtentZ)) * 0.5, 20.0);
            Segment(Centre, Centre + Axes.GetAxis(1) * Arrow, Color, Thickness);
            return;
        }
        const double Half = 20.0;
        const EngineCore::Vector3 H(Half, Half, Half);
        Context.DrawDebugBox(GameCore::AABB(Centre - H, Centre + H), Color, Thickness);
        if(Document.IsSelected(InEntity.GetID()))
        {
            const double Reach = std::max(double(InProbe.FarClip), 0.0);
            const EngineCore::Vector3 X(1.0, 0.0, 0.0), Y(0.0, 1.0, 0.0), Z(0.0, 0.0, 1.0);
            Ring(Centre, X, Z, Reach, GizmoColor(EngineCore::Color(120, 200, 255), false), Thickness * 0.5f);
        }
    });

    /* Reflection probes: a small diamond, and (selected, or always with gizmos on) its influence --
     * its box, or three rings of its radius. */
    World.Each<RenderCore::ReflectionProbeComponent>(
        [&](const GameCore::Entity& InEntity, RenderCore::ReflectionProbeComponent& InProbe)
    {
        EngineCore::Color Color;
        float Thickness = 0.0f;
        if(!Style(InEntity, EngineCore::Color(255, 210, 120), Color, Thickness)) { return; }
        const EngineCore::Vector3 Centre = InEntity.GetWorldPose().Position;
        const double D = 25.0;
        const EngineCore::Vector3 Tips[6] = { Centre + EngineCore::Vector3(D, 0, 0), Centre + EngineCore::Vector3(-D, 0, 0),
                                              Centre + EngineCore::Vector3(0, D, 0), Centre + EngineCore::Vector3(0, -D, 0),
                                              Centre + EngineCore::Vector3(0, 0, D), Centre + EngineCore::Vector3(0, 0, -D) };
        for(int Around = 0; Around < 4; ++Around)
        {
            const int Side[4] = { 0, 4, 1, 5 };
            const EngineCore::Vector3& A = Tips[Side[Around]];
            const EngineCore::Vector3& B = Tips[Side[(Around + 1) % 4]];
            Segment(A, B, Color, Thickness);
            Segment(A, Tips[2], Color, Thickness);
            Segment(A, Tips[3], Color, Thickness);
        }
        if(!Document.IsSelected(InEntity.GetID())) { return; }
        const EngineCore::Color Reach = GizmoColor(EngineCore::Color(255, 210, 120), false);
        const EngineCore::Vector3& Box = InProbe.BoxExtent;
        if(Box.GetX() > 0.0 && Box.GetY() > 0.0 && Box.GetZ() > 0.0)
        {
            Context.DrawDebugBox(GameCore::AABB(Centre - Box, Centre + Box), Reach, Thickness * 0.5f);
            return;
        }
        const EngineCore::Vector3 X(1.0, 0.0, 0.0), Y(0.0, 1.0, 0.0), Z(0.0, 0.0, 1.0);
        const double Radius = std::max(double(InProbe.Radius), 1.0);
        Ring(Centre, X, Z, Radius, Reach, Thickness * 0.5f);
        Ring(Centre, X, Y, Radius, Reach, Thickness * 0.5f);
        Ring(Centre, Y, Z, Radius, Reach, Thickness * 0.5f);
    });
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

namespace
{
    /* Play-in-Editor's fallback when SDL could not take the viewport: Qt's key
     * to the engine's key name. Qt keys are layout-dependent, unlike SDL's
     * physical ones -- one reason SDL is the normal path. */
    const char* QtKeyToInputName(const int InKey)
    {
        static const char* const Letters[] = { "A","B","C","D","E","F","G","H","I","J","K","L","M",
                                               "N","O","P","Q","R","S","T","U","V","W","X","Y","Z" };
        static const char* const Digits[]  = { "0","1","2","3","4","5","6","7","8","9" };
        static const char* const FKeys[]   = { "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12" };
        if(InKey >= Qt::Key_A && InKey <= Qt::Key_Z)   { return Letters[InKey - Qt::Key_A]; }
        if(InKey >= Qt::Key_0 && InKey <= Qt::Key_9)   { return Digits[InKey - Qt::Key_0]; }
        if(InKey >= Qt::Key_F1 && InKey <= Qt::Key_F12) { return FKeys[InKey - Qt::Key_F1]; }
        switch(InKey)
        {
            case Qt::Key_Space:     return "Space";
            case Qt::Key_Return:    return "Return";
            case Qt::Key_Enter:     return "KP_Enter";
            case Qt::Key_Escape:    return "Escape";
            case Qt::Key_Tab:       return "Tab";
            case Qt::Key_Backspace: return "Backspace";
            case Qt::Key_Left:      return "Left";
            case Qt::Key_Right:     return "Right";
            case Qt::Key_Up:        return "Up";
            case Qt::Key_Down:      return "Down";
            case Qt::Key_Shift:     return "LeftShift";
            case Qt::Key_Control:   return "LeftCtrl";
            case Qt::Key_Alt:       return "LeftAlt";
            case Qt::Key_Insert:    return "Insert";
            case Qt::Key_Delete:    return "Delete";
            case Qt::Key_Home:      return "Home";
            case Qt::Key_End:       return "End";
            case Qt::Key_PageUp:    return "PageUp";
            case Qt::Key_PageDown:  return "PageDown";
            default:                return nullptr;
        }
    }

    const char* QtButtonToInputName(const Qt::MouseButton InButton)
    {
        switch(InButton)
        {
            case Qt::LeftButton:   return "Mouse.Left";
            case Qt::MiddleButton: return "Mouse.Middle";
            case Qt::RightButton:  return "Mouse.Right";
            case Qt::BackButton:   return "Mouse.X1";
            case Qt::ForwardButton:return "Mouse.X2";
            default:               return nullptr;
        }
    }
}

void ViewportWidget::mousePressEvent(QMouseEvent* InEvent)
{
    if(NimbleEngine::InputState* Game = Context.GetPlayFallbackInput())
    {
        if(const char* Name = QtButtonToInputName(InEvent->button())) { Game->Press(NimbleEngine::FindInputKey(Name)); }
    }
    /* The game's view while playing: the editor camera does not move. */
    if(!Context.IsEditorViewActive())
    {
        return;
    }
    setFocus(Qt::MouseFocusReason);
    LastMousePos = InEvent->position().toPoint();
    PressPos     = LastMousePos;

    /* A right click abandons a handle drag, as Esc does. */
    if(Drag == EDrag::Gizmo)
    {
        if(InEvent->button() == Qt::RightButton)
        {
            FinishGizmoDrag(true);
        }
        return;
    }
    if(Drag != EDrag::None)
    {
        return;   // one drag at a time; another button is ignored until release
    }

    const bool bAlt = (InEvent->modifiers() & Qt::AltModifier) != 0;

    /* A transform handle takes the press -- before click-to-select, which
     * waits for the release. The axis gizmo in the corner keeps its clicks. */
    if(InEvent->button() == Qt::LeftButton && !bAlt && HitGizmoAxis(PressPos) < 0)
    {
        if(const GameCore::TransformComponent* Target = GetGizmoTarget())
        {
            const QPointF At = InEvent->position();
            Gizmo.SetView(Camera, GetLogicalSize(), float(devicePixelRatioF()));
            const TransformGizmo::EHandle Handle = Gizmo.Hit(PoseOf(*Target), At.x(), At.y());
            if(Handle != TransformGizmo::EHandle::None && Gizmo.BeginDrag(Handle, PoseOf(*Target), At.x(), At.y()))
            {
                /* The whole drag is one undo entry -- for every entity it moves. */
                static const char* const Labels[4] = { "Edit", "Move", "Rotate", "Scale" };
                const QString Label = QString::fromLatin1(Labels[int(Gizmo.GetMode())]);
                GizmoEntity = Document.GetSelection();
                GizmoGroup.clear();
                const std::vector<GameCore::EntityID> Roots = Document.GetSelectedRoots();
                if(Roots.size() > 1 && std::find(Roots.begin(), Roots.end(), GizmoEntity) != Roots.end())
                {
                    GameCore::World& World = Document.GetWorld();
                    const GameCore::TransformPose Start = World.GetEntity(GizmoEntity).GetWorldPose();
                    for(const GameCore::EntityID Each : Roots)
                    {
                        if(Each != GizmoEntity && World.GetEntity(Each).GetComponent<GameCore::TransformComponent>())
                        {
                            const GameCore::TransformPose Pose = World.GetEntity(Each).GetWorldPose();
                            GizmoGroup.push_back({ Each, GameCore::TransformMath::Relative(Start, Pose, Pose.Rotation) });
                        }
                    }
                    Document.BeginGroupEdit(Roots, GameCore::TransformComponent::StaticType().ID, Label);
                }
                else
                {
                    Document.BeginEditGesture(Label);
                }

                Drag        = EDrag::Gizmo;
                DragButton  = Qt::LeftButton;
                GizmoHover  = Handle;
                emit GizmoDragStatus(Gizmo.DescribeDrag());
                return;
            }
        }
    }
    switch(InEvent->button())
    {
        case Qt::LeftButton:   Drag = bAlt ? EDrag::Orbit : EDrag::None; break;
        case Qt::RightButton:  Drag = bAlt ? EDrag::Dolly : EDrag::Look; break;
        case Qt::MiddleButton: Drag = EDrag::Pan;                        break;
        default:               return;
    }
    DragButton    = InEvent->button();
    DragPivot     = GetPivot();
    bContextClick = InEvent->button() == Qt::RightButton && !bAlt;
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* InEvent)
{
    if(NimbleEngine::InputState* Game = Context.GetPlayFallbackInput())
    {
        /* Physical pixels, as SDL reports them. */
        const double Ratio = devicePixelRatioF();
        if(bHoldingMouse)
        {
            /* Held: each move is its distance from the centre, and the cursor
             * goes back there -- whose own move event reads as no distance. */
            const QPoint Centre = rect().center();
            const QPoint Moved  = InEvent->position().toPoint() - Centre;
            if(!Moved.isNull())
            {
                const QPoint At = (QPointF(Centre) * Ratio).toPoint();
                Game->MoveMouse(At.x(), At.y(), int(std::lround(Moved.x() * Ratio)), int(std::lround(Moved.y() * Ratio)));
                FallbackMousePos = At;
                QCursor::setPos(mapToGlobal(Centre));
            }
            return;
        }
        const QPoint At = (InEvent->position() * Ratio).toPoint();
        Game->MoveMouse(At.x(), At.y(), At.x() - FallbackMousePos.x(), At.y() - FallbackMousePos.y());
        FallbackMousePos = At;
    }
    /* The game's view while playing: the editor camera does not move. */
    if(!Context.IsEditorViewActive())
    {
        return;
    }
    const QPoint Pos   = InEvent->position().toPoint();
    const QPoint Delta = Pos - LastMousePos;
    LastMousePos = Pos;

    if(bContextClick && (Pos - PressPos).manhattanLength() > ClickSlopPixels)
    {
        bContextClick = false;   // it turned the camera
    }

    const QPointF At = InEvent->position();
    if(Drag == EDrag::Gizmo)
    {
        Gizmo.SetView(Camera, GetLogicalSize(), float(devicePixelRatioF()));
        ApplyGizmoPose(GizmoEntity, Gizmo.UpdateDrag(At.x(), At.y(), GetGizmoSnap(InEvent->modifiers())));
        emit GizmoDragStatus(Gizmo.DescribeDrag());
        return;
    }
    if(Drag == EDrag::None)
    {
        const GameCore::TransformComponent* Target = GetGizmoTarget();
        Gizmo.SetView(Camera, GetLogicalSize(), float(devicePixelRatioF()));
        GizmoHover = Target ? Gizmo.Hit(PoseOf(*Target), At.x(), At.y()) : TransformGizmo::EHandle::None;
    }

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
        case EDrag::Gizmo:
            break;
    }
}

void ViewportWidget::mouseReleaseEvent(QMouseEvent* InEvent)
{
    if(NimbleEngine::InputState* Game = Context.GetPlayFallbackInput())
    {
        if(const char* Name = QtButtonToInputName(InEvent->button())) { Game->Release(NimbleEngine::FindInputKey(Name)); }
    }
    /* The game's view while playing: the editor camera does not move. */
    if(!Context.IsEditorViewActive())
    {
        return;
    }
    if(Drag == EDrag::Gizmo)
    {
        if(InEvent->button() == DragButton)
        {
            FinishGizmoDrag(false);
        }
        return;   // a handle drag never selects
    }

    const bool bWasDragging = Drag != EDrag::None;
    if(InEvent->button() == DragButton)
    {
        Drag       = EDrag::None;
        DragButton = Qt::NoButton;
    }

    /* A right click that stayed put: the context menu. */
    if(InEvent->button() == Qt::RightButton && bContextClick)
    {
        bContextClick = false;
        ShowContextMenu(InEvent->position().toPoint());
        return;
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

    /* A prefab instance is selected whole, by its root. Ctrl toggles it in
     * or out of the selection, Shift adds it; a click on nothing with either
     * leaves the selection alone. */
    const GameCore::EntityID Target = Document.GetPickTarget(Pick(Pos));
    if(InEvent->modifiers() & Qt::ControlModifier)
    {
        if(Target.IsSet()) { Document.ToggleSelected(Target); }
    }
    else if(InEvent->modifiers() & Qt::ShiftModifier)
    {
        if(Target.IsSet()) { Document.AddToSelection(Target); }
    }
    else
    {
        Document.SetSelection(Target);
    }
}

void ViewportWidget::wheelEvent(QWheelEvent* InEvent)
{
    if(NimbleEngine::InputState* Game = Context.GetPlayFallbackInput())
    {
        Game->AddWheel(float(InEvent->angleDelta().x()) / 120.0f, float(InEvent->angleDelta().y()) / 120.0f);
    }
    /* The game's view while playing: the editor camera does not move. */
    if(!Context.IsEditorViewActive())
    {
        return;
    }
    const double Notches = double(InEvent->angleDelta().y()) / 120.0;
    Camera.Dolly(GetPivot(), Notches * double(Camera.FocusDistance) * DollyFractionPerNotch * double(Camera.SpeedScale));
}

void ViewportWidget::keyPressEvent(QKeyEvent* InEvent)
{
    if(InEvent->isAutoRepeat())
    {
        return;
    }
    /* Esc cancels a drag, else ends a run. Not a window shortcut, so text
     * fields keep Esc. */
    if(InEvent->key() == Qt::Key_Escape && Drag == EDrag::Gizmo)
    {
        FinishGizmoDrag(true);
        return;
    }
    if(InEvent->key() == Qt::Key_Escape && Context.IsPlaying())
    {
        Context.StopPlay();
        return;
    }
    if(NimbleEngine::InputState* Game = Context.GetPlayFallbackInput())
    {
        if(const char* Name = QtKeyToInputName(InEvent->key())) { Game->Press(NimbleEngine::FindInputKey(Name)); }
    }
    if(!Context.IsEditorViewActive())
    {
        return;
    }

    switch(InEvent->key())
    {
        case Qt::Key_F:    FrameSelection(); return;
        case Qt::Key_Home: FrameAll();       return;
        default:           break;
    }

    /* Handle modes on 1-4 (WASD and Q/E fly), except while dragging. Still
     * recorded as held. */
    const bool bPlainKey = (InEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == 0;
    if(bPlainKey && Drag != EDrag::Look && Drag != EDrag::Gizmo)
    {
        using EMode = TransformGizmo::EMode;
        switch(InEvent->key())
        {
            case Qt::Key_1: SetGizmoMode(EMode::None);      break;
            case Qt::Key_2: SetGizmoMode(EMode::Translate); break;
            case Qt::Key_3: SetGizmoMode(EMode::Rotate);    break;
            case Qt::Key_4: SetGizmoMode(EMode::Scale);     break;
            case Qt::Key_X:
                SetGizmoSpace(Gizmo.GetSpace() == TransformGizmo::ESpace::World ? TransformGizmo::ESpace::Local
                                                                               : TransformGizmo::ESpace::World);
                return;
            default: break;
        }
    }
    HeldKeys.insert(InEvent->key());
}

void ViewportWidget::keyReleaseEvent(QKeyEvent* InEvent)
{
    if(!InEvent->isAutoRepeat())
    {
        HeldKeys.remove(InEvent->key());
        if(NimbleEngine::InputState* Game = Context.GetPlayFallbackInput())
        {
            if(const char* Name = QtKeyToInputName(InEvent->key())) { Game->Release(NimbleEngine::FindInputKey(Name)); }
        }
    }
}

void ViewportWidget::focusOutEvent(QFocusEvent* InEvent)
{
    /* A release that lands in another widget would otherwise leave the
     * camera flying, or a drag stuck on. */
    FinishGizmoDrag(false);
    HeldKeys.clear();
    if(NimbleEngine::InputState* Game = Context.GetPlayFallbackInput())
    {
        Game->ReleaseAll();
    }
    Drag       = EDrag::None;
    DragButton = Qt::NoButton;
    ReleaseMouseHold();   // the cursor is the user's again; a click takes it back
    QWidget::focusOutEvent(InEvent);
}

// === Transform handles =======================================================

void ViewportWidget::SetGizmoMode(const TransformGizmo::EMode InMode)
{
    if(Gizmo.GetMode() == InMode)
    {
        return;
    }
    FinishGizmoDrag(false);
    Gizmo.SetMode(InMode);
    GizmoHover = TransformGizmo::EHandle::None;
    emit GizmoModeChanged(InMode);
}

void ViewportWidget::SetGizmoSpace(const TransformGizmo::ESpace InSpace)
{
    if(Gizmo.GetSpace() == InSpace)
    {
        return;
    }
    FinishGizmoDrag(false);
    Gizmo.SetSpace(InSpace);
    GizmoHover = TransformGizmo::EHandle::None;
    emit GizmoSpaceChanged(InSpace);
}

void ViewportWidget::SetGizmoSnap(const bool bInSnap)
{
    if(bGizmoSnap == bInSnap)
    {
        return;
    }
    bGizmoSnap = bInSnap;
    emit GizmoSnapChanged(bInSnap);
}

TransformGizmo::Snap ViewportWidget::GetGizmoSnap(const Qt::KeyboardModifiers InModifiers) const
{
    TransformGizmo::Snap Snap;
    Snap.bEnabled      = bGizmoSnap != ((InModifiers & Qt::ControlModifier) != 0);
    Snap.Translate     = double(GEditorSnapTranslate.GetFloat());
    Snap.RotateDegrees = double(GEditorSnapRotate.GetFloat());
    Snap.Scale         = double(GEditorSnapScale.GetFloat());
    return Snap;
}

GameCore::TransformComponent* ViewportWidget::GetGizmoTarget() const
{
    if(!Context.IsEditorViewActive() || Gizmo.GetMode() == TransformGizmo::EMode::None)
    {
        return nullptr;
    }
    const GameCore::EntityID Target = Drag == EDrag::Gizmo ? GizmoEntity : Document.GetSelection();
    GameCore::World& World = Document.GetWorld();
    if(!World.IsAlive(Target))
    {
        return nullptr;
    }
    return World.GetEntity(Target).GetComponent<GameCore::TransformComponent>();
}

void ViewportWidget::ApplyGizmoPose(const GameCore::EntityID InEntity, const TransformGizmo::Pose& InPose)
{
    GameCore::World& World = Document.GetWorld();
    if(!World.IsAlive(InEntity))
    {
        return;
    }
    /* The gizmo works in world space; the local values that land there. */
    GameCore::TransformPose Pose;
    Pose.Position = InPose.Position;
    Pose.Rotation = InPose.Rotation;
    Pose.Scale    = InPose.Scale;
    if(!World.GetEntity(InEntity).SetWorldPose(Pose))
    {
        return;
    }

    /* The render proxy picks the Transform up on its own next frame; the
     * bounds need a refit now, static or not, so the outline follows --
     * the children's too. */
    World.RefreshEntityBounds(InEntity);
    Document.NotifyPropertyEdited(InEntity, GameCore::TransformComponent::StaticType().ID);

    /* The rest of the group keeps where it stood relative to the dragged one. */
    if(InEntity == GizmoEntity)
    {
        for(const auto& [Each, Relative] : GizmoGroup)
        {
            if(World.IsAlive(Each) && World.GetEntity(Each).SetWorldPose(GameCore::TransformMath::Compose(Pose, Relative)))
            {
                World.RefreshEntityBounds(Each);
                Document.NotifyPropertyEdited(Each, GameCore::TransformComponent::StaticType().ID);
            }
        }
    }
}

void ViewportWidget::FinishGizmoDrag(const bool bInCancel)
{
    if(Drag != EDrag::Gizmo)
    {
        return;
    }
    /* A cancel writes the start pose back inside the gesture, so the entry
     * merges back to nothing and is dropped. */
    if(!GizmoGroup.empty())
    {
        Document.EndGroupEdit(bInCancel);   // a cancel puts every one back
    }
    else
    {
        if(bInCancel)
        {
            ApplyGizmoPose(GizmoEntity, Gizmo.GetDragStart());
        }
        Document.EndEditGesture();
    }
    GizmoGroup.clear();
    Gizmo.EndDrag();
    Drag        = EDrag::None;
    DragButton  = Qt::NoButton;
    GizmoEntity = GameCore::InvalidEntityID();
    emit GizmoDragStatus(QString());
}

// === Dropping a mesh =========================================================

/* The .nmmesh in a drag, inside the project, as its runtime path; else empty. */
QString ViewportWidget::DroppedMesh(const QMimeData* InData) const
{
    const QString File = EditorContent::SingleDroppedFile(InData);
    if(!File.endsWith(QStringLiteral(".nmmesh"), Qt::CaseInsensitive)) { return QString(); }
    return EditorContent::ToRuntimePath(File);
}

QString ViewportWidget::DroppedPrefab(const QMimeData* InData) const
{
    const QString File = EditorContent::SingleDroppedFile(InData);
    if(Document.IsSingleRoot() || !File.endsWith(QStringLiteral(".nmprefab"), Qt::CaseInsensitive)) { return QString(); }
    return EditorContent::ToRuntimePath(File).isEmpty() ? QString() : File;
}

QString ViewportWidget::DroppedParticleSystem(const QMimeData* InData) const
{
    return EditorContent::ParticleSystemPathFor(EditorContent::SingleDroppedFile(InData));
}

bool ViewportWidget::CanDrop(const QMimeData* InData) const
{
    return !Document.IsWorldBorrowed()
        && (!DroppedMesh(InData).isEmpty() || !DroppedPrefab(InData).isEmpty() || !DroppedParticleSystem(InData).isEmpty());
}

void ViewportWidget::dragEnterEvent(QDragEnterEvent* InEvent)
{
    if(CanDrop(InEvent->mimeData())) { InEvent->acceptProposedAction(); }
    else                             { InEvent->ignore(); }
}

void ViewportWidget::dragMoveEvent(QDragMoveEvent* InEvent)
{
    if(CanDrop(InEvent->mimeData())) { InEvent->acceptProposedAction(); }
    else                             { InEvent->ignore(); }
}

/* Creates (one undo step) and selects an entity drawing the mesh, running the
 * particle system, or an instance of the prefab: where the ray meets z = 0 if
 * reasonably near, else 600 units along it. */
void ViewportWidget::dropEvent(QDropEvent* InEvent)
{
    if(Document.IsWorldBorrowed())
    {
        InEvent->ignore();
        return;
    }

    EngineCore::Vector3 RayOrigin, RayDir;
    const QPointF At = InEvent->position();
    Camera.ScreenRay(At.x(), At.y(), GetLogicalSize(), RayOrigin, RayDir);
    double T = 600.0;
    if(std::abs(RayDir.GetZ()) > 1e-6)
    {
        const double OnPlane = -RayOrigin.GetZ() / RayDir.GetZ();
        if(OnPlane > 0.0 && OnPlane < 20000.0) { T = OnPlane; }
    }
    const EngineCore::Vector3 Spot = RayOrigin + RayDir * T;

    const QString PrefabFile = DroppedPrefab(InEvent->mimeData());
    if(!PrefabFile.isEmpty())
    {
        /* Through the registry, which reads the staged copy beside the exe. */
        EditorPrefabs::Stage(PrefabFile);
        const GameCore::EntityID Placed = Document.PlacePrefab(EditorContent::ToRuntimePath(PrefabFile), Spot);
        if(!Placed.IsSet())
        {
            InEvent->ignore();
            return;
        }
        Document.SetSelection(Placed);
        InEvent->acceptProposedAction();
        setFocus();
        return;
    }

    const QString Particles = DroppedParticleSystem(InEvent->mimeData());
    const QByteArray ParticlesUtf8 = Particles.toUtf8();
    if(!Particles.isEmpty() && ParticlesUtf8.size() < int(RenderCore::RENDER_PARTICLE_SYSTEM_PATH_MAX))
    {
        EditorContent::StageAsset(EditorContent::SingleDroppedFile(InEvent->mimeData()));
        const GameCore::EntityID Made = Document.CreateEntity(QFileInfo(Particles).completeBaseName(), Spot,
                                                             [&ParticlesUtf8](const GameCore::Entity& InEntity)
        {
            RenderCore::ParticleSystemComponent Component;
            std::memcpy(Component.System, ParticlesUtf8.constData(), size_t(ParticlesUtf8.size()));
            InEntity.AddComponent(Component);
        });
        Document.SetSelection(Made);
        InEvent->acceptProposedAction();
        setFocus();
        return;
    }

    const QString Path = DroppedMesh(InEvent->mimeData());
    const QByteArray Utf8 = Path.toUtf8();
    if(Path.isEmpty() || Utf8.size() >= int(RenderCore::RENDER_MESH_PATH_MAX))
    {
        InEvent->ignore();
        return;
    }

    const GameCore::EntityID Made = Document.CreateEntity(QFileInfo(Path).completeBaseName(), Spot,
                                                         [&Utf8](const GameCore::Entity& InEntity)
    {
        RenderCore::RenderComponent Render;
        std::memcpy(Render.Mesh, Utf8.constData(), size_t(Utf8.size()));
        InEntity.AddComponent(Render);
    });
    Document.SetSelection(Made);

    InEvent->acceptProposedAction();
    setFocus();
}

// === Picking =================================================================

GameCore::EntityID ViewportWidget::Pick(const QPoint& InLogicalPos) const
{
    EngineCore::Vector3 RayOrigin, RayDir;
    Camera.ScreenRay(InLogicalPos.x(), InLogicalPos.y(), GetLogicalSize(), RayOrigin, RayDir);

    GameCore::EntityID Best;
    double BestT = std::numeric_limits<double>::max();

    /* Ray vs each quad in its local space (inverse GetWorldMatrix, where it is
     * a Width x Height square at z = 0). t is preserved by the affine map. */
    Document.GetWorld().Each<RenderCore::RenderComponent>(
        [&](const GameCore::Entity& InEntity, RenderCore::RenderComponent& InRender)
    {
        if(!InRender.bVisible)
        {
            return;
        }
        const GameCore::TransformComponent* Transform = InEntity.GetComponent<GameCore::TransformComponent>();
        if(!Transform)
        {
            return;
        }
        Affine3x4 Inverse;
        if(!InvertAffine(InEntity.GetWorldMatrix(), Inverse))
        {
            return;   // degenerate -- drawn as a line at most, nothing to hit
        }
        const EngineCore::Vector3 LocalOrigin = Inverse.Apply(RayOrigin, 1.0);
        const EngineCore::Vector3 LocalDir    = Inverse.Apply(RayDir, 0.0);

        /* A mesh: its triangles, in the same local space, nearest hit wins.
         * One whose file cannot be loaded draws a quad, and is picked as one. */
        const std::string MeshPath(InRender.Mesh, strnlen(InRender.Mesh, RenderCore::RENDER_MESH_PATH_MAX));
        if(const std::shared_ptr<const RenderCore::MeshData> Mesh = MeshPath.empty() ? nullptr : RenderCore::MeshAssetCache::Find(MeshPath))
        {
            const double T = RayHitsMesh(*Mesh, LocalOrigin, LocalDir);
            if(T > 0.0 && T < BestT)
            {
                BestT = T;
                Best  = InEntity.GetID();
            }
            return;
        }

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
        if(std::abs(HitX) <= 0.5 && std::abs(HitY) <= 0.5)   // the unit quad
        {
            BestT = T;
            Best  = InEntity.GetID();
        }
    });

    /* A light has no shape to hit: its position, within a few pixels, unless
     * a mesh is nearer the camera. */
    const double PickX = InLogicalPos.x(), PickY = InLogicalPos.y();
    double BestPixels = LightPickPixels;
    GameCore::EntityID BestLight;
    const auto TryLight = [&](const GameCore::Entity& InEntity)
    {
        const EngineCore::Vector3 Position = InEntity.GetWorldPose().Position;
        double X = 0.0, Y = 0.0;
        if(!Camera.WorldToScreen(Position, GetLogicalSize(), X, Y))
        {
            return;
        }
        const double Pixels = std::hypot(X - PickX, Y - PickY);
        const double T      = (Position - RayOrigin).Dot(RayDir);
        if(Pixels <= BestPixels && T > 0.0 && T < BestT)
        {
            BestPixels = Pixels;
            BestLight  = InEntity.GetID();
        }
    };
    GameCore::World& World = Document.GetWorld();
    World.Each<RenderCore::PointLightComponent>([&](const GameCore::Entity& InEntity, RenderCore::PointLightComponent&) { TryLight(InEntity); });
    World.Each<RenderCore::SpotLightComponent>([&](const GameCore::Entity& InEntity, RenderCore::SpotLightComponent&) { TryLight(InEntity); });
    World.Each<RenderCore::DirectionalLightComponent>([&](const GameCore::Entity& InEntity, RenderCore::DirectionalLightComponent&) { TryLight(InEntity); });
    World.Each<RenderCore::SkyLightComponent>([&](const GameCore::Entity& InEntity, RenderCore::SkyLightComponent&) { TryLight(InEntity); });
    World.Each<RenderCore::SceneCaptureProbeComponent>([&](const GameCore::Entity& InEntity, RenderCore::SceneCaptureProbeComponent&)
    {
        TryLight(InEntity);
    });
    World.Each<RenderCore::ReflectionProbeComponent>([&](const GameCore::Entity& InEntity, RenderCore::ReflectionProbeComponent&)
    {
        TryLight(InEntity);
    });
    return BestLight.IsSet() ? BestLight : Best;
}

#endif // WITH_EDITOR
