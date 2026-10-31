#ifndef GAMECORE_SCALABILITY_H
#define GAMECORE_SCALABILITY_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/GameCore/DetailMode.h"
#include "Public/GameCore/GameCoreAPI.h"

/*
 * Scalability: the detail tier and the power profile, each a config overlay
 * (CoreConfig.h's EConfigOverlay) laid over the project's .ini --
 *
 *     compiled default < project .ini < [PowerProfile_*] < [Scalability_<Tier>] < User.ini / console
 *
 * [Scalability]
 *   DetailMode      Low | Medium | High | Ultra       the tier: picks [Scalability_<Tier>]
 *                                                      and which entity buckets render
 *   PowerProfile    Auto | PowerSaver | PowerMax       picks [PowerProfile_Saver|Max];
 *                                                      Auto is PowerSaver on battery or mobile
 *   AutoPollSeconds how often Auto re-reads the power source; 0 never
 *
 * Each [Scalability_<Tier>] / [PowerProfile_*] section holds Section.Key=Value
 * lines, any config var. A key the project leaves out takes the engine's own
 * table (Scalability.cpp), so every project gets working tiers with no ini at
 * all. The two are independent: the profile shapes how the frame is run (job
 * occupancy, pacing, queue depth), the tier what is drawn. RenderCore's
 * SCALABILITY.md is the full guide.
 *
 * Game thread only.
 */
namespace GameCore
{
namespace Scalability
{
    enum class EPowerProfile : uint8_t { PowerSaver = 0, PowerMax = 1 };

    /* What the platform says it is running on. */
    enum class EPowerSource : uint8_t
    {
        Unknown = 0,   // no answer -- treated as mains power
        NoBattery,     // a desktop
        Plugged,       // a battery, charging or charged
        OnBattery,
    };

    /* The host's way to read the power source (the launcher's, from
     * GameWindowSystem::GetPowerSource); nullptr, the default, is Unknown. */
    using PowerSourceFn = EPowerSource (*)();
    GAMECORE_API void SetPowerSourceQuery(PowerSourceFn InFn);

    /*
     * Builds both overlays from the config now loaded. The host calls it once
     * after the project .ini and before User.ini, so a player's values land on
     * top; later changes to the [Scalability] vars re-apply on their own.
     * Again after the project's sections are reloaded (the editor's Project
     * Settings) to pick up new tables.
     */
    GAMECORE_API void Initialize();

    /* Re-reads the power source every AutoPollSeconds while the profile is
     * Auto, and switches profile when it changes. Once a frame. */
    GAMECORE_API void Tick(double InDeltaSeconds);

    GAMECORE_API EDetailMode   GetDetailMode();
    /* Through the config var, so a settings menu's save keeps it. */
    GAMECORE_API void          SetDetailMode(EDetailMode InMode);

    /* The resolved profile: never Auto. */
    GAMECORE_API EPowerProfile GetPowerProfile();
    /* "Auto", "PowerSaver" or "PowerMax" -- what the setting says. */
    GAMECORE_API const char*   GetPowerProfileSetting();
    /* False, nothing changed, if InText is none of those (case-insensitive;
     * "Saver" and "Max" also do). */
    GAMECORE_API bool          SetPowerProfileSetting(const char* InText);

    GAMECORE_API EPowerSource  GetPowerSource();

    GAMECORE_API const char*   PowerProfileName(EPowerProfile InProfile);   // "PowerSaver" / "PowerMax"
    GAMECORE_API const char*   PowerSourceName(EPowerSource InSource);

    /* Bumps whenever the tier or the profile is applied -- for a renderer
     * noticing a tier change without polling the config. */
    GAMECORE_API uint32_t      GetSerial();

    /* "Section.Key" = value lines a tier / profile resolves to: the engine's
     * table with the project's section over it -- what the overlay holds. */
    struct Override
    {
        std::string Path;
        std::string Value;
    };
    GAMECORE_API std::vector<Override> ResolveDetailOverrides(EDetailMode InMode);
    GAMECORE_API std::vector<Override> ResolvePowerOverrides(EPowerProfile InProfile);
}
}

#endif //GAMECORE_SCALABILITY_H
