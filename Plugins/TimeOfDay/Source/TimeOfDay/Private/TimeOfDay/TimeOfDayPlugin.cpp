#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"

#include "Public/TimeOfDay/TimeOfDay.h"

/* The plugin's entry: the clocks' system on every level's World. Its
 * component and script library registered themselves as the library loaded. */

namespace
{
    bool GStarted = false;

    /* Its address is this plugin's handle on each World's systems. */
    char GSystemOwner = 0;

    void TickClocks(GameCore::World& InWorld, const float InDeltaSeconds, void*)
    {
        TimeOfDay::Tick(InWorld, InDeltaSeconds);
    }

    /* After the gameplay that may set the clock, before the frame is drawn. */
    void AttachWorld(GameCore::World& InWorld, void*)
    {
        InWorld.RegisterSystem("TimeOfDay", GameCore::TP_PostUpdate, &TickClocks, &GSystemOwner);
    }

    void DetachWorld(GameCore::World& InWorld, void*)
    {
        InWorld.UnregisterAllSystems(&GSystemOwner);
    }

    class TimeOfDayPlugin : public NimbleEngine::IPlugin
    {
    public:
        bool OnStartup(NimbleEngine::PluginContext& InContext) override
        {
            NimbleEngine::PluginWorldHook Hook;
            Hook.Name   = "TimeOfDay";
            Hook.Attach = &AttachWorld;
            Hook.Detach = &DetachWorld;
            InContext.AddWorldHook(Hook);
            GStarted = true;
            return true;
        }

        void OnShutdown(NimbleEngine::PluginContext&) override
        {
            TimeOfDay::ClearPresetCache();
            GStarted = false;
        }
    };
}

namespace TimeOfDay
{
    bool IsPluginStarted()
    {
        return GStarted;
    }
}

NIMBLE_IMPLEMENT_PLUGIN(TimeOfDayPlugin)
