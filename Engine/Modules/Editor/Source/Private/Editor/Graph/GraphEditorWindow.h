#ifndef NIMBLEEDITOR_GRAPHEDITORWINDOW_H
#define NIMBLEEDITOR_GRAPHEDITORWINDOW_H

#if WITH_EDITOR

#include <QList>
#include <QMainWindow>

class GraphEditorTab;
class QTabWidget;

/*
 * One window, a tab per open graph; menus, docks and shortcuts follow the
 * current tab. Created with the first graph, closed with the last. Closing
 * prompts for unsaved graphs. Ctrl+(Shift+)Tab cycles; middle-click closes.
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

    /* Closes after confirmation (and the window with its last tab). */
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
