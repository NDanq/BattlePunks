/*
 * TimeOfDayTests -- the TimeOfDay plugin, and through it the plugin host
 * (NimbleEngine/Plugin.h) as a game sees it: the build's manifest and staged
 * files beside this executable, the plugin's config under a project's, its
 * content reachable only as "TimeOfDay:/...", its library started, and its
 * system on a World through the host's world hooks. Then the plugin's own
 * presets, sun path and clock.
 *
 * Runs from the runtime directory (nimble_add_plugin_test), where the
 * build left Plugins/Plugins.manifest and Plugins/TimeOfDay/.
 */
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Public/RenderCore/Components/DirectionalLightComponent.h"
#include "Public/RenderCore/Components/LightShadow.h"
#include "Public/RenderCore/Components/SkyLightComponent.h"
#include "Public/RenderCore/CustomRenderPass.h"
#include "Public/TimeOfDay/TimeOfDay.h"
#include "Public/TimeOfDay/TimeOfDayComponent.h"

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const std::string& What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What.c_str());
    }
}

namespace
{
    bool Near(const float InA, const float InB, const float InTolerance = 0.01f)
    {
        return std::fabs(InA - InB) <= InTolerance;
    }

    float ConfigFloat(const char* InPath)
    {
        const EngineCore::ConfigVar* Var = EngineCore::ConfigFindVarByPath(InPath);
        return Var ? Var->GetFloat() : -1.0f;
    }

    int GHourEvents = 0;
    double GLastHour = -1.0;
    void OnHour(GameCore::EventID, const GameCore::EventArgs& InArgs, void*)
    {
        ++GHourEvents;
        if(InArgs.Count > 0) { GLastHour = InArgs.Args[0].Number; }
    }

    void TestPrefixes()
    {
        std::printf("prefixes\n");
        Expect(NimbleEngine::GetPluginPrefix("TimeOfDay:/Presets/A.todpreset") == "TimeOfDay", "a plugin path has its prefix");
        Expect(NimbleEngine::GetPluginPrefix("Ab1:/x") == "Ab1", "letters then digits make a prefix");
        Expect(NimbleEngine::GetPluginPrefix("C:/Windows/x").empty(), "a drive letter is not a prefix");
        Expect(NimbleEngine::GetPluginPrefix("Content/Presets/A.todpreset").empty(), "a project path has no prefix");
        Expect(NimbleEngine::GetPluginPrefix("1Ab:/x").empty(), "a prefix starts with a letter");
        Expect(NimbleEngine::GetPluginPrefix("Time Of:/x").empty(), "a prefix has no spaces");
        Expect(NimbleEngine::GetPluginPrefix("TimeOfDay:x").empty(), "a prefix ends in :/");
    }

    /* The host end to end, on what the build staged beside this executable. */
    void TestHost(EngineCore::LayeredFileProvider& InAssets)
    {
        std::printf("host\n");
        Expect(TimeOfDay::IsPluginStarted() == false, "not started before LoadModules");

        const int Count = NimbleEngine::Plugins::Discover(std::string(), false);
        Expect(Count >= 1, "the manifest lists a plugin");
        bool bListed = false;
        for(size_t Index = 0; Index < NimbleEngine::Plugins::GetCount(); ++Index)
        {
            bListed = bListed || NimbleEngine::Plugins::GetNameAt(Index) == "TimeOfDay";
        }
        Expect(bListed, "the manifest lists TimeOfDay");

        /* Its Config/DefaultTimeOfDay.ini, over the var's C++ default (30). */
        Expect(Near(ConfigFloat("TimeOfDay.DayLengthMinutes"), 24.0f), "the plugin's config applied");
        /* A project's file, loaded after, wins. */
        EngineCore::LoadConfigText("[TimeOfDay]\nDayLengthMinutes=12\n");
        Expect(Near(ConfigFloat("TimeOfDay.DayLengthMinutes"), 12.0f), "the project's config wins over the plugin's");
        EngineCore::LoadConfigText("[TimeOfDay]\nDayLengthMinutes=24\n");

        NimbleEngine::Plugins::MountContent(InAssets);
        Expect(InAssets.Exists("TimeOfDay:/Presets/Default.todpreset"), "content reads by its prefix");
        Expect(!InAssets.Exists("Presets/Default.todpreset"), "content does not read without its prefix");
        Expect(!InAssets.Exists("Content/Presets/Default.todpreset"), "content does not read as the project's");
        Expect(!InAssets.Exists("TimeOfDay:/Content/Presets/Default.todpreset"), "the prefix is Content/ itself");
        Expect(!InAssets.Exists("Nope:/Presets/Default.todpreset"), "a prefix no plugin owns reads nothing");

        Expect(NimbleEngine::Plugins::LoadModules(), "the plugin's library loads");
        Expect(TimeOfDay::IsPluginStarted(), "its IPlugin started");
        Expect(NimbleEngine::Plugins::IsLoaded("TimeOfDay"), "the host knows it started");
        Expect(NimbleEngine::Plugins::LoadModules(), "a second LoadModules is a no-op");

        /* Its system arrives on a World through the host's hook. */
        GameCore::World World;
        World.GetEvents().Subscribe("TimeOfDayHour", &OnHour);
        NimbleEngine::Plugins::AttachWorld(World);

        GameCore::Entity Sun = World.CreateEntity("Sun");
        Sun.AddComponent<GameCore::TransformComponent>();
        Sun.AddComponent<RenderCore::DirectionalLightComponent>();
        Sun.AddComponent<RenderCore::SkyLightComponent>();
        TimeOfDay::TimeOfDayComponent Init;
        Init.Hour             = 11.5f;
        Init.DayLengthMinutes = 1.0f;   // an hour every 2.5 s
        Init.bDrawSky         = 0;      // the preset's colours, not the atmosphere's
        Sun.AddComponent(Init);
        Expect(RenderCore::GetCustomRenderPassCount(&World) == 2,
               "the sky's passes are registered on the World: the background and the sky image-based lighting sees");

        World.Tick(2.5f);
        const TimeOfDay::TimeOfDayComponent* Clock = Sun.GetComponent<TimeOfDay::TimeOfDayComponent>();
        Expect(Clock && Near(Clock->Hour, 12.5f), "the system advanced the clock an hour");
        Expect(GHourEvents == 1 && GLastHour == 12.0, "TimeOfDayHour fired as 12 began");

        const RenderCore::DirectionalLightComponent* Light = Sun.GetComponent<RenderCore::DirectionalLightComponent>();
        const TimeOfDay::TimeOfDayKey Expected =
            TimeOfDay::Evaluate(TimeOfDay::LoadPreset("TimeOfDay:/Presets/Default.todpreset"), 12.5f);
        Expect(Light && Near(Light->Intensity, Expected.SunIntensity), "the sun's intensity follows the preset");
        Expect(Light && Light->Color == Expected.SunColor, "the sun's colour follows the preset");
        const RenderCore::SkyLightComponent* Sky = Sun.GetComponent<RenderCore::SkyLightComponent>();
        Expect(Sky && Near(Sky->Intensity, Expected.SkyIntensity), "the sky follows the preset");

        const GameCore::TransformComponent* Transform = Sun.GetComponent<GameCore::TransformComponent>();
        Expect(Transform && RenderCore::LightForward(Transform->Rotation).GetY() < -0.5f, "the noon sun shines down");

        /* With the sky drawn, its colours come from the atmosphere. */
        Sun.GetComponent<TimeOfDay::TimeOfDayComponent>()->bDrawSky = 1;
        Sun.GetComponent<TimeOfDay::TimeOfDayComponent>()->Hour     = 17.4f;
        World.Tick(0.0f);
        Expect(Light && Light->Color.R > Light->Color.B + 40, "a low sun through the air is warm");
        Expect(Sky && Sky->SkyColor.B >= Sky->SkyColor.R, "the evening sky's ambient is still blue");
        Expect(Light && Near(Light->Intensity, TimeOfDay::Evaluate(TimeOfDay::LoadPreset("TimeOfDay:/Presets/Default.todpreset"), 17.4f).SunIntensity),
               "the preset still sets the intensity");
        Sun.GetComponent<TimeOfDay::TimeOfDayComponent>()->Hour = 12.5f;

        /* The editor's edited World: the sky, not the clock (nothing ticks there). */
        {
            GameCore::World Edited;
            NimbleEngine::Plugins::AttachEditorWorld(Edited);
            NimbleEngine::Plugins::AttachEditorWorld(Edited);
            Expect(RenderCore::GetCustomRenderPassCount(&Edited) == 2, "an edited World gets the sky's passes, once");
            GameCore::Entity EditedSun = Edited.CreateEntity("Sun");
            EditedSun.AddComponent<GameCore::TransformComponent>();
            TimeOfDay::TimeOfDayComponent Still;
            Still.Hour = 9.0f;
            EditedSun.AddComponent(Still);
            Edited.Tick(2.5f);
            Expect(Near(EditedSun.GetComponent<TimeOfDay::TimeOfDayComponent>()->Hour, 9.0f), "and no clock: its Hour stays");
            NimbleEngine::Plugins::DetachEditorWorld(Edited);
            Expect(RenderCore::GetCustomRenderPassCount(&Edited) == 0, "detached, the edited World's passes are gone");
        }

        NimbleEngine::Plugins::DetachWorld(World);
        Expect(RenderCore::GetCustomRenderPassCount(&World) == 0, "detached, the sky's passes are gone");
        World.Tick(2.5f);
        Expect(Clock && Near(Clock->Hour, 12.5f), "detached, the clock stops");

        NimbleEngine::Plugins::ShutdownModules();
        Expect(!TimeOfDay::IsPluginStarted(), "OnShutdown ran");
        NimbleEngine::Plugins::AttachWorld(World);
        World.Tick(2.5f);
        Expect(Clock && Near(Clock->Hour, 12.5f), "a shut-down plugin's hooks are gone");

        NimbleEngine::Plugins::UnmountContent(InAssets);
        Expect(!InAssets.Exists("TimeOfDay:/Presets/Default.todpreset"), "unmounted, the content is gone");
        NimbleEngine::Plugins::Close();
        Expect(NimbleEngine::Plugins::GetCount() == 0, "closed, the host forgets the plugins");
    }

    void TestPresets()
    {
        std::printf("presets\n");
        std::vector<TimeOfDay::TimeOfDayKey> Keys;
        std::string Why;
        Expect(TimeOfDay::ParsePreset("[Key_B]\nHour=18\nSunIntensity=1\n[Key_A]\nHour=6\nSunIntensity=3\nSunColor=10,20,30\n", Keys, &Why),
               "a preset parses: " + Why);
        Expect(Keys.size() == 2 && Keys[0].Hour == 6.0f && Keys[1].Hour == 18.0f, "keys sort by hour");
        Expect(Keys.size() == 2 && Keys[0].SunColor == EngineCore::Color(10, 20, 30), "a colour parses");

        Expect(Near(TimeOfDay::Evaluate(Keys, 12.0f).SunIntensity, 2.0f), "halfway between keys blends halfway");
        Expect(Near(TimeOfDay::Evaluate(Keys, 0.0f).SunIntensity, 2.0f), "midnight blends from the last key to the first");
        Expect(Near(TimeOfDay::Evaluate(Keys, 30.0f).SunIntensity, 3.0f), "hours wrap at 24");

        Expect(!TimeOfDay::ParsePreset("[Key_A]\nSunIntensity=1\n", Keys, &Why), "a key without an Hour is refused");
        Expect(!TimeOfDay::ParsePreset("[Key_A]\nHour=6\nSunColor=1,2\n", Keys, &Why), "a short colour is refused");
        Expect(!TimeOfDay::ParsePreset("[Key_A]\nHour=6\nSunny=1\n", Keys, &Why), "an unknown key is refused");
        Expect(!TimeOfDay::ParsePreset("; nothing\n", Keys, &Why), "a preset without keys is refused");

        /* A file that is not there falls back to the built-in day. */
        Expect(!TimeOfDay::LoadPreset("TimeOfDay:/Presets/Missing.todpreset").empty(), "a missing preset still yields keys");
    }

    void TestSunPath()
    {
        std::printf("sun path\n");
        const float Noon = TimeOfDay::SunRotation(12.0f, 0.0f, 35.0f).GetX();
        Expect(Near(Noon, 55.0f), "the noon sun stands 90 - latitude up");
        Expect(TimeOfDay::SunRotation(6.5f, 0.0f, 35.0f).GetX() < Noon, "the morning sun is lower");
        Expect(TimeOfDay::SunRotation(6.0f, 0.0f, 35.0f).GetX() >= 4.0f, "the sun never grazes the horizon");
        Expect(Near(TimeOfDay::SunRotation(12.0f, 0.0f, 35.0f).GetY(), 0.0f), "noon faces north yaw");
        Expect(Near(TimeOfDay::SunRotation(12.0f, 30.0f, 35.0f).GetY(), 30.0f), "north yaw turns the arc");
        Expect(TimeOfDay::SunRotation(8.0f, 0.0f, 35.0f).GetY() < TimeOfDay::SunRotation(16.0f, 0.0f, 35.0f).GetY(),
               "the sun sweeps across the day");
        Expect(Near(TimeOfDay::SunRotation(0.0f, 0.0f, 35.0f).GetX(), Noon), "the moon tops its arc at midnight");
        Expect(TimeOfDay::IsDaytime(12.0f) && !TimeOfDay::IsDaytime(20.0f) && !TimeOfDay::IsDaytime(-1.0f), "day is 6 to 18");

        Expect(TimeOfDay::SunDirection(12.0f, 0.0f, 35.0f).GetY() > 0.8f, "the true sun is high at noon");
        Expect(TimeOfDay::SunDirection(0.0f, 0.0f, 35.0f).GetY() < -0.8f, "and under the ground at midnight");
        Expect(std::fabs(TimeOfDay::SunDirection(6.0f, 0.0f, 35.0f).GetY()) < 0.01f, "it rises at 6");
        const EngineCore::Vector3 Moon = TimeOfDay::SunRotation(0.0f, 0.0f, 35.0f);
        const EngineCore::Vector3 MoonTravel = RenderCore::LightForward(Moon);
        const EngineCore::Vector3 SunAtMidnight = TimeOfDay::SunDirection(0.0f, 0.0f, 35.0f);
        Expect(Near(float(MoonTravel.GetY()), float(SunAtMidnight.GetY()), 0.02f), "the moonlight comes from opposite the sun");
    }

    void TestAtmosphere()
    {
        std::printf("atmosphere\n");
        const EngineCore::Vector3 Up(0.0f, 1.0f, 0.0f);
        const EngineCore::Vector3 Noon = TimeOfDay::SunDirection(12.0f, 0.0f, 35.0f);
        const TimeOfDay::SkyRGB Zenith = TimeOfDay::EvaluateSky(Up, Noon, 1.0f);
        Expect(Zenith.B > Zenith.G && Zenith.G > Zenith.R && Zenith.R > 0.0f, "the noon zenith is blue");

        const TimeOfDay::SkyRGB HighSun = TimeOfDay::SunTransmittance(Noon, 1.0f);
        const TimeOfDay::SkyRGB LowSun  = TimeOfDay::SunTransmittance(TimeOfDay::SunDirection(6.3f, 0.0f, 35.0f), 1.0f);
        Expect(HighSun.R > 0.7f && HighSun.B > 0.5f, "the noon sun comes through nearly white");
        Expect(LowSun.R > LowSun.B * 3.0f, "a rising sun comes through red");
        const TimeOfDay::SkyRGB Set = TimeOfDay::SunTransmittance(TimeOfDay::SunDirection(0.0f, 0.0f, 35.0f), 1.0f);
        Expect(Set.R == 0.0f && Set.G == 0.0f && Set.B == 0.0f, "a set sun comes through not at all");

        const TimeOfDay::SkyRGB Hazy = TimeOfDay::EvaluateSky(Up, Noon, 6.0f);
        Expect(Hazy.R / Hazy.B > Zenith.R / Zenith.B, "haze whitens the sky");
        const TimeOfDay::SkyRGB Night = TimeOfDay::EvaluateSky(Up, TimeOfDay::SunDirection(0.0f, 0.0f, 35.0f), 1.0f);
        Expect(Night.B < Zenith.B * 0.001f, "the sun lights nothing at midnight");
    }
}

int main()
{
    /* The asset seam a game installs: the working directory, plus layers. */
    EngineCore::FilesystemProvider Disk;
    EngineCore::LayeredFileProvider Assets(&Disk, false);
    EngineCore::SetFileProvider(&Assets);

    TestPrefixes();
    TestHost(Assets);

    /* Again for the plugin's own tests, which read its preset. */
    NimbleEngine::Plugins::Discover(std::string(), false);
    NimbleEngine::Plugins::MountContent(Assets);
    TestPresets();
    TestSunPath();
    TestAtmosphere();
    NimbleEngine::Plugins::UnmountContent(Assets);
    NimbleEngine::Plugins::Close();

    EngineCore::SetFileProvider(nullptr);
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
