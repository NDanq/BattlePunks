#ifndef NIMBLEEDITOR_GRAPHASSETS_H
#define NIMBLEEDITOR_GRAPHASSETS_H

#if WITH_EDITOR

#include <functional>
#include <string>

#include <QString>
#include <QStringList>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/Schema.h"

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
                     NodeGraph::CompileResult& OutResult, QString& OutMessage);

    /* Runs cook_material_graphs.py -check on a generated shader, in the
     * background; InDone gets (passed, compiler output). Skipped -- InDone is
     * never called -- when the editor was built without Python. */
    void CheckShader(const QString& InShaderPath, QObject* InContext, std::function<void(bool, const QString&)> InDone);
    bool CanCheckShaders();

    /* Every *.nmgraph under the project root, recompiled and its output
     * rewritten if changed. Returns the number that failed; OutLines gets one
     * line per graph. */
    int RegenerateAll(QStringList& OutLines);

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
