#ifndef NIMBLEEDITOR_PREFABEDITORTAB_H
#define NIMBLEEDITOR_PREFABEDITORTAB_H

#if WITH_EDITOR

#include <QMainWindow>
#include <QString>

#include "Public/GameCore/Prefab.h"

class DetailsPanel;
class EditorContext;
class EditorDocument;
class OutlinerPanel;
class PrefabScriptPanel;
class QAction;
class QComboBox;
class QLabel;
class ViewportPanel;
namespace GameCore { class ScriptEngine; class World; }

/*
 * One prefab in the Prefab Editor (PREFABS.md section 5): its own
 * EditorDocument, drawn in its own viewport by its own Renderer scene.
 *
 *   Hierarchy  the Outliner on the prefab: one root, everything under it
 *   Details    the selected entity's name and components
 *   Viewport   the prefab alone
 *   Script     the prefab's Lua script, or a link to its script graph
 *   Toolbar    Save, Simulate (with physics, on a ground of its own, when the
 *              prefab has Rigid Bodies), the tick mode, the script
 *
 * SIMULATE runs the prefab as a game would, in a World of its own: the
 * document borrows it (EditorDocument::BorrowWorld), so the prefab being
 * edited is untouched and Stop simply drops the simulation. Its Lua runs in a
 * script engine of its own; game-module script libraries are not loaded.
 */
class PrefabEditorTab : public QMainWindow
{
    Q_OBJECT

public:
    explicit PrefabEditorTab(EditorContext& InContext, QWidget* InParent = nullptr);
    ~PrefabEditorTab() override;

    /* False, with the reason logged, if InPath is not a readable prefab. */
    bool Load(const QString& InPath);

    /* Absolute; "native:<Name>" for a native prefab. */
    const QString& GetPath() const { return Path; }

    /* A native prefab (NativePrefab.h): its layout is code's. It can be
     * looked at and simulated; Save forks it into an asset (SaveAsAsset). */
    bool IsNative() const { return bNative; }

    /* "Door.nmprefab*". */
    QString GetTabTitle() const;

    bool IsDirty() const;

    /* The prefab, then its Lua script; both staged. A message box on failure.
     * A native prefab saves as an asset instead. */
    bool Save();

    /* Asks where, writes the layout on screen as an ordinary .nmprefab that
     * keeps the native callbacks by name, and opens it. */
    bool SaveAsAsset();

    /* Stops a simulation and offers to save. False if the user cancelled. */
    bool ConfirmClose();

    bool IsSimulating() const { return SimWorld != nullptr; }
    void StartSimulation();
    void StopSimulation();

signals:
    /* Saved, or edited since: the tab's title changed. */
    void TitleChanged();

    /* File > Close: the window asks ConfirmClose, then removes the tab. */
    void CloseRequested();

private:
    enum class EScriptKind { None, Lua, Graph };

    void BuildToolBar();
    void BuildActions();
    void BuildDocks();

    /* The toolbar and Script dock from Asset's settings, without marking dirty. */
    void SyncSettingsControls();

    /* The Script box's choice: a script beside the prefab, made if missing. */
    void SetScriptKind(EScriptKind InKind);
    void ChooseScript();
    void SetScriptPath(const QString& InRuntimePath);

    void MarkSettingsDirty();
    void UpdateSimulationControls();

    EditorContext&  Context;
    EditorDocument* Document = nullptr;
    QString         Path;

    /* Name, script and tick mode, and the entities as last loaded or saved. */
    GameCore::PrefabAsset Asset;
    bool bSettingsDirty = false;   // name, script or tick mode changed since saving
    bool bNative        = false;
    bool bSyncing       = false;   // inside SyncSettingsControls

    ViewportPanel*     ViewportArea = nullptr;
    OutlinerPanel*     Hierarchy    = nullptr;
    DetailsPanel*      Details      = nullptr;
    PrefabScriptPanel* ScriptPanel  = nullptr;

    QAction*   SaveAction     = nullptr;
    QAction*   SimulateAction = nullptr;
    QAction*   StopAction     = nullptr;
    QComboBox* TickModeBox    = nullptr;
    QComboBox* ScriptBox      = nullptr;
    QAction*   ChooseAction   = nullptr;
    QLabel*    SimulateLabel  = nullptr;

    /* While simulating. */
    GameCore::World*        SimWorld   = nullptr;
    bool                    bSimPhysics = false;   // SimWorld is simulated (NimbleEngine::StartSimulating)
    GameCore::ScriptEngine* SimScripts = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PREFABEDITORTAB_H
