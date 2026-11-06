#ifndef NIMBLEEDITOR_UIDESIGNERVIEWPORT_H
#define NIMBLEEDITOR_UIDESIGNERVIEWPORT_H

#if WITH_EDITOR

#include <QPointF>
#include <QSize>
#include <QWidget>

#include "Public/Editor/EditorContext.h"
#include "Public/RenderCore/UIDrawList.h"
#include "Public/UI/UISystem.h"
#include "Private/Editor/UI/UIPreviewRuntime.h"

class EditorDocument;
class UIDesignDocument;

/*
 * The UI Designer's canvas: the document drawn by the game's own UI renderer
 * (WYSIWYG -- the fonts, layout and batching a game gets), laid out at a
 * design resolution (a preset: 1920x1080, ultrawide, portrait...) and fitted
 * into the widget, with the tools drawn by the same renderer over it -- Qt
 * cannot paint over the native surface. It is an EditorViewportClient of an
 * empty World of its own, so it has a scene to draw its list into.
 *
 * DESIGN: click selects (anything shown, hit-test visibility aside); drag
 * moves a Canvas child (Position, or Margin on a stretched axis); the eight
 * handles resize one; dragging a box's child reorders it. Arrows nudge,
 * Delete, Ctrl+D, Ctrl+C / Ctrl+V; the middle button pans, the wheel zooms,
 * F fits. A widget type dropped from the palette is added where it lands.
 * Each gesture is one undo step, committed on release.
 *
 * PREVIEW: input goes to the document as in a game -- hover, press, focus,
 * navigation -- and its actions and UI graph run, in a scratch World and
 * script engine of the preview's own (UIPreviewRuntime): what a graph does
 * to config vars is undone when the preview stops. Saving the graph reloads it.
 */
class UIDesignerViewport : public QWidget, public EditorViewportClient
{
    Q_OBJECT

public:
    UIDesignerViewport(EditorContext& InContext, UIDesignDocument& InDesign, QWidget* InParent = nullptr);
    ~UIDesignerViewport() override;

    EditorDocument&          GetViewportDocument() override { return *SceneDocument; }
    EngineCore::WindowHandle GetViewportWindow() override;
    bool                     PrepareFrame(float InDeltaSeconds, bool bInEditorView, Frame& OutFrame) override;

    /* The resolution the document is laid out at. */
    void  SetDesignSize(const QSize& InSize);
    QSize GetDesignSize() const { return DesignSize; }

    /* Physical pixels per design pixel; 0 fits the frame in the view. */
    void  SetZoom(float InZoom);
    float GetZoom() const { return Zoom; }
    void  FitToView();

    void SetPreview(bool bInPreview);
    bool IsPreview() const { return bPreview; }

    void SetSnap(bool bInSnap) { bSnap = bInSnap; }
    bool IsSnap() const { return bSnap; }

    /* The scene it draws into, once it has drawn (EditorContext::RequestSceneCapture). */
    RenderCore::RenderSceneID GetScene() const { return Scene; }

    /* The preview's instance of the document (its widgets by asset index); null if it does not instance. */
    UI::UIDocumentInstance* GetInstance() const;

    QPaintEngine* paintEngine() const override { return nullptr; }

signals:
    void StatusText(const QString& InText);
    void ZoomChanged(float InScale);

protected:
    void paintEvent(QPaintEvent* InEvent) override;
    void mousePressEvent(QMouseEvent* InEvent) override;
    void mouseMoveEvent(QMouseEvent* InEvent) override;
    void mouseReleaseEvent(QMouseEvent* InEvent) override;
    void wheelEvent(QWheelEvent* InEvent) override;
    void keyPressEvent(QKeyEvent* InEvent) override;
    void leaveEvent(QEvent* InEvent) override;
    void dragEnterEvent(QDragEnterEvent* InEvent) override;
    void dragMoveEvent(QDragMoveEvent* InEvent) override;
    void dropEvent(QDropEvent* InEvent) override;

private:
    enum class EDrag { None, Move, Resize, Reorder, Pan };

    /* The document instanced again (after an edit). */
    void Rebuild();

    /* Every Switcher above the selection shows the page it is on -- in the
     * preview only; the document keeps its own ActiveIndex. */
    void RevealSelection();
    void ScrollToSelection();
    bool bScrollToSelection = false;

    /* The widget's size in physical pixels, and the frame's scale and top-left there. */
    EngineCore::Size GetPixelSize() const;
    float   GetFrameScale() const;
    QPointF GetFrameOrigin() const;

    /* A widget position (logical) to the frame's pixels, and to the document's layout units. */
    UI::UIVec2 ToFrame(const QPointF& InLogical) const;
    UI::UIVec2 ToLayout(const QPointF& InLogical) const;

    /* The selection's parent is a Canvas: it moves and resizes. Its parent's index. */
    bool IsCanvasChild(int InWidget) const;
    bool IsBoxChild(int InWidget, bool* bOutVertical = nullptr) const;

    /* The resize handle under InFrame (0-7 clockwise from the top-left); -1 none. */
    int HandleAt(const UI::UIVec2& InFrame) const;

    void BeginDrag(EDrag InDrag, QMouseEvent* InEvent, int InHandle);
    void UpdateDrag(const QPointF& InLogical);
    void FinishDrag(bool bInCommit);

    /* Where a dragged box child would land: its parent's child to go before (-1 last), and the line to draw. */
    void FindReorderTarget(const QPointF& InLogical, int& OutBefore, RenderCore::UIRect& OutLine) const;

    void Compose();
    void Nudge(float InX, float InY);

    EditorContext&    Context;
    UIDesignDocument& Design;
    EditorDocument*   SceneDocument = nullptr;

    RenderCore::RenderSceneID Scene = RenderCore::RENDER_INVALID_SCENE;
    UI::UISystem         System;
    UI::UIDocumentHandle Handle = UI::UI_DOCUMENT_INVALID;

    /* The Preview's scripts: the document's logic runs while previewing. */
    UIPreviewRuntime Runtime;
    uint32_t         ErrorsShown = 0;

    RenderCore::UIDrawList List;
    uint64_t               ComposedSerial = ~uint64_t(0);
    bool                   bComposeDirty  = true;

    QSize   DesignSize{ 1920, 1080 };
    float   Zoom = 0.0f;
    QPointF Pan;
    bool    bPreview = false;
    bool    bSnap    = true;
    int     Hovered  = -1;

    // The gesture in progress.
    EDrag   Drag       = EDrag::None;
    int     DragHandle = -1;
    QPointF DragStart;
    QPointF PanStart;
    int     DragWidget = -1;
    UI::UIVec2   StartPosition, StartSize;
    UI::UIMargin StartMargin;
    std::vector<UI::UIPropertyText> DragValues;   // what the gesture set, committed on release
    int                 ReorderBefore = -1;
    RenderCore::UIRect  ReorderLine;
    bool                bDragMoved = false;

    // Preview input, gathered between frames.
    UI::UIInputFrame PendingInput;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIDESIGNERVIEWPORT_H
