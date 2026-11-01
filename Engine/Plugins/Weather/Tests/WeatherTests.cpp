/*
 * WeatherTests -- the Weather plugin through the plugin host, as a game sees
 * it: its config and content staged beside this executable, its library
 * started, its system on a World through the host's world hooks. Then what a
 * Weather component asks of the renderer (RenderCore's RenderWeather.h): the
 * wind, the wetness stepping with the rain, the mist as Volumetric Fog, and
 * the rain's particle system round the eye.
 *
 * Runs from the runtime directory (nimble_add_plugin_test), where the build
 * left Plugins/Plugins.manifest and Plugins/Weather/.
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/SkyLightComponent.h"
#include "Public/RenderCore/CustomRenderPass.h"
#include "Public/RenderCore/ParticleSystemAsset.h"
#include "Public/RenderCore/PostProcessStack.h"
#include "Public/RenderCore/RenderWeather.h"
#include "Public/Weather/Weather.h"
#include "Public/Weather/WeatherComponent.h"

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
    bool Near(const double InA, const double InB, const double InTolerance = 0.001)
    {
        return std::fabs(InA - InB) <= InTolerance;
    }

    float ConfigFloat(const char* InPath)
    {
        const EngineCore::ConfigVar* Var = EngineCore::ConfigFindVarByPath(InPath);
        return Var ? Var->GetFloat() : -100.0f;
    }

    /* The rain's entity in InWorld; null before the rain first fell. */
    RenderCore::ParticleSystemComponent* RainEmitter(GameCore::World& InWorld)
    {
        RenderCore::ParticleSystemComponent* Found = nullptr;
        InWorld.Each<RenderCore::ParticleSystemComponent>([&Found](GameCore::Entity, RenderCore::ParticleSystemComponent& InEmitter)
        {
            if(!Found && std::strcmp(InEmitter.System, Weather::RAIN_SYSTEM_PATH) == 0) { Found = &InEmitter; }
        });
        return Found;
    }

    void TestMath()
    {
        std::printf("math\n");
        const EngineCore::Vector3 North = Weather::WindDirection(0.0f);
        const EngineCore::Vector3 East  = Weather::WindDirection(90.0f);
        Expect(Near(North.GetZ(), 1.0) && Near(North.GetX(), 0.0), "yaw 0 blows toward +Z");
        Expect(Near(East.GetX(), 1.0) && Near(East.GetZ(), 0.0), "yaw 90 blows toward +X");
        const Weather::WeatherComponent Default;
        const EngineCore::Vector3 Breeze = Weather::WindDirection(Default.WindYaw);
        const RenderCore::RenderWeather Calm;
        Expect(Near(Breeze.GetX(), Calm.WindDirection[0], 0.01) && Near(Breeze.GetZ(), Calm.WindDirection[2], 0.01),
               "a default Weather blows the way the renderer's default wind does");

        Expect(Near(Weather::StepWetness(0.0f, 1.0f, 30.0f, 60.0f, 240.0f), 0.5), "full rain soaks half way in half the wet time");
        Expect(Near(Weather::StepWetness(0.0f, 0.5f, 30.0f, 60.0f, 240.0f), 0.25), "half the rain, half as fast");
        Expect(Near(Weather::StepWetness(0.9f, 1.0f, 60.0f, 60.0f, 240.0f), 1.0), "no wetter than soaked");
        Expect(Near(Weather::StepWetness(1.0f, 0.0f, 120.0f, 60.0f, 240.0f), 0.5), "no rain dries it");
        Expect(Near(Weather::StepWetness(0.1f, 0.0f, 120.0f, 60.0f, 240.0f), 0.0), "no drier than dry");

        Weather::WeatherComponent Misty;
        Misty.MistDensity = 0.5f;
        Misty.MistHeight  = 120.0f;
        Misty.MistFalloff = 500.0f;
        Misty.Rain        = 1.0f;
        Misty.RainMist    = 0.25f;
        Misty.Wetness     = 0.7f;
        Misty.WindYaw     = 90.0f;
        Misty.WindStrength = 2.0f;
        const RenderCore::RenderWeather Out = Weather::ToRenderWeather(Misty);
        Expect(Out.bMist == 1, "mist turns the fog on");
        Expect(Near(Out.MistDensity, 0.75 * Weather::WEATHER_MIST_DENSITY_SCALE, 1e-7), "the rain thickens the mist");
        Expect(Near(Out.MistBaseHeight, 120.0) && Near(Out.MistHeightFalloff, 1.0 / 500.0, 1e-7), "its height and falloff");
        Expect(Near(Out.Wetness, 0.7) && Near(Out.Rain, 1.0) && Near(Out.WindStrength, 2.0), "wetness, rain and wind strength");
        Expect(Near(Out.WindDirection[0], 1.0), "the wind's direction");
        Expect(Weather::ToRenderWeather(Weather::WeatherComponent()).bMist == 0, "no mist and no rain leave the fog alone");

        RenderCore::PostProcessSettings Settings;
        RenderCore::ApplyRenderWeatherFog(Out, Settings);
        Expect(Settings.bVolumetricFog == 1 && Near(Settings.FogDensity, Out.MistDensity, 1e-7)
               && Near(Settings.FogBaseHeight, 120.0), "the mist drives Volumetric Fog");

        RenderCore::ParticleSystemDesc Base;
        Base.Emitters.resize(2);
        Base.Emitters[0].Velocity = EngineCore::Vector3(0.0, -900.0, 0.0);
        const std::shared_ptr<RenderCore::ParticleSystemDesc> Blown = Weather::BlowRain(Base, EngineCore::Vector3(250.0, 0.0, 0.0));
        Expect(Blown && Near(Blown->Emitters[0].Velocity.GetX(), 250.0) && Near(Blown->Emitters[0].Velocity.GetY(), -900.0),
               "the wind carries the drops along");
        Expect(Blown && Blown->Emitters[1].HasModule(RenderCore::PEM_Velocity), "every emitter moves");

        const EngineCore::Vector3 Origin = Weather::RainOrigin(EngineCore::Vector3(100.0, 50.0, 0.0), EngineCore::Vector3(250.0, 0.0, 0.0));
        Expect(Near(Origin.GetY(), 50.0 + Weather::RAIN_HEIGHT), "the rain is born above the eye");
        Expect(Origin.GetX() < 100.0, "and upwind of it");
    }

    void TestRenderWeather()
    {
        std::printf("render weather\n");
        int Key = 0;
        bool bSet = true;
        RenderCore::GetRenderWeather(&Key, &bSet);
        Expect(!bSet, "a World has no weather until one is set");
        RenderCore::RenderWeather In;
        In.WindDirection[0] = 3.0f; In.WindDirection[1] = 0.0f; In.WindDirection[2] = 4.0f;
        In.Wetness = 2.0f;
        RenderCore::SetRenderWeather(&Key, In);
        const RenderCore::RenderWeather Out = RenderCore::GetRenderWeather(&Key, &bSet);
        Expect(bSet && Near(Out.WindDirection[0], 0.6) && Near(Out.WindDirection[2], 0.8), "the wind's direction is made unit");
        Expect(Near(Out.Wetness, 1.0), "wetness is 0-1");
        RenderCore::ClearRenderWeather(&Key);
        RenderCore::GetRenderWeather(&Key, &bSet);
        Expect(!bSet, "cleared");
    }

    /* The host end to end, on what the build staged beside this executable. */
    void TestHost(EngineCore::LayeredFileProvider& InAssets)
    {
        std::printf("host\n");
        Expect(!Weather::IsPluginStarted(), "not started before LoadModules");
        Expect(NimbleEngine::Plugins::Discover(std::string(), false) >= 1, "the manifest lists a plugin");
        bool bListed = false;
        for(size_t Index = 0; Index < NimbleEngine::Plugins::GetCount(); ++Index)
        {
            bListed = bListed || NimbleEngine::Plugins::GetNameAt(Index) == "Weather";
        }
        Expect(bListed, "the manifest lists Weather");
        Expect(Near(ConfigFloat("Weather.RainDensity"), 1.0) && Near(ConfigFloat("Weather.Rain"), -1.0), "the plugin's config applied");

        NimbleEngine::Plugins::MountContent(InAssets);
        Expect(InAssets.Exists("Weather:/Particles/Rain.nmparticle"), "the rain reads by its prefix");
        const std::shared_ptr<const RenderCore::ParticleSystemDesc> Rain = RenderCore::ParticleSystemCache::Find(Weather::RAIN_SYSTEM_PATH);
        Expect(Rain && Rain->Emitters.size() == 3, "the rain's system parses, three emitters");
        Expect(Rain && Rain->Emitters[0].Billboard == RenderCore::PB_VelocityAligned && Rain->Emitters[0].Velocity.GetY() < -500.0,
               "its drops fall as streaks");

        Expect(NimbleEngine::Plugins::LoadModules(), "the plugin's library loads");
        Expect(Weather::IsPluginStarted(), "its IPlugin started");

        GameCore::World World;
        NimbleEngine::Plugins::AttachWorld(World);
        const size_t PassesBefore = RenderCore::GetCustomRenderPassCount(&World);

        /* No Weather: no weather, no rain. */
        World.Tick(0.1f);
        bool bSet = true;
        RenderCore::GetRenderWeather(&World, &bSet);
        Expect(!bSet, "a World without a Weather has none");
        Expect(RainEmitter(World) == nullptr, "and no rain");
        Expect(RenderCore::GetCustomRenderPassCount(&World) == PassesBefore, "and no pass of the plugin's");

        GameCore::Entity Eye = World.CreateEntity("Eye");
        GameCore::TransformComponent EyeAt;
        EyeAt.Position = EngineCore::Vector3(1000.0, 200.0, -500.0);
        Eye.AddComponent(EyeAt);
        Eye.AddComponent<RenderCore::CameraComponent>();

        GameCore::Entity Sky = World.CreateEntity("Sky");
        RenderCore::SkyLightComponent SkyLight;
        SkyLight.SkyColor  = EngineCore::Color(200, 120, 80);
        SkyLight.Intensity = 1.0f;
        Sky.AddComponent(SkyLight);

        GameCore::Entity Level = World.CreateEntity("Weather");
        Weather::WeatherComponent Init;
        Init.Rain         = 1.0f;
        Init.MistDensity  = 0.4f;
        Init.WetSeconds   = 10.0f;
        Init.WindYaw      = 90.0f;
        Init.WindStrength = 1.0f;
        Level.AddComponent(Init);

        World.Tick(5.0f);
        Expect(RenderCore::GetCustomRenderPassCount(&World) == PassesBefore + 1, "a Weather brings the eye's pass");
        const Weather::WeatherComponent* State = Level.GetComponent<Weather::WeatherComponent>();
        Expect(State && Near(State->Wetness, 0.5), "the rain wets it");
        const RenderCore::RenderWeather Given = RenderCore::GetRenderWeather(&World, &bSet);
        Expect(bSet && Near(Given.Wetness, 0.5) && Given.bMist == 1 && Near(Given.WindDirection[0], 1.0), "handed to the renderer");

        RenderCore::ParticleSystemComponent* Emitter = RainEmitter(World);
        Expect(Emitter != nullptr && Emitter->bEmitting == 1 && Near(Emitter->SpawnRateScale, 1.0), "the rain falls");
        Expect(Emitter && Emitter->Tint.R > Emitter->Tint.B, "lit by the sky's colour");
        if(Emitter)
        {
            const EngineCore::Vector3 At = Emitter->Owner.GetEntity().GetWorldPose().Position;
            Expect(Near(At.GetY(), 200.0 + Weather::RAIN_HEIGHT, 1.0) && At.GetX() < 1000.0 && Near(At.GetZ(), -500.0, 1.0),
                   "above the camera, upwind");
        }
        const std::shared_ptr<const RenderCore::ParticleSystemDesc> Blown = RenderCore::ParticleSystemCache::Find(Weather::RAIN_SYSTEM_PATH);
        Expect(Blown && Near(Blown->Emitters[0].Velocity.GetX(), Weather::RAIN_WIND_SPEED, 41.0), "the stored system is blown by the wind");

        /* The [Weather] Rain pin. */
        EngineCore::LoadConfigText("[Weather]\nRain=0.25\n");
        World.Tick(0.0f);
        Expect(State && Near(State->Rain, 0.25) && Emitter && Near(Emitter->SpawnRateScale, 0.25), "Weather.Rain pins the rain");
        EngineCore::LoadConfigText("[Weather]\nRain=-1\n");

        Level.GetComponent<Weather::WeatherComponent>()->Rain = 0.0f;
        World.Tick(60.0f);
        Expect(Emitter && Emitter->bEmitting == 0, "the rain stops");
        Expect(State && Near(State->Wetness, 0.25), "and it dries");

        NimbleEngine::Plugins::DetachWorld(World);
        RenderCore::GetRenderWeather(&World, &bSet);
        Expect(!bSet, "detached, the World's weather is cleared");
        Expect(RenderCore::GetCustomRenderPassCount(&World) == 0, "and every plugin's pass gone");
        Expect(RainEmitter(World) == nullptr, "and its rain");

        NimbleEngine::Plugins::ShutdownModules();
        Expect(!Weather::IsPluginStarted(), "OnShutdown ran");
        NimbleEngine::Plugins::UnmountContent(InAssets);
        NimbleEngine::Plugins::Close();
    }
}

int main()
{
    /* The asset seam a game installs: the working directory, plus layers. */
    EngineCore::FilesystemProvider Disk;
    EngineCore::LayeredFileProvider Assets(&Disk, false);
    EngineCore::SetFileProvider(&Assets);

    TestMath();
    TestRenderWeather();
    TestHost(Assets);

    EngineCore::SetFileProvider(nullptr);
    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
