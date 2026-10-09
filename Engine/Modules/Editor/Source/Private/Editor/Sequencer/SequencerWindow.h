#ifndef NIMBLEEDITOR_SEQUENCERWINDOW_H
#define NIMBLEEDITOR_SEQUENCERWINDOW_H

#if WITH_EDITOR

#include <QDockWidget>
#include <QList>

class SequencerTab;
class QMainWindow;
class QStackedWidget;
class QTabWidget;

/*
 * The Sequencer: a dock of the main window, a tab per open sequence. The
 * main window makes it at startup (Install), hidden and tabbed with the
 * Content Browser along the bottom; opening a sequence shows and raises it.
 * Like any dock it floats out, docks anywhere, and is in View. With no
 * sequence open it offers Open and New.
 *
 * Closing it (its x) prompts for unsaved sequences, then closes them all,
 * putting the level back. Ctrl+(Shift+)Tab cycles; middle-click closes a tab.
 */
class SequencerWindow : public QDockWidget
{
    Q_OBJECT

public:
    /* Makes the dock in InHost, hidden, tabbed with InTabifyWith (may be null). */
    static SequencerWindow& Install(QMainWindow& InHost, QDockWidget* InTabifyWith);

    /* The dock; made floating, on its own, if no main window installed one (tests). */
    static SequencerWindow& Get();

    /* The dock, or nullptr if there is none. */
    static SequencerWindow* Find();

    /* Adds InTab, which the dock then owns, and shows it. */
    void AddTab(SequencerTab* InTab);

    /* Shows and raises the dock with InTab current. */
    void ShowTab(SequencerTab* InTab);

    /* Closes after confirmation. */
    bool CloseTab(SequencerTab* InTab);

    /* Every tab, after confirmation; false if one was cancelled. */
    bool CloseAllTabs();

    QList<SequencerTab*> GetTabs() const;

    /* The tab in front; nullptr if none. */
    SequencerTab* CurrentTab() const;

protected:
    void closeEvent(QCloseEvent* InEvent) override;
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    explicit SequencerWindow(QWidget* InParent = nullptr);

    void UpdateTitles();
    void StepTab(int InBy);

    QStackedWidget* Pages = nullptr;   // 0: nothing open, 1: the tabs
    QTabWidget*     Tabs  = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SEQUENCERWINDOW_H
