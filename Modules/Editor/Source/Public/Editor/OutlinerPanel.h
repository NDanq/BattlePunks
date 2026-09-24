#ifndef NIMBLEEDITOR_OUTLINERPANEL_H
#define NIMBLEEDITOR_OUTLINERPANEL_H

#if WITH_EDITOR

#include <QWidget>

#include "Public/GameCore/EntityID.h"

class EditorContext;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * Every live entity in the World, by display name, with the components each
 * one carries -- flat, since GameCore has no hierarchy yet. Selection is
 * two-way with EditorContext. Rebuilt on EntitiesChanged, which the context
 * coalesces to at most once per event-loop turn.
 *
 * Right-click creates an empty entity or destroys the one under the cursor
 * (Edit > Delete, on the main window, destroys the selection).
 */
class OutlinerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit OutlinerPanel(EditorContext& InContext, QWidget* InParent = nullptr);

private:
    void Rebuild();
    void ApplyFilter();
    void SyncSelectionFromContext(GameCore::EntityID InEntity);
    void ShowContextMenu(const QPoint& InPos);

    static GameCore::EntityID EntityOf(const QTreeWidgetItem* InItem);

    EditorContext& Context;

    QLineEdit*   Filter = nullptr;
    QTreeWidget* Tree   = nullptr;
    QLabel*      Footer = nullptr;

    /* Set while this panel is pushing a selection into the tree, so the
     * tree's own signal does not echo it straight back. */
    bool bSyncing = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_OUTLINERPANEL_H
