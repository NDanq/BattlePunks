#include "Public/Weather/WeatherComponent.h"

#include <algorithm>

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"

#include "Public/Weather/Weather.h"

/* WeatherComponent: registration, saves, the editor's fields, its fields in
 * Lua (entity:GetComponent("Weather").Rain), and the "Weather" script
 * library. All of it registers as the plugin's library loads. */

namespace Weather
{
    GAMECORE_DEFINE_COMPONENT(WeatherComponent, "Weather")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(WeatherComponent, 1)
    {
        Ar.Field("Rain",              C.Rain);
        Ar.Field("RainMist",          C.RainMist);
        Ar.Flag("RainParticles",      C.bRainParticles);
        Ar.Field("MistDensity",       C.MistDensity);
        Ar.Field("MistHeight",        C.MistHeight);
        Ar.Field("MistFalloff",       C.MistFalloff);
        Ar.Field("Wetness",           C.Wetness);
        Ar.Flag("WetnessFollowsRain", C.bWetnessFollowsRain);
        Ar.Field("WetSeconds",        C.WetSeconds);
        Ar.Field("DrySeconds",        C.DrySeconds);
        Ar.Field("WindYaw",           C.WindYaw);
        Ar.Field("WindStrength",      C.WindStrength);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(WeatherComponent)
    {
        Bind.Field("Rain",              &WeatherComponent::Rain);
        Bind.Field("RainMist",          &WeatherComponent::RainMist);
        Bind.Flag("RainParticles",      &WeatherComponent::bRainParticles);
        Bind.Field("MistDensity",       &WeatherComponent::MistDensity);
        Bind.Field("MistHeight",        &WeatherComponent::MistHeight);
        Bind.Field("MistFalloff",       &WeatherComponent::MistFalloff);
        Bind.Field("Wetness",           &WeatherComponent::Wetness);
        Bind.Flag("WetnessFollowsRain", &WeatherComponent::bWetnessFollowsRain);
        Bind.Field("WetSeconds",        &WeatherComponent::WetSeconds);
        Bind.Field("DrySeconds",        &WeatherComponent::DrySeconds);
        Bind.Field("WindYaw",           &WeatherComponent::WindYaw);
        Bind.Field("WindStrength",      &WeatherComponent::WindStrength);
    }

    /* The server's weather is the weather. */
    GAMECORE_DEFINE_COMPONENT_REPLICATION(WeatherComponent)
    {
        Rep.Field("Rain",              &WeatherComponent::Rain);
        Rep.Field("RainMist",          &WeatherComponent::RainMist);
        Rep.Flag("RainParticles",      &WeatherComponent::bRainParticles);
        Rep.Field("MistDensity",       &WeatherComponent::MistDensity);
        Rep.Field("MistHeight",        &WeatherComponent::MistHeight);
        Rep.Field("MistFalloff",       &WeatherComponent::MistFalloff);
        Rep.Field("Wetness",           &WeatherComponent::Wetness);
        Rep.Flag("WetnessFollowsRain", &WeatherComponent::bWetnessFollowsRain);
        Rep.Field("WindYaw",           &WeatherComponent::WindYaw);
        Rep.Field("WindStrength",      &WeatherComponent::WindStrength);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(WeatherComponent)
    {
        Props.TypeCategory("Lighting");
        Props.Category("Rain");
        Props.Field("Rain", &WeatherComponent::Rain);
        Props.Range(0.0f, 1.0f);
        Props.Tooltip("0 dry to 1 a downpour: the drops round the camera, the rain's mist, and (following) the wetness.");
        Props.Flag("Rain Particles", &WeatherComponent::bRainParticles);
        Props.Tooltip("Draw the drops. Off, the rain still thickens the mist and wets the ground.");
        Props.Field("Rain Mist", &WeatherComponent::RainMist);
        Props.Range(0.0f, 1.0f);
        Props.Tooltip("Mist Density the rain adds at Rain 1.");
        Props.Category("Mist");
        Props.Field("Mist Density", &WeatherComponent::MistDensity);
        Props.Range(0.0f, 1.0f);
        Props.Tooltip("Drives the renderer's Volumetric Fog: 0 leaves the level's fog alone, 1 is thick "
                      "(about 30 m of sight at 1 unit = 1 cm). Needs an HDR scene.");
        Props.Field("Mist Height", &WeatherComponent::MistHeight);
        Props.Range(-100000.0f, 100000.0f);
        Props.Tooltip("World Y at and below which the mist is at full density.");
        Props.Field("Mist Falloff", &WeatherComponent::MistFalloff);
        Props.Range(1.0f, 100000.0f);
        Props.Tooltip("World units above Mist Height over which the mist thins to a third (1/e).");
        Props.Category("Wetness");
        Props.Field("Wetness", &WeatherComponent::Wetness);
        Props.Range(0.0f, 1.0f);
        Props.Tooltip("0 dry to 1 soaked: upward-facing surfaces darken and gloss up. Set by the rain while Follows Rain is on.");
        Props.Flag("Follows Rain", &WeatherComponent::bWetnessFollowsRain);
        Props.Tooltip("Wetness soaks up under rain and dries after it.");
        Props.Field("Wet Seconds", &WeatherComponent::WetSeconds);
        Props.Range(0.1f, 3600.0f);
        Props.Tooltip("Seconds to soak from dry at Rain 1 (longer in lighter rain).");
        Props.Field("Dry Seconds", &WeatherComponent::DrySeconds);
        Props.Range(0.1f, 3600.0f);
        Props.Tooltip("Seconds to dry from soaked once the rain stops.");
        Props.Category("Wind");
        Props.Field("Wind Yaw", &WeatherComponent::WindYaw);
        Props.Range(-180.0f, 180.0f);
        Props.Tooltip("Degrees about +Y the wind blows toward: 0 is +Z, 90 is +X.");
        Props.Field("Wind Strength", &WeatherComponent::WindStrength);
        Props.Range(0.0f, 5.0f);
        Props.Tooltip("0 calm, 1 the breeze foliage materials are made for (their Wind node), more a gale; the rain drifts with it.");
    }
#endif // WITH_EDITOR
}

namespace
{
    Weather::WeatherComponent* Find(GameCore::ScriptCall& InCall)
    {
        Weather::WeatherComponent* Found = Weather::FindWeather(InCall.GetWorld());
        if(!Found)
        {
            InCall.Error("this world has no Weather component");
        }
        return Found;
    }

    float Unit(const double InValue)
    {
        return (std::min)(1.0f, (std::max)(0.0f, float(InValue)));
    }

    void ScriptGetRain(GameCore::ScriptCall& InCall)
    {
        if(Weather::WeatherComponent* Found = Find(InCall)) { InCall.PushNumber(Found->Rain); }
    }

    void ScriptSetRain(GameCore::ScriptCall& InCall)
    {
        const double Rain = InCall.GetNumber(1);
        if(InCall.HasError()) { return; }
        if(Weather::WeatherComponent* Found = Find(InCall)) { Found->Rain = Unit(Rain); }
    }

    void ScriptGetWetness(GameCore::ScriptCall& InCall)
    {
        if(Weather::WeatherComponent* Found = Find(InCall)) { InCall.PushNumber(Found->Wetness); }
    }

    void ScriptSetWetness(GameCore::ScriptCall& InCall)
    {
        const double Wetness = InCall.GetNumber(1);
        if(InCall.HasError()) { return; }
        if(Weather::WeatherComponent* Found = Find(InCall)) { Found->Wetness = Unit(Wetness); }
    }

    void ScriptSetMist(GameCore::ScriptCall& InCall)
    {
        const double Density = InCall.GetNumber(1);
        if(InCall.HasError()) { return; }
        if(Weather::WeatherComponent* Found = Find(InCall)) { Found->MistDensity = Unit(Density); }
    }

    void ScriptSetWind(GameCore::ScriptCall& InCall)
    {
        const double Yaw      = InCall.GetNumber(1);
        const double Strength = InCall.GetNumber(2);
        if(InCall.HasError()) { return; }
        if(Weather::WeatherComponent* Found = Find(InCall))
        {
            Found->WindYaw      = float(Yaw);
            Found->WindStrength = (std::max)(0.0f, float(Strength));
        }
    }
}

/*
 * The level's weather from Lua and script graphs (its component's fields are
 * also reachable as entity:GetComponent("Weather").Rain and so on):
 *
 *     Weather.SetRain(0.7)
 *     Weather.SetWind(90, 2.5)
 */
GAMECORE_DEFINE_SCRIPT_LIBRARY(Weather)
{
    Lib.Function("GetRain",    &ScriptGetRain,    "-> Rain:number");
    Lib.Function("SetRain",    &ScriptSetRain,    "Rain:number");
    Lib.Function("GetWetness", &ScriptGetWetness, "-> Wetness:number");
    Lib.Function("SetWetness", &ScriptSetWetness, "Wetness:number");
    Lib.Function("SetMist",    &ScriptSetMist,    "Density:number");
    Lib.Function("SetWind",    &ScriptSetWind,    "Yaw:number, Strength:number");
}
