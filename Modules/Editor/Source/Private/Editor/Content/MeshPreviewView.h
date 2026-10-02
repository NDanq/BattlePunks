#ifndef NIMBLEEDITOR_MESHPREVIEWVIEW_H
#define NIMBLEEDITOR_MESHPREVIEWVIEW_H

#if WITH_EDITOR

#include <cstdint>

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

    QString     Mesh;
    QString     Material;
    QStringList Sections;
    QColor      Tint = QColor(255, 255, 255);
    float       Yaw   = 30.0f;
    float       Pitch = 20.0f;
    float       Zoom  = 1.0f;

    bool        bInFlight = false;
    bool        bPending  = false;   // something changed while a request was out
    bool        bAnimate  = false;
    bool        bDragging = false;
    QPoint      LastMouse;

    QImage      Image;
    QString     Error;
    QTimer*     Resized  = nullptr;  // coalesces a drag of the window's edge
    QTimer*     Animator = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_MESHPREVIEWVIEW_H
