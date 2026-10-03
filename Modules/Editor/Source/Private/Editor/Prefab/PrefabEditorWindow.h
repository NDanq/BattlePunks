#ifndef NIMBLEEDITOR_PREFABEDITORWINDOW_H
#define NIMBLEEDITOR_PREFABEDITORWINDOW_H

#if WITH_EDITOR

#include <QList>
#include <QMainWindow>

class PrefabEditorTab;
class QTabWidget;

/*
 * The Prefab Editor: one window, a tab per open prefab, like the Graph
 * Editor. Created with the first prefab, closed with the last. Closing
 * prompts for unsaved prefabs. Ctrl+(Shift+)Tab cycles; middle-click closes.
 */
class PrefabEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    /* The window, made (hidden) on first use. */
    static PrefabEditorWindow& Get();

    /* The window, or nullptr if no prefab is open. */
    static PrefabEditorWindow* Find();

    /* Adds InTab, which the window then owns, and shows it. */
    void AddTab(PrefabEditorTab* InTab);

    /* Brings the window forward with InTab current. */
    void ShowTab(PrefabEditorTab* InTab);

    /* Closes after confirmation (and the window with its last tab). */
    bool CloseTab(PrefabEditorTab* InTab);

    QList<PrefabEditorTab*> GetTabs() const;

    /* The tab in front; nullptr if none. */
    PrefabEditorTab* CurrentTab() const;

protected:
    void closeEvent(QCloseEvent* InEvent) override;
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    explicit PrefabEditorWindow(QWidget* InParent = nullptr);

    void             UpdateTitles();
    void             StepTab(int InBy);

    QTabWidget* Tabs = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PREFABEDITORWINDOW_H
