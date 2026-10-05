#ifndef NIMBLEEDITOR_UIHIERARCHYPANEL_H
#define NIMBLEEDITOR_UIHIERARCHYPANEL_H

#if WITH_EDITOR

#include <QTreeWidget>

class UIDesignDocument;

/*
 * The document's widgets as a tree, in document order. Selecting here
 * selects in the canvas and back; dragging a row reparents or reorders (into
 * a row, or between two); a palette type dropped on a row adds one there.
 * Right-click: duplicate, copy, paste, delete.
 */
class UIHierarchyPanel : public QTreeWidget
{
    Q_OBJECT

public:
    UIHierarchyPanel(UIDesignDocument& InDesign, QWidget* InParent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* InEvent) override;
    void dragMoveEvent(QDragMoveEvent* InEvent) override;
    void dropEvent(QDropEvent* InEvent) override;
    void keyPressEvent(QKeyEvent* InEvent) override;

private:
    void Rebuild();
    void SyncSelection();
    void ShowMenu(const QPoint& InPos);

    /* Where a drop at InPos goes: the parent, and its child to go before (-1 last). False if nowhere. */
    bool DropTarget(const QPoint& InPos, int& OutParent, int& OutBefore) const;

    UIDesignDocument&         Design;
    QList<QTreeWidgetItem*>   Items;   // by widget index
    bool                      bSyncing = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIHIERARCHYPANEL_H
