#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"

#include <cstdint>
#include <cstdio>

/*
 * Sandbox's launcher. The game is the SandboxGame module, loaded (not linked)
 * by Application::ExecGame from [Game] Module, "SandboxGame" if unset.
 *
 * No log handler, deliberately: without one a Fatal aborts instead of
 * continuing.
 */
int main(int /*argc*/, char** /*argv*/)
{
    {
        NimbleEngine::Application App;
        App.ExecGame("SandboxGame");
    }

    /* After the Application and module are gone (CoreGarbageCollector.h). */
    const uint64_t Leaked = EngineCore::Memory::ReportLeaks();

    if(Leaked != 0)
    {
        std::fprintf(stderr, "Sandbox: %llu allocation(s) leaked\n",
                     static_cast<unsigned long long>(Leaked));
        return 1;
    }

    return 0;
}
