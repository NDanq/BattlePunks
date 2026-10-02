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
class GraphEditorTab;

/*
 * Graphs as project files: locations, saving, and open / create / regenerate.
 *
 * OUTPUT is written beside the graph (committed) and also staged beside the
 * exe, so the next Play uses it without a build. New scripts still need
 * adding to the game's staged list.
 *
 * MATERIALS are staged by name (<runtime>/Materials, cooked shaders under
 * Shaders/Cooked/<Platform>/Materials) and reloaded by the renderer.
 */
namespace EditorGraphs
{
    inline QString     ToQ(const std::string& InText) { return QString::fromUtf8(InText.c_str()); }
    inline std::string ToStd(const QString& InText)   { return std::string(InText.toUtf8().constData()); }

    /* The folder holding Config/DefaultEngine.ini, else the working directory. */
    QString GetProjectRoot();

    /* Where a new graph of this schema is offered: <root>/Scripts, <root>/Materials. */
    QString GetDefaultFolder(const NodeGraph::Schema& InSchema);

    /* Build trees, Binaries, dot-folders. */
    bool IsIgnoredFolder(const QString& InName);

    /* The same relative path under the exe's folder; empty if not applicable. */
    QString GetStagedPath(const QString& InPath);

    /* True if InGraphPath is open in a graph window. */
    bool IsGraphOpen(const QString& InGraphPath);

    /* "Node Graphs (*.material.nmgraph *.script.nmgraph);;Material Graphs (...)". */
    QString GetFileFilter(const NodeGraph::Schema* InOnly = nullptr);

    /* Compiles, writes and stages the output. False on compile or write
     * failure; OutMessage is for the status bar. */
    bool WriteOutput(const QString& InGraphPath, const NodeGraph::Graph& InGraph,
                     NodeGraph::CompileResult& OutResult, QString& OutMessage,
                     QObject* InContext = nullptr, std::function<void(bool, const QString&)> InStaged = nullptr);

    /* Background cook_material_graphs.py -check; InDone(passed, output).
     * Never calls InDone without Python. */
    void CheckShader(const QString& InShaderPath, QObject* InContext, std::function<void(bool, const QString&)> InDone);
    bool CanCheckShaders();

    /* Recompiles every graph in the project; returns the failure count. */
    int RegenerateAll(QStringList& OutLines);

    // --- Materials in the viewport ----------------------------------------------

    /* Set by the main window; nullptr when there is none. */
    void SetEditorContext(EditorContext* InContext);

    /* A material graph's name at run time: its file's base name, "Glow". */
    QString GetMaterialName(const QString& InGraphPath);

    /* Stages, cooks (in the background) and reloads a compiled material.
     * InDone(worked, message) on InContext's thread, unless it is gone.
     * Requests during a cook coalesce to the latest. */
    void StageMaterial(const QString& InName, const NodeGraph::CompileResult& InResult, QObject* InContext,
                       std::function<void(bool, const QString&)> InDone);

    /* Renderer preview of InName; InDone gets the image or null and why.
     * Not called if InContext is gone. */
    void RequestMaterialPreview(const QString& InName, int InSize, const QColor& InTint, QObject* InContext,
                                std::function<void(const QImage&, const QString&)> InDone);

    /* A mesh, as the mesh viewer and the material Preview dock draw it
     * (Renderer::RequestMeshPreview); answered as RequestMaterialPreview is. */
    struct MeshPreviewOptions
    {
        QString     Mesh;        // runtime path
        QString     Material;    // every section's; "" uses Sections
        QStringList Sections;    // per section; "" keeps the mesh's own
        int         Width  = 256;
        int         Height = 256;
        float       Yaw    = 30.0f;
        float       Pitch  = 20.0f;
        float       Zoom   = 1.0f;
        QColor      Tint   = QColor(255, 255, 255);
    };
    void RequestMeshPreview(const MeshPreviewOptions& InOptions, QObject* InContext,
                            std::function<void(const QImage&, const QString&)> InDone);

    /* Restages the saved graph after unsaved preview changes are discarded. */
    void RestoreMaterial(const QString& InGraphPath);

    /* Undoably sets the selection's RenderComponent material (adding one if
     * needed). False with why. */
    bool ApplyMaterialToSelection(const QString& InName, QString& OutMessage);

    // --- Windows ----------------------------------------------------------------

    /* Adds or focuses the tab; nullptr (message box shown) on load failure. */
    GraphEditorTab* OpenGraph(const QString& InPath, QWidget* InParent);

    /* Asks where, writes a starter graph of InSchema there, and opens it. */
    GraphEditorTab* CreateGraph(const NodeGraph::Schema& InSchema, QWidget* InParent);

    /* Offers to save each dirty graph. False if cancelled. */
    bool CloseAllGraphs();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHASSETS_H
