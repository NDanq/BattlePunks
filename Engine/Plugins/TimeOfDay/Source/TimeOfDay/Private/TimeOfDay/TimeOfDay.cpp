#include "Public/TimeOfDay/TimeOfDay.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/RenderCore/Components/DirectionalLightComponent.h"
#include "Public/RenderCore/Components/SkyLightComponent.h"
#include "Public/TimeOfDay/TimeOfDayComponent.h"

/* Presets, the sun's path and the per-frame step. The values a game sees
 * are the plugin's Config/DefaultTimeOfDay.ini, under the project's file;
 * these defaults only hold where neither is read (a bare test). */
NIMBLE_CONFIG_FLOAT(GTimeOfDayDayLength, "TimeOfDay", "DayLengthMinutes", 30.0f, nullptr);
NIMBLE_CONFIG_FLOAT(GTimeOfDaySpeed,     "TimeOfDay", "Speed",            1.0f,  nullptr);
/* 0-24 pins every clock at that hour (a preview, a screenshot: -TimeOfDay.Hour=18.5); below 0, off. */
NIMBLE_CONFIG_FLOAT(GTimeOfDayPinnedHour, "TimeOfDay", "Hour",            -1.0f, nullptr);

namespace TimeOfDay
{
    /* TimeOfDaySky.cpp: the lights' colours from the atmosphere. */
    void SkyLightColors(const TimeOfDayComponent& InClock, EngineCore::Color& InOutSun, EngineCore::Color& OutSky,
                        EngineCore::Color& OutGround, bool& bOutDay);

    namespace
    {
        std::string Trim(const std::string& InText)
        {
            const size_t First = InText.find_first_not_of(" \t\r\n");
            if(First == std::string::npos) { return std::string(); }
            const size_t Last = InText.find_last_not_of(" \t\r\n");
            return InText.substr(First, Last - First + 1);
        }

        bool ParseFloat(const std::string& InText, float& OutValue)
        {
            char* End = nullptr;
            const float Value = std::strtof(InText.c_str(), &End);
            if(End == InText.c_str() || Trim(End).size() != 0) { return false; }
            OutValue = Value;
            return true;
        }

        bool ParseColor(const std::string& InText, EngineCore::Color& OutColor)
        {
            int Channels[3] = {};
            size_t From = 0;
            for(int Index = 0; Index < 3; ++Index)
            {
                const size_t Comma = InText.find(',', From);
                const std::string Part = Trim(InText.substr(From, Comma == std::string::npos ? std::string::npos : Comma - From));
                char* End = nullptr;
                const long Value = std::strtol(Part.c_str(), &End, 10);
                if(Part.empty() || *End != '\0' || Value < 0 || Value > 255) { return false; }
                Channels[Index] = int(Value);
                if(Index < 2)
                {
                    if(Comma == std::string::npos) { return false; }
                    From = Comma + 1;
                }
                else if(Comma != std::string::npos)
                {
                    return false;
                }
            }
            OutColor = EngineCore::Color(uint8_t(Channels[0]), uint8_t(Channels[1]), uint8_t(Channels[2]));
            return true;
        }

        float WrapHour(const float InHour)
        {
            float Hour = std::fmod(InHour, 24.0f);
            if(Hour < 0.0f) { Hour += 24.0f; }
            return Hour;
        }

        uint8_t LerpByte(const uint8_t InA, const uint8_t InB, const float InT)
        {
            return uint8_t(std::lround(float(InA) + (float(InB) - float(InA)) * InT));
        }

        EngineCore::Color LerpColor(const EngineCore::Color& InA, const EngineCore::Color& InB, const float InT)
        {
            return EngineCore::Color(LerpByte(InA.R, InB.R, InT), LerpByte(InA.G, InB.G, InT), LerpByte(InA.B, InB.B, InT));
        }

        /* A clear day, for a preset that will not load. */
        const std::vector<TimeOfDayKey>& BuiltInKeys()
        {
            static const std::vector<TimeOfDayKey> Keys = []()
            {
                std::vector<TimeOfDayKey> Result(3);
                Result[0].Hour = 0.0f;  Result[0].SunColor = { 90, 110, 170 };  Result[0].SunIntensity = 0.15f;
                Result[0].SkyColor = { 20, 26, 45 };    Result[0].GroundColor = { 10, 10, 14 }; Result[0].SkyIntensity = 0.15f;
                Result[1].Hour = 12.0f; Result[1].SunColor = { 255, 250, 240 }; Result[1].SunIntensity = 3.2f;
                Result[1].SkyColor = { 130, 150, 190 }; Result[1].GroundColor = { 65, 60, 55 }; Result[1].SkyIntensity = 0.65f;
                Result[2].Hour = 19.0f; Result[2].SunColor = { 255, 120, 70 };  Result[2].SunIntensity = 0.7f;
                Result[2].SkyColor = { 95, 70, 95 };    Result[2].GroundColor = { 36, 28, 28 }; Result[2].SkyIntensity = 0.3f;
                return Result;
            }();
            return Keys;
        }

        /* Game thread only, as every World tick is. */
        std::map<std::string, std::vector<TimeOfDayKey>>& PresetCache()
        {
            static std::map<std::string, std::vector<TimeOfDayKey>> Cache;
            return Cache;
        }
    }

    bool ParsePreset(const std::string& InText, std::vector<TimeOfDayKey>& OutKeys, std::string* OutError)
    {
        std::vector<TimeOfDayKey> Keys;
        bool bInKey = false;
        bool bHasHour = false;
        int  LineNumber = 0;

        const auto Fail = [&](const std::string& InWhy)
        {
            if(OutError) { *OutError = "line " + std::to_string(LineNumber) + ": " + InWhy; }
            return false;
        };

        size_t Begin = 0;
        while(Begin <= InText.size())
        {
            size_t End = InText.find('\n', Begin);
            if(End == std::string::npos) { End = InText.size(); }
            const std::string Line = Trim(InText.substr(Begin, End - Begin));
            Begin = End + 1;
            ++LineNumber;

            if(Line.empty() || Line[0] == ';' || Line[0] == '#') { continue; }
            if(Line.front() == '[' && Line.back() == ']')
            {
                if(bInKey && !bHasHour) { return Fail("the key before this one has no Hour"); }
                bInKey   = Line.compare(0, 5, "[Key_") == 0;
                bHasHour = false;
                if(bInKey) { Keys.emplace_back(); }
                continue;
            }
            if(!bInKey) { continue; }

            const size_t Eq = Line.find('=');
            if(Eq == std::string::npos) { return Fail("expected Key=Value"); }
            const std::string Key   = Trim(Line.substr(0, Eq));
            const std::string Value = Trim(Line.substr(Eq + 1));
            TimeOfDayKey& Out = Keys.back();

            bool bOk = true;
            if(Key == "Hour")              { bOk = ParseFloat(Value, Out.Hour) && Out.Hour >= 0.0f && Out.Hour <= 24.0f; bHasHour = bOk; }
            else if(Key == "SunColor")     { bOk = ParseColor(Value, Out.SunColor); }
            else if(Key == "SunIntensity") { bOk = ParseFloat(Value, Out.SunIntensity); }
            else if(Key == "SkyColor")     { bOk = ParseColor(Value, Out.SkyColor); }
            else if(Key == "GroundColor")  { bOk = ParseColor(Value, Out.GroundColor); }
            else if(Key == "SkyIntensity") { bOk = ParseFloat(Value, Out.SkyIntensity); }
            else                           { return Fail("unknown key '" + Key + "'"); }
            if(!bOk) { return Fail("'" + Value + "' is not a valid " + Key); }
        }
        if(bInKey && !bHasHour) { return Fail("the last key has no Hour"); }
        if(Keys.empty())
        {
            if(OutError) { *OutError = "no [Key_*] sections"; }
            return false;
        }

        std::stable_sort(Keys.begin(), Keys.end(), [](const TimeOfDayKey& InA, const TimeOfDayKey& InB) { return InA.Hour < InB.Hour; });
        OutKeys = std::move(Keys);
        return true;
    }

    const std::vector<TimeOfDayKey>& LoadPreset(const std::string& InPath)
    {
        std::map<std::string, std::vector<TimeOfDayKey>>& Cache = PresetCache();
        const auto Found = Cache.find(InPath);
        if(Found != Cache.end())
        {
            return Found->second;
        }

        std::vector<TimeOfDayKey> Keys;
        std::string Text;
        std::string Why;
        EngineCore::IFileProvider* Provider = EngineCore::GetFileProvider();
        if(!Provider || !Provider->ReadAllText(InPath, Text))
        {
            Why = "cannot be read";
        }
        else if(!ParsePreset(Text, Keys, &Why))
        {
            Keys.clear();
        }
        if(Keys.empty())
        {
            CORE_LOG_WARN("[TimeOfDay] preset '" + InPath + "' " + Why + "; using the built-in clear day")
            Keys = BuiltInKeys();
        }
        return Cache.emplace(InPath, std::move(Keys)).first->second;
    }

    void ClearPresetCache()
    {
        PresetCache().clear();
    }

    TimeOfDayKey Evaluate(const std::vector<TimeOfDayKey>& InKeys, const float InHour)
    {
        const std::vector<TimeOfDayKey>& Keys = InKeys.empty() ? BuiltInKeys() : InKeys;
        const float Hour = WrapHour(InHour);

        /* The keys either side of Hour, wrapping past the last into the first. */
        size_t Next = 0;
        while(Next < Keys.size() && Keys[Next].Hour <= Hour) { ++Next; }
        const TimeOfDayKey& A = Keys[(Next + Keys.size() - 1) % Keys.size()];
        const TimeOfDayKey& B = Keys[Next % Keys.size()];

        float Span = B.Hour - A.Hour;
        float Into = Hour - A.Hour;
        if(Span <= 0.0f) { Span += 24.0f; }
        if(Into < 0.0f)  { Into += 24.0f; }
        const float T = Span > 0.0f ? (std::min)(1.0f, Into / Span) : 0.0f;

        TimeOfDayKey Result;
        Result.Hour         = Hour;
        Result.SunColor     = LerpColor(A.SunColor, B.SunColor, T);
        Result.SunIntensity = A.SunIntensity + (B.SunIntensity - A.SunIntensity) * T;
        Result.SkyColor     = LerpColor(A.SkyColor, B.SkyColor, T);
        Result.GroundColor  = LerpColor(A.GroundColor, B.GroundColor, T);
        Result.SkyIntensity = A.SkyIntensity + (B.SkyIntensity - A.SkyIntensity) * T;
        return Result;
    }

    bool IsDaytime(const float InHour)
    {
        const float Hour = WrapHour(InHour);
        return Hour >= 6.0f && Hour < 18.0f;
    }

    EngineCore::Vector3 SunRotation(const float InHour, const float InNorthYaw, const float InLatitude)
    {
        /* Across its arc, 0 at rising to 1 at setting: the sun 6 to 18, the
         * moon 18 to 6. */
        const float Hour   = WrapHour(InHour);
        const float Across = IsDaytime(Hour) ? (Hour - 6.0f) / 12.0f : WrapHour(Hour - 18.0f) / 12.0f;

        /* Highest at the middle of the arc, never quite down to the horizon,
         * where a sun-grazing shadow would stretch to the far plane. */
        const float Peak      = (std::max)(10.0f, 90.0f - std::fabs(InLatitude));
        const float Elevation = (std::max)(4.0f, Peak * std::sin(Across * 3.14159265f));

        /* Light travels away from the sun: westward in the morning (the sun
         * in the east), eastward in the evening. Pitch tips +Z down
         * (LightForward); yaw sweeps it across. */
        const float Yaw = InNorthYaw - 90.0f + Across * 180.0f;
        return EngineCore::Vector3(Elevation, Yaw, 0.0f);
    }

    void Tick(GameCore::World& InWorld, const float InDeltaSeconds)
    {
        const float Speed      = GTimeOfDaySpeed.GetFloat();
        const float DefaultDay = GTimeOfDayDayLength.GetFloat();
        const float Pinned     = GTimeOfDayPinnedHour.GetFloat();
        static const char* HourEvent = "TimeOfDayHour";

        InWorld.Each<TimeOfDayComponent>([&](GameCore::Entity InEntity, TimeOfDayComponent& Clock)
        {
            const float Before = Clock.Hour;
            if(Clock.bRunning && Speed > 0.0f)
            {
                const float Minutes = Clock.DayLengthMinutes > 0.0f ? Clock.DayLengthMinutes : DefaultDay;
                if(Minutes > 0.0f)
                {
                    Clock.Hour += InDeltaSeconds * Speed * 24.0f / (Minutes * 60.0f);
                }
            }
            if(Pinned >= 0.0f) { Clock.Hour = Pinned; }
            Clock.Hour = WrapHour(Clock.Hour);

            /* TimeOfDayHour(hour, clock) as each whole hour begins. */
            if(int(std::floor(Before)) != int(std::floor(Clock.Hour)))
            {
                GameCore::EventArgs Args;
                Args.Args[0] = GameCore::EventArg::Make(double(std::floor(Clock.Hour)));
                Args.Args[1] = GameCore::EventArg::Make(InEntity.GetID());
                Args.Count   = 2;
                InWorld.GetEvents().Emit(HourEvent, Args);
            }

            Clock.Preset[sizeof(Clock.Preset) - 1] = '\0';
            TimeOfDayKey Now = Evaluate(LoadPreset(Clock.Preset), Clock.Hour);
            if(Clock.bDrawSky)
            {
                /* The colours the drawn sky implies; the preset keeps every
                 * intensity, and the moonlight's colour. */
                bool bDay = false;
                SkyLightColors(Clock, Now.SunColor, Now.SkyColor, Now.GroundColor, bDay);
            }

            if(GameCore::TransformComponent* Transform = InEntity.GetComponent<GameCore::TransformComponent>())
            {
                Transform->SetWorldRotation(SunRotation(Clock.Hour, Clock.NorthYaw, Clock.Latitude));
            }
            if(RenderCore::DirectionalLightComponent* Sun = InEntity.GetComponent<RenderCore::DirectionalLightComponent>())
            {
                Sun->Color     = Now.SunColor;
                Sun->Intensity = Now.SunIntensity * Clock.SunIntensityScale;
            }
            if(RenderCore::SkyLightComponent* Sky = InEntity.GetComponent<RenderCore::SkyLightComponent>())
            {
                Sky->SkyColor    = Now.SkyColor;
                Sky->GroundColor = Now.GroundColor;
                Sky->Intensity   = Now.SkyIntensity;
            }
        });
    }
}
