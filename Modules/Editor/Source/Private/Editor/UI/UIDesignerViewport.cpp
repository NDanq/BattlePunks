#if WITH_EDITOR

#include "Private/Editor/UI/UIDesignerViewport.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>

#include "Public/Editor/EditorDocument.h"
#include "Public/RenderCore/Renderer.h"
#include "Public/RenderCore/RenderView.h"
#include "Public/UI/UIWidgets.h"
#include "Private/Editor/UI/UIDesignDocument.h"
#include "Private/Editor/UI/UIPalettePanel.h"

namespace
{
    using RenderCore::PackUIColor;

    const uint32_t BACKDROP   = PackUIColor(0.075f, 0.08f, 0.095f, 1.0f);
    const uint32_t FRAME_FILL = PackUIColor(0.20f, 0.22f, 0.26f, 1.0f);
    const uint32_t FRAME_EDGE = PackUIColor(1.0f, 1.0f, 1.0f, 0.25f);
    const uint32_t HOVER      = PackUIColor(0.45f, 0.75f, 1.0f, 0.9f);
    const uint32_t SELECTED   = PackUIColor(1.0f, 0.62f, 0.15f, 1.0f);
    const uint32_t PARENT     = PackUIColor(1.0f, 1.0f, 1.0f, 0.35f);
    const uint32_t HANDLE     = PackUIColor(1.0f, 1.0f, 1.0f, 1.0f);
    const uint32_t ANCHOR     = PackUIColor(0.4f, 1.0f, 0.55f, 0.95f);
    const uint32_t INSERT     = PackUIColor(0.4f, 1.0f, 0.55f, 1.0f);

    constexpr float HANDLE_PIXELS = 7.0f;
    constexpr float GRID          = 4.0f;   // layout units a dragged position snaps to

    /* Handles 0-7, clockwise from the top-left: which edges each moves (-1 low, 1 high, 0 neither). */
    const int HANDLE_X[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };
    const int HANDLE_Y[8] = { -1, -1, -1, 0, 1, 1, 1, 0 };

    float Snapped(const float InValue, const bool bInSnap)
    {
        return bInSnap ? std::round(InValue / GRID) * GRID : std::round(InValue);
    }
}

UIDesignerViewport::UIDesignerViewport(EditorContext& InContext, UIDesignDocument& InDesign, QWidget* InParent)
    : QWidget(InParent)
    , Context(InContext)
    , Design(InDesign)
{
    /* A native child window the renderer presents to, as ViewportWidget's. */
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setMinimumSize(160, 120);

    /* An empty World: the scene the list is drawn into. */
    SceneDocument = new EditorDocument(this);

    System.SetExecuteActions(false);   // the preview logs what a click would do
    connect(&Design, &UIDesignDocument::Changed, this, [this](bool) { Rebuild(); });
    connect(&Design, &UIDesignDocument::SelectionChanged, this, [this](int) { bComposeDirty = true; });
    Rebuild();

    Context.AddViewport(this);
}

UIDesignerViewport::~UIDesignerViewport()
{
    /* While the native window exists: its swapchain goes with it. */
    Context.RemoveViewport(this);
}

EngineCore::WindowHandle UIDesignerViewport::GetViewportWindow()
{
    return EngineCore::WindowHandle(reinterpret_cast<void*>(winId()));
}

UI::UIDocumentInstance* UIDesignerViewport::GetInstance() const
{
    return System.Find(Handle);
}

void UIDesignerViewport::Rebuild()
{
    /* Not by path: the designer's copy is the edited one, not the cache's. */
    System.CloseAll(true);
    Handle = System.OpenAsset(Design.GetSharedAsset(), std::string());
    Hovered = -1;
    bComposeDirty = true;
    if(Handle == UI::UI_DOCUMENT_INVALID)
    {
        emit StatusText(QStringLiteral("The document cannot be shown as it is; the Console says why"));
    }
}

void UIDesignerViewport::SetDesignSize(const QSize& InSize)
{
    if(InSize.width() <= 0 || InSize.height() <= 0) return;
    DesignSize = InSize;
    bComposeDirty = true;
}

void UIDesignerViewport::SetZoom(const float InZoom)
{
    Zoom = std::max(InZoom, 0.0f);
    if(Zoom == 0.0f) Pan = QPointF();
    bComposeDirty = true;
    emit ZoomChanged(GetFrameScale());
}

void UIDesignerViewport::FitToView()
{
    SetZoom(0.0f);
}

void UIDesignerViewport::SetPreview(const bool bInPreview)
{
    FinishDrag(false);
    bPreview = bInPreview;
    if(UI::UIDocumentInstance* Instance = GetInstance())
    {
        Instance->SetHovered(-1);
        Instance->SetPressed(-1);
    }
    Hovered = -1;
    bComposeDirty = true;
}

// === Geometry =================================================================

EngineCore::Size UIDesignerViewport::GetPixelSize() const
{
    const qreal Ratio = devicePixelRatioF();
    return EngineCore::Size(int(std::lround(width() * Ratio)), int(std::lround(height() * Ratio)));
}

float UIDesignerViewport::GetFrameScale() const
{
    if(Zoom > 0.0f) return Zoom;
    const EngineCore::Size Pixels = GetPixelSize();
    const float Fit = std::min(float(Pixels.Width) * 0.92f / float(DesignSize.width()),
                               float(Pixels.Height) * 0.92f / float(DesignSize.height()));
    return std::max(Fit, 0.01f);
}

QPointF UIDesignerViewport::GetFrameOrigin() const
{
    const EngineCore::Size Pixels = GetPixelSize();
    const float Scale = GetFrameScale();
    return QPointF(std::round((float(Pixels.Width) - float(DesignSize.width()) * Scale) * 0.5f + Pan.x()),
                   std::round((float(Pixels.Height) - float(DesignSize.height()) * Scale) * 0.5f + Pan.y()));
}

UI::UIVec2 UIDesignerViewport::ToFrame(const QPointF& InLogical) const
{
    const qreal Ratio = devicePixelRatioF();
    const QPointF Origin = GetFrameOrigin();
    return UI::UIVec2{ float(InLogical.x() * Ratio - Origin.x()), float(InLogical.y() * Ratio - Origin.y()) };
}

UI::UIVec2 UIDesignerViewport::ToLayout(const QPointF& InLogical) const
{
    const UI::UIVec2 Frame = ToFrame(InLogical);
    const UI::UIDocumentInstance* Instance = GetInstance();
    const float Scale = Instance && Instance->GetScale() > 0.0f ? Instance->GetScale() : 1.0f;
    return UI::UIVec2{ Frame.X / Scale, Frame.Y / Scale };
}

bool UIDesignerViewport::IsCanvasChild(const int InWidget) const
{
    const UI::UIDocumentInstance* Instance = GetInstance();
    if(!Instance || InWidget <= 0 || InWidget >= int(Instance->GetWidgetCount())) return false;
    return dynamic_cast<const UI::UICanvas*>(Instance->GetWidget(uint32_t(Instance->GetParent(uint32_t(InWidget))))) != nullptr;
}

bool UIDesignerViewport::IsBoxChild(const int InWidget, bool* bOutVertical) const
{
    const UI::UIDocumentInstance* Instance = GetInstance();
    if(!Instance || InWidget <= 0 || InWidget >= int(Instance->GetWidgetCount())) return false;
    const UI::UIWidget* Parent = Instance->GetWidget(uint32_t(Instance->GetParent(uint32_t(InWidget))));
    const bool bVertical = dynamic_cast<const UI::UIVerticalBox*>(Parent) != nullptr;
    if(bOutVertical) *bOutVertical = bVertical;
    return bVertical || dynamic_cast<const UI::UIHorizontalBox*>(Parent) != nullptr;
}

int UIDesignerViewport::HandleAt(const UI::UIVec2& InFrame) const
{
    const int Selected = Design.GetSelected();
    const UI::UIDocumentInstance* Instance = GetInstance();
    if(bPreview || !Instance || !IsCanvasChild(Selected)) return -1;
    const RenderCore::UIRect R = Instance->GetPixelRect(uint32_t(Selected));
    const float X[3] = { R.X, R.X + R.W * 0.5f, R.X + R.W };
    const float Y[3] = { R.Y, R.Y + R.H * 0.5f, R.Y + R.H };
    const float Reach = HANDLE_PIXELS * float(devicePixelRatioF());
    for(int Each = 0; Each < 8; ++Each)
    {
        const float HX = X[HANDLE_X[Each] + 1], HY = Y[HANDLE_Y[Each] + 1];
        if(std::fabs(InFrame.X - HX) <= Reach && std::fabs(InFrame.Y - HY) <= Reach) return Each;
    }
    return -1;
}

// === Frame ====================================================================

void UIDesignerViewport::paintEvent(QPaintEvent* /*InEvent*/)
{
    /* Nothing: the frame timer presents. */
}

bool UIDesignerViewport::PrepareFrame(const float InDeltaSeconds, const bool /*bInEditorView*/, Frame& OutFrame)
{
    if(!isVisible()) return false;
    const EngineCore::Size Pixels = GetPixelSize();
    if(Pixels.IsEmpty()) return false;

    const float Scale = GetFrameScale();
    if(bPreview)
    {
        UI::UIInputFrame Input = PendingInput;
        PendingInput = UI::UIInputFrame();
        PendingInput.MousePixels = Input.MousePixels;
        PendingInput.bHasMouse   = Input.bHasMouse;
        for(int Each = 0; Each < 3; ++Each) { PendingInput.MouseDown[Each] = Input.MouseDown[Each]; }
        System.ProcessInput(Input);
        const size_t Logged = System.GetActionLog().size();
        System.RunQueuedActions();
        if(System.GetActionLog().size() != Logged)
        {
            emit StatusText(QStringLiteral("Preview: %1 (logged, not run)").arg(QString::fromStdString(System.GetActionLog().back())));
        }
    }
    System.Tick(InDeltaSeconds);
    System.Update(UI::UIVec2{ float(DesignSize.width()) * Scale, float(DesignSize.height()) * Scale });

    /* Atlases first: the list may name a font baked this frame. An unchanged
     * frame submits nothing -- the scene keeps the list it has. */
    if(RenderCore::Renderer* Renderer = Context.GetRenderer())
    {
        System.GetFonts().Upload(*Renderer);
    }
    if(bComposeDirty || ComposedSerial != System.GetDrawListSerial())
    {
        Compose();
    }

    OutFrame.Pixels = Pixels;
    OutFrame.View   = RenderCore::RenderView::MakeOrthographic(Pixels, EngineCore::Vector3(0.0, 0.0, -10.0), -1.0f, 1000.0f);
    OutFrame.Window = GetViewportWindow();
    return true;
}

void UIDesignerViewport::Compose()
{
    RenderCore::Renderer* Renderer = Context.GetRenderer();
    Scene = Context.GetOverlayScene();
    if(!Renderer || Scene == RenderCore::RENDER_INVALID_SCENE) return;

    bComposeDirty  = false;
    ComposedSerial = System.GetDrawListSerial();

    const EngineCore::Size Pixels = GetPixelSize();
    const float   Scale  = GetFrameScale();
    const QPointF Origin = GetFrameOrigin();
    const float   OX = float(Origin.x()), OY = float(Origin.y());
    const float   Ratio = float(devicePixelRatioF());
    const float   Line = std::max(1.0f, std::round(Ratio));

    List.Reset();
    List.AddRect({ 0.0f, 0.0f, float(Pixels.Width), float(Pixels.Height) }, BACKDROP);
    const RenderCore::UIRect FrameRect{ OX, OY, std::round(float(DesignSize.width()) * Scale), std::round(float(DesignSize.height()) * Scale) };
    List.AddRect(FrameRect, FRAME_FILL);

    /* The document, clipped to its frame as a screen would. */
    List.PushClip(FrameRect);
    List.Append(System.GetDrawList(), OX, OY);
    List.PopClip();
    List.AddOutline({ FrameRect.X - Line, FrameRect.Y - Line, FrameRect.W + 2 * Line, FrameRect.H + 2 * Line }, Line, FRAME_EDGE);

    const UI::UIDocumentInstance* Instance = GetInstance();
    if(Instance && !bPreview)
    {
        const auto Moved = [OX, OY](const RenderCore::UIRect& InRect)
        {
            return RenderCore::UIRect{ InRect.X + OX, InRect.Y + OY, InRect.W, InRect.H };
        };
        const int Selected = Design.GetSelected();
        if(Hovered >= 0 && Hovered != Selected && Hovered < int(Instance->GetWidgetCount()))
        {
            List.AddOutline(Moved(Instance->GetPixelRect(uint32_t(Hovered))), Line, HOVER);
        }
        if(Selected >= 0 && Selected < int(Instance->GetWidgetCount()))
        {
            const RenderCore::UIRect R = Moved(Instance->GetPixelRect(uint32_t(Selected)));
            if(Selected > 0)
            {
                const int32_t Parent = Instance->GetParent(uint32_t(Selected));
                List.AddOutline(Moved(Instance->GetPixelRect(uint32_t(Parent))), Line, PARENT);
            }
            List.AddOutline({ R.X - Line, R.Y - Line, R.W + 2 * Line, R.H + 2 * Line }, 2.0f * Line, SELECTED);

            if(IsCanvasChild(Selected))
            {
                /* Handles, and the anchors in the parent: where it is pinned. */
                const float H = HANDLE_PIXELS * Ratio;
                const float X[3] = { R.X, R.X + R.W * 0.5f, R.X + R.W };
                const float Y[3] = { R.Y, R.Y + R.H * 0.5f, R.Y + R.H };
                for(int Each = 0; Each < 8; ++Each)
                {
                    const float HX = X[HANDLE_X[Each] + 1], HY = Y[HANDLE_Y[Each] + 1];
                    List.AddRect({ HX - H * 0.5f - Line, HY - H * 0.5f - Line, H + 2 * Line, H + 2 * Line }, SELECTED);
                    List.AddRect({ HX - H * 0.5f, HY - H * 0.5f, H, H }, HANDLE);
                }
                const UI::UIWidget& W = *Instance->GetWidget(uint32_t(Selected));
                const RenderCore::UIRect P = Moved(Instance->GetPixelRect(uint32_t(Instance->GetParent(uint32_t(Selected)))));
                const float AX[2] = { P.X + P.W * W.Anchors.Left, P.X + P.W * W.Anchors.Right };
                const float AY[2] = { P.Y + P.H * W.Anchors.Top,  P.Y + P.H * W.Anchors.Bottom };
                const float A = 5.0f * Ratio;
                for(int I = 0; I < 2; ++I)
                {
                    for(int J = 0; J < 2; ++J) { List.AddRect({ AX[I] - A * 0.5f, AY[J] - A * 0.5f, A, A }, ANCHOR); }
                }
            }
        }
        if(Drag == EDrag::Reorder && ReorderLine.W > 0.0f)
        {
            List.AddRect(Moved(ReorderLine), INSERT);
        }
    }

    Renderer->SubmitUIDrawList(Scene, List);
}

// === Mouse ====================================================================

void UIDesignerViewport::mousePressEvent(QMouseEvent* InEvent)
{
    setFocus();
    const QPointF At = InEvent->position();
    if(InEvent->button() == Qt::MiddleButton)
    {
        BeginDrag(EDrag::Pan, InEvent, -1);
        return;
    }
    if(bPreview)
    {
        const int Button = InEvent->button() == Qt::LeftButton ? 0 : InEvent->button() == Qt::RightButton ? 1 : -1;
        if(Button >= 0)
        {
            PendingInput.MousePressed[Button] = true;
            PendingInput.MouseDown[Button]    = true;
        }
        return;
    }
    if(InEvent->button() != Qt::LeftButton) return;

    const UI::UIVec2 Frame = ToFrame(At);
    const int Handle = HandleAt(Frame);
    if(Handle >= 0)
    {
        BeginDrag(EDrag::Resize, InEvent, Handle);
        return;
    }
    const UI::UIDocumentInstance* Instance = GetInstance();
    const bool bInFrame = Frame.X >= 0 && Frame.Y >= 0 && Frame.X < float(DesignSize.width()) * GetFrameScale()
                       && Frame.Y < float(DesignSize.height()) * GetFrameScale();
    int Hit = Instance && bInFrame ? Instance->HitTest(Frame, true) : -1;
    if(Hit < 0 && bInFrame) Hit = 0;   // the frame itself: the root
    Design.Select(Hit);

    if(IsCanvasChild(Hit))   { BeginDrag(EDrag::Move, InEvent, -1); }
    else if(IsBoxChild(Hit)) { BeginDrag(EDrag::Reorder, InEvent, -1); }
}

void UIDesignerViewport::mouseMoveEvent(QMouseEvent* InEvent)
{
    const QPointF At = InEvent->position();
    if(bPreview)
    {
        PendingInput.MousePixels = ToFrame(At);
        PendingInput.bHasMouse   = true;
        if(Drag == EDrag::Pan) UpdateDrag(At);
        return;
    }
    if(Drag != EDrag::None)
    {
        UpdateDrag(At);
        return;
    }

    const UI::UIDocumentInstance* Instance = GetInstance();
    const UI::UIVec2 Frame = ToFrame(At);
    const int Handle = HandleAt(Frame);
    static const Qt::CursorShape HANDLE_CURSORS[8] = { Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                                       Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor };
    setCursor(Handle >= 0 ? HANDLE_CURSORS[Handle] : Qt::ArrowCursor);

    const int Hit = Instance ? Instance->HitTest(Frame, true) : -1;
    if(Hit != Hovered)
    {
        Hovered = Hit;
        bComposeDirty = true;
        if(Instance && Hit >= 0)
        {
            const UI::UIWidget* W = Instance->GetWidget(uint32_t(Hit));
            emit StatusText(QStringLiteral("%1 '%2'").arg(QString::fromStdString(W->GetType()->Name), QString::fromStdString(W->Name)));
        }
    }
}

void UIDesignerViewport::mouseReleaseEvent(QMouseEvent* InEvent)
{
    if(InEvent->button() == Qt::MiddleButton && Drag == EDrag::Pan)
    {
        FinishDrag(true);
        return;
    }
    if(bPreview)
    {
        const int Button = InEvent->button() == Qt::LeftButton ? 0 : InEvent->button() == Qt::RightButton ? 1 : -1;
        if(Button >= 0)
        {
            PendingInput.MouseReleased[Button] = true;
            PendingInput.MouseDown[Button]     = false;
        }
        return;
    }
    if(InEvent->button() == Qt::LeftButton) FinishDrag(true);
}

void UIDesignerViewport::wheelEvent(QWheelEvent* InEvent)
{
    /* About the cursor: the point under it stays under it. */
    const float Before = GetFrameScale();
    const float Steps  = float(InEvent->angleDelta().y()) / 120.0f;
    const float After  = std::clamp(Before * std::pow(1.15f, Steps), 0.05f, 8.0f);
    const qreal Ratio  = devicePixelRatioF();
    const QPointF Cursor = InEvent->position() * Ratio;
    const QPointF Origin = GetFrameOrigin();
    const QPointF Design = (Cursor - Origin) / qreal(Before);
    Zoom = After;
    const QPointF NewOrigin = GetFrameOrigin();
    Pan += (Cursor - Design * qreal(After)) - NewOrigin;
    bComposeDirty = true;
    emit ZoomChanged(After);
}

void UIDesignerViewport::leaveEvent(QEvent* /*InEvent*/)
{
    if(bPreview) { PendingInput.bHasMouse = false; }
    if(Hovered >= 0) { Hovered = -1; bComposeDirty = true; }
}

// === Gestures =================================================================

void UIDesignerViewport::BeginDrag(const EDrag InDrag, QMouseEvent* InEvent, const int InHandle)
{
    Drag       = InDrag;
    DragHandle = InHandle;
    DragStart  = InEvent->position();
    PanStart   = Pan;
    DragWidget = Design.GetSelected();
    bDragMoved = false;
    DragValues.clear();
    ReorderBefore = -1;
    ReorderLine   = RenderCore::UIRect();
    if(const UI::UIDocumentInstance* Instance = GetInstance(); Instance && DragWidget >= 0 && DragWidget < int(Instance->GetWidgetCount()))
    {
        const UI::UIWidget& W = *Instance->GetWidget(uint32_t(DragWidget));
        StartPosition = W.Position;
        StartSize     = W.Size;
        StartMargin   = W.Margin;
    }
}

void UIDesignerViewport::UpdateDrag(const QPointF& InLogical)
{
    if(Drag == EDrag::Pan)
    {
        if(Zoom == 0.0f) Zoom = GetFrameScale();   // panning leaves Fit
        Pan = PanStart + (InLogical - DragStart) * devicePixelRatioF();
        bComposeDirty = true;
        return;
    }
    UI::UIDocumentInstance* Instance = GetInstance();
    if(!Instance || DragWidget < 0 || DragWidget >= int(Instance->GetWidgetCount())) return;
    if(!bDragMoved && (InLogical - DragStart).manhattanLength() < 3.0) return;   // a click, not a drag
    bDragMoved = true;

    if(Drag == EDrag::Reorder)
    {
        RenderCore::UIRect Line;
        FindReorderTarget(InLogical, ReorderBefore, Line);
        ReorderLine = Line;
        bComposeDirty = true;
        return;
    }

    UI::UIWidget& W = *Instance->GetWidget(uint32_t(DragWidget));
    const float Scale = Instance->GetScale() > 0.0f ? Instance->GetScale() : 1.0f;
    const float DX = float((InLogical.x() - DragStart.x()) * devicePixelRatioF()) / Scale;
    const float DY = float((InLogical.y() - DragStart.y()) * devicePixelRatioF()) / Scale;

    UI::UIVec2   Position = StartPosition, Size = StartSize;
    UI::UIMargin Margin   = StartMargin;
    const bool   bPointX = W.Anchors.Left == W.Anchors.Right, bPointY = W.Anchors.Top == W.Anchors.Bottom;
    const auto Axis = [this](const bool bInPoint, const int InEdge, const float InDelta, const float InPivot,
                             float& InOutPosition, float& InOutSize, float& InOutMarginLow, float& InOutMarginHigh,
                             const float InStartPosition, const float InStartSize, const float InStartLow, const float InStartHigh)
    {
        if(InEdge == 0) return;
        if(bInPoint)
        {
            if(InEdge < 0)
            {
                InOutSize     = std::max(1.0f, Snapped(InStartSize - InDelta, bSnap));
                const float Moved = InStartSize - InOutSize;
                InOutPosition = InStartPosition + Moved * (1.0f - InPivot);
            }
            else
            {
                InOutSize     = std::max(1.0f, Snapped(InStartSize + InDelta, bSnap));
                InOutPosition = InStartPosition + (InOutSize - InStartSize) * InPivot;
            }
        }
        else if(InEdge < 0) { InOutMarginLow  = Snapped(InStartLow + InDelta, bSnap); }
        else                { InOutMarginHigh = Snapped(InStartHigh - InDelta, bSnap); }
    };

    if(Drag == EDrag::Move)
    {
        if(bPointX) Position.X = Snapped(StartPosition.X + DX, bSnap);
        else        { const float D = Snapped(DX, bSnap); Margin.Left = StartMargin.Left + D; Margin.Right = StartMargin.Right - D; }
        if(bPointY) Position.Y = Snapped(StartPosition.Y + DY, bSnap);
        else        { const float D = Snapped(DY, bSnap); Margin.Top = StartMargin.Top + D; Margin.Bottom = StartMargin.Bottom - D; }
    }
    else if(Drag == EDrag::Resize && DragHandle >= 0)
    {
        Axis(bPointX, HANDLE_X[DragHandle], DX, W.Pivot.X, Position.X, Size.X, Margin.Left, Margin.Right,
             StartPosition.X, StartSize.X, StartMargin.Left, StartMargin.Right);
        Axis(bPointY, HANDLE_Y[DragHandle], DY, W.Pivot.Y, Position.Y, Size.Y, Margin.Top, Margin.Bottom,
             StartPosition.Y, StartSize.Y, StartMargin.Top, StartMargin.Bottom);
    }

    /* Live, on the preview's instance; the document hears of it on release. */
    DragValues.clear();
    if(Position != StartPosition) DragValues.push_back({ "Position", UI::FormatVec2(Position) });
    if(Size != StartSize)         DragValues.push_back({ "Size", UI::FormatVec2(Size) });
    if(Margin != StartMargin)     DragValues.push_back({ "Margin", UI::FormatMargin(Margin) });
    Instance->SetProperty(W, "Position", UI::UIValue::Vec2(Position));
    Instance->SetProperty(W, "Size", UI::UIValue::Vec2(Size));
    Instance->SetProperty(W, "Margin", UI::UIValue::Margin(Margin));
    bComposeDirty = true;
    emit StatusText(QStringLiteral("Position %1  Size %2").arg(QString::fromStdString(UI::FormatVec2(Position)),
                                                               QString::fromStdString(UI::FormatVec2(Size))));
}

void UIDesignerViewport::FinishDrag(const bool bInCommit)
{
    const EDrag Was = Drag;
    Drag = EDrag::None;
    if(Was == EDrag::None || Was == EDrag::Pan || !bDragMoved)
    {
        bComposeDirty = true;
        return;
    }
    if(!bInCommit)
    {
        Rebuild();   // the live values go
        return;
    }
    if(Was == EDrag::Reorder)
    {
        const UI::UIDocumentAsset& Asset = Design.GetAsset();
        if(DragWidget > 0 && DragWidget < int(Asset.Widgets.size()))
        {
            Design.MoveWidget(DragWidget, Asset.Widgets[size_t(DragWidget)].Parent, ReorderBefore);
        }
        ReorderLine = RenderCore::UIRect();
        bComposeDirty = true;
        return;
    }
    if(DragValues.empty()) return;
    const int Widget = DragWidget;
    const std::vector<UI::UIPropertyText> Values = DragValues;
    Design.Edit(Was == EDrag::Resize ? QStringLiteral("Resize") : QStringLiteral("Move"), [Widget, Values](UI::UIDocumentAsset& InAsset)
    {
        for(const UI::UIPropertyText& Each : Values) { InAsset.Widgets[size_t(Widget)].Set(Each.Name, Each.Value); }
    });
}

void UIDesignerViewport::FindReorderTarget(const QPointF& InLogical, int& OutBefore, RenderCore::UIRect& OutLine) const
{
    OutBefore = -1;
    OutLine   = RenderCore::UIRect();
    const UI::UIDocumentInstance* Instance = GetInstance();
    bool bVertical = false;
    if(!Instance || !IsBoxChild(DragWidget, &bVertical)) return;

    const UI::UIVec2 Frame = ToFrame(InLogical);
    const int32_t Parent = Instance->GetParent(uint32_t(DragWidget));
    const RenderCore::UIRect P = Instance->GetPixelRect(uint32_t(Parent));
    const float Thick = 3.0f * float(devicePixelRatioF());
    float Edge = bVertical ? P.Y + P.H : P.X + P.W;
    for(int32_t Child = Instance->GetFirstChild(uint32_t(Parent)); Child >= 0; Child = Instance->GetNextSibling(uint32_t(Child)))
    {
        if(Child == DragWidget) continue;
        const RenderCore::UIRect R = Instance->GetPixelRect(uint32_t(Child));
        const float Centre = bVertical ? R.Y + R.H * 0.5f : R.X + R.W * 0.5f;
        if((bVertical ? Frame.Y : Frame.X) < Centre)
        {
            OutBefore = Child;
            Edge = bVertical ? R.Y : R.X;
            break;
        }
    }
    OutLine = bVertical ? RenderCore::UIRect{ P.X, Edge - Thick * 0.5f, P.W, Thick }
                        : RenderCore::UIRect{ Edge - Thick * 0.5f, P.Y, Thick, P.H };
}

// === Keys =====================================================================

void UIDesignerViewport::Nudge(const float InX, const float InY)
{
    const int Selected = Design.GetSelected();
    const UI::UIDocumentInstance* Instance = GetInstance();
    if(!Instance || !IsCanvasChild(Selected)) return;
    const UI::UIWidget& W = *Instance->GetWidget(uint32_t(Selected));
    if(W.Anchors.Left != W.Anchors.Right || W.Anchors.Top != W.Anchors.Bottom) return;
    const UI::UIVec2 Position{ W.Position.X + InX, W.Position.Y + InY };
    Design.Edit(QStringLiteral("Nudge"), [Selected, Position](UI::UIDocumentAsset& InAsset)
    {
        InAsset.Widgets[size_t(Selected)].Set("Position", UI::FormatVec2(Position));
    }, QStringLiteral("nudge:%1").arg(Selected));
}

void UIDesignerViewport::keyPressEvent(QKeyEvent* InEvent)
{
    if(bPreview)
    {
        switch(InEvent->key())
        {
            case Qt::Key_Up:     PendingInput.bNavUp    = true; break;
            case Qt::Key_Down:   PendingInput.bNavDown  = true; break;
            case Qt::Key_Left:   PendingInput.bNavLeft  = true; break;
            case Qt::Key_Right:  PendingInput.bNavRight = true; break;
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Space:  PendingInput.bAccept   = true; break;
            case Qt::Key_Escape: PendingInput.bBack     = true; break;
            case Qt::Key_Tab:    PendingInput.bNext     = true; break;
            case Qt::Key_Backtab: PendingInput.bPrevious = true; break;
            default: QWidget::keyPressEvent(InEvent); return;
        }
        return;
    }

    const bool  bControl = InEvent->modifiers() & Qt::ControlModifier;
    const float Step     = (InEvent->modifiers() & Qt::ShiftModifier) ? 10.0f : 1.0f;
    switch(InEvent->key())
    {
        case Qt::Key_Delete:
        case Qt::Key_Backspace: Design.DeleteSelected(); return;
        case Qt::Key_Left:  Nudge(-Step, 0.0f); return;
        case Qt::Key_Right: Nudge(Step, 0.0f);  return;
        case Qt::Key_Up:    Nudge(0.0f, -Step); return;
        case Qt::Key_Down:  Nudge(0.0f, Step);  return;
        case Qt::Key_F:     FitToView(); return;
        case Qt::Key_Escape:
            if(Drag != EDrag::None) { FinishDrag(false); return; }
            if(Design.GetSelected() > 0) { Design.Select(Design.GetAsset().Widgets[size_t(Design.GetSelected())].Parent); }
            return;
        default: break;
    }
    if(bControl && InEvent->key() == Qt::Key_D) { Design.DuplicateSelected(); return; }
    if(bControl && InEvent->key() == Qt::Key_C) { QApplication::clipboard()->setText(Design.CopySelected()); return; }
    if(bControl && InEvent->key() == Qt::Key_V)
    {
        if(!Design.Paste(QApplication::clipboard()->text())) emit StatusText(QStringLiteral("Nothing to paste here: the clipboard holds no widget, or there is no room"));
        return;
    }
    QWidget::keyPressEvent(InEvent);
}

// === Drops from the palette ===================================================

void UIDesignerViewport::dragEnterEvent(QDragEnterEvent* InEvent)
{
    if(!bPreview && InEvent->mimeData()->hasFormat(UIPalettePanel::MIME_TYPE)) InEvent->acceptProposedAction();
}

void UIDesignerViewport::dragMoveEvent(QDragMoveEvent* InEvent)
{
    if(!bPreview && InEvent->mimeData()->hasFormat(UIPalettePanel::MIME_TYPE)) InEvent->acceptProposedAction();
}

void UIDesignerViewport::dropEvent(QDropEvent* InEvent)
{
    const QString Type = QString::fromUtf8(InEvent->mimeData()->data(UIPalettePanel::MIME_TYPE));
    const UI::UIDocumentInstance* Instance = GetInstance();
    if(Type.isEmpty() || !Instance) return;

    /* Into what it lands on, or the nearest ancestor with room. */
    const UI::UIDocumentAsset& Asset = Design.GetAsset();
    int Parent = Instance->HitTest(ToFrame(InEvent->position()), true);
    while(Parent > 0 && !Asset.CanTakeChild(Parent)) { Parent = Asset.Widgets[size_t(Parent)].Parent; }
    if(Parent < 0) Parent = 0;
    if(!Asset.CanTakeChild(Parent))
    {
        emit StatusText(QStringLiteral("No room for a %1 there").arg(Type));
        return;
    }

    std::vector<UI::UIPropertyText> Properties;
    if(dynamic_cast<const UI::UICanvas*>(Instance->GetWidget(uint32_t(Parent))))
    {
        const UI::UIVec2 Layout = ToLayout(InEvent->position());
        const RenderCore::UIRect P = Instance->GetRect(uint32_t(Parent));
        Properties.push_back({ "Position", UI::FormatVec2(UI::UIVec2{ Snapped(Layout.X - P.X, bSnap), Snapped(Layout.Y - P.Y, bSnap) }) });
    }
    if(Type == QStringLiteral("Text")) Properties.push_back({ "Text", "Text" });
    if(Design.AddWidget(Type, Parent, -1, Properties) < 0)
    {
        emit StatusText(QStringLiteral("A %1 cannot go there").arg(Type));
    }
    InEvent->acceptProposedAction();
}

#endif // WITH_EDITOR
