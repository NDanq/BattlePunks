#ifndef NIMBLEEDITOR_GRAPHASSETS_H
#define NIMBLEEDITOR_GRAPHASSETS_H

#if WITH_EDITOR

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>

#include "Public/NodeGraph/Graph.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/Schema.h"
#include "Public/RenderCore/RenderTypes.h"

class EditorContext;
class QObject;
class QWidget;
class GraphEditorTab;

/*
 * Graphs as project files: locations, saving, and open / create / regenerate.
 *
 * GENERATED CODE is stored in the graph file itself (<gen_code>; NodeGraph's
 * README) -- nothing is written beside the graph. A script graph is staged
 * beside the exe as is (the runtime runs its <gen_code>), so the next Play
 * uses it without a build; new scripts still need adding to the game's
 * staged list.
 *
 * MATERIALS are named by their graph's runtime path
 * ("Materials/Glow.material.nmgraph"), staged cooked (<runtime>/Materials/Glow.material,
 * Shaders/Cooked/<Platform>/Materials/Glow@<key>...) and reloaded by the renderer.
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

    /* Compiles InOutGraph, stores the result as its generated code (kept as
     * it was if the graph has errors) and writes the graph file. False with
     * OutError only if the file could not be written. */
    bool CompileAndSave(const QString& InGraphPath, NodeGraph::Graph& InOutGraph,
                        NodeGraph::CompileResult& OutResult, std::string& OutError);

    /* After CompileAndSave: stages the generated code for the viewport and
     * Play. False if the graph has errors; OutMessage is for the status bar. */
    bool StageOutput(const QString& InGraphPath, const NodeGraph::Graph& InGraph,
                     const NodeGraph::CompileResult& InResult, QString& OutMessage,
                     QObject* InContext = nullptr, std::function<void(bool, const QString&)> InStaged = nullptr);

    /* Background cook_material_graphs.py -check of a saved material graph's
     * <gen_code>; InDone(passed, output). Never calls InDone without Python. */
    void CheckShader(const QString& InGraphPath, QObject* InContext, std::function<void(bool, const QString&)> InDone);
    bool CanCheckShaders();

    // --- Shader debug information (Editor Settings > Shaders > ShaderPDB) ---------

    /* [Shaders] ShaderPDB: the shader cooks also write debug information. */
    bool IsShaderPDBEnabled();

    /* Engine/Shaders/PDBs; the cook writes below <root>/<Target>/. */
    QString GetShaderPDBRoot();

    /* Once, after the user's settings load and before the renderer starts:
     * points [Shaders] PDBDir, which GDI finds the files by, at the root
     * above, unless an .ini set it. */
    void InstallShaderPDBDir();

    /*
     * With ShaderPDB on, writes the debug information of everything the build
     * cooks -- Engine/Content/Shaders, the engine's materials, the project's
     * Content/Materials -- for InBackend (Vulkan, D3D12; OpenGL's shaders are
     * source already) in the background, logged when done. A material saved
     * in the editor writes its own as it cooks. No argument: the editor's
     * backend.
     */
    void CookShaderPDBs();
    void CookShaderPDBs(RenderCore::ERenderBackend InBackend);

    /* Recompiles every graph in the project; returns the failure count. */
    int RegenerateAll(QStringList& OutLines);

    /*
     * Rebuilds the node graph schemas so script graphs offer what the game
     * module declares: its components' bindings and its script libraries
     * (GameCore/ScriptLibrary.h). Those register when the module's image loads,
     * so unless bInGameIsLoaded (a Play is running on the current build) the
     * module ([Game] Module) is loaded just long enough to copy them, and
     * unloaded again. False, logged, if there is no module to load; the
     * schemas then have the engine's nodes only. A graph already open keeps
     * the nodes it had until it is reopened (NodeGraph::RefreshSchemas).
     */
    bool RefreshGameDeclarations(bool bInGameIsLoaded);

    // --- Materials in the viewport ----------------------------------------------

    /* Set by the main window; nullptr when there is none. */
    void SetEditorContext(EditorContext* InContext);
    EditorContext* GetEditorContext();

    /* What a RenderComponent names a material graph by: its runtime path,
     * "Materials/Glow.material.nmgraph" (the engine's: "Engine/Content/...").
     * Empty for a graph outside the project and the engine's content. */
    QString GetMaterialPath(const QString& InGraphPath);

    /* Stages, cooks (in the background) and reloads a compiled material,
     * InMaterial its runtime path. InDone(worked, message) on InContext's
     * thread, unless it is gone. Requests during a cook coalesce to the latest. */
    void StageMaterial(const QString& InMaterial, const NodeGraph::CompileResult& InResult, QObject* InContext,
                       std::function<void(bool, const QString&)> InDone);

    /* Whether InMaterial (a runtime path) is being cooked, or waits to be:
     * a preview now would draw what it was before. */
    bool IsMaterialCooking(const QString& InMaterial);

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
        std::vector<float> SkinPalette;   // a skinned mesh's pose, 12 floats a bone; empty: its reference pose
    };
    void RequestMeshPreview(const MeshPreviewOptions& InOptions, QObject* InContext,
                            std::function<void(const QImage&, const QString&)> InDone);

    /* Restages the saved graph after unsaved preview changes are discarded. */
    void RestoreMaterial(const QString& InGraphPath);

    /* Undoably sets the selection's RenderComponent material, InMaterial a
     * graph's runtime path (adding a RenderComponent if needed). False with why. */
    bool ApplyMaterialToSelection(const QString& InMaterial, QString& OutMessage);

    // --- Windows ----------------------------------------------------------------

    /* Adds or focuses the tab; nullptr (message box shown) on load failure. */
    GraphEditorTab* OpenGraph(const QString& InPath, QWidget* InParent);

    /* Asks where (starting in InFolder, or the schema's default folder when
     * empty), writes a starter graph of InSchema there, and opens it. */
    GraphEditorTab* CreateGraph(const NodeGraph::Schema& InSchema, QWidget* InParent, const QString& InFolder = QString());

    /* Writes a starter graph of InSchema at InPath (compiled and staged) without
     * opening it, with InProperties as its graph properties (a UI graph's
     * Document). False with OutError if it could not be written. */
    bool CreateGraphFile(const NodeGraph::Schema& InSchema, const QString& InPath, QString& OutError,
                         const std::map<std::string, std::string>& InProperties = {});

    /* Offers to save each dirty graph. False if cancelled. */
    bool CloseAllGraphs();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHASSETS_H
