#ifndef NIMBLEEDITOR_PREFABASSETS_H
#define NIMBLEEDITOR_PREFABASSETS_H

#if WITH_EDITOR

#include <QString>
#include <QStringList>

#include "Public/GameCore/EntityID.h"

class EditorContext;
class EditorDocument;
class PrefabEditorTab;
class QWidget;

/*
 * Prefabs as project files (.nmprefab, PREFABS.md): where new ones go, and
 * opening, creating and closing them in the Prefab Editor.
 *
 * A prefab names its script by runtime path, like everything the game loads
 * ("Prefabs/Door.lua": project-relative, staged at the same path beside the
 * exe). Saving a prefab or its Lua script stages it there too, so the next
 * Simulate or Play uses it without a build.
 */
namespace EditorPrefabs
{
    /* Set by the main window; nullptr when there is none. */
    void           SetEditorContext(EditorContext* InContext);
    EditorContext* GetEditorContext();

    /* <project>/Prefabs. */
    QString GetDefaultFolder();

    /* A runtime path ("Prefabs/Door.lua") as a file in the project; and back
     * (empty if outside it). */
    QString ToProjectPath(const QString& InRuntimePath);
    QString ToRuntimePath(const QString& InProjectPath);

    /* Copies InPath to its staged path beside the exe, if it has one. */
    void Stage(const QString& InPath);

    /* Adds or focuses the tab; nullptr (message box shown) if it cannot be
     * opened -- no renderer, or not a readable prefab. */
    PrefabEditorTab* OpenPrefab(const QString& InPath, QWidget* InParent);

    /* Asks for a name, writes a one-entity prefab ("Root", a Transform) in
     * InFolder (GetDefaultFolder() if empty), and opens it. */
    PrefabEditorTab* CreatePrefab(const QString& InFolder, QWidget* InParent);

    /* Asks for a registered native prefab and a name, writes a fork of its
     * layout in InFolder (GetDefaultFolder() if empty) -- editable, keeping
     * the native callbacks by name -- and opens it. Given InNative, there is
     * no dialog: InName, or the part after the last '.', names the file. */
    PrefabEditorTab* CreatePrefabFromNative(const QString& InFolder, QWidget* InParent,
                                            const QString& InNative = QString(), const QString& InName = QString());

    /* Offers to save each unsaved prefab, stopping any simulation. False if
     * cancelled. */
    bool CloseAllPrefabs();

    /* Asks where, writes InEntity's subtree there as a prefab, and replaces
     * it in InDocument with an
     * instance (EditorDocument::ReplaceWithPrefab). Refuses an entity in, or
     * holding, an instance: prefabs do not nest. A message box says why. */
    bool ConvertToPrefab(EditorDocument& InDocument, GameCore::EntityID InEntity, QWidget* InParent);

    /* Migration (PREFABS.md section 6): every loose entity tree in
     * InDocument -- one with no prefab instance in it -- into a prefab
     * instance. One prefab per distinct tree (each entity's component types,
     * in tree order) is written to GetDefaultFolder(), named for
     * the first such tree; each tree keeps its own values as overrides. Asks
     * first. Returns how many trees were converted. */
    int ConvertLevelToPrefabs(EditorDocument& InDocument, QWidget* InParent);

    /* Why InEntity cannot be converted; empty if it can. */
    QString WhyNotConvertible(EditorDocument& InDocument, GameCore::EntityID InEntity);

    // --- Native prefabs (GameCore/NativePrefab.h) --------------------------------
    // They exist only while the game module is loaded, which in the editor is
    // briefly (EditorGraphs::RefreshGameDeclarations). Their layouts are built
    // into the prefab registry then, so a level holding "native:" instances
    // loads, and they can be placed and opened, after the module unloads.

    /* While the game module is loaded: every native prefab, built and cached. */
    void CacheNativePrefabs();

    /* Their names ("Sandbox.Marker") as last cached. */
    QStringList GetNativePrefabs();

    /* "native:<Name>" for InName. */
    QString NativePrefabPath(const QString& InName);
    bool    IsNativePrefabPath(const QString& InPath);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PREFABASSETS_H
