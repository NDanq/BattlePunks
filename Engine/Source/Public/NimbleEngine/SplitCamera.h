#ifndef NIMBLEENGINE_SPLITCAMERA_H
#define NIMBLEENGINE_SPLITCAMERA_H

#include <cstdint>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"

/*
 * SPLITSCREEN's DYNAMIC SPLIT (SPLITSCREEN.md, "Dynamic split"): local players
 * close together share one camera; as they part, the window splits along a
 * line at right angles to the way between them, which turns as they move and
 * fades in, and closes again without a seam when they come back together.
 * Only for cameras that share one orientation -- top-down, isometric, a fixed
 * side or three-quarter view; free-look third-person cameras use the fixed
 * layouts instead.
 *
 * Two players: the shared camera looks at their midpoint M. Once they no
 * longer fit within Margin of the screen's centre, it splits: with T the
 * separation that just fits and L theirs, view i looks at
 * M + (P_i - M) * (1 - T / L), each player held a fixed way from the centre on
 * their own side. At L = T both are M, so the split opens with no jump.
 * Three or four: one shared camera on their centroid while all fit, else the
 * fixed layout, a camera on each.
 */
namespace NimbleEngine
{
    struct SplitCameraSettings
    {
        bool                bEnabled = false;                         // [Splitscreen] Mode=Dynamic
        EngineCore::Vector3 Rotation{ 55.0, 0.0, 0.0 };               // the cameras' Euler degrees, as a Transform's
        float               Distance = 1500.0f;                       // from what a camera looks at, back along its forward
        float               FovY     = 50.0f;
        float               ZNear    = 10.0f;
        float               ZFar     = 50000.0f;
        float               Margin   = 0.6f;                          // how far out (0-1 of half the screen) a player may get before it splits
        float               DividerPixels = 4.0f;
        EngineCore::Color   DividerColor{ 0, 0, 0, 255 };
        float               TurnRate = 8.0f;                          // how fast the split line follows the players (1/s); 0 at once
    };

    /* One frame's solution. */
    struct SplitCameraFrame
    {
        uint32_t            ViewCount = 1;     // 1: one shared camera
        EngineCore::Vector3 Focus[4];          // what each view's camera looks at
        bool                bSites = false;    // two views cut by a line (the sites below), not the fixed layout
        float               SiteX[4] = {};     // each view's site, window pixels, row 0 the top
        float               SiteY[4] = {};
        float               DividerAlpha = 0.0f;
    };

    /* The camera position for a focus: back along the settings' forward. */
    NE_API EngineCore::Vector3 SplitCameraPosition(const SplitCameraSettings& InSettings, const EngineCore::Vector3& InFocus);

    /*
     * The split for InCount players at InPlayers (1-4) over a window of
     * InWindow pixels. InOutAngle is the split line's direction across the
     * screen (radians), kept between frames so it turns smoothly (TurnRate,
     * over InDeltaSeconds); bInOutAngleSet false snaps it.
     */
    NE_API void SolveSplitCamera(const SplitCameraSettings& InSettings, const EngineCore::Vector3* InPlayers, uint32_t InCount,
                                 EngineCore::Size InWindow, float InDeltaSeconds, float& InOutAngle, bool& bInOutAngleSet,
                                 SplitCameraFrame& OutFrame);
}

#endif //NIMBLEENGINE_SPLITCAMERA_H
