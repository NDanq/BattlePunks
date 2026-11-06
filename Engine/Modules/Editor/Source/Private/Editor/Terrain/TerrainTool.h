#ifndef NIMBLEEDITOR_TERRAINTOOL_H
#define NIMBLEEDITOR_TERRAINTOOL_H

#if WITH_EDITOR

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <QString>

#include "Public/Editor/ViewportTool.h"
#include "Public/GameCore/TerrainBrush.h"

#include "Private/Editor/Terrain/FoliageEditing.h"
#include "Private/Editor/Terrain/TerrainEditing.h"

/*
 * The terrain brushes in the viewport (TerrainToolsWindow sets it). Left
 * drag on a terrain sculpts or paints it under a ring that follows the
 * ground; a stroke is one undo step.
 *
 *   Shift          Raise lowers; Paint smooths the paint
 *   [ / ]          brush smaller / larger (Ctrl+wheel too)
 *   Esc, right     cancel the stroke, putting the terrain back
 *
 * Flatten levels toward the height where the stroke began.
 *
 * In foliage mode (the dock's Foliage tab) the same brush paints the chosen
 * foliage types onto the terrain, or erases them (Shift erases while painting).
 */
class TerrainTool final : public ViewportTool
{
public:
    /* What the dock sets. Radius is in world units. */
    struct Settings
    {
        uint8_t Tool         = GameCore::TT_Raise;
        bool    bLower       = false;     // Raise, inverted (the dock's Lower)
        float   Radius       = 600.0f;
        float   Strength     = 0.5f;
        uint8_t Falloff      = GameCore::TF_Smooth;
        float   Rate         = 400.0f;    // Raise / Noise, units a second at full strength
        float   TargetHeight = 0.0f;      // Set Height, world Y
        uint8_t Layer        = 0;         // Paint
        float   NoiseScale   = 800.0f;

        bool                  bFoliage     = false;              // the brush places foliage, not ground
        uint8_t               FoliageTool  = GameCore::FT_Paint;
        std::vector<uint16_t> FoliageTypes;                      // which it paints; empty: every type
    };

    Settings& GetSettings() { return Current; }

    /* Called when the tool changes a setting itself (the keys): the dock follows. */
    std::function<void()> OnSettingsChanged;

    bool IsStroking() const { return ActiveStroke != nullptr || FoliageStroke != nullptr; }

    /* Set by a foliage stroke that could not make the terrain's foliage (no
     * level file, say); the dock shows it. */
    QString LastError;

    // === ViewportTool ======================================================
    bool OnPress(ViewportWidget& InViewport, const Ray& InRay, Qt::KeyboardModifiers InModifiers) override;
    void OnDrag(ViewportWidget& InViewport, const Ray& InRay, Qt::KeyboardModifiers InModifiers) override;
    void OnRelease(ViewportWidget& InViewport, bool bInCancel) override;
    void OnHover(ViewportWidget& InViewport, const Ray* InRay, Qt::KeyboardModifiers InModifiers) override;
    void Tick(ViewportWidget& InViewport, float InDeltaSeconds) override;
    void QueueOverlay(ViewportWidget& InViewport) override;
    bool OnKey(ViewportWidget& InViewport, QKeyEvent* InEvent) override;
    bool OnWheel(ViewportWidget& InViewport, QWheelEvent* InEvent) override;

private:
    /* The op a dab runs, with Shift's inversion. */
    GameCore::TerrainBrushOp MakeOp(float InDeltaSeconds) const;
    QString MakeLabel() const;
    void    Resize(float InFactor);

    /* Where the cursor is on InTarget's terrain (the stroke's), or on any terrain. */
    bool Trace(const Ray& InRay);

    Settings Current;

    bool                       bHit = false;       // the cursor is over a terrain
    EngineCore::Vector3        HitPoint;
    EditorTerrain::Target      HitTarget;
    bool                       bShift = false;
    std::unique_ptr<EditorTerrain::Stroke> ActiveStroke;
    std::unique_ptr<EditorFoliage::Stroke> FoliageStroke;
    GameCore::FoliageBrushOp               FoliageOp;
    float                      StrokeHeight = 0.0f;   // terrain space, where Flatten began
};

#endif // WITH_EDITOR

#endif //NIMBLEEDITOR_TERRAINTOOL_H
