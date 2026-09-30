#ifndef NIMBLEEDITOR_GRAPHASSETS_H
#define NIMBLEEDITOR_GRAPHASSETS_H

#if WITH_EDITOR

#include <functional>
#include <string>

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/Schema.h"

class EditorContext;
class QObject;
class QWidget;
class GraphEditorWindow;

/*
 * Graphs as project files: where they live, what a save writes, and the
 * editor-wide operations on them (open one, make one, regenerate them all).
 *
 * WHERE OUTPUT GOES. A graph's output is written beside it -- Scripts/Mover
 * .script.nmgraph -> Scripts/Mover.lua -- and committed like any source. A run
 * reads scripts relative to the working directory, which is the executable's
 * folder, where the build stages Scripts/ (Games/Sandbox's CMakeLists, which
 * lists each script). So a save ALSO copies the output there, at the same
 * path relative to the project root: the next Play uses the new script with
 * no build in between. A new graph's output still has to be added to the
 * game's staged list for a clean build to ship it.
 *
 * MATERIALS are staged by NAME, not path: RenderCore loads material "Glow"
 * from <runtime>/Materials/Glow.material and the cooked shader under
 * <runtime>/Shaders/Cooked/<Platform>/Materials/, wherever the graph lives. A
 * save (and, with Live Preview on, every successful recompile) writes the
 * .material there, cooks the shader for the viewport's backend in the
 * background, and has the renderer reload it -- so a primitive naming the
 * material shows the change on the next frame.
 */
namespace EditorGraphs
{
    inline QString     ToQ(const std::string& InText) { return QString::fromUtf8(InText.c_str()); }
    inline std::string ToStd(const QString& InText)   { return std::string(InText.toUtf8().constData()); }

    /* The game's root: the folder holding Config/DefaultEngine.ini. The
     * working directory if the editor was built without a project. */
    QString GetProjectRoot();

    /* Where a new graph of this schema is offered: <root>/Scripts, <root>/Shaders. */
    QString GetDefaultFolder(const NodeGraph::Schema& InSchema);

    /* "Node Graphs (*.material.nmgraph *.script.nmgraph);;Material Graphs (...)". */
    QString GetFileFilter(const NodeGraph::Schema* InOnly = nullptr);

    /* Compiles InGraph and, if it compiled, writes its output beside
     * InGraphPath and stages a copy into the runtime folder. OutMessage says
     * what happened, for a status bar. False if it did not compile or a file
     * could not be written. */
    bool WriteOutput(const QString& InGraphPath, const NodeGraph::Graph& InGraph,
                     NodeGraph::CompileResult& OutResult, QString& OutMessage,
                     QObject* InContext = nullptr, std::function<void(bool, const QString&)> InStaged = nullptr);

    /* Runs cook_material_graphs.py -check on a generated shader, in the
     * background; InDone gets (passed, compiler output). Skipped -- InDone is
     * never called -- when the editor was built without Python. */
    void CheckShader(const QString& InShaderPath, QObject* InContext, std::function<void(bool, const QString&)> InDone);
    bool CanCheckShaders();

    /* Every *.nmgraph under the project root, recompiled and its output
     * rewritten if changed. Returns the number that failed; OutLines gets one
     * line per graph. */
    int RegenerateAll(QStringList& OutLines);

    // --- Materials in the viewport ----------------------------------------------

    /* The context whose renderer materials are cooked for and reloaded into.
     * Set by the main window; nullptr before it has one or once it is gone. */
    void SetEditorContext(EditorContext* InContext);

    /* A material graph's name at run time: its file's base name, "Glow". */
    QString GetMaterialName(const QString& InGraphPath);

    /*
     * Stages a compiled material -- InResult's shader and .material -- for the
     * viewport: the .material into <runtime>/Materials/, the shader cooked in
     * the background for the renderer's backend, then the renderer told to
     * reload it. InDone gets (worked, what happened), on InContext's thread;
     * never called if InContext goes first. Requests for one material while
     * its cook runs are coalesced: the latest runs next, the rest are dropped.
     */
    void StageMaterial(const QString& InName, const NodeGraph::CompileResult& InResult, QObject* InContext,
                       std::function<void(bool, const QString&)> InDone);

    /* Asks the viewport's renderer for a picture of material InName -- a quad
     * filling InSize x InSize pixels, InTint its Instance Tint -- and calls
     * InDone with it once drawn, or with a null image and why not. Polled on
     * InContext's thread; never called if InContext goes first. */
    void RequestMaterialPreview(const QString& InName, int InSize, const QColor& InTint, QObject* InContext,
                                std::function<void(const QImage&, const QString&)> InDone);

    /* Stages the graph SAVED at InGraphPath -- after a preview of unsaved
     * changes was discarded, so the viewport shows what is on disk again. */
    void RestoreMaterial(const QString& InGraphPath);

    /* Points the selected entity's Primitive at material InName, adding a
     * Primitive if it has none -- recorded, undoable. False, with why, when
     * there is no selection or the name is too long. */
    bool ApplyMaterialToSelection(const QString& InName, QString& OutMessage);

    // --- Windows ----------------------------------------------------------------

    /* The graph's editor window, opened or raised. nullptr (with a message box
     * on InParent) if the file cannot be loaded. */
    GraphEditorWindow* OpenGraph(const QString& InPath, QWidget* InParent);

    /* Asks where, writes a starter graph of InSchema there, and opens it. */
    GraphEditorWindow* CreateGraph(const NodeGraph::Schema& InSchema, QWidget* InParent);

    /* Offers to save each open graph with unsaved changes. False if the user
     * cancelled -- the caller should not close. */
    bool CloseAllGraphs();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHASSETS_H
