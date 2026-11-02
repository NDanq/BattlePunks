#ifndef WEATHER_WEATHER_H
#define WEATHER_WEATHER_H

#include <memory>

#include "Public/Math/Vector.h"
#include "Public/RenderCore/ParticleSystemAsset.h"
#include "Public/RenderCore/RenderWeather.h"
#include "Public/Weather/WeatherAPI.h"

namespace GameCore
{
    class World;
}

/*
 * Weather's code side: what the plugin runs on every level's World
 * (WeatherPlugin.cpp) -- the component's wetness stepped, its values handed to
 * the renderer, the rain placed round the camera. A game that links the
 * plugin may call these directly.
 */
namespace Weather
{
    struct WeatherComponent;

    /* The rain's particle system, as shipped: Content/Particles/Rain.nmparticle. */
    constexpr const char* RAIN_SYSTEM_PATH = "Weather:/Particles/Rain.nmparticle";

    /* The rain's geometry, world units (1 cm): the drops are born in a box
     * RAIN_HEIGHT above the eye, fall at about RAIN_FALL_SPEED and drift
     * RAIN_WIND_SPEED x WindStrength along the wind. */
    constexpr float RAIN_HEIGHT     = 900.0f;
    constexpr float RAIN_FALL_SPEED = 900.0f;
    constexpr float RAIN_WIND_SPEED = 250.0f;

    /* The unit vector the wind blows toward at InYawDegrees about +Y
     * (0: +Z, 90: +X). */
    WEATHER_API EngineCore::Vector3 WindDirection(float InYawDegrees);

    /* InWetness after InDeltaSeconds of InRain: soaking at InRain /
     * InWetSeconds a second while it rains, drying at 1 / InDrySeconds once it
     * has stopped (Rain under 0.01). 0 to 1. */
    WEATHER_API float StepWetness(float InWetness, float InRain, float InDeltaSeconds, float InWetSeconds, float InDrySeconds);

    /* What InWeather asks of the renderer: its wind, wetness and rain, and
     * its mist (on when MistDensity + RainMist x Rain is above 0). The
     * [Weather] Rain and Wetness pins are applied by Tick, not here. */
    WEATHER_API RenderCore::RenderWeather ToRenderWeather(const WeatherComponent& InWeather);

    /* InBase with every emitter's Velocity carried InDrift further: the
     * rain blown by the wind. */
    WEATHER_API std::shared_ptr<RenderCore::ParticleSystemDesc> BlowRain(const RenderCore::ParticleSystemDesc& InBase,
                                                                          const EngineCore::Vector3& InDrift);

    /* Where the rain's emitter stands for an eye at InEye: RAIN_HEIGHT above
     * it, upwind by how far a drop drifts on its way down, so the drops
     * arrive round the eye. */
    WEATHER_API EngineCore::Vector3 RainOrigin(const EngineCore::Vector3& InEye, const EngineCore::Vector3& InDrift);

    /* InWorld's Weather component (the first); null if it has none. */
    WEATHER_API WeatherComponent* FindWeather(GameCore::World& InWorld);

    /* The rain (an entity with a Particle System, made on first need) and
     * the camera-tracking pass on InWorld; what the plugin's world hook
     * does. Detach before the World goes: it clears the World's weather. */
    WEATHER_API void AttachWorld(GameCore::World& InWorld);
    WEATHER_API void DetachWorld(GameCore::World& InWorld);

    /* One frame of InWorld's weather: its component's wetness stepped (with
     * the [Weather] pins), RenderCore::SetRenderWeather, and the rain
     * placed, scaled and lit. With no component the World's weather is
     * cleared and the rain stops. What the plugin's system runs. */
    WEATHER_API void Tick(GameCore::World& InWorld, float InDeltaSeconds);

    /* The eye the rain follows: the last frame's main view, else the first
     * active camera, else the origin. False if neither was found. */
    WEATHER_API bool GetRainEye(GameCore::World& InWorld, EngineCore::Vector3& OutEye);

    /* Whether the plugin host started this plugin (Plugins::LoadModules). */
    WEATHER_API bool IsPluginStarted();
}

#endif //WEATHER_WEATHER_H
