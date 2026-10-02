#if WITH_EDITOR

#include "Private/Editor/Content/MeshPreviewView.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include "Private/Editor/Graph/GraphAssets.h"

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

void MeshPreviewView::ResetView()
{
    Yaw   = 30.0f;
    Pitch = 20.0f;
    Zoom  = 1.0f;
    Refresh();
}

void MeshPreviewView::Refresh()
{
    if(bInFlight)
    {
        bPending = true;
        return;
    }
    Send();
}

void MeshPreviewView::Send()
{
    if(bInFlight || !isVisible())
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

    bInFlight = true;
    bPending  = false;
    const qreal Ratio = devicePixelRatioF();
    const int   Width  = std::clamp(int(width() * Ratio), 32, 1024);
    const int   Height = std::clamp(int(height() * Ratio), 32, 1024);

    const auto Done = [this](const QImage& InImage, const QString& InError)
    {
        bInFlight = false;
        Image = InImage;
        Error = InError;
        update();
        emit Drawn(InImage.isNull() ? InError : QString());

        if(bPending)      { Send(); }
        else if(bAnimate) { Animator->start(); }
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
