#ifndef NIMBLEEDITOR_GRAPHEDITORWINDOW_H
#define NIMBLEEDITOR_GRAPHEDITORWINDOW_H

#if WITH_EDITOR

#include <QMainWindow>
#include <QTimer>

#include "Public/NodeGraph/GraphCompiler.h"

class GraphDetailsPanel;
class GraphDocument;
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
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHEDITORWINDOW_H
