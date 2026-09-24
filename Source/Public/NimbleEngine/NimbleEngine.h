#ifndef NIMBLEENGINE_NIMBLEENGINE_H
#define NIMBLEENGINE_NIMBLEENGINE_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/Core/CoreFileProvider.h"
#include "Public/Core/CoreTypes.h"
#include "Public/Core/CoreThread.h"

/*
 * Forward-declared, not included: GameWindow is linked PRIVATE so this header
 * cannot leak it to a game (Engine/CMakeLists.txt). An #include here means
 * something grew a dependency it should not have. GDI does not appear at all --
 * it lives behind RenderCore, the one image allowed to link it.
 */
class GameWindow;

/*
 * GameCore and RenderCore are the deliberate PUBLIC exceptions: World/Entity and
 * Renderer/RenderCommand are the game-facing surface, so Engine/CMakeLists.txt
 * links both PUBLIC and a game includes their headers itself.
 */
namespace GameCore
{
    class ScriptEngine;
}

namespace RenderCore
{
    class Renderer;
}

namespace NimbleEngine
{
    class ILevel;

    /*
     * THE GAME EXECUTABLE'S HOST: the program around a GameSession. It owns what
     * a game never sees directly -- the working directory and config, the SDL
     * window and event pump, frame pacing, the process-lifetime ScriptEngine
     * and Renderer, and each frame's fence, garbage collection, present and
     * EndFrame -- and drives one GameSession through it. The game itself (level,
     * World, mode, state, save/load) is the session's; see GameSession.h for why
     * the two are separate.
     *
     * The main thread is the game thread and the SDL pump thread; GameWindowSystem
     * CORE_CHECKs that, and macOS requires it, so the game cannot have its own
     * thread. The render thread is reached only through the Renderer and
     * ENQUEUE_RENDER_COMMAND.
     *
     * One frame of lag, not a fence: the session's Tick runs OnTick while the
     * render thread drains the previous frame, and EndFrame's frame marker is
     * what lets a mid-tick command land in a specific frame (RenderCommand.h).
     */
    class NE_API Application final : public IGameHost
    {
    public:
        Application() = default;
        ~Application() override = default;

        Application(const Application&)            = delete;
        Application& operator=(const Application&) = delete;

        /* No level of its own: window, loop, clear. An empty engine-owned level
         * still exists, so the renderer and scripts have a World to bind. */
        void Exec();

        /* Runs InLevel, which must outlive the call. The primary entry point. */
        void Exec(ILevel& InLevel);

        /* InMode against an empty engine-owned level -- for a mode that spawns
         * its own entities, small games and the smoke tests. */
        void Exec(IGameMode& InMode);

        /* Ends the loop after this frame -- the host's own quit, e.g. from a
         * close request. A game asks through GameSession::RequestQuit, which
         * the loop reads the same way. Game thread only. */
        void RequestQuit() { bQuitRequested = true; }

        /* The running session, valid while Exec runs (after Initialize, before
         * Shutdown). CORE_CHECKed outside that window. */
        GameSession& GetSession();

        // === IGameHost ===========================================================

        /* Pacing target; 0 is unlimited. Next frame on; initial value from
         * [Display] FrameRateLimit. Game thread only. */
        void     SetFrameRateLimit(uint32_t InFramesPerSecond) override;
        uint32_t GetFrameRateLimit() const override;

    private:
        bool Initialize();
        void Shutdown();

        /* The real Exec: engine up, level loaded, loop, level unloaded, engine
         * down. The level-less overloads pass a .cpp-local empty level. */
        void ExecWithLevel(ILevel& InLevel);

        /*
         * One frame: apply a pending level swap, pump, FrameContext, the
         * session's Tick, fence, collect, present, end. False when the loop
         * should stop. Pacing is in ExecWithLevel instead: its timer scope is a
         * .cpp-local type, and it is skipped on the way out.
         */
        bool RunFrame(uint64_t InMaxFrames);

        /* Where assets (cooked shaders above all) come from; installed by
         * Initialize(). A member because it must outlive the render thread's
         * shader loads, and by value because it owns only a string. */
        EngineCore::FilesystemProvider AssetProvider;

        GameWindow*              Window        = nullptr;

        /* Outlives the window: PumpEvents reaps closed windows, so after the loop
         * only the id is safe to ask about. Never 0 on a live window. */
        uint32_t                 WindowID      = 0;

        /* Captured once in Initialize() -- GetNativeHandle() is a live SDL
         * query -- and handed to Rendering->Initialize(). */
        EngineCore::WindowHandle NativeWindow;

        /* Process lifetime, via Memory::Alloc/Dealloc. One lua_State and one
         * render thread serve every level, lent to the session. */
        GameCore::ScriptEngine*  Scripts   = nullptr;
        RenderCore::Renderer*    Rendering = nullptr;

        /* Created by Initialize() once Scripts and Rendering exist; destroyed by
         * Shutdown() before them. */
        GameSession*             Session   = nullptr;

        bool                     bQuitRequested    = false;
        uint64_t                 FrameNumber       = 0;

        /* Steady-clock ns of the previous tick, for DeltaSeconds. int64_t to keep
         * <chrono> out of this header. Zero until the first frame. */
        int64_t                  PreviousTickNs      = 0;

        /* 0 = unlimited. See SetFrameRateLimit. */
        uint32_t                 FrameRateLimit    = 0;

        /* Steady-clock ns when the next frame should start. A deadline, not a
         * duration, so one late frame does not shift every later one. Zero
         * until established. */
        int64_t                  NextFrameDeadlineNs = 0;
    };
};

#endif //NIMBLEENGINE_NIMBLEENGINE_H
