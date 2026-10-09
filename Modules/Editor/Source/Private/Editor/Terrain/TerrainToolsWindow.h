#ifndef NIMBLEEDITOR_TERRAINTOOLSWINDOW_H
#define NIMBLEEDITOR_TERRAINTOOLSWINDOW_H

#if WITH_EDITOR

#include <QDockWidget>
#include <QString>

#include "Private/Editor/Terrain/TerrainTool.h"

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMainWindow;
class QSlider;
class QTabWidget;

/*
 * Terrain mode: a dock of the main window (View > Terrain, or the Create
 * menu's Terrain), beside the Details panel.
 *
 *   Create   a new flat terrain for the level (its file goes in the level's
 *            Terrain folder), heightmap import and export
 *   Sculpt   Raise, Lower, Smooth, Flatten, Noise, Set Height
 *   Paint    a layer (the terrain material's four), or Smooth Paint
 *
 * While Sculpt or Paint is in front and the dock is shown, the viewport's
 * left button is the brush (TerrainTool); Create, or hiding the dock, gives
 * it back to selection. The brush settings are shared by both.
 */
class TerrainToolsWindow : public QDockWidget
{
    Q_OBJECT

public:
    /* Makes the dock in InHost, hidden, tabbed with InTabifyWith (may be null). */
    static TerrainToolsWindow& Install(QMainWindow& InHost, QDockWidget* InTabifyWith);

    /* The dock, or nullptr if none was installed. */
    static TerrainToolsWindow* Find();

    /* Shows and raises it, on Sculpt. */
    static void Show();

    /* Picks a tool by name ("Raise", "Lower", "Smooth", "Flatten", "Noise",
     * "SetHeight", "Paint", "SmoothPaint"), showing the dock; "off" goes back
     * to Create. False for an unknown name. */
    static bool SelectTool(const QString& InName);

    TerrainTool& GetTool() { return Tool; }

    /* Re-reads the terrain the tools edit (the selection's, or the only one). */
    void RefreshTarget();

private:
    explicit TerrainToolsWindow(QWidget* InParent);

    QWidget* MakeCreatePage();
    QWidget* MakeSculptPage();
    QWidget* MakePaintPage();
    QWidget* MakeBrushPanel();

    /* The viewport's tool follows the dock: shown and on Sculpt/Paint. */
    void UpdateViewportTool();
    void PullSettings();    // widgets -> Tool
    void PushSettings();    // Tool -> widgets (the [ ] keys)
    void CreateTerrain();
    void ImportHeightmap();
    void ExportHeightmap();

    TerrainTool Tool;

    QLabel*         TargetLabel   = nullptr;
    QTabWidget*     Modes         = nullptr;
    QWidget*        BrushPanel    = nullptr;
    QLabel*         HintLabel     = nullptr;

    QLineEdit*      CreateName      = nullptr;
    QComboBox*      CreateSize      = nullptr;
    QDoubleSpinBox* CreateSpacing   = nullptr;
    QDoubleSpinBox* CreateHeight    = nullptr;
    QCheckBox*      CreateCollision = nullptr;
    QDoubleSpinBox* ImportLow       = nullptr;
    QDoubleSpinBox* ImportHigh      = nullptr;

    QButtonGroup*   SculptTools   = nullptr;
    QButtonGroup*   PaintTools    = nullptr;
    QListWidget*    Layers        = nullptr;

    QDoubleSpinBox* Radius        = nullptr;
    QSlider*        Strength      = nullptr;
    QLabel*         StrengthValue = nullptr;
    QComboBox*      Falloff       = nullptr;
    QDoubleSpinBox* Rate          = nullptr;
    QDoubleSpinBox* TargetHeight  = nullptr;
    QDoubleSpinBox* NoiseScale    = nullptr;

    bool bPushing = false;
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_TERRAINTOOLSWINDOW_H
