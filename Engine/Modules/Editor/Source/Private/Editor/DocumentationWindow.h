#ifndef NIMBLEEDITOR_DOCUMENTATIONWINDOW_H
#define NIMBLEEDITOR_DOCUMENTATIONWINDOW_H

#if WITH_EDITOR

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QUrl>

class QAction;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

/*
 * Help > Documentation (F1): every Markdown file of the open project and of
 * the engine -- the modules' READMEs and design notes, a game's Docs/ --
 * listed by folder and rendered in place.
 *
 * Relative links to other .md files open here (Back/Forward follow them),
 * "#section" links jump to GitHub-style heading anchors, anything else opens
 * outside the editor. The page reloads when its file changes on disk.
 *
 * Skipped while listing: hidden folders, External/, Binaries/, Saved/,
 * Intermediate/ and build trees (cmake-build-*, any folder with a
 * CMakeCache.txt).
 *
 * One window; opening it again brings it forward.
 */
class DocumentationWindow final : public QMainWindow
{
    Q_OBJECT

public:
    /* Shows the window, making it if need be, at InPath if given (an .md
     * file), else where it was, else at the project's or engine's README. */
    static DocumentationWindow* Open(const QString& InPath = QString());

    /* Closes the window if open: it has no parent, so would otherwise keep
     * the editor running after its main window closes. */
    static void CloseIfOpen();

    /* The .md files under InRoot, absolute and sorted, skipping what the
     * class comment lists. Exposed for tests. */
    static QStringList FindMarkdownFiles(const QString& InRoot);

    /* GitHub's anchor for a heading: lower case, punctuation dropped, spaces
     * to '-' ("Position in the tree" -> "position-in-the-tree"). */
    static QString HeadingAnchor(const QString& InHeading);

private:
    DocumentationWindow();

    void BuildUi();
    void Rescan();
    void ShowFile(const QString& InPath, const QString& InFragment = QString());
    void OnSourceChanged(const QUrl& InSource);
    void OnLinkClicked(const QUrl& InLink);
    void OnFileChanged(const QString& InPath);
    void ApplyFilter(const QString& InText);
    void DecorateDocument();
    void SelectInTree(const QString& InPath);
    QString DefaultPage() const;

    QString       ProjectRoot;
    QString       EngineRoot;

    QLineEdit*    Filter   = nullptr;
    QTreeWidget*  Files    = nullptr;
    QTreeWidget*  Outline  = nullptr;
    QTextBrowser* Page     = nullptr;
    QLabel*       PathLabel = nullptr;
    QAction*      BackAction    = nullptr;
    QAction*      ForwardAction = nullptr;
    QFileSystemWatcher* Watcher = nullptr;
    QString       CurrentPath;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_DOCUMENTATIONWINDOW_H
