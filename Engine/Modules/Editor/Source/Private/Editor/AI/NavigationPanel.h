#ifndef NIMBLEEDITOR_NAVIGATIONPANEL_H
#define NIMBLEEDITOR_NAVIGATIONPANEL_H

#if WITH_EDITOR && WITH_AICORE

#include <utility>
#include <vector>

#include <QDockWidget>

#include "Private/Editor/AI/PathTesterTool.h"

class QCheckBox;
class QLabel;
class QMainWindow;
class QPushButton;
class QTimer;

/*
 * Navigation: a dock of the main window (World > Navigation), one level at a
 * time (Engine/Modules/AICore/README.md, "In the editor").
 *
 *   status     no volume / not built / up to date / out of date / building,
 *              and the last build's tiles, polygons, time and size
 *   Build navigation   the level's navmesh from its Nav Mesh Volume and
 *              geometry, written to the volume's NavMesh file (beside the
 *              level when it names none, which it then does)
 *   Rebuild on change  while on, an edit to the level rebuilds it a moment
 *              later (in memory; written with the level's next save)
 *   overlay    the navmesh, tile bounds, obstacles, modifier boxes
 *   Path tester  the viewport's left button picks a path's ends
 *              (PathTesterTool); its corners and length here
 *   Add        a Nav Mesh Volume over the scene, a Nav Modifier, a Nav
 *              Obstacle, in front of the camera
 *
 * The volume's own settings (agent size, voxels, tiles) are its component's
 * fields in the Details panel.
 */
class NavigationPanel final : public QDockWidget
{
    Q_OBJECT

public:
    static NavigationPanel& Install(QMainWindow& InHost, QDockWidget* InTabifyWith);
    static NavigationPanel* Find();
    static void Show();

    /* Builds now; false (and the status says why) if it cannot. */
    bool Build(bool bInSave);
    /* The Path tester on or off, as its checkbox ("ai.testpath"). */
    void SetPathTester(bool bInOn);
    /* The overlay checkboxes from the flags, after something else changed
     * them ("ai.show"). */
    void SyncToggles();

private:
    explicit NavigationPanel(QWidget* InParent);

    void Refresh();
    void ScheduleRebuild();
    void AddVolume();
    void AddModifier();
    void AddObstacle();
    void UpdateTool();
    void UpdatePathLine();

    QLabel*      Status = nullptr;
    QLabel*      Stats = nullptr;
    QLabel*      PathLine = nullptr;
    QPushButton* BuildButton = nullptr;
    QCheckBox*   AutoRebuild = nullptr;
    QCheckBox*   PathTester = nullptr;
    QTimer*      RebuildTimer = nullptr;
    QTimer*      RefreshTimer = nullptr;
    PathTesterTool Tool;
    bool bUnsavedBake = false;
    /* The overlay's toggles here, kept in step with the viewport's Show menu. */
    std::vector<std::pair<QCheckBox*, bool*>> Synced;
};

#endif // WITH_EDITOR && WITH_AICORE

#endif //NIMBLEEDITOR_NAVIGATIONPANEL_H
