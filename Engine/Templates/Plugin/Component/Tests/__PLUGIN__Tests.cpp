/*
 * @PLUGIN_NAME@Tests: the plugin's library on its own, without a game. Starts
 * it through the entry points NIMBLE_IMPLEMENT_PLUGIN exports, as the host
 * does, runs its world hook on a World, and checks the component steps.
 * `ctest -L plugin` runs it.
 */
#include <cmath>
#include <cstdio>
#include <string>

#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Public/@PLUGIN_NAME@/@PLUGIN_NAME@Component.h"

extern "C" uint32_t                 NimblePluginGetABIVersion();
extern "C" NimbleEngine::IPlugin*   NimblePluginCreate();
extern "C" void                     NimblePluginDestroy(NimbleEngine::IPlugin* InPlugin);

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

int main()
{
    Expect(NimblePluginGetABIVersion() == NimbleEngine::NIMBLE_PLUGIN_ABI_VERSION, "built for this engine's plugin ABI");

    NimbleEngine::PluginContext Context;
    NimbleEngine::IPlugin* Plugin = NimblePluginCreate();
    Expect(Plugin && Plugin->OnStartup(Context) && @PLUGIN_NAME@::IsStarted(), "it starts");

    /* Its world hook puts its system on a World, as a game's level gets it. */
    GameCore::World World;
    NimbleEngine::Plugins::AttachWorld(World);
    GameCore::Entity Thing = World.CreateEntity("Thing");
    @PLUGIN_NAME@::@PLUGIN_NAME@Component Init;
    Init.Rate = 3.0f;
    Thing.AddComponent(Init);

    World.Tick(2.0f);
    const auto* Component = Thing.GetComponent<@PLUGIN_NAME@::@PLUGIN_NAME@Component>();
    Expect(Component && std::fabs(Component->Total - 6.0f) < 0.001f, "its system stepped the component: 3/s for 2 s");

    NimbleEngine::Plugins::DetachWorld(World);
    if(Plugin)
    {
        Plugin->OnShutdown(Context);
        NimblePluginDestroy(Plugin);
    }
    NimbleEngine::Plugins::Close();   // forgets the hook
    Expect(!@PLUGIN_NAME@::IsStarted(), "it stops");

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
