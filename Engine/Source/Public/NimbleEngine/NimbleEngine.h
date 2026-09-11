#ifndef NIMBLEENGINE_NIMBLEENGINE_H
#define NIMBLEENGINE_NIMBLEENGINE_H

#include <atomic>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreTypes.h"
#include "Public/Core/CoreThread.h"

/*
 * Forward-declared, NOT included. Engine/CMakeLists.txt:149-151 links GameWindow
 * and GDI PRIVATE precisely so this header cannot leak either one to a game, and
 * the comment there says that an #include appearing here is the signal that
 * something grew a dependency it should not have. Both are only ever held as
 * pointers, so a declaration is all this needs.
 */
class GameWindow;
class GDIContext;

namespace NimbleEngine
{
    class Application;

    /*
     * What a game is told about the frame it is ticking.
     *
     * A VALUE, assembled by the engine on the game thread and handed down. It
     * carries no pointers into engine state on purpose: everything here has
     * already been read on the thread that owns it, so a game cannot reach
     * something it is not allowed to touch by following a member.
     *
     * ViewportPixels in particular is the reason this exists rather than the
     * game being handed the window. GameWindow's accessors are cached reads
     * that deliberately do NOT assert the pump thread, so a cross-thread call
     * would be an unsynchronised read nothing catches. The engine reads it once,
     * on the right thread, and the game gets the number.
     *
     * Not NE_API: a header-only aggregate with no out-of-line members
     * instantiates in the consumer, and dllimport on it buys C4251 noise at
     * best. NimbleEngineAPI.h states that rule.
     */
    struct FrameContext
    {
        /* Wall time since the previous tick. Zero on the first frame -- there is
         * no previous one, and inventing a value would be a lie a game would
         * integrate. */
        double           DeltaSeconds = 0.0;

        /* Ticks completed before this one, so the first tick sees 0. */
        uint64_t         FrameNumber  = 0;

        /* PHYSICAL pixels. Size a viewport from this, not from logical units. */
        EngineCore::Size ViewportPixels;
    };

    /*
     * What a game implements, and the whole of what the engine calls.
     *
     * =============================== THREADING ===============================
     *
     * EVERY METHOD HERE RUNS ON THE GAME THREAD, which is also the main thread
     * and the SDL pump thread. OnTick in particular runs CONCURRENTLY with the
     * render thread executing the PREVIOUS frame's commands -- that overlap is
     * the point of the split -- so a game must not touch renderer state, and
     * there is deliberately no way from here to do so.
     * ========================================================================
     *
     * SIMULATION ONLY, FOR NOW. There is no OnRender and no drawing call,
     * because commands must capture strictly by value for the frame pacing to be
     * sound, and that rule is only enforceable while the engine authors every
     * command. Handing a game the enqueue macros would make a captured pointer a
     * live data race against the next frame's simulation with nothing to catch
     * it. Drawing arrives with render proxies, as one piece of work.
     *
     * Not NE_API: a pure interface with no out-of-line members has nothing to
     * export. The engine calls through the vtable the game's own compiler built.
     */
    class IGame
    {
    public:
        virtual ~IGame() = default;

        /*
         * After the engine is up -- window created, render thread running and
         * past its readiness check -- and before the first tick. Returning false
         * aborts startup and the engine shuts down cleanly.
         *
         * InApp outlives the game for the duration of Exec, so stashing it to
         * call RequestQuit() later is the intended use.
         */
        virtual bool OnStartup(Application& InApp) { (void)InApp; return true; }

        /* Once per frame. The only method a game must implement. */
        virtual void OnTick(const FrameContext& InFrame) = 0;

        /* After the loop ends, before the window and render thread go away. Runs
         * whether the loop ended by quit request, window close, or a failed
         * OnStartup -- so it pairs with construction, not with OnStartup. */
        virtual void OnShutdown() {}
    };

    /*
     * The application: the game thread, and the render thread it owns.
     *
     * =========================== THREAD TOPOLOGY ============================
     *
     * MAIN THREAD == game thread == SDL pump thread. GameWindowSystem latches
     * its pump thread at Initialize() and CORE_CHECKs every entry point against
     * it, and on macOS that thread must be the process main thread -- so there
     * is no freedom here to give the game its own thread.
     *
     * RENDER THREAD owns GDI. It is the thread that calls AllocateResourcesGDI(),
     * which is what makes GDIInRenderThread() true for it, and it is the sole
     * consumer of GRenderCommands.
     *
     * The split is genuine: simulation of frame N overlaps the render thread
     * draining frame N-1. Command SUBMISSION is fenced behind FrameDone, so no
     * snapshotting or render proxies are needed yet -- see Exec().
     *
     * WHAT A COMMAND MAY CAPTURE. By value, always. Under this fence a captured
     * pointer into game state is a live data race against the simulation of the
     * next frame, not merely bad style.
     * ========================================================================
     */
    class NE_API Application
    {
    public:
        /* The engine with no game attached: opens the window, runs the frame
         * loop and the thread split, draws the clear. What the repository had
         * before IGame existed, and what the engine's own bring-up uses. */
        void Exec();

        /* Runs InGame, which must outlive the call. */
        void Exec(IGame& InGame);

        /*
         * Ends the loop after the current frame completes.
         *
         * GAME THREAD ONLY -- from inside OnTick or OnStartup. It is a plain
         * bool, not an atomic, because the only writer is the thread that reads
         * it; making it callable from anywhere would be a different and much
         * larger promise about shutdown ordering.
         */
        void RequestQuit();

    private:
        bool Initialize();
        void Shutdown();

        /* The real Exec. The two public overloads differ only in whether they
         * have a game to pass. */
        void ExecWithGame(IGame* InGame);

        /* Game-thread half of the frame. Neither touches GDI. */
        void Simulate(IGame* InGame, const FrameContext& InFrame);
        void BuildRenderCommands();

        /* Main-thread side of the handshake with the render thread. */
        bool StartRenderThread();
        void StopRenderThread();

        /* The render thread's entire life, and the drain it runs. Both execute
         * on the render thread and nowhere else. */
        void RenderThreadMain();
        void DrainRenderCommands();

        GameWindow*              Window        = nullptr;

        /*
         * The window's registry id, kept because it stays meaningful after the
         * GameWindow itself is gone. PumpEvents REAPS windows the user closed,
         * so once the loop exits, Window may already be destroyed and only the
         * id can safely be asked about it. Never 0 on a live window.
         */
        uint32_t                 WindowID      = 0;

        /* Captured ONCE, on the main thread, in Initialize(). GetNativeHandle()
         * is a live SDL query rather than a cached read, so it is not something
         * to call per frame or from the render thread. */
        EngineCore::WindowHandle NativeWindow;

        /* Owned here: ReleaseGDI() deletes GDynamicGDI and GSync, and never
         * touches the context it was assigned. */
        GDIContext*              RenderContext = nullptr;

        EngineCore::Thread       RenderThread;

        /* render -> main: AllocateResourcesGDI() has run, bRenderInitOK is
         * readable. */
        EngineCore::Semaphore    RenderReady;

        /* main -> render: commands for a frame are queued. */
        EngineCore::Semaphore    RenderKick;

        /* render -> main: that frame is drained and the queue is empty. */
        EngineCore::Semaphore    FrameDone;

        /* Cross-thread, hence atomic. Everything below them is main-thread only
         * and stays plain. */
        std::atomic<bool>        bRenderStopRequested{false};
        std::atomic<bool>        bRenderInitOK{false};

        bool                     bFrameInFlight    = false;
        bool                     bRenderThreadLive = false;
        bool                     bGDIInitialized   = false;
        bool                     bQuitRequested    = false;
        uint64_t                 FrameNumber       = 0;
    };
};

#endif //NIMBLEENGINE_NIMBLEENGINE_H
