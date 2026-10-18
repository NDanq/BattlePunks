#if WITH_EDITOR

#include "Private/Editor/Content/MeshPreviewView.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include "Private/Editor/Graph/GraphAssets.h"
#include "Public/Editor/EditorContext.h"

MeshPreviewView::MeshPreviewView(QWidget* InParent) : QWidget(InParent)
{
    setMinimumSize(96, 96);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Drawn by the viewport's renderer. Drag to orbit, wheel to zoom, double-click to reset"));

    Resized = new QTimer(this);
    Resized->setSingleShot(true);
    Resized->setInterval(120);
    connect(Resized, &QTimer::timeout, this, &MeshPreviewView::Refresh);

    /* Animating: the next request goes out this long after the last answer. */
    Animator = new QTimer(this);
    Animator->setSingleShot(true);
    Animator->setInterval(33);
    connect(Animator, &QTimer::timeout, this, &MeshPreviewView::Send);
}

void MeshPreviewView::SetMesh(const QString& InRuntimePath)
{
    Mesh = InRuntimePath;
    Refresh();
}

void MeshPreviewView::SetMaterial(const QString& InName)
{
    Material = InName;
    Refresh();
}

void MeshPreviewView::SetSections(const QStringList& InMaterials)
{
    Sections = InMaterials;
    Refresh();
}

void MeshPreviewView::SetTint(const QColor& InTint)
{
    Tint = InTint;
    Refresh();
}

void MeshPreviewView::SetAnimate(const bool bInAnimate)
{
    bAnimate = bInAnimate;
    if(bAnimate) { Refresh(); }
}

void MeshPreviewView::SetSkinPalette(std::vector<float> InPalette)
{
    SkinPalette = std::move(InPalette);
    Refresh();
}

void MeshPreviewView::SetFrameSource(std::function<void()> InSource)
{
    FrameSource = std::move(InSource);
    disconnect(FrameTick);
    FrameTick = QMetaObject::Connection();
    if(!FrameSource)
    {
        return;
    }
    /* Paced by the editor's frames: a request each, rendered with that frame. */
    if(EditorContext* Ctx = EditorGraphs::GetEditorContext())
    {
        FrameTick = connect(Ctx, &EditorContext::BeforeFrame, this, [this]() { Send(); });
    }
    Refresh();
}

void MeshPreviewView::SetSkeletonOverlay(std::vector<float> InSegments, const float InBoundsMin[3], const float InBoundsMax[3],
                                         std::vector<uint8_t> InMarked)
{
    Segments = std::move(InSegments);
    Marked   = std::move(InMarked);
    std::copy_n(InBoundsMin, 3, BoundsMin);
    std::copy_n(InBoundsMax, 3, BoundsMax);
    update();
}

bool MeshPreviewView::Project(const float* InPoint, const QRectF& InTarget, QPointF& OutPoint) const
{
    /* RenderContext's preview camera, restated: orbiting the bounds' centre,
     * far enough back to frame their sphere in a 40 degree field. */
    constexpr double Pi = 3.14159265358979;
    constexpr double HalfFov = 20.0 * Pi / 180.0;
    double Centre[3], Radius = 0.0;
    for(int Axis = 0; Axis < 3; ++Axis)
    {
        Centre[Axis] = (double(BoundsMin[Axis]) + BoundsMax[Axis]) * 0.5;
        const double Half = (double(BoundsMax[Axis]) - BoundsMin[Axis]) * 0.5;
        Radius += Half * Half;
    }
    Radius = std::max(std::sqrt(Radius), 0.01);
    const double Yaw   = double(ShownYaw) * Pi / 180.0;
    const double Pitch = double(std::clamp(ShownPitch, -85.0f, 85.0f)) * Pi / 180.0;
    const double Zoom  = double(std::clamp(ShownZoom, 0.1f, 20.0f));
    const double Forward[3] = { std::sin(Yaw) * std::cos(Pitch), -std::sin(Pitch), std::cos(Yaw) * std::cos(Pitch) };
    const double Distance = Radius / std::sin(HalfFov) * 1.1 / Zoom;

    /* +Y up; the image's right is forward x up (the renderer's LookAt). */
    double Right[3] = { -Forward[2], 0.0, Forward[0] };
    const double RightLength = std::max(std::sqrt(Right[0] * Right[0] + Right[2] * Right[2]), 1e-9);
    Right[0] /= RightLength;
    Right[2] /= RightLength;
    const double Up[3] = { Right[1] * Forward[2] - Right[2] * Forward[1], Right[2] * Forward[0] - Right[0] * Forward[2],
                           Right[0] * Forward[1] - Right[1] * Forward[0] };

    double Relative[3];
    for(int Axis = 0; Axis < 3; ++Axis) { Relative[Axis] = double(InPoint[Axis]) - (Centre[Axis] - Forward[Axis] * Distance); }
    const double Depth = Relative[0] * Forward[0] + Relative[1] * Forward[1] + Relative[2] * Forward[2];
    if(Depth <= 1e-3)
    {
        return false;
    }
    const double Aspect = InTarget.width() / std::max(InTarget.height(), 1.0);
    const double X = (Relative[0] * Right[0] + Relative[1] * Right[1] + Relative[2] * Right[2]) / (Depth * std::tan(HalfFov) * Aspect);
    const double Y = (Relative[0] * Up[0] + Relative[1] * Up[1] + Relative[2] * Up[2]) / (Depth * std::tan(HalfFov));
    OutPoint = QPointF(InTarget.left() + (X * 0.5 + 0.5) * InTarget.width(), InTarget.top() + (0.5 - Y * 0.5) * InTarget.height());
    return true;
}

void MeshPreviewView::ResetView()
{
    Yaw   = 30.0f;
    Pitch = 20.0f;
    Zoom  = 1.0f;
    Refresh();
}

void MeshPreviewView::Refresh()
{
    if(bInFrameSource)
    {
        return;   // the request being sent takes it
    }
    if(InFlight > 0)
    {
        bPending = !FrameSource;   // a FrameSource's next frame takes it
        return;
    }
    Send();
}

void MeshPreviewView::Send()
{
    /* A few out at once for a FrameSource: one a frame, each a frame or two to answer. */
    if(InFlight >= (FrameSource ? 3 : 1) || !isVisible())
    {
        bPending = !isVisible();   // drawn when shown
        return;
    }
    if(Mesh.isEmpty() && Material.isEmpty())
    {
        Image = QImage();
        Error = QStringLiteral("Nothing to preview");
        update();
        return;
    }

    /* The frame's pose, as of now: sent at once, it is never older than one round trip. */
    if(FrameSource)
    {
        const std::function<void()> Source = FrameSource;   // it may clear itself (a clip that stops)
        bInFrameSource = true;
        Source();
        bInFrameSource = false;
    }

    ++InFlight;
    bPending  = false;
    const uint64_t Serial = ++Sent;
    const qreal Ratio = devicePixelRatioF();
    const int   Width  = std::clamp(int(width() * Ratio), 32, 1024);
    const int   Height = std::clamp(int(height() * Ratio), 32, 1024);

    /* The overlay goes with the image it was sent for. */
    const std::vector<float> SentSegments = Segments;
    const std::vector<uint8_t> SentMarked = Marked;
    const float SentYaw = Yaw, SentPitch = Pitch, SentZoom = Zoom;
    const auto Done = [this, Serial, SentSegments, SentMarked, SentYaw, SentPitch, SentZoom](const QImage& InImage, const QString& InError)
    {
        --InFlight;
        if(Serial < Shown)
        {
            return;   // overtaken by a newer answer
        }
        Shown = Serial;
        Image = InImage;
        Error = InError;
        ShownSegments = SentSegments;
        ShownMarked   = SentMarked;
        ShownYaw   = SentYaw;
        ShownPitch = SentPitch;
        ShownZoom  = SentZoom;
        update();
        emit Drawn(InImage.isNull() ? InError : QString());

        /* A FrameSource with no editor frames to pace it draws again at once --
         * after a failure, not before the Animator's wait (a renderer that is
         * not running answers at once). */
        if(bPending)                        { Send(); }
        else if(FrameSource && !FrameTick)  { InImage.isNull() ? Animator->start() : Send(); }
        else if(bAnimate && !FrameSource)   { Animator->start(); }
    };

    if(Mesh.isEmpty())
    {
        EditorGraphs::RequestMaterialPreview(Material, std::min(Width, Height), Tint, this, Done);
        return;
    }
    EditorGraphs::MeshPreviewOptions Options;
    Options.Mesh     = Mesh;
    Options.Material = Material;
    Options.Sections = Sections;
    Options.Width    = Width;
    Options.Height   = Height;
    Options.Yaw      = Yaw;
    Options.Pitch    = Pitch;
    Options.Zoom     = Zoom;
    Options.Tint     = Tint;
    Options.SkinPalette = SkinPalette;
    EditorGraphs::RequestMeshPreview(Options, this, Done);
}

void MeshPreviewView::paintEvent(QPaintEvent*)
{
    QPainter Painter(this);
    Painter.fillRect(rect(), QColor(38, 40, 46));   // the renderer's clear, so a resize does not flash
    if(Image.isNull())
    {
        Painter.setPen(palette().color(QPalette::PlaceholderText));
        Painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap,
                         Error.isEmpty() ? QStringLiteral("Drawing...") : Error);
        return;
    }
    /* At its own size, centred; scaled down if the widget shrank since. */
    QSizeF Size = QSizeF(Image.size()) / devicePixelRatioF();
    if(Size.width() > width() || Size.height() > height()) { Size.scale(QSizeF(size()), Qt::KeepAspectRatio); }
    const QRectF Target(QPointF((width() - Size.width()) * 0.5, (height() - Size.height()) * 0.5), Size);
    Painter.setRenderHint(QPainter::SmoothPixmapTransform);
    Painter.drawImage(Target, Image);

    /* The skeleton, over everything: each bone a line to its parent, a dot at the joint. */
    if(!ShownSegments.empty())
    {
        Painter.setRenderHint(QPainter::Antialiasing);
        const QColor Bone(255, 196, 64), Flagged(240, 64, 64);
        for(size_t At = 0; At + 5 < ShownSegments.size(); At += 6)
        {
            const bool bMarked = At / 6 < ShownMarked.size() && ShownMarked[At / 6] != 0;
            Painter.setPen(QPen(bMarked ? Flagged : Bone, 1.6));
            Painter.setBrush(bMarked ? Flagged : Bone);
            QPointF Child, Parent;
            if(!Project(&ShownSegments[At], Target, Child)) { continue; }
            Painter.drawEllipse(Child, 2.2, 2.2);
            if(Project(&ShownSegments[At + 3], Target, Parent)) { Painter.drawLine(Child, Parent); }
        }
    }
}

void MeshPreviewView::resizeEvent(QResizeEvent* InEvent)
{
    QWidget::resizeEvent(InEvent);
    Resized->start();
}

void MeshPreviewView::showEvent(QShowEvent* InEvent)
{
    QWidget::showEvent(InEvent);
    if(bPending || Image.isNull()) { Refresh(); }
}

void MeshPreviewView::mousePressEvent(QMouseEvent* InEvent)
{
    if(InEvent->button() == Qt::LeftButton && !Mesh.isEmpty())
    {
        bDragging = true;
        LastMouse = InEvent->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
    }
}

void MeshPreviewView::mouseMoveEvent(QMouseEvent* InEvent)
{
    if(!bDragging) { return; }
    const QPoint Now   = InEvent->position().toPoint();
    const QPoint Delta = Now - LastMouse;
    LastMouse = Now;
    Yaw  -= float(Delta.x()) * 0.5f;
    Pitch = std::clamp(Pitch + float(Delta.y()) * 0.5f, -85.0f, 85.0f);
    Refresh();
}

void MeshPreviewView::mouseReleaseEvent(QMouseEvent* InEvent)
{
    if(InEvent->button() == Qt::LeftButton)
    {
        bDragging = false;
        setCursor(Qt::OpenHandCursor);
    }
}

void MeshPreviewView::mouseDoubleClickEvent(QMouseEvent*)
{
    ResetView();
}

void MeshPreviewView::wheelEvent(QWheelEvent* InEvent)
{
    if(Mesh.isEmpty()) { return; }
    const float Steps = float(InEvent->angleDelta().y()) / 120.0f;
    Zoom = std::clamp(Zoom * std::pow(1.15f, Steps), 0.2f, 10.0f);
    Refresh();
}

#endif // WITH_EDITOR
