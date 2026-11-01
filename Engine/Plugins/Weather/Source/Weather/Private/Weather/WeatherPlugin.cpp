#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"

#include "Public/Weather/Weather.h"

/* The plugin's entry: the weather's system on every level's World. Its
 * component and script library registered themselves as the library loaded. */

namespace
{
    bool GStarted = false;

    /* Its address is this plugin's handle on each World's systems. */
    char GSystemOwner = 0;

    void TickWeather(GameCore::World& InWorld, const float InDeltaSeconds, void*)
    {
        Weather::Tick(InWorld, InDeltaSeconds);
    }

    /* After the gameplay (and a sequence) that may set it, before the frame is drawn. */
    void AttachWorld(GameCore::World& InWorld, void*)
    {
        InWorld.RegisterSystem("Weather", GameCore::TP_PostUpdate, &TickWeather, &GSystemOwner);
        Weather::AttachWorld(InWorld);
    }

    void DetachWorld(GameCore::World& InWorld, void*)
    {
        Weather::DetachWorld(InWorld);
        InWorld.UnregisterAllSystems(&GSystemOwner);
    }

    class WeatherPlugin : public NimbleEngine::IPlugin
    {
    public:
        bool OnStartup(NimbleEngine::PluginContext& InContext) override
        {
            NimbleEngine::PluginWorldHook Hook;
            Hook.Name   = "Weather";
            Hook.Attach = &AttachWorld;
            Hook.Detach = &DetachWorld;
            InContext.AddWorldHook(Hook);
            GStarted = true;
            return true;
        }

        void OnShutdown(NimbleEngine::PluginContext&) override
        {
            GStarted = false;
        }
    };
}

namespace Weather
{
    bool IsPluginStarted()
    {
        return GStarted;
    }
}

NIMBLE_IMPLEMENT_PLUGIN(WeatherPlugin)
