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
 * A flat list of live entities and their components, selection synced with
 * EditorContext, rebuilt on EntitiesChanged. Right-click creates or destroys.
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

    /* Suppresses the tree's echo while we set its selection. */
    bool bSyncing = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_OUTLINERPANEL_H
