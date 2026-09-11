#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"

#include <cstdint>
#include <cstdio>

/*
 * Sandbox: the example game, and the first executable in this repository.
 *
 * It exists to be the place engine features are USED -- the working reference
 * for how a game drives NimbleEngine, and the thing that exercises each new
 * engine implementation against something that actually runs. A feature with no
 * caller here is a feature nothing has tried.
 *
 * IT IS NEARLY EMPTY TODAY BECAUSE THE ENGINE GIVES IT NOTHING TO DO. The whole
 * public surface of NimbleEngine::Application is Exec(); the per-frame hooks
 * (Simulate, BuildRenderCommands) are private and non-virtual, and GameWindow
 * and GDI are linked PRIVATE so a game cannot reach a window or a draw call
 * either. So this file currently demonstrates exactly one thing: that the
 * engine starts, runs its game/render thread split, and shuts down clean.
 *
 * Growing it means giving Application a seam a game can attach to first. Until
 * that exists, resist putting game behaviour here by reaching around the engine
 * -- an example that bypasses the API it is meant to demonstrate is worse than
 * a short one.
 *
 * NO LOG HANDLER IS INSTALLED, and that is a decision rather than an omission.
 * With none installed, CoreLog's Fatal path writes to stderr and abort()s.
 * Installing one converts every CORE_CHECK and GDI_CHECK into
 * print-and-continue, which while first bringing threads up is the strictly
 * harder failure to debug: the process keeps running past the violation and
 * crashes somewhere unrelated. A game will want a handler. This does not.
 */
int main(int /*argc*/, char** /*argv*/)
{
    {
        NimbleEngine::Application App;
        App.Exec();
    }

    /*
     * AFTER the Application is destroyed, so its own members are not counted,
     * and explicit rather than a static destructor for the reason
     * CoreGarbageCollector.h gives: a report at static-destruction time runs in
     * an order nothing specifies and blames teardown ordering for leaks.
     *
     * This is also what proves the drain loop's `delete Command` -- and the
     * virtual destructor it relies on -- actually work. A command leaked per
     * frame shows up here and nowhere else.
     */
    const uint64_t Leaked = EngineCore::Memory::ReportLeaks();

    if(Leaked != 0)
    {
        std::fprintf(stderr, "Sandbox: %llu allocation(s) leaked\n",
                     static_cast<unsigned long long>(Leaked));
        return 1;
    }

    return 0;
}
