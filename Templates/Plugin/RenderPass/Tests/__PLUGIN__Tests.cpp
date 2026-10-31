/*
 * @PLUGIN_NAME@Tests: the plugin's library on its own, without a game. Starts
 * it through the entry points NIMBLE_IMPLEMENT_PLUGIN exports, as the host
 * does, and checks its world hook puts its pass on a World and takes it off.
 * Drawing needs a renderer, so the picture itself is checked by running the
 * game. `ctest -L plugin` runs it.
 */
#include <cstdio>
#include <string>

#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Public/RenderCore/CustomRenderPass.h"
#include "Public/@PLUGIN_NAME@/@PLUGIN_NAME@.h"

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

    GameCore::World World;
    NimbleEngine::Plugins::AttachWorld(World);
    Expect(RenderCore::GetCustomRenderPassCount(&World) == 1, "its world hook registers the pass");
    NimbleEngine::Plugins::DetachWorld(World);
    Expect(RenderCore::GetCustomRenderPassCount(&World) == 0, "and takes it off again");

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
