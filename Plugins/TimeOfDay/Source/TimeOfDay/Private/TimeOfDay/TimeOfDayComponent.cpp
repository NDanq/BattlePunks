#include "Public/TimeOfDay/TimeOfDayComponent.h"

#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/World.h"

#include "Public/TimeOfDay/TimeOfDay.h"

/* TimeOfDayComponent: registration, saves, the editor's fields, and the
 * "TimeOfDay" script library. All of it registers as the plugin's library
 * loads, as an engine module's does. */

namespace TimeOfDay
{
    GAMECORE_DEFINE_COMPONENT(TimeOfDayComponent, "Time Of Day")

    GAMECORE_DEFINE_COMPONENT_SERIALIZER(TimeOfDayComponent, 1)
    {
        Ar.Field("Hour",             C.Hour);
        Ar.Field("DayLengthMinutes", C.DayLengthMinutes);
        Ar.Flag("Running",           C.bRunning);
        Ar.Field("NorthYaw",         C.NorthYaw);
        Ar.Field("Latitude",         C.Latitude);
        Ar.Field("SunIntensityScale", C.SunIntensityScale);
        Ar.Field("Preset",           C.Preset);
    }

    GAMECORE_DEFINE_COMPONENT_SCRIPT(TimeOfDayComponent)
    {
        Bind.Field("Hour",             &TimeOfDayComponent::Hour);
        Bind.Field("DayLengthMinutes", &TimeOfDayComponent::DayLengthMinutes);
        Bind.Flag("Running",           &TimeOfDayComponent::bRunning);
        Bind.Field("NorthYaw",         &TimeOfDayComponent::NorthYaw);
        Bind.Field("Latitude",         &TimeOfDayComponent::Latitude);
        Bind.Field("SunIntensityScale", &TimeOfDayComponent::SunIntensityScale);
    }

    /* The server's clock is the clock; a client runs its copy between updates. */
    GAMECORE_DEFINE_COMPONENT_REPLICATION(TimeOfDayComponent)
    {
        Rep.Field("Hour",             &TimeOfDayComponent::Hour);
        Rep.Field("DayLengthMinutes", &TimeOfDayComponent::DayLengthMinutes);
        Rep.Flag("Running",           &TimeOfDayComponent::bRunning);
    }

#if WITH_EDITOR
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(TimeOfDayComponent)
    {
        Props.TypeCategory("Lighting");
        Props.Category("Time Of Day");
        Props.Field("Hour", &TimeOfDayComponent::Hour);
        Props.Range(0.0f, 24.0f);
        Props.Tooltip("The time, 0 to 24. Sets the entity's Directional Light and Sky Light, and its rotation.");
        Props.Field("Day Length (min)", &TimeOfDayComponent::DayLengthMinutes);
        Props.Range(0.0f, 1440.0f);
        Props.Tooltip("Real minutes per game day; 0 uses [TimeOfDay] DayLengthMinutes.");
        Props.Flag("Running", &TimeOfDayComponent::bRunning);
        Props.Field("North Yaw", &TimeOfDayComponent::NorthYaw);
        Props.Range(-180.0f, 180.0f);
        Props.Field("Latitude", &TimeOfDayComponent::Latitude);
        Props.Range(-80.0f, 80.0f);
        Props.Tooltip("The noon sun stands 90 - Latitude degrees above the horizon.");
        Props.Field("Sun Intensity Scale", &TimeOfDayComponent::SunIntensityScale);
        Props.Range(0.0f, 20.0f);
        Props.Tooltip("Multiplies the preset's sun intensity, for a level exposed for a brighter sun.");
        Props.Field("Preset", &TimeOfDayComponent::Preset);
        Props.Tooltip("A .todpreset: TimeOfDay:/Presets/Default.todpreset, or the project's own Content/....");
    }
#endif // WITH_EDITOR
}

namespace
{
    /* The world's first clock: what the library's calls act on. */
    TimeOfDay::TimeOfDayComponent* FirstClock(GameCore::World& InWorld)
    {
        TimeOfDay::TimeOfDayComponent* Found = nullptr;
        InWorld.Each<TimeOfDay::TimeOfDayComponent>([&Found](GameCore::Entity, TimeOfDay::TimeOfDayComponent& InClock)
        {
            if(!Found) { Found = &InClock; }
        });
        return Found;
    }

    TimeOfDay::TimeOfDayComponent* Clock(GameCore::ScriptCall& InCall)
    {
        TimeOfDay::TimeOfDayComponent* Found = FirstClock(InCall.GetWorld());
        if(!Found)
        {
            InCall.Error("this world has no Time Of Day component");
        }
        return Found;
    }

    void ScriptGetHour(GameCore::ScriptCall& InCall)
    {
        TimeOfDay::TimeOfDayComponent* Found = Clock(InCall);
        if(Found) { InCall.PushNumber(Found->Hour); }
    }

    void ScriptSetHour(GameCore::ScriptCall& InCall)
    {
        const double Hour = InCall.GetNumber(1);
        if(InCall.HasError()) { return; }
        if(TimeOfDay::TimeOfDayComponent* Found = Clock(InCall))
        {
            float Wrapped = float(Hour) - 24.0f * float(int(Hour / 24.0));
            if(Wrapped < 0.0f) { Wrapped += 24.0f; }
            Found->Hour = Wrapped;
        }
    }

    void ScriptSetRunning(GameCore::ScriptCall& InCall)
    {
        const bool bRunning = InCall.GetBool(1);
        if(InCall.HasError()) { return; }
        if(TimeOfDay::TimeOfDayComponent* Found = Clock(InCall))
        {
            Found->bRunning = bRunning ? 1 : 0;
        }
    }

    void ScriptIsDaytime(GameCore::ScriptCall& InCall)
    {
        TimeOfDay::TimeOfDayComponent* Found = Clock(InCall);
        if(Found) { InCall.PushBool(TimeOfDay::IsDaytime(Found->Hour)); }
    }
}

/*
 * The level's clock from Lua and script graphs:
 *
 *     TimeOfDay.SetHour(18.5)
 *     Events.On("TimeOfDayHour", function(hour, clock) ... end)
 */
GAMECORE_DEFINE_SCRIPT_LIBRARY(TimeOfDay)
{
    Lib.Function("GetHour",    &ScriptGetHour,    "-> Hour:number");
    Lib.Function("SetHour",    &ScriptSetHour,    "Hour:number");
    Lib.Function("SetRunning", &ScriptSetRunning, "Running:bool");
    Lib.Function("IsDaytime",  &ScriptIsDaytime,  "-> Daytime:bool");
}
