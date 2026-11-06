/*
 * @PLUGIN_NAME@Tests: the plugin's library on its own, without a game. Calls
 * the entry points NIMBLE_IMPLEMENT_PLUGIN exports, as the host does: the
 * ABI it reports, then OnStartup and OnShutdown. `ctest -L plugin` runs it.
 */
#include <cstdio>
#include <string>

#include "Public/NimbleEngine/Plugin.h"
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
    Expect(Plugin != nullptr, "the plugin is made");
    Expect(Plugin && Plugin->OnStartup(Context), "it starts");
    Expect(@PLUGIN_NAME@::IsStarted(), "and says so");

    if(Plugin)
    {
        Plugin->OnShutdown(Context);
        NimblePluginDestroy(Plugin);
    }
    Expect(!@PLUGIN_NAME@::IsStarted(), "it stops");

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
