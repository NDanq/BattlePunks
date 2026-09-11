#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreGarbageCollector.h"

#include <cstdint>
#include <cstdio>

/*
 * The Sandbox host: the first executable in this repository, and the thing that
 * proves the engine layer actually runs.
 *
 * DELIBERATELY ALMOST EMPTY. Everything it could plausibly do -- install a log
 * handler, parse arguments, own a frame counter -- belongs either to
 * NimbleEngine::Application or to a real game, and putting any of it here would
 * make this file the place engine behaviour accumulates by accident.
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
