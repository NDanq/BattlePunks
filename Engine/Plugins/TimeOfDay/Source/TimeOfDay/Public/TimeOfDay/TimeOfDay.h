#ifndef TIMEOFDAY_TIMEOFDAY_H
#define TIMEOFDAY_TIMEOFDAY_H

#include <string>
#include <vector>

#include "Public/Core/CoreTypes.h"
#include "Public/Math/Vector.h"
#include "Public/TimeOfDay/TimeOfDayAPI.h"

namespace GameCore
{
    class World;
}

/*
 * TimeOfDay's code side: presets, the sun's path and the per-frame step the
 * plugin runs on every level's World (TimeOfDayPlugin.cpp). A game that
 * links the plugin may call these directly.
 */
namespace TimeOfDay
{
    /* One [Key_*] of a .todpreset. */
    struct TimeOfDayKey
    {
        float             Hour         = 0.0f;
        EngineCore::Color SunColor{ 255, 255, 255 };
        float             SunIntensity = 3.0f;
        EngineCore::Color SkyColor{ 120, 130, 150 };
        EngineCore::Color GroundColor{ 60, 55, 50 };
        float             SkyIntensity = 0.6f;
    };

    /* InText's keys, sorted by hour. False, with OutError, if it has none or
     * a value will not parse. */
    TIMEOFDAY_API bool ParsePreset(const std::string& InText, std::vector<TimeOfDayKey>& OutKeys, std::string* OutError = nullptr);

    /* The preset at InPath, through the asset provider, cached by path. A
     * preset that will not load is logged once and yields the built-in
     * clear day. Never empty. */
    TIMEOFDAY_API const std::vector<TimeOfDayKey>& LoadPreset(const std::string& InPath);

    /* Forgets every cached preset: the next LoadPreset reads its file again. */
    TIMEOFDAY_API void ClearPresetCache();

    /* InKeys at InHour (wrapped into 0-24), blended linearly, across
     * midnight from the last key to the first. Hour of the result is InHour. */
    TIMEOFDAY_API TimeOfDayKey Evaluate(const std::vector<TimeOfDayKey>& InKeys, float InHour);

    /* The light's Transform Rotation (Euler degrees) at InHour: the sun's
     * from 6 to 18, the moon's on the same arc the rest of the night. */
    TIMEOFDAY_API EngineCore::Vector3 SunRotation(float InHour, float InNorthYaw, float InLatitude);

    /* True from 6 to 18. */
    TIMEOFDAY_API bool IsDaytime(float InHour);

    /* Every Time Of Day component in InWorld: Hour advanced by
     * InDeltaSeconds and its lights set. What the plugin's system runs. */
    TIMEOFDAY_API void Tick(GameCore::World& InWorld, float InDeltaSeconds);

    /* Whether the plugin host started this plugin (Plugins::LoadModules). */
    TIMEOFDAY_API bool IsPluginStarted();
}

#endif //TIMEOFDAY_TIMEOFDAY_H
