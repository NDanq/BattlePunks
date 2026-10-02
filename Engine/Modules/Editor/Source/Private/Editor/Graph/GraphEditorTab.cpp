#if WITH_EDITOR

#include "Private/Editor/Graph/GraphEditorTab.h"

#include <functional>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDockWidget>
#include <QFileInfo>
#include <QFontDatabase>
#include <QInputDialog>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTextBlock>
#include <QUndoStack>

#include "Public/Core/CoreLog.h"
#include "Public/NodeGraph/GraphFile.h"
#include "Private/Editor/Content/MeshPreviewView.h"
#include "Private/Editor/Graph/GraphAssets.h"
#include "Private/Editor/Graph/GraphDetailsPanel.h"
#include "Private/Editor/Graph/GraphDocument.h"
#include "Private/Editor/Graph/GraphEditorWindow.h"
#include "Private/Editor/Graph/GraphItems.h"
#include "Private/Editor/Graph/GraphScene.h"
#include "Private/Editor/Graph/GraphView.h"
#include "Private/Editor/Graph/NodePalette.h"

using namespace NodeGraph;
using EditorGraphs::ToQ;
using EditorGraphs::ToStd;

namespace
{
    constexpr int CompileDelayMs = 250;
    constexpr int PreviewPixels  = 256;

    /* The Preview dock's shapes: the engine's meshes (staged beside every
     * executable), and the flat quad the preview drew before them. */
    struct PreviewShapeChoice { const char* Label; const char* Mesh; };
    const PreviewShapeChoice PreviewShapes[] = {
        { "Sphere", "Engine/Content/Meshes/Sphere.nmmesh" },
        { "Box",    "Engine/Content/Meshes/Box.nmmesh" },
        { "Plane",  "Engine/Content/Meshes/Plane.nmmesh" },
        { "Quad",   "" },
    };
    constexpr int NodeRole = Qt::UserRole;

    /* A GUID in a QVariant, as its text. */
    QString GuidText(const NodeGuid& InGuid) { return InGuid.IsSet() ? ToQ(GuidToString(InGuid)) : QString(); }
}

GraphEditorTab::GraphEditorTab(GraphDocument* InDocument, QWidget* InParent)
    : QMainWindow(InParent, Qt::Widget)
    , Document(InDocument)
{
    Document->setParent(this);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);

    Scene = new GraphScene(*Document, this);
    View  = new GraphView(Scene, this);
    setCentralWidget(View);

    auto* DetailsDock = new QDockWidget(QStringLiteral("Details"), this);
    DetailsDock->setObjectName(QStringLiteral("GraphDetailsDock"));
    Details = new GraphDetailsPanel(*Document, *Scene, DetailsDock);
    DetailsDock->setWidget(Details);
    addDockWidget(Qt::RightDockWidgetArea, DetailsDock);

    auto* CodeDock = new QDockWidget(QStringLiteral("Generated %1").arg(ToQ(Document->GetSchema().GetOutputExtension())), this);
    CodeDock->setObjectName(QStringLiteral("GraphCodeDock"));
    CodeView = new QPlainTextEdit(CodeDock);
    CodeView->setReadOnly(true);
    CodeView->setLineWrapMode(QPlainTextEdit::NoWrap);
    CodeView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    CodeDock->setWidget(CodeView);
    addDockWidget(Qt::BottomDockWidgetArea, CodeDock);

    auto* MessagesDock = new QDockWidget(QStringLiteral("Messages"), this);
    MessagesDock->setObjectName(QStringLiteral("GraphMessagesDock"));
    Messages = new QListWidget(MessagesDock);
    MessagesDock->setWidget(Messages);
    addDockWidget(Qt::BottomDockWidgetArea, MessagesDock);
    splitDockWidget(CodeDock, MessagesDock, Qt::Horizontal);

    resizeDocks({ DetailsDock }, { 320 }, Qt::Horizontal);
    resizeDocks({ CodeDock }, { 230 }, Qt::Vertical);
    resizeDocks({ CodeDock, MessagesDock }, { 700, 400 }, Qt::Horizontal);

    bIsMaterial = Document->GetSchema().GetName() == "Material";
    if(bIsMaterial)
    {
        BuildPreviewDock();
        /* Over Details, on the right: the picture first. */
        splitDockWidget(findChild<QDockWidget*>(QStringLiteral("GraphPreviewDock")), DetailsDock, Qt::Vertical);
    }

    BuildActions();

    // --- Wiring ------------------------------------------------------------------
    CompileTimer.setSingleShot(true);
    CompileTimer.setInterval(CompileDelayMs);
    connect(&CompileTimer, &QTimer::timeout, this, &GraphEditorTab::Recompile);
    connect(Document, &GraphDocument::Changed, this, [this]() { CompileTimer.start(); });
    connect(Document, &GraphDocument::DirtyChanged, this, &GraphEditorTab::TitleChanged);

    connect(Scene, &GraphScene::PaletteRequested, this, &GraphEditorTab::ShowPalette);
    connect(Scene, &GraphScene::CommentEditRequested, this, &GraphEditorTab::EditComment);
    connect(Scene, &GraphScene::DeleteRequested, this, &GraphEditorTab::DeleteSelection);

    /* A line of code, or a message, leads to its node. */
    connect(CodeView, &QPlainTextEdit::cursorPositionChanged, this, [this]()
    {
        const int Line = CodeView->textCursor().blockNumber();
        if(Line >= 0 && Line < int(LastResult.LineNodes.size()) && LastResult.LineNodes[size_t(Line)].IsSet())
        {
            FocusNode(LastResult.LineNodes[size_t(Line)]);
        }
    });
    connect(Messages, &QListWidget::itemActivated, this, [this](QListWidgetItem* InItem)
    {
        NodeGuid Node;
        if(GuidFromString(ToStd(InItem->data(NodeRole).toString()), Node)) { FocusNode(Node); }
    });
    connect(Messages, &QListWidget::itemClicked, Messages, &QListWidget::itemActivated);

    Recompile();
    QTimer::singleShot(0, this, &GraphEditorTab::FrameAll);
}

void GraphEditorTab::BuildActions()
{
    QUndoStack& History = Document->GetHistory();

    // --- File --------------------------------------------------------------------
    QMenu* File = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* SaveAction = File->addAction(QStringLiteral("&Save"));
    SaveAction->setShortcut(QKeySequence::Save);
    connect(SaveAction, &QAction::triggered, this, &GraphEditorTab::Save);

    QAction* Reveal = File->addAction(QStringLiteral("Copy &Path"));
    connect(Reveal, &QAction::triggered, this, [this]() { QApplication::clipboard()->setText(Document->GetPath()); });

    File->addSeparator();
    QAction* Close = File->addAction(QStringLiteral("&Close"));
    Close->setShortcut(QKeySequence::Close);
    connect(Close, &QAction::triggered, this, &GraphEditorTab::CloseRequested);

    // --- Edit ----------------------------------------------------------------------
    QMenu* Edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    QAction* Undo = History.createUndoAction(this, QStringLiteral("&Undo"));
    Undo->setShortcut(QKeySequence::Undo);
    QAction* Redo = History.createRedoAction(this, QStringLiteral("&Redo"));
    Redo->setShortcuts({ QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y) });
    Edit->addAction(Undo);
    Edit->addAction(Redo);
    Edit->addSeparator();

    /* Canvas-only: these must not steal Delete or Ctrl+C from a text field in
     * the Details dock, so they live on the view, active while it has focus. */
    const auto CanvasAction = [&](const QString& InText, const QKeySequence& InKey, auto InSlot) -> QAction*
    {
        auto* Action = new QAction(InText, View);
        Action->setShortcut(InKey);
        Action->setShortcutContext(Qt::WidgetShortcut);
        connect(Action, &QAction::triggered, this, InSlot);
        View->addAction(Action);
        Edit->addAction(Action);
        return Action;
    };
    CanvasAction(QStringLiteral("Cu&t"), QKeySequence::Cut, [this]() { CopySelection(); DeleteSelection(); });
    CanvasAction(QStringLiteral("&Copy"), QKeySequence::Copy, &GraphEditorTab::CopySelection);
    CanvasAction(QStringLiteral("&Paste"), QKeySequence::Paste, &GraphEditorTab::PasteClipboard);
    CanvasAction(QStringLiteral("D&uplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), [this]() { CopySelection(); PasteClipboard(); });
    QAction* Delete = CanvasAction(QStringLiteral("&Delete"), QKeySequence::Delete, &GraphEditorTab::DeleteSelection);
    auto* Backspace = new QAction(View);   // Delete's second key, not listed twice in the menu
    Backspace->setShortcut(Qt::Key_Backspace);
    Backspace->setShortcutContext(Qt::WidgetShortcut);
    connect(Backspace, &QAction::triggered, Delete, &QAction::trigger);
    View->addAction(Backspace);
    CanvasAction(QStringLiteral("Select &All"), QKeySequence::SelectAll, [this]()
    {
        for(QGraphicsItem* Item : Scene->items()) { Item->setSelected(Item->flags() & QGraphicsItem::ItemIsSelectable); }
    });

    Edit->addSeparator();
    CanvasAction(QStringLiteral("Add &Node..."), QKeySequence(Qt::Key_Tab), [this]()
    {
        ShowPalette(View->CursorScenePos(), false, PinRef());
    });
    CanvasAction(QStringLiteral("Add Co&mment"), QKeySequence(Qt::Key_C), &GraphEditorTab::AddCommentAroundSelection);

    // --- Material ------------------------------------------------------------------
    if(bIsMaterial)
    {
        QMenu* Material = menuBar()->addMenu(QStringLiteral("&Material"));

        QAction* Live = Material->addAction(QStringLiteral("&Live Preview"));
        Live->setCheckable(true);
        Live->setChecked(bLivePreview);
        Live->setToolTip(QStringLiteral("Cook every clean recompile into the viewport, so primitives using this material "
                                        "show the change at once -- before it is saved"));
        connect(Live, &QAction::toggled, this, [this](const bool bInOn)
        {
            bLivePreview = bInOn;
            if(bInOn) { Recompile(); }
        });

        QAction* Apply = Material->addAction(QStringLiteral("&Apply to Selected Entity"));
        Apply->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
        Apply->setToolTip(QStringLiteral("Set the selected entity's RenderComponent Material to this one (adds a RenderComponent if it has none)"));
        connect(Apply, &QAction::triggered, this, [this]()
        {
            QString Message;
            EditorGraphs::ApplyMaterialToSelection(EditorGraphs::GetMaterialName(Document->GetPath()), Message);
            statusBar()->showMessage(Message, 6000);
        });

        Material->addSeparator();
        QAction* Name = Material->addAction(QStringLiteral("RenderComponents refer to it as \"%1\"")
                                            .arg(EditorGraphs::GetMaterialName(Document->GetPath())));
        Name->setEnabled(false);
    }

    // --- View ----------------------------------------------------------------------
    QMenu* ViewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QAction* Frame = ViewMenu->addAction(QStringLiteral("&Frame All"));
    Frame->setShortcut(QKeySequence(Qt::Key_Home));
    connect(Frame, &QAction::triggered, this, &GraphEditorTab::FrameAll);
    QAction* FrameSelected = ViewMenu->addAction(QStringLiteral("Frame &Selection"));
    FrameSelected->setShortcut(QKeySequence(Qt::Key_F));
    FrameSelected->setShortcutContext(Qt::WidgetShortcut);
    View->addAction(FrameSelected);
    connect(FrameSelected, &QAction::triggered, this, [this]()
    {
        QRectF Bounds;
        for(QGraphicsItem* Item : Scene->selectedItems()) { Bounds |= Item->sceneBoundingRect(); }
        if(Bounds.isEmpty()) { FrameAll(); }
        else                 { View->Frame(Bounds); }
    });
    ViewMenu->addSeparator();
    for(QDockWidget* Dock : findChildren<QDockWidget*>()) { ViewMenu->addAction(Dock->toggleViewAction()); }
}

// === Compile ====================================================================

void GraphEditorTab::Recompile()
{
    LastResult = CompileResult();
    CompileGraph(Document->GetGraph(), LastResult);

    /* Keep the reader's place in the code across recompiles. Blocked, so
     * restoring the cursor does not count as clicking a line. */
    const int Scroll = CodeView->verticalScrollBar()->value();
    {
        const QSignalBlocker Block(CodeView);
        CodeView->setPlainText(LastResult.HasErrors() ? QStringLiteral("-- The graph has errors; see Messages.")
                                                      : ToQ(LastResult.Code));
    }
    CodeView->verticalScrollBar()->setValue(Scroll);

    Messages->clear();
    for(const Diagnostic& Each : LastResult.Diagnostics)
    {
        const QString Prefix = Each.Severity == ESeverity::Error ? QStringLiteral("Error")
                             : Each.Severity == ESeverity::Warning ? QStringLiteral("Warning") : QStringLiteral("Note");
        auto* Item = new QListWidgetItem(QStringLiteral("%1: %2").arg(Prefix, ToQ(Each.Message)), Messages);
        Item->setData(NodeRole, GuidText(Each.Node));
        if(Each.Severity == ESeverity::Error)   { Item->setForeground(QColor(0xE0, 0x50, 0x50)); }
        if(Each.Severity == ESeverity::Warning) { Item->setForeground(QColor(0xD0, 0x90, 0x30)); }
    }
    if(LastResult.Diagnostics.empty())
    {
        new QListWidgetItem(QStringLiteral("Compiled cleanly."), Messages);
    }

    Scene->SetDiagnostics(LastResult);

    if(bIsMaterial && bLivePreview && !LastResult.HasErrors())
    {
        bPreviewedUnsaved = Document->IsDirty();
        const QString Name = EditorGraphs::GetMaterialName(Document->GetPath());
        EditorGraphs::StageMaterial(Name, LastResult, this, [this, Name](const bool bInWorked, const QString& InWhat)
        {
            auto* Item = new QListWidgetItem(QStringLiteral("Preview: %1 %2").arg(Name, InWhat), Messages);
            if(!bInWorked)
            {
                Item->setForeground(QColor(0xE0, 0x50, 0x50));
                PreviewStatus->setText(QStringLiteral("Not updated: %1").arg(InWhat));
                return;
            }
            RefreshPreview();
        });
    }
}

// === Preview ====================================================================

GraphEditorTab::~GraphEditorTab()
{
    /* Qt deletes children in the order they were made -- the document first,
     * then the scene -- but the panels read both: deleting a selected item
     * emits selectionChanged into Details. Views go first, the document last. */
    CompileTimer.stop();
    delete Details;
    Details = nullptr;
    delete View;
    View = nullptr;
    delete Scene;
    Scene = nullptr;
    delete Document;
    Document = nullptr;
}

void GraphEditorTab::BuildPreviewDock()
{
    auto* Dock = new QDockWidget(QStringLiteral("Preview"), this);
    Dock->setObjectName(QStringLiteral("GraphPreviewDock"));

    auto* Page   = new QWidget(Dock);
    auto* Layout = new QVBoxLayout(Page);

    auto* ShapeRow = new QHBoxLayout();
    PreviewShape = new QComboBox(Page);
    for(const PreviewShapeChoice& Each : PreviewShapes)
    {
        PreviewShape->addItem(QString::fromLatin1(Each.Label), QString::fromLatin1(Each.Mesh));
    }
    PreviewShape->setToolTip(QStringLiteral("What the material is drawn on: one of the engine's meshes, or a flat quad. "
                                            "A PostProcess material always fills the image"));
    ShapeRow->addWidget(new QLabel(QStringLiteral("Shape"), Page));
    ShapeRow->addWidget(PreviewShape);
    auto* Animate = new QCheckBox(QStringLiteral("Animate"), Page);
    Animate->setToolTip(QStringLiteral("Keep drawing, for materials that move with Time"));
    ShapeRow->addWidget(Animate);
    ShapeRow->addStretch(1);
    Layout->addLayout(ShapeRow);

    PreviewView = new MeshPreviewView(Page);
    PreviewView->setObjectName(QStringLiteral("GraphPreviewView"));
    PreviewView->setMinimumSize(PreviewPixels, PreviewPixels);
    PreviewView->SetMesh(PreviewShape->currentData().toString());
    PreviewView->SetTint(PreviewTint);
    Layout->addWidget(PreviewView, 1);
    connect(PreviewShape, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
    {
        PreviewView->SetMesh(PreviewShape->currentData().toString());
    });
    connect(Animate, &QCheckBox::toggled, PreviewView, &MeshPreviewView::SetAnimate);

    PreviewStatus = new QLabel(QStringLiteral("Waiting for the first cook..."), Page);
    PreviewStatus->setWordWrap(true);
    PreviewStatus->setForegroundRole(QPalette::PlaceholderText);
    Layout->addWidget(PreviewStatus);

    auto* Row = new QHBoxLayout();
    auto* Tint = new QPushButton(QStringLiteral("Instance Tint..."), Page);
    Tint->setToolTip(QStringLiteral("The tint the preview passes to the Instance Tint input "
                                    "(in the level, white)"));
    connect(Tint, &QPushButton::clicked, this, [this]()
    {
        const QColor Picked = QColorDialog::getColor(PreviewTint, this, QStringLiteral("Instance Tint"),
                                                     QColorDialog::ShowAlphaChannel);
        if(Picked.isValid())
        {
            PreviewTint = Picked;
            PreviewView->SetTint(PreviewTint);
        }
    });
    Row->addWidget(Tint);

    auto* Refresh = new QPushButton(QStringLiteral("Refresh"), Page);
    Refresh->setToolTip(QStringLiteral("Draw the preview again (the last cooked version)"));
    connect(Refresh, &QPushButton::clicked, this, &GraphEditorTab::RefreshPreview);
    Row->addWidget(Refresh);
    Row->addStretch(1);
    Layout->addLayout(Row);

    connect(PreviewView, &MeshPreviewView::Drawn, this, [this](const QString& InError)
    {
        PreviewStatus->setText(!InError.isEmpty() ? InError
                               : bLivePreview || !Document->IsDirty() ? QStringLiteral("As the level draws it -- drag to orbit")
                                                                      : QStringLiteral("The saved version (Live Preview is off)"));
    });

    Dock->setWidget(Page);
    addDockWidget(Qt::RightDockWidgetArea, Dock);
}

void GraphEditorTab::RefreshPreview()
{
    if(!PreviewView) { return; }
    PreviewStatus->setText(QStringLiteral("Drawing..."));
    PreviewView->SetMaterial(EditorGraphs::GetMaterialName(Document->GetPath()));   // draws again
}

bool GraphEditorTab::Save()
{
    QString Error, Message;
    const auto Staged = [this](const bool bInWorked, const QString& InWhat)
    {
        if(bInWorked) { RefreshPreview(); }
        else if(PreviewStatus) { PreviewStatus->setText(QStringLiteral("Not updated: %1").arg(InWhat)); }
    };
    if(!Document->Save(Error, Message, this, bIsMaterial ? std::function<void(bool, const QString&)>(Staged) : nullptr))
    {
        QMessageBox::warning(this, QStringLiteral("Save Graph"), QStringLiteral("Could not save\n%1\n\n%2").arg(Document->GetPath(), Error));
        return false;
    }
    statusBar()->showMessage(Message, 6000);
    bPreviewedUnsaved = false;   // the save staged what is now on disk

    /* A material's output through the real shader compiler, in the background. */
    const Schema* S = FindSchema(Document->GetGraph().SchemaName);
    if(S && S->GetName() == "Material" && EditorGraphs::CanCheckShaders() && !LastResult.HasErrors())
    {
        const QString Shader = ToQ(GetGraphOutputPath(ToStd(Document->GetPath()), *S));
        EditorGraphs::CheckShader(Shader, this, [this, Shader](const bool bInPassed, const QString& InOutput)
        {
            auto* Item = new QListWidgetItem(bInPassed ? QStringLiteral("glslc: %1 compiles.").arg(QFileInfo(Shader).fileName())
                                                       : QStringLiteral("glslc: %1").arg(InOutput.trimmed()), Messages);
            if(!bInPassed) { Item->setForeground(QColor(0xE0, 0x50, 0x50)); }
            statusBar()->showMessage(bInPassed ? QStringLiteral("Shader check passed") : QStringLiteral("Shader check FAILED -- see Messages"), 6000);
        });
    }
    return true;
}

// === Canvas commands ============================================================

void GraphEditorTab::ShowPalette(const QPointF& InScenePos, const bool bInFromPin, const PinRef& InPin)
{
    auto* Palette = new NodePalette(*Document, bInFromPin, InPin, this);
    connect(Palette, &NodePalette::Chosen, this, [this, InScenePos, bInFromPin, InPin]
            (const std::string& InType, const std::map<std::string, std::string>& InProperties)
    {
        const NodeGuid Added = Document->AddNode(InType, InScenePos, InProperties, bInFromPin ? &InPin : nullptr);
        QTimer::singleShot(0, this, [this, Added]() { Scene->SelectNodes({ Added }); });   // after the queued rebuild
    });
    Palette->Popup(View->mapToGlobal(View->mapFromScene(InScenePos)));
}

void GraphEditorTab::FocusNode(const NodeGuid& InNode)
{
    if(NodeItem* Item = Scene->FindNodeItem(InNode))
    {
        Scene->SelectNodes({ InNode });
        View->centerOn(Item);
    }
}

void GraphEditorTab::CopySelection()
{
    const std::vector<NodeGuid> Selected = Scene->GetSelectedNodes();
    if(Selected.empty()) { return; }
    QApplication::clipboard()->setText(ToQ(WriteGraphFragment(Document->GetGraph(), Selected)));
}

void GraphEditorTab::PasteClipboard()
{
    const QString Text = QApplication::clipboard()->text();
    if(Text.isEmpty()) { return; }

    const std::vector<NodeGuid> Pasted = Document->Paste(ToStd(Text), View->CursorScenePos());
    if(Pasted.empty())
    {
        statusBar()->showMessage(QStringLiteral("Nothing to paste: the clipboard holds no %1 graph nodes")
                                 .arg(ToQ(Document->GetSchema().GetName()).toLower()), 4000);
        return;
    }
    QTimer::singleShot(0, this, [this, Pasted]() { Scene->SelectNodes(Pasted); });
}

void GraphEditorTab::DeleteSelection()
{
    const std::vector<NodeGuid> Nodes = Scene->GetSelectedNodes();
    const std::vector<int> Comments = Scene->GetSelectedComments();
    if(!Nodes.empty() || !Comments.empty()) { Document->RemoveNodes(Nodes, Comments); }
}

void GraphEditorTab::AddCommentAroundSelection()
{
    QRectF Bounds;
    for(const NodeGuid& Each : Scene->GetSelectedNodes())
    {
        if(NodeItem* Item = Scene->FindNodeItem(Each)) { Bounds |= Item->sceneBoundingRect(); }
    }
    if(Bounds.isEmpty()) { Bounds = QRectF(View->CursorScenePos(), QSizeF(300.0, 160.0)); }
    Bounds.adjust(-24.0, -48.0, 24.0, 24.0);

    Comment Added;
    Added.X = float(Bounds.x());
    Added.Y = float(Bounds.y());
    Added.Width  = float(Bounds.width());
    Added.Height = float(Bounds.height());
    Added.Text = "Comment";
    Document->AddComment(Added);
    QTimer::singleShot(0, this, [this]() { EditComment(int(Document->GetGraph().Comments.size()) - 1); });
}

void GraphEditorTab::EditComment(const int InIndex)
{
    const std::vector<NodeGraph::Comment>& Comments = Document->GetGraph().Comments;
    if(InIndex < 0 || InIndex >= int(Comments.size())) { return; }

    bool bOk = false;
    const QString Text = QInputDialog::getMultiLineText(this, QStringLiteral("Comment"), QStringLiteral("Text:"),
                                                        ToQ(Comments[size_t(InIndex)].Text), &bOk);
    if(bOk) { Document->SetCommentText(InIndex, Text); }
}

void GraphEditorTab::FrameAll()
{
    View->Frame(Scene->itemsBoundingRect());
}

// === Tab ========================================================================

QString GraphEditorTab::GetTabTitle() const
{
    return QFileInfo(Document->GetPath()).fileName() + (Document->IsDirty() ? QStringLiteral("*") : QString());
}

bool GraphEditorTab::ConfirmClose()
{
    if(!Document->IsDirty()) { return true; }

    /* The question is about this graph, so it is the one showing. */
    GraphEditorWindow::Get().ShowTab(this);
    const QMessageBox::StandardButton Answer = QMessageBox::question(
        this, QStringLiteral("Unsaved Graph"),
        QStringLiteral("%1 has unsaved changes. Save them?").arg(QFileInfo(Document->GetPath()).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if(Answer == QMessageBox::Cancel) { return false; }
    if(Answer == QMessageBox::Save)   { return Save(); }

    /* Discarded: the viewport goes back to what is saved. */
    if(bIsMaterial && bPreviewedUnsaved) { EditorGraphs::RestoreMaterial(Document->GetPath()); }
    return true;
}

#endif // WITH_EDITOR
