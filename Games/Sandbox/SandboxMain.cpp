#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"

#include <cstdint>
#include <cstdio>

/*
 * Sandbox's launcher. The game itself -- levels, mode, state, systems -- is in
 * the SandboxGame module (SandboxGame.cpp), which this does not link: it runs
 * the engine and lets Application::ExecGame load the module named by
 * [Game] Module in Config/DefaultEngine.ini, "SandboxGame" if unset.
 *
 * No log handler, deliberately: without one a Fatal aborts, while a handler
 * turns every CORE_CHECK into print-and-continue -- harder to debug this early.
 */
int main(int /*argc*/, char** /*argv*/)
{
    {
        NimbleEngine::Application App;
        App.ExecGame("SandboxGame");
    }

    /* After the Application is destroyed and the module unloaded, so neither's
     * members are counted; explicit rather than a static destructor
     * (CoreGarbageCollector.h). */
    const uint64_t Leaked = EngineCore::Memory::ReportLeaks();

    if(Leaked != 0)
    {
        std::fprintf(stderr, "Sandbox: %llu allocation(s) leaked\n",
                     static_cast<unsigned long long>(Leaked));
        return 1;
    }

    return 0;
}
