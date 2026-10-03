#ifndef NIMBLEEDITOR_PREFABASSETS_H
#define NIMBLEEDITOR_PREFABASSETS_H

#if WITH_EDITOR

#include <QString>

class EditorContext;
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

    /* Offers to save each unsaved prefab, stopping any simulation. False if
     * cancelled. */
    bool CloseAllPrefabs();
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PREFABASSETS_H
