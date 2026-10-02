#ifndef NIMBLEEDITOR_OUTLINERPANEL_H
#define NIMBLEEDITOR_OUTLINERPANEL_H

#if WITH_EDITOR

#include <QHash>
#include <QSet>
#include <QWidget>

#include "Public/GameCore/EntityID.h"

class EditorContext;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * The live entities as their hierarchy, with their components, selection
 * synced with EditorContext, rebuilt on EntitiesChanged. Dragging an entity
 * onto another parents it (keeping it where it is in the world); dropping it
 * between rows parents it to that row's parent, and onto empty space
 * detaches it. Right-click creates, parents or destroys.
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

    /* A drop: InDragged under InParent (unset: detach). */
    void Reparent(GameCore::EntityID InDragged, GameCore::EntityID InParent);

    static GameCore::EntityID EntityOf(const QTreeWidgetItem* InItem);

    EditorContext& Context;

    QLineEdit*   Filter = nullptr;
    QTreeWidget* Tree   = nullptr;
    QLabel*      Footer = nullptr;

    /* Packed EntityID -> its row, as of the last Rebuild. */
    QHash<qulonglong, QTreeWidgetItem*> Items;

    /* Parents the user collapsed; everything else starts expanded, so a new
     * child is visible. Packed ids, stale ones harmless. */
    QSet<qulonglong> Collapsed;

    /* Suppresses the tree's echo while we set its selection. */
    bool bSyncing = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_OUTLINERPANEL_H
