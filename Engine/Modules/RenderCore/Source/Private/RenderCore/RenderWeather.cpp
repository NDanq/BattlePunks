#include "Public/RenderCore/RenderWeather.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "Public/RenderCore/PostProcessStack.h"

namespace RenderCore
{
    namespace
    {
        std::mutex& WeatherMutex()
        {
            static std::mutex Mutex;
            return Mutex;
        }

        std::unordered_map<const void*, RenderWeather>& Weathers()
        {
            static std::unordered_map<const void*, RenderWeather> Map;
            return Map;
        }
    }

    void SetRenderWeather(const void* InWorld, const RenderWeather& InWeather)
    {
        RenderWeather Clean = InWeather;
        /* A unit direction, flat or not; none at all falls back to the default's. */
        const float Length = std::sqrt(Clean.WindDirection[0] * Clean.WindDirection[0] + Clean.WindDirection[1] * Clean.WindDirection[1]
                                       + Clean.WindDirection[2] * Clean.WindDirection[2]);
        if(Length > 1e-6f)
        {
            for(float& Each : Clean.WindDirection) { Each /= Length; }
        }
        else
        {
            const RenderWeather Default;
            std::copy(Default.WindDirection, Default.WindDirection + 3, Clean.WindDirection);
        }
        Clean.WindStrength      = (std::max)(0.0f, Clean.WindStrength);
        Clean.Wetness           = (std::min)(1.0f, (std::max)(0.0f, Clean.Wetness));
        Clean.Rain              = (std::min)(1.0f, (std::max)(0.0f, Clean.Rain));
        Clean.MistDensity       = (std::max)(0.0f, Clean.MistDensity);
        Clean.MistHeightFalloff = (std::max)(0.0f, Clean.MistHeightFalloff);

        std::lock_guard<std::mutex> Lock(WeatherMutex());
        Weathers()[InWorld] = Clean;
    }

    void ClearRenderWeather(const void* InWorld)
    {
        std::lock_guard<std::mutex> Lock(WeatherMutex());
        Weathers().erase(InWorld);
    }

    RenderWeather GetRenderWeather(const void* InWorld, bool* bOutSet)
    {
        std::lock_guard<std::mutex> Lock(WeatherMutex());
        const auto Found = Weathers().find(InWorld);
        if(bOutSet) { *bOutSet = Found != Weathers().end(); }
        return Found != Weathers().end() ? Found->second : RenderWeather();
    }

    void ApplyRenderWeatherFog(const RenderWeather& InWeather, PostProcessSettings& InOutSettings)
    {
        if(!InWeather.bMist)
        {
            return;
        }
        InOutSettings.bVolumetricFog   = 1;
        InOutSettings.FogDensity       = (std::max)(0.0f, InWeather.MistDensity);
        InOutSettings.FogBaseHeight    = InWeather.MistBaseHeight;
        InOutSettings.FogHeightFalloff = (std::max)(0.0f, InWeather.MistHeightFalloff);
    }
}
