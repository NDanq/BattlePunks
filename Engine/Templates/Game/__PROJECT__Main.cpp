#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"

#include <cstdint>
#include <cstdio>

/*
 * @PROJECT_NAME@'s launcher. The game is the @PROJECT_NAME@Game module, loaded (not
 * linked) by Application::ExecGame from [Game] Module, "@PROJECT_NAME@Game" if unset.
 */
int main(int /*argc*/, char** /*argv*/)
{
    {
        NimbleEngine::Application App;
        App.ExecGame("@PROJECT_NAME@Game");
    }

    /* After the Application and module are gone (CoreGarbageCollector.h). */
    const uint64_t Leaked = EngineCore::Memory::ReportLeaks();

    if(Leaked != 0)
    {
        std::fprintf(stderr, "@PROJECT_NAME@: %llu allocation(s) leaked\n",
                     static_cast<unsigned long long>(Leaked));
        return 1;
    }

    return 0;
}
