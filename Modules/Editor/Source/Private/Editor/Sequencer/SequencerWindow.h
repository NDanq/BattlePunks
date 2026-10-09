#ifndef NIMBLEEDITOR_SEQUENCERWINDOW_H
#define NIMBLEEDITOR_SEQUENCERWINDOW_H

#if WITH_EDITOR

#include <QList>
#include <QMainWindow>

class SequencerTab;
class QTabWidget;

/*
 * The Sequencer: one window, a tab per open sequence, like the Particle
 * Editor. Created with the first sequence, closed with the last. Closing
 * prompts for unsaved sequences. Ctrl+(Shift+)Tab cycles; middle-click closes.
 */
class SequencerWindow : public QMainWindow
{
    Q_OBJECT

public:
    /* The window, made (hidden) on first use. */
    static SequencerWindow& Get();

    /* The window, or nullptr if no sequence is open. */
    static SequencerWindow* Find();

    /* Adds InTab, which the window then owns, and shows it. */
    void AddTab(SequencerTab* InTab);

    /* Brings the window forward with InTab current. */
    void ShowTab(SequencerTab* InTab);

    /* Closes after confirmation (and the window with its last tab). */
    bool CloseTab(SequencerTab* InTab);

    QList<SequencerTab*> GetTabs() const;

    /* The tab in front; nullptr if none. */
    SequencerTab* CurrentTab() const;

protected:
    void closeEvent(QCloseEvent* InEvent) override;
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    explicit SequencerWindow(QWidget* InParent = nullptr);

    void             UpdateTitles();
    void             StepTab(int InBy);

    QTabWidget* Tabs = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_SEQUENCERWINDOW_H
