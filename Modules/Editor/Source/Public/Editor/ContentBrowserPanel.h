#ifndef NIMBLEEDITOR_CONTENTBROWSERPANEL_H
#define NIMBLEEDITOR_CONTENTBROWSERPANEL_H

#if WITH_EDITOR

#include <QStringList>
#include <QWidget>

class QAction;
class QFileSystemModel;
class QHBoxLayout;
class QLineEdit;
class QModelIndex;
class QToolButton;
class QTreeView;
class ContentItemView;
class ContentListProxy;
class ContentTreeProxy;

/*
 * The Content Browser dock: the project's files -- the game's folder, the one
 * holding Config/DefaultEngine.ini -- as a folder tree beside the current
 * folder's contents, minus build trees and dot-folders.
 *
 *   Navigate     click a folder in the tree, double-click one in the list,
 *                Back / Up, or a segment of the path bar
 *   Organise     New Folder, Rename (F2), Delete (to the recycle bin), and
 *                drag items onto a folder to move them. A graph's generated
 *                files and a world's entity folder go wherever it goes; a
 *                graph open in a window, or the open world, is left alone.
 *   Open         a graph in its window, a world in the viewport, anything
 *                else with the system's own application
 *   Import       drop files from outside the project, or Import...: meshes
 *                and images go through the Import Wizard into engine assets
 *
 * Assets drag out as files: onto the viewport, or onto a Details field that
 * names one (a RenderComponent's Mesh or Material).
 */
class ContentBrowserPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ContentBrowserPanel(QWidget* InParent = nullptr);

    QString GetProjectRoot() const { return Root; }
    QString GetCurrentFolder() const { return CurrentFolder; }

    /* Shows InFolder (inside the project), remembering where it came from. */
    void SetCurrentFolder(const QString& InFolder);

    /* Where an import lands when nothing more specific is asked: the current
     * folder, or <root>/Content when the browser is at the root. */
    QString GetImportFolder() const;

    /* Opens the Import Wizard for InFiles into InDestFolder. */
    void ImportFiles(const QStringList& InFiles, const QString& InDestFolder);

    /* Asks for files, then imports them into GetImportFolder(). */
    void ImportWithDialog();

    /* The open world's file, protected from moves and deletes. */
    void SetOpenWorld(const QString& InPath) { OpenWorld = InPath; }

signals:
    void OpenWorldRequested(const QString& InPath);

    /* One line for the main window's status bar. */
    void StatusMessage(const QString& InText);

private:
    friend class ContentItemView;
    friend class ContentListProxy;

    void BuildToolBar(QHBoxLayout* InLayout);
    void RebuildPathBar();
    void UpdateActions();
    void NavigateBack();
    void NavigateUp();

    QString PathOf(const QModelIndex& InListIndex) const;
    QStringList SelectedPaths() const;

    void OpenItem(const QString& InPath);
    void ShowContextMenu(const QPoint& InPos);
    void NewFolder();
    void RenameSelected();
    void DeleteSelected();
    void ReimportSelected();
    void ShowInExplorer(const QString& InPath);

    /* Drops onto the list (from ContentItemView): moves from inside the
     * project, imports or copies from outside. */
    void HandleDrop(const QStringList& InFiles, const QString& InTargetFolder);
    void MoveInto(const QStringList& InPaths, const QString& InTargetFolder);

    /* A rename from the list's editor: the item and its companions. */
    bool RenameItem(const QString& InPath, const QString& InNewName);

    /* Why InPath may not be moved, renamed or deleted, or empty if it may. */
    QString WhyProtected(const QString& InPath) const;

    void SelectFiles(const QStringList& InPaths);

    QString Root;
    QString CurrentFolder;
    QString OpenWorld;
    QStringList History;

    QFileSystemModel*  Model     = nullptr;
    ContentTreeProxy*  TreeProxy = nullptr;
    ContentListProxy*  ListProxy = nullptr;
    QTreeView*         Tree      = nullptr;
    ContentItemView*   List      = nullptr;
    QLineEdit*         Filter    = nullptr;
    QHBoxLayout*       PathBar   = nullptr;

    QToolButton* BackButton = nullptr;
    QToolButton* UpButton   = nullptr;
    QAction*     RenameAction = nullptr;
    QAction*     DeleteAction = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_CONTENTBROWSERPANEL_H
