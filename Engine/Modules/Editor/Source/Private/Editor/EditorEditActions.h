#ifndef NIMBLEEDITOR_EDITOREDITACTIONS_H
#define NIMBLEEDITOR_EDITOREDITACTIONS_H

#if WITH_EDITOR

#include <functional>
#include <vector>

#include <QKeySequence>
#include <QString>

class EditorDocument;
class QAction;
class QMenu;
class QWidget;

/*
 * Cut, Copy, Paste, Duplicate, Rename and Delete on a document's selection
 * (EditorDocument's clipboard operations), for the viewport's and the
 * Outliner's context menus, their keys, and the Edit menus.
 *
 * KEYS belong to the widget the selection is made in (Install: active while
 * focus is in it), not the window: a window-wide Del or F2 would be ambiguous
 * with the Content Browser's own, and Ctrl+C with a text field's. The Edit
 * menus show the keys without owning them (AddToMenu).
 */
namespace EditorEditActions
{
    enum class EAction { Cut, Copy, Paste, Duplicate, Rename, Delete, Count };

    QString      GetLabel(EAction InAction);
    QKeySequence GetKey(EAction InAction);

    /* There is something to act on: a selection, or something to paste
     * (Rename: exactly one entity selected). */
    bool IsAvailable(EAction InAction, const EditorDocument& InDocument);

    /* Does it. InRename renames the selected entity: inline in the Outliner,
     * PromptRename elsewhere. */
    void Run(EAction InAction, EditorDocument& InDocument, const std::function<void()>& InRename);

    /* The selected entity's new name, asked for in a dialog over InParent. */
    void PromptRename(QWidget* InParent, EditorDocument& InDocument);

    /* The six on InOwner, their keys active while focus is in it, enabled as
     * the document allows. InAllowed (optional) can hold them off -- the
     * viewport, while the camera flies on WASD with Ctrl held. */
    void Install(QWidget* InOwner, EditorDocument& InDocument, std::function<void()> InRename,
                 std::function<bool()> InAllowed = nullptr);

    /* The six as items of InMenu, enabled as the document allows now, in
     * EAction order. Their keys are shown, not taken (see KEYS). */
    std::vector<QAction*> AddToMenu(QMenu* InMenu, EditorDocument& InDocument, const std::function<void()>& InRename);

    /* AddToMenu's items enabled as the document allows now (a menu's aboutToShow). */
    void Refresh(const std::vector<QAction*>& InItems, const EditorDocument& InDocument);
}

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_EDITOREDITACTIONS_H
