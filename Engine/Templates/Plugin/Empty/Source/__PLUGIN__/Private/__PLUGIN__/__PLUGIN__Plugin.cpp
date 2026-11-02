#include "Public/@PLUGIN_NAME@/@PLUGIN_NAME@.h"

#include "Public/Core/CoreLog.h"
#include "Public/NimbleEngine/Plugin.h"

/*
 * @PLUGIN_NAME@'s entry point. The host opens this library once per run,
 * before the game module, and calls OnStartup; OnShutdown at exit.
 *
 * What registers itself as the library loads needs nothing here: components
 * (GAMECORE_DEFINE_COMPONENT), script libraries (GAMECORE_DEFINE_SCRIPT_LIBRARY),
 * config vars (NIMBLE_CONFIG_*). OnStartup is for what needs the host -- a
 * system on every level's World:
 *
 *     NimbleEngine::PluginWorldHook Hook;
 *     Hook.Attach = [](GameCore::World& InWorld, void*) { InWorld.RegisterSystem(...); };
 *     Hook.Detach = [](GameCore::World& InWorld, void*) { InWorld.UnregisterAllSystems(...); };
 *     InContext.AddWorldHook(Hook);
 */

namespace
{
    bool GStarted = false;

    class @PLUGIN_NAME@Plugin : public NimbleEngine::IPlugin
    {
    public:
        bool OnStartup(NimbleEngine::PluginContext& InContext) override
        {
            CORE_LOG("[" + InContext.GetPluginName() + "] started")
            GStarted = true;
            return true;   // false refuses to start
        }

        void OnShutdown(NimbleEngine::PluginContext&) override
        {
            GStarted = false;   // undo whatever OnStartup registered by hand
        }
    };
}

namespace @PLUGIN_NAME@
{
    bool IsStarted() { return GStarted; }
}

/* Exactly one per library, at file scope, outside any namespace. */
NIMBLE_IMPLEMENT_PLUGIN(@PLUGIN_NAME@Plugin)
