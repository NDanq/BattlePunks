#ifndef RENDERCORE_RENDERWEATHER_H
#define RENDERCORE_RENDERWEATHER_H

#include <cstdint>

#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * A World's weather as the renderer sees it (RENDERPIPELINE.md, "Weather"):
 * the values a weather system (the Weather plugin) hands down once a frame,
 * keyed by the World's address as SetPostProcessOverride is. Nothing in
 * RenderCore simulates weather; it only carries these to the GPU:
 *
 *   * WIND, every material's: the scene block's Scene_Wind (xyz the unit
 *     direction it blows toward, w its strength), read by the material
 *     graph's Wind node. A World with no weather set gets the default below,
 *     so foliage that sways along Wind looks the same with or without one.
 *   * WETNESS: Scene_Weather.x. The deferred Lighting stage, image-based
 *     lighting and screen-space reflections darken and smooth upward-facing
 *     G-buffer surfaces by it (Wetness.glsl) -- no material authoring.
 *   * RAIN: Scene_Weather.y, for materials and passes that want it.
 *   * MIST: with bMist, the frame's Volumetric Fog is turned on with these
 *     density and height settings, over whatever the Post Process volumes
 *     blended and under a sequence's override (WorldBridge).
 *
 * POD. Any thread; the game thread sets it.
 */
namespace RenderCore
{
    struct PostProcessSettings;

    struct RenderWeather
    {
        float   WindDirection[3]  = { 0.8f, 0.0f, 0.6f };   // toward which it blows, world space, unit
        float   WindStrength      = 1.0f;                   // 0 calm, 1 the breeze foliage was authored for
        float   Wetness           = 0.0f;                   // 0 dry to 1 soaked
        float   Rain              = 0.0f;                   // 0 to 1
        uint8_t bMist             = 0;                      // drive Volumetric Fog with the three below
        float   MistDensity       = 0.0f;                   // per world unit, at and below MistBaseHeight
        float   MistBaseHeight    = 0.0f;                   // world Y
        float   MistHeightFalloff = 0.0008f;                // per world unit above it
    };

    /* InWorld's weather, from now until cleared; keyed by address, so clear it
     * before the World goes. */
    RENDERCORE_API void SetRenderWeather(const void* InWorld, const RenderWeather& InWeather);
    RENDERCORE_API void ClearRenderWeather(const void* InWorld);

    /* InWorld's, or a default RenderWeather if none is set. bOutSet (optional)
     * says which. */
    RENDERCORE_API RenderWeather GetRenderWeather(const void* InWorld, bool* bOutSet = nullptr);

    /* InWeather's mist laid over InOutSettings: Volumetric Fog on, at its
     * density and heights. Nothing without bMist. */
    RENDERCORE_API void ApplyRenderWeatherFog(const RenderWeather& InWeather, PostProcessSettings& InOutSettings);
}

#endif //RENDERCORE_RENDERWEATHER_H
