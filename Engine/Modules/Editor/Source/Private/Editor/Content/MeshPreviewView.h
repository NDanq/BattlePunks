#ifndef NIMBLEEDITOR_MESHPREVIEWVIEW_H
#define NIMBLEEDITOR_MESHPREVIEWVIEW_H

#if WITH_EDITOR

#include <cstdint>
#include <functional>
#include <vector>

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QStringList>
#include <QWidget>

class QTimer;

/*
 * A mesh -- or a material -- as the viewport's renderer draws it, read back
 * into a widget (Renderer::RequestMeshPreview): the mesh viewer's and the
 * material Preview dock's. Drag to orbit, wheel to zoom, double-click to
 * reset. The image is drawn at the widget's size (up to 1024), one request
 * at a time: a change while one is out is sent when it returns.
 *
 * With no mesh it draws the material on a square quad (RequestMaterialPreview),
 * which is also what a PostProcess material always gets.
 */
class MeshPreviewView final : public QWidget
{
    Q_OBJECT

public:
    explicit MeshPreviewView(QWidget* InParent = nullptr);

    /* A runtime path ("Engine/Content/Meshes/Sphere.nmmesh"), or "" for the quad. */
    void SetMesh(const QString& InRuntimePath);

    /* Every section's material ("" uses SetSections'). */
    void SetMaterial(const QString& InName);

    /* Section i's material ("" keeps the mesh's own); at most 8 are previewed. */
    void SetSections(const QStringList& InMaterials);

    void SetTint(const QColor& InTint);

    /* Redraw continuously, for materials that move (Time). */
    void SetAnimate(bool bInAnimate);

    /* A skinned mesh's pose, as Renderer::MeshPreview takes it (12 floats a
     * bone); empty: its reference pose. Drawn with the next request. */
    void SetSkinPalette(std::vector<float> InPalette);

    /* While set, redraws every editor frame (EditorContext::BeforeFrame),
     * calling InSource just before each request: what it sets (a pose) is
     * as new as the image it goes with. Empty: redraws only on change. */
    void SetFrameSource(std::function<void()> InSource);

    /* Bones drawn over the image: segments, bone to parent, in model space
     * (6 floats each) -- framed as the renderer frames a mesh with these
     * bounds. Empty: none. */
    void SetSkeletonOverlay(std::vector<float> InSegments, const float InBoundsMin[3], const float InBoundsMax[3]);

    void ResetView();

    /* Draw again with the current settings (a recooked material, say). */
    void Refresh();

    QSize sizeHint() const override { return QSize(320, 240); }

signals:
    /* After each answer: "" when the image is new, else why there is none. */
    void Drawn(const QString& InError);

protected:
    void paintEvent(QPaintEvent* InEvent) override;
    void resizeEvent(QResizeEvent* InEvent) override;
    void showEvent(QShowEvent* InEvent) override;
    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;
    void mouseDoubleClickEvent(QMouseEvent* InEvent) override;
    void wheelEvent(QWheelEvent* InEvent) override;

private:
    void Send();

    /* Where a model-space point lands in InTarget, by the preview's camera
     * as it was for the image shown; false behind the camera. */
    bool Project(const float* InPoint, const QRectF& InTarget, QPointF& OutPoint) const;

    QString     Mesh;
    QString     Material;
    QStringList Sections;
    QColor      Tint = QColor(255, 255, 255);
    float       Yaw   = 30.0f;
    float       Pitch = 20.0f;
    float       Zoom  = 1.0f;

    int         InFlight  = 0;       // requests out; more than one only for a FrameSource
    uint64_t    Sent      = 0;       // requests made, and the newest one shown: an older answer is dropped
    uint64_t    Shown     = 0;
    bool        bPending  = false;   // something changed while a request was out
    bool        bAnimate  = false;
    bool        bInFrameSource = false;   // FrameSource is running: its changes join this request
    std::function<void()> FrameSource;
    QMetaObject::Connection FrameTick;
    bool        bDragging = false;
    QPoint      LastMouse;

    QImage      Image;
    QString     Error;

    std::vector<float> SkinPalette;
    std::vector<float> Segments;           // as set
    std::vector<float> ShownSegments;      // as they were for Image
    float       BoundsMin[3] = {0.0f, 0.0f, 0.0f};
    float       BoundsMax[3] = {0.0f, 0.0f, 0.0f};
    float       ShownYaw = 30.0f, ShownPitch = 20.0f, ShownZoom = 1.0f;
    QTimer*     Resized  = nullptr;  // coalesces a drag of the window's edge
    QTimer*     Animator = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_MESHPREVIEWVIEW_H
