#ifndef NIMBLEEDITOR_UIEDITORWINDOW_H
#define NIMBLEEDITOR_UIEDITORWINDOW_H

#if WITH_EDITOR

#include <QList>
#include <QMainWindow>

class UIEditorTab;
class QTabWidget;

/*
 * The UI Designer: one window, a tab per open UI document, like the Prefab
 * Editor. Created with the first document, closed with the last. Closing
 * prompts for unsaved documents. Ctrl+(Shift+)Tab cycles; middle-click closes.
 */
class UIEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    /* The window, made (hidden) on first use. */
    static UIEditorWindow& Get();

    /* The window, or nullptr if no document is open. */
    static UIEditorWindow* Find();

    /* Adds InTab, which the window then owns, and shows it. */
    void AddTab(UIEditorTab* InTab);

    /* Brings the window forward with InTab current. */
    void ShowTab(UIEditorTab* InTab);

    /* Closes after confirmation (and the window with its last tab). */
    bool CloseTab(UIEditorTab* InTab);

    QList<UIEditorTab*> GetTabs() const;

    /* The tab in front; nullptr if none. */
    UIEditorTab* CurrentTab() const;

protected:
    void closeEvent(QCloseEvent* InEvent) override;
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    explicit UIEditorWindow(QWidget* InParent = nullptr);

    void             UpdateTitles();
    void             StepTab(int InBy);

    QTabWidget* Tabs = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_UIEDITORWINDOW_H
