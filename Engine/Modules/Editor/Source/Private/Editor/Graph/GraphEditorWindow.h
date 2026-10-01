#ifndef NIMBLEEDITOR_GRAPHEDITORWINDOW_H
#define NIMBLEEDITOR_GRAPHEDITORWINDOW_H

#if WITH_EDITOR

#include <QList>
#include <QMainWindow>

class GraphEditorTab;
class QTabWidget;

/*
 * The graph editor: ONE top-level window, a tab per open graph, material and
 * script alike. Each tab (GraphEditorTab) brings its own menus and docks, so
 * the menu bar and the Details, code and Messages docks are the current
 * graph's -- and so are the shortcuts, since a hidden tab's are inactive.
 *
 * Made by the first graph opened, and gone with the last tab closed. Closing
 * the window offers to save every graph with unsaved changes; cancelling any
 * one keeps it open. Ctrl+Tab and Ctrl+Shift+Tab step through the tabs; a
 * middle click closes one.
 */
class GraphEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    /* The window, made (hidden) on first use. */
    static GraphEditorWindow& Get();

    /* The window, or nullptr if no graph is open. */
    static GraphEditorWindow* Find();

    /* Adds InTab, which the window then owns, and shows it. */
    void AddTab(GraphEditorTab* InTab);

    /* Brings the window forward with InTab current. */
    void ShowTab(GraphEditorTab* InTab);

    /* Asks InTab to confirm, then removes and deletes it -- and the window
     * with its last tab. False if the user cancelled. */
    bool CloseTab(GraphEditorTab* InTab);

    QList<GraphEditorTab*> GetTabs() const;

protected:
    void closeEvent(QCloseEvent* InEvent) override;
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    explicit GraphEditorWindow(QWidget* InParent = nullptr);

    GraphEditorTab* CurrentTab() const;
    void            UpdateTitles();
    void            StepTab(int InBy);

    QTabWidget* Tabs = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_GRAPHEDITORWINDOW_H
