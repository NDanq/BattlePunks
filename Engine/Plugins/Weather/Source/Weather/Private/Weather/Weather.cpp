#include "Public/Weather/Weather.h"

#include <algorithm>
#include <cmath>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/Weather/WeatherComponent.h"

/* The component's step and what it asks of the renderer. The values a game
 * sees are the plugin's Config/DefaultWeather.ini, under the project's file;
 * these defaults only hold where neither is read (a bare test). */
/* 0-1 pins every Weather's Rain (-Weather.Rain=0.8); below 0, off. */
NIMBLE_CONFIG_FLOAT(GWeatherPinnedRain,    "Weather", "Rain",    -1.0f, nullptr);
/* 0-1 pins every Weather's Wetness; below 0, off. */
NIMBLE_CONFIG_FLOAT(GWeatherPinnedWetness, "Weather", "Wetness", -1.0f, nullptr);

namespace Weather
{
    /* WeatherRain.cpp: the drops for this frame (InWeather null: none). */
    void UpdateRain(GameCore::World& InWorld, const WeatherComponent* InWeather);

    namespace
    {
        constexpr float Pi = 3.14159265f;

        float Unit(const float InValue)
        {
            return (std::min)(1.0f, (std::max)(0.0f, InValue));
        }

        /* Warned once a process: a second Weather in a World does nothing. */
        bool GWarnedSecond = false;
    }

    EngineCore::Vector3 WindDirection(const float InYawDegrees)
    {
        const float Yaw = InYawDegrees * Pi / 180.0f;
        return EngineCore::Vector3(std::sin(Yaw), 0.0f, std::cos(Yaw));
    }

    float StepWetness(const float InWetness, const float InRain, const float InDeltaSeconds, const float InWetSeconds,
                      const float InDrySeconds)
    {
        const float Rain = Unit(InRain);
        float Wetness    = Unit(InWetness);
        if(Rain >= 0.01f)
        {
            Wetness += InDeltaSeconds * Rain / (std::max)(InWetSeconds, 0.01f);
        }
        else
        {
            Wetness -= InDeltaSeconds / (std::max)(InDrySeconds, 0.01f);
        }
        return Unit(Wetness);
    }

    RenderCore::RenderWeather ToRenderWeather(const WeatherComponent& InWeather)
    {
        RenderCore::RenderWeather Out;
        const EngineCore::Vector3 Wind = WindDirection(InWeather.WindYaw);
        Out.WindDirection[0] = float(Wind.GetX());
        Out.WindDirection[1] = float(Wind.GetY());
        Out.WindDirection[2] = float(Wind.GetZ());
        Out.WindStrength     = (std::max)(0.0f, InWeather.WindStrength);
        Out.Wetness          = Unit(InWeather.Wetness);
        Out.Rain             = Unit(InWeather.Rain);

        const float Mist = (std::max)(0.0f, InWeather.MistDensity) + (std::max)(0.0f, InWeather.RainMist) * Out.Rain;
        Out.bMist             = Mist > 0.0f ? 1 : 0;
        Out.MistDensity       = Mist * WEATHER_MIST_DENSITY_SCALE;
        Out.MistBaseHeight    = InWeather.MistHeight;
        Out.MistHeightFalloff = 1.0f / (std::max)(InWeather.MistFalloff, 1.0f);
        return Out;
    }

    WeatherComponent* FindWeather(GameCore::World& InWorld)
    {
        WeatherComponent* Found = nullptr;
        bool bSecond = false;
        InWorld.Each<WeatherComponent>([&Found, &bSecond](GameCore::Entity, WeatherComponent& InWeather)
        {
            if(Found) { bSecond = true; }
            else      { Found = &InWeather; }
        });
        if(bSecond && !GWarnedSecond)
        {
            GWarnedSecond = true;
            CORE_LOG_WARN("[Weather] a World has more than one Weather component; only the first counts")
        }
        return Found;
    }

    void Tick(GameCore::World& InWorld, const float InDeltaSeconds)
    {
        WeatherComponent* Weather = FindWeather(InWorld);
        if(!Weather)
        {
            RenderCore::ClearRenderWeather(&InWorld);
            UpdateRain(InWorld, nullptr);
            return;
        }

        const float PinnedRain    = GWeatherPinnedRain.GetFloat();
        const float PinnedWetness = GWeatherPinnedWetness.GetFloat();
        if(PinnedRain >= 0.0f) { Weather->Rain = Unit(PinnedRain); }
        Weather->Rain = Unit(Weather->Rain);

        if(PinnedWetness >= 0.0f)
        {
            Weather->Wetness = Unit(PinnedWetness);
        }
        else if(Weather->bWetnessFollowsRain)
        {
            Weather->Wetness = StepWetness(Weather->Wetness, Weather->Rain, InDeltaSeconds, Weather->WetSeconds, Weather->DrySeconds);
        }

        RenderCore::SetRenderWeather(&InWorld, ToRenderWeather(*Weather));
        UpdateRain(InWorld, Weather);
    }
}
