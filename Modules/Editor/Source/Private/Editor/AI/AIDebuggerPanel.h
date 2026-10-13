#ifndef NIMBLEEDITOR_AIDEBUGGERPANEL_H
#define NIMBLEEDITOR_AIDEBUGGERPANEL_H

#if WITH_EDITOR && WITH_AICORE

#include <utility>
#include <vector>

#include <QDockWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QMainWindow;
class QTimer;
class QTreeWidget;

/*
 * AI Debugger: a dock (World > AI Debugger) for Play-in-Editor's agents.
 *
 *   agents     every running brain in the playing level; picking one selects
 *              it in the Outliner too, and the viewport draws only its sight
 *   the brain  the running branch, root first, with how long each node has
 *              run and, under a utility selector, every child's score
 *   blackboard every key and its value
 *   perception what it remembers: seen or heard, where, how long ago
 *   navigation its status, speed, distance left and corners
 *
 * The viewport overlay (EditorAI): its path, its sight cone, a green line to
 * each target it sees and an amber one to each it remembers. Refreshed four
 * times a second. A networked Play reads the listen server's (or the
 * standalone game's) AI: a client's has none.
 */
class AIDebuggerPanel final : public QDockWidget
{
    Q_OBJECT

public:
    static AIDebuggerPanel& Install(QMainWindow& InHost, QDockWidget* InTabifyWith);
    static AIDebuggerPanel* Find();
    static void Show();

private:
    explicit AIDebuggerPanel(QWidget* InParent);

    void Refresh();
    void FillAgents();
    void FillAgent();

    QLabel*      Header = nullptr;
    QListWidget* Agents = nullptr;
    QTreeWidget* Details = nullptr;
    QTimer*      Timer = nullptr;
    std::vector<std::pair<QCheckBox*, bool*>> Synced;   // as NavigationPanel's
};

#endif // WITH_EDITOR && WITH_AICORE

#endif //NIMBLEEDITOR_AIDEBUGGERPANEL_H
