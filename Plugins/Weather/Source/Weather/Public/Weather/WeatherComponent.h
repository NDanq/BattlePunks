#ifndef WEATHER_WEATHERCOMPONENT_H
#define WEATHER_WEATHERCOMPONENT_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/Weather/WeatherAPI.h"

namespace Weather
{
    /*
     * "Weather". The level's weather, one per World (the first one found
     * counts; more are warned about once). Each frame the plugin hands it to
     * the renderer (RenderCore's RenderWeather.h):
     *
     *   * RAIN, 0-1: a particle rain round the camera, as many drops as Rain
     *     asks for ([Weather] RainDensity times), blown by the wind and lit
     *     by the world's Sky Light; it thickens the mist by RainMist x Rain.
     *   * MIST: the renderer's own Volumetric Fog, turned on with MistDensity
     *     (0-1; 1 thick: about 30 m of sight at world units of 1 cm) at and
     *     below MistHeight (world Y), thinning to 1/e every MistFalloff world
     *     units above it. 0 (and no rain mist) leaves the level's Post Process
     *     fog alone. The fog's other settings (steps, anisotropy, albedo) are
     *     the volumes' or the Volumetric Fog node's defaults; needs an HDR scene.
     *   * WETNESS, 0-1: upward-facing surfaces of the G-buffer darken and
     *     gloss up, with no material authoring. With bWetnessFollowsRain it
     *     soaks up under rain (fully in WetSeconds at Rain 1) and dries in
     *     DrySeconds once the rain stops; otherwise it is what you set.
     *   * WIND: WindYaw degrees about +Y (0 blows toward +Z, 90 toward +X) at
     *     WindStrength (1 the breeze foliage materials are authored for, 0
     *     calm): every material's Wind node, and the rain's drift.
     *
     * The entity needs nothing else. The server's weather is the weather:
     * every field replicates.
     */
    struct WEATHER_API WeatherComponent
    {
        GAMECORE_COMPONENT_BODY()

        float   Rain        = 0.0f;     // 0 to 1
        float   RainMist    = 0.2f;     // MistDensity added at Rain 1
        uint8_t bRainParticles = 1;     // draw the drops (wetness and mist follow Rain either way)

        float   MistDensity = 0.0f;     // 0 to 1: x WEATHER_MIST_DENSITY_SCALE per world unit
        float   MistHeight  = 0.0f;     // world Y: full density at and below it
        float   MistFalloff = 1000.0f;  // world units above MistHeight per 1/e

        float   Wetness     = 0.0f;     // 0 to 1
        uint8_t bWetnessFollowsRain = 1;
        float   WetSeconds  = 60.0f;    // to soak, from dry, at Rain 1
        float   DrySeconds  = 240.0f;   // to dry, from soaked, once the rain stops

        float   WindYaw      = 53.13f;  // degrees about +Y: 0 toward +Z, 90 toward +X
        float   WindStrength = 1.0f;    // 0 calm, 1 a breeze, more a gale
    };

    /* MistDensity 1, in Volumetric Fog's units (per world unit). */
    constexpr float WEATHER_MIST_DENSITY_SCALE = 0.001f;
}

#endif //WEATHER_WEATHERCOMPONENT_H
