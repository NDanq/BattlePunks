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
        void Exec();

    private:
        bool Initialize();
        void Shutdown();

        /* Game-thread half of the frame. Neither touches GDI. */
        void Simulate();
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
