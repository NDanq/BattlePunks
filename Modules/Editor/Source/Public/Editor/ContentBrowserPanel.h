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
 * The Content Browser dock: the game's project folder as a tree plus the
 * current folder's contents (build trees and dot-folders hidden).
 *
 *   Navigate     tree, double-click, Back / Up, or the path bar
 *   Organise     New Folder, Rename (F2), Delete (recycle bin), drag to move.
 *                Generated files and entity folders move with their owner;
 *                open graphs and the open world are protected.
 *   Open         graphs in their window, worlds in the viewport, else the
 *                system application
 *   Import       drop external files or Import... (Import Wizard)
 *
 * Assets drag out onto the viewport or a matching Details field.
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

    /* The current folder, or <root>/Content at the root. */
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

    /* Moves from inside the project; imports or copies from outside. */
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
