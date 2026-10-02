#ifndef NIMBLEEDITOR_GRAPHEDITORTAB_H
#define NIMBLEEDITOR_GRAPHEDITORTAB_H

#if WITH_EDITOR

#include <cstdint>

#include <QColor>
#include <QMainWindow>
#include <QTimer>

#include "Public/NodeGraph/GraphCompiler.h"

class GraphDetailsPanel;
class GraphDocument;
class MeshPreviewView;
class QComboBox;
class QLabel;
class GraphScene;
class GraphView;
class QListWidget;
class QPlainTextEdit;

/*
 * One graph tab with its own menus and docks: canvas, Details, live
 * generated code and messages (click to select the source node). Save writes
 * the graph and output; material shaders are then checked in the background.
 *
 * Materials get a Preview dock rendered by the viewport's renderer -- on one
 * of the engine's meshes (Sphere, Box, Plane) to orbit, or a flat quad. Live
 * Preview (default on) cooks each clean recompile into the level too.
 */
class GraphEditorTab : public QMainWindow
{
    Q_OBJECT

public:
    GraphEditorTab(GraphDocument* InDocument, QWidget* InParent = nullptr);
    ~GraphEditorTab() override;

    GraphDocument& GetDocument() const { return *Document; }

    bool Save();

    /* Offers to save unsaved changes. False if the user cancelled. */
    bool ConfirmClose();

    /* "Glow.material.nmgraph*": the tab's text. */
    QString GetTabTitle() const;

signals:
    /* Saved, or edited since: the tab's title changed. */
    void TitleChanged();

    /* File > Close: the window asks ConfirmClose, then removes the tab. */
    void CloseRequested();

private:
    void BuildActions();
    void Recompile();
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

    /* bPreviewedUnsaved: the viewport shows unsaved changes, to restore on discard. */
    bool bIsMaterial       = false;
    bool bLivePreview      = true;
    bool bPreviewedUnsaved = false;

    /* Refreshed after every cook. */
    MeshPreviewView* PreviewView   = nullptr;
    QComboBox*       PreviewShape  = nullptr;
    QLabel*          PreviewStatus = nullptr;
    QColor           PreviewTint   = QColor(255, 255, 255);
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHEDITORTAB_H
