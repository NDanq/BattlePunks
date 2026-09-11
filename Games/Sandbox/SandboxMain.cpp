#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreGarbageCollector.h"
#include "Public/Core/CoreLog.h"
#include "Public/Core/CorePlatform.h"
#include "Public/Core/CoreThread.h"

#include <cstdint>
#include <cstdio>
#include <string>

/*
 * Sandbox: the example game, and the first executable in this repository.
 *
 * It exists to be the place engine features are USED -- the working reference
 * for how a game drives NimbleEngine, and the thing that exercises each new
 * engine implementation against something that actually runs. A feature with no
 * caller here is a feature nothing has tried.
 *
 * WHAT IT DEMONSTRATES TODAY is the whole of what the engine currently offers a
 * game: the IGame seam. NimbleEngine::Application owns the window, the frame
 * loop and the game/render thread split; this file owns game state and gets
 * ticked once per frame on the game thread.
 *
 * There is no drawing here, and that is the engine's current shape rather than
 * an omission -- IGame is simulation-only until render proxies land, because
 * render commands must capture by value for the frame pacing to be sound and
 * that rule is only enforceable while the engine authors every command. When
 * drawing arrives, it arrives as another IGame method and this file grows one.
 *
 * NO LOG HANDLER IS INSTALLED, and that is a decision rather than an omission.
 * With none installed, CoreLog's Fatal path writes to stderr and abort()s.
 * Installing one converts every CORE_CHECK and GDI_CHECK into
 * print-and-continue, which while the engine is this young is the strictly
 * harder failure to debug: the process keeps running past the violation and
 * crashes somewhere unrelated. A shipping game will want a handler. This does
 * not.
 */

/*
 * A GAME-OWNED config var, declared here rather than in the engine. It lands in
 * the same registry the engine's own NIMBLE_CONFIG_* use and is read from the
 * same Config/DefaultEngine.ini, which is the point worth demonstrating: a game
 * extends the config surface without the engine knowing about it.
 *
 * Distinct from the engine's [Application] MaxFrames, and deliberately so.
 * MaxFrames is an engine bring-up knob that counts iterations; this is game
 * logic expressed in game terms, and it ends the run by asking the engine to
 * stop rather than by the engine counting on the game's behalf.
 */
NIMBLE_CONFIG_INT(GSandboxRunSeconds, "Sandbox", "RunSeconds", 0, nullptr);

namespace
{
    std::string Num(uint64_t InValue)   { return std::to_string(InValue); }

    /* Two decimals, without dragging <iomanip> in for one call site. */
    std::string Fixed2(double InValue)
    {
        char Buffer[64];
        std::snprintf(Buffer, sizeof(Buffer), "%.2f", InValue);
        return std::string(Buffer);
    }
}

class SandboxGame : public NimbleEngine::IGame
{
public:
    bool OnStartup(NimbleEngine::Application& InApp) override
    {
        App = &InApp;

        /* Recorded so the per-second line can assert it rather than assume it.
         * The engine documents OnTick as game-thread-only; this is the game
         * checking that promise from the outside, which is the sort of thing an
         * example is for. */
        GameThread = EngineCore::appThreadID();

        RunSeconds = double(GSandboxRunSeconds.GetInt());

        CORE_LOG("[Sandbox] game starting on thread " + Num(uint64_t(GameThread))
                 + (RunSeconds > 0.0
                    ? "; will quit after " + Fixed2(RunSeconds) + "s"
                    : "; running until the window closes"))
        return true;
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        /*
         * Runs CONCURRENTLY with the render thread draining the previous frame.
         * Nothing here may touch the renderer or the window -- and nothing here
         * can, which is the point of the context being a value.
         */
        CORE_CHECK(EngineCore::appThreadID() == GameThread,
                   "[Sandbox] OnTick ran on a thread other than the one OnStartup saw")

        ElapsedSeconds += InFrame.DeltaSeconds;
        TicksThisSecond++;

        /* A moving object with no renderer to show it: the state is real, and
         * the log line below is the only way to observe it until drawing
         * exists. Wrapped so it stays inside the viewport rather than growing
         * without bound. */
        const double SpeedPixelsPerSecond = 120.0;
        const int    ViewportWidth        = InFrame.ViewportPixels.Width > 0
                                          ? InFrame.ViewportPixels.Width
                                          : 1;

        PositionX += SpeedPixelsPerSecond * InFrame.DeltaSeconds;
        while(PositionX >= double(ViewportWidth))
        {
            PositionX -= double(ViewportWidth);
        }

        /*
         * The game deciding it is done, which is what RequestQuit is for. The
         * engine finishes the frame in flight and leaves the loop at the top.
         */
        if(RunSeconds > 0.0 && ElapsedSeconds >= RunSeconds && App)
        {
            CORE_LOG("[Sandbox] RunSeconds reached; asking the engine to quit")
            App->RequestQuit();
        }

        if(ElapsedSeconds - LastReportSeconds >= 1.0)
        {
            CORE_LOG("[Sandbox] t=" + Fixed2(ElapsedSeconds) + "s"
                     + " frame=" + Num(InFrame.FrameNumber)
                     + " fps=" + Num(TicksThisSecond)
                     + " dt=" + Fixed2(InFrame.DeltaSeconds * 1000.0) + "ms"
                     + " viewport=" + Num(uint64_t(InFrame.ViewportPixels.Width))
                     + "x" + Num(uint64_t(InFrame.ViewportPixels.Height))
                     + " x=" + Fixed2(PositionX))

            LastReportSeconds = ElapsedSeconds;
            TicksThisSecond   = 0;
        }
    }

    void OnShutdown() override
    {
        CORE_LOG("[Sandbox] game stopping after " + Fixed2(ElapsedSeconds) + "s")
        App = nullptr;
    }

private:
    /* Stashed in OnStartup so OnTick can call RequestQuit(). Valid for the
     * duration of Exec and cleared in OnShutdown. */
    NimbleEngine::Application* App = nullptr;

    EngineCore::ThreadID GameThread = 0;

    double   RunSeconds        = 0.0;
    double   ElapsedSeconds    = 0.0;
    double   LastReportSeconds = 0.0;
    double   PositionX         = 0.0;
    uint64_t TicksThisSecond   = 0;
};

int main(int /*argc*/, char** /*argv*/)
{
    {
        SandboxGame Game;
        NimbleEngine::Application App;
        App.Exec(Game);
    }

    /*
     * AFTER the Application and the game are destroyed, so their own members are
     * not counted, and explicit rather than a static destructor for the reason
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
