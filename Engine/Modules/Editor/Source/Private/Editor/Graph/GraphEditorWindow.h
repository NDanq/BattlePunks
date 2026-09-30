#ifndef NIMBLEEDITOR_GRAPHEDITORWINDOW_H
#define NIMBLEEDITOR_GRAPHEDITORWINDOW_H

#if WITH_EDITOR

#include <cstdint>

#include <QColor>
#include <QMainWindow>
#include <QTimer>

#include "Public/NodeGraph/GraphCompiler.h"

class GraphDetailsPanel;
class GraphDocument;
class QLabel;
class GraphScene;
class GraphView;
class QListWidget;
class QPlainTextEdit;

/*
 * One graph, in its own window: the canvas in the middle, Details on the
 * right, and below it the code the graph generates and the compiler's
 * messages -- both live, recompiled a moment after every change. Clicking a
 * line of code or a message selects the node it came from.
 *
 * Save (Ctrl+S) writes the .nmgraph and its output; a material's output is
 * then checked by the real shader compiler in the background.
 *
 * A MATERIAL has a Preview dock: the material on a quad, drawn by the
 * viewport's own renderer into a target of its own and read back -- the same
 * backend and cooked shader the level uses. With Material > Live Preview on
 * (the default) each clean recompile is cooked, so the dock AND every
 * primitive in the level naming this material show it -- saved or not.
 * Material > Apply to Selected Entity points the selection at it.
 */
class GraphEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    GraphEditorWindow(GraphDocument* InDocument, QWidget* InParent = nullptr);

    GraphDocument& GetDocument() const { return *Document; }

    bool Save();

    /* Offers to save unsaved changes. False if the user cancelled. */
    bool ConfirmClose();

protected:
    void closeEvent(QCloseEvent* InEvent) override;

private:
    void BuildActions();
    void Recompile();
    void UpdateTitle();
    void ShowPalette(const QPointF& InScenePos, bool bInFromPin, const NodeGraph::PinRef& InPin);
    void FocusNode(const NodeGraph::NodeGuid& InNode);

    void BuildPreviewDock();
    void RefreshPreview();

    void CopySelection();
    void PasteClipboard();
    void DeleteSelection();
    void AddCommentAroundSelection();
    void EditComment(int InIndex);
    void FrameAll();

    GraphDocument*     Document = nullptr;
    GraphScene*        Scene    = nullptr;
    GraphView*         View     = nullptr;
    GraphDetailsPanel* Details  = nullptr;
    QPlainTextEdit*    CodeView = nullptr;
    QListWidget*       Messages = nullptr;

    QTimer                   CompileTimer;
    NodeGraph::CompileResult LastResult;
    bool                     bClosingConfirmed = false;

    /* Material graphs: every clean recompile is cooked into the viewport
     * (EditorGraphs::StageMaterial). bPreviewedUnsaved is set when what the
     * viewport shows is not what is saved, so a discard can put it back. */
    bool bIsMaterial       = false;
    bool bLivePreview      = true;
    bool bPreviewedUnsaved = false;

    /* The Preview dock: the material drawn by the viewport's own renderer
     * (EditorGraphs::RequestMaterialPreview), refreshed after every cook.
     * PreviewGeneration drops an answer that a newer request overtook. */
    QLabel*   PreviewImage  = nullptr;
    QLabel*   PreviewStatus = nullptr;
    QColor    PreviewTint   = QColor(255, 255, 255);
    uint64_t  PreviewGeneration = 0;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHEDITORWINDOW_H
