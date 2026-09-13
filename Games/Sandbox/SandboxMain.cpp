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
 * Sandbox: the example game. CMakeLists.txt says why it exists; this is what it
 * currently demonstrates, which is the whole of what the engine offers a game
 * so far -- the IGame seam. Application owns the window, the frame loop and the
 * thread split; this file owns game state and is ticked once per frame.
 *
 * No drawing: IGame is simulation-only until render proxies land. When drawing
 * arrives it arrives as another IGame method and this file grows one.
 *
 * NO LOG HANDLER IS INSTALLED, deliberately. Without one, CoreLog's Fatal path
 * writes to stderr and abort()s; installing one turns every CORE_CHECK and
 * GDI_CHECK into print-and-continue, which is the harder failure to debug while
 * the engine is this young. A shipping game will want a handler.
 */

/* A GAME-OWNED config var, reaching the same registry and the same .ini as the
 * engine's own -- which is the point. Distinct from [Application] MaxFrames:
 * that counts iterations for engine bring-up, this is game logic that ends the
 * run by asking the engine to stop. */
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

        /* Recorded so OnTick can assert the engine's game-thread-only promise
         * from the outside, rather than assume it. */
        GameThread = EngineCore::appThreadID();

        RunSeconds = double(GSandboxRunSeconds.GetInt());

        const uint32_t Limit = InApp.GetFrameRateLimit();

        CORE_LOG("[Sandbox] game starting on thread " + Num(uint64_t(GameThread))
                 + "; frame limit "
                 + (Limit > 0 ? Num(Limit) + " fps" : std::string("unlimited"))
                 + (RunSeconds > 0.0
                    ? "; will quit after " + Fixed2(RunSeconds) + "s"
                    : "; running until the window closes"))
        return true;
    }

    void OnTick(const NimbleEngine::FrameContext& InFrame) override
    {
        /* Runs CONCURRENTLY with the render thread draining the previous
         * frame. Nothing here can touch the renderer or the window, which is
         * the point of the context being a value. */
        CORE_CHECK(EngineCore::appThreadID() == GameThread,
                   "[Sandbox] OnTick ran on a thread other than the one OnStartup saw")

        ElapsedSeconds += InFrame.DeltaSeconds;
        TicksThisSecond++;

        /* A moving object with no renderer to show it; the log line below is
         * the only way to observe it until drawing exists. */
        const double SpeedPixelsPerSecond = 120.0;
        const int    ViewportWidth        = InFrame.ViewportPixels.Width > 0
                                          ? InFrame.ViewportPixels.Width
                                          : 1;

        PositionX += SpeedPixelsPerSecond * InFrame.DeltaSeconds;
        while(PositionX >= double(ViewportWidth))
        {
            PositionX -= double(ViewportWidth);
        }

        /* The game deciding it is done. The engine finishes the frame in
         * flight and leaves the loop at the top. */
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
    /* Stashed in OnStartup so OnTick can call RequestQuit(). */
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
     * AFTER both are destroyed, so their members are not counted, and explicit
     * rather than a static destructor -- CoreGarbageCollector.h explains why.
     * This is also what proves the drain loop's `delete Command`, and the
     * virtual destructor under it, actually work.
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
