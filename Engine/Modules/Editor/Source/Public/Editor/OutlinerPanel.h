#ifndef NIMBLEEDITOR_OUTLINERPANEL_H
#define NIMBLEEDITOR_OUTLINERPANEL_H

#if WITH_EDITOR

#include <cstdint>
#include <vector>

#include <QHash>
#include <QSet>
#include <QString>
#include <QWidget>

#include "Public/GameCore/EntityID.h"

class EditorDocument;
class QLabel;
class QKeyEvent;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * The live entities as their hierarchy, with their components, selection
 * synced with EditorDocument, rebuilt on EntitiesChanged. Dragging an entity
 * onto another parents it (keeping it where it is in the world); dropping it
 * between rows parents it to that row's parent, and onto empty space
 * detaches it. Right-click creates, parents or destroys.
 *
 * FOLDERS (a level, not a prefab): top-level entities sorted into folders
 * (EditorDocument's Outliner folders), listed before the entities at each
 * level. Dropping the selection onto a folder, or between rows inside one,
 * moves it there -- a child is detached where it stands; a folder dropped
 * onto another moves inside it. F2 renames a selected folder in its row, Del
 * deletes it (what it held moves up a level); right-click for the rest.
 */
class OutlinerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit OutlinerPanel(EditorDocument& InDocument, QWidget* InParent = nullptr);

private:
    uint32_t SeenScalabilitySerial = 0;   // GameCore::Scalability::GetSerial at the last check
    void Rebuild();
    void QueueRebuild();
    void ApplyFilter();
    void SyncSelectionFromDocument(GameCore::EntityID InEntity);
    void ShowContextMenu(const QPoint& InPos);

    /* F2 / Rename: the selected entity's name, edited in its row. */
    void RenameSelected();

    /* A drop: InDragged under InParent (unset: detach). */
    void Reparent(GameCore::EntityID InDragged, GameCore::EntityID InParent);

    /* A drop of InDragged onto InNewParent's row (nullptr: the top level). */
    void Drop(QTreeWidgetItem* InDragged, QTreeWidgetItem* InNewParent);

    /* Folders, unless this is a prefab (a single-root document). */
    bool HasFolders() const;

    /* A new folder inside InParent ("": at the top), its name edited in its row. */
    void CreateFolderIn(const QString& InParent);

    /* F2 and Del on folder rows, when no entity is selected. True if used. */
    bool HandleFolderKey(const QKeyEvent* InEvent);

    /* The selected folder rows' paths, deepest first. */
    std::vector<QString> SelectedFolders() const;

    static GameCore::EntityID EntityOf(const QTreeWidgetItem* InItem);

    /* A folder row's path; "" for an entity's. */
    static QString FolderOf(const QTreeWidgetItem* InItem);

    EditorDocument& Document;

    QLineEdit*   Filter = nullptr;
    QTreeWidget* Tree   = nullptr;
    QLabel*      Footer = nullptr;

    /* Packed EntityID -> its row, as of the last Rebuild. */
    QHash<qulonglong, QTreeWidgetItem*> Items;

    /* Folder path -> its row, as of the last Rebuild. */
    QHash<QString, QTreeWidgetItem*> FolderItems;

    /* Parents the user collapsed; everything else starts expanded, so a new
     * child is visible. Packed ids, stale ones harmless. */
    QSet<qulonglong> Collapsed;
    QSet<QString>    CollapsedFolders;

    /* Suppresses the tree's echo while we set its selection. */
    bool bSyncing = false;

    bool bRebuildQueued = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_OUTLINERPANEL_H
