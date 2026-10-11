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
class QEvent;

/*
 * Help > Documentation (F1): the engine's documentation, every Markdown file
 * under Engine/Docs (guides, and a reference page per component in
 * Components/), listed by folder and rendered in place. Nothing else is
 * shown: a path outside Engine/Docs is refused, a link to one too.
 *
 * Relative links to other .md files open here (Back/Forward follow them),
 * "#section" links jump to GitHub-style heading anchors, other links open
 * outside the editor. The page reloads when its file changes on disk.
 *
 * One window; opening it again brings it forward.
 */
class DocumentationWindow final : public QMainWindow
{
    Q_OBJECT

public:
    /* Shows the window, making it if need be, at InPath if given (an .md file
     * under Engine/Docs; any other is ignored), else where it was, else at
     * the docs' README. */
    static DocumentationWindow* Open(const QString& InPath = QString());

    /* Whether InPath is a file under Engine/Docs -- all this window shows. */
    static bool IsDocumentation(const QString& InPath);

    /* Closes the window if open: it has no parent, so would otherwise keep
     * the editor running after its main window closes. */
    static void CloseIfOpen();

    /* The .md files under InRoot, absolute and sorted, skipping hidden folders,
     * External/, Binaries/, Build/, Saved/, Intermediate/ and build trees.
     * Exposed for tests. */
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

    /* Images wider than the page are drawn at its width (aspect kept), so a
     * screenshot is seen whole rather than cut off at the pane's edge; run
     * again when the page is resized. */
    void FitImages();
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;
    void SelectInTree(const QString& InPath);
    QString DefaultPage() const;

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
