#ifndef NIMBLEEDITOR_PARTICLEEDITORWINDOW_H
#define NIMBLEEDITOR_PARTICLEEDITORWINDOW_H

#if WITH_EDITOR

#include <QList>
#include <QMainWindow>

class ParticleEditorTab;
class QTabWidget;

/*
 * The Particle Editor: one window, a tab per open particle system, like the Prefab
 * Editor. Created with the first system, closed with the last. Closing
 * prompts for unsaved systems. Ctrl+(Shift+)Tab cycles; middle-click closes.
 */
class ParticleEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    /* The window, made (hidden) on first use. */
    static ParticleEditorWindow& Get();

    /* The window, or nullptr if no system is open. */
    static ParticleEditorWindow* Find();

    /* Adds InTab, which the window then owns, and shows it. */
    void AddTab(ParticleEditorTab* InTab);

    /* Brings the window forward with InTab current. */
    void ShowTab(ParticleEditorTab* InTab);

    /* Closes after confirmation (and the window with its last tab). */
    bool CloseTab(ParticleEditorTab* InTab);

    QList<ParticleEditorTab*> GetTabs() const;

    /* The tab in front; nullptr if none. */
    ParticleEditorTab* CurrentTab() const;

protected:
    void closeEvent(QCloseEvent* InEvent) override;
    bool eventFilter(QObject* InWatched, QEvent* InEvent) override;

private:
    explicit ParticleEditorWindow(QWidget* InParent = nullptr);

    void             UpdateTitles();
    void             StepTab(int InBy);

    QTabWidget* Tabs = nullptr;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_PARTICLEEDITORWINDOW_H
