#ifndef NIMBLEENGINE_NIMBLEENGINE_H
#define NIMBLEENGINE_NIMBLEENGINE_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreTypes.h"
#include "Public/Core/CoreThread.h"

/*
 * Forward-declared, NOT included. Engine/CMakeLists.txt links GameWindow
 * PRIVATE precisely so this header cannot leak it to a game, and the comment
 * there says that an #include appearing here is the signal that something
 * grew a dependency it should not have. Only ever held as a pointer, so a
 * declaration is all this needs.
 *
 * GDI is NOT forward-declared here, or anywhere in this header, and that is
 * new: GDI moved entirely behind RenderCore (Engine/Modules/RenderCore), the
 * one image now allowed to link it -- see RenderCoreInternal.h's own comment
 * for why. NimbleEngine no longer owns a GDIContext, no longer starts a
 * render thread, and no longer drains a command queue; RenderCore::Renderer
 * does all three, and this header reaches it only through the forward
 * declaration below.
 */
class GameWindow;

/*
 * GameCore and RenderCore are the two PUBLIC exceptions to the rule above --
 * Engine/CMakeLists.txt links both PUBLIC, deliberately, because World/Entity
 * and Renderer/RenderCommand ARE the game-facing surface: a game reaches its
 * entities through Application::GetWorld() and draws through
 * Application::GetRenderer() plus ENQUEUE_RENDER_COMMAND, and both return
 * types have to be complete enough for a game's own #include to make sense
 * of. See the PUBLIC link's own comment in Engine/CMakeLists.txt for why
 * these are the deliberate exceptions rather than a dependency that leaked
 * in by accident.
 */
namespace GameCore
{
    class World;
    class ScriptEngine;
}

namespace RenderCore
{
    class Renderer;
}

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
     * the point of the split -- so a game must not touch RenderCore::RenderContext
     * or a RenderProxy directly; both are render-thread-only types.
     * ========================================================================
     *
     * A GAME DRAWS THROUGH THREE DOORS, none of which is an OnRender method:
     *
     *   1. Add a RenderCore::PrimitiveComponent to an entity (via
     *      Application::GetWorld()) and it draws itself, automatically, every
     *      frame -- Application::GetRenderer()'s own WorldBridge extracts it.
     *   2. Register a game-defined component + RenderProxy pair through
     *      RenderCore::RegisterProxyType before binding the World, the same
     *      way PrimitiveComponent registers itself.
     *   3. ENQUEUE_RENDER_COMMAND from any game-thread code, at any point
     *      during OnTick -- ONE FRAME OF LAG, not a fence: the command sits
     *      behind whatever frame boundary Application::Simulate's own
     *      EndFrame() call draws next, so a command issued mid-tick still
     *      lands in a specific, well-defined frame rather than racing ahead
     *      of one already presenting.
     *
     * WHAT A COMMAND MAY CAPTURE. Trivially copyable and trivially
     * destructible only -- see RenderCommand.h's own capture rule. A
     * RenderProxy* or RenderProxyHandle is fine to capture; a pointer into
     * plain game-thread state (an IGame member, a local) is not, for the
     * same live-data-race reason FrameContext itself is a value.
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
     * The application: the game thread, and the render thread RenderCore owns
     * on its behalf.
     *
     * =========================== THREAD TOPOLOGY ============================
     *
     * MAIN THREAD == game thread == SDL pump thread. GameWindowSystem latches
     * its pump thread at Initialize() and CORE_CHECKs every entry point against
     * it, and on macOS that thread must be the process main thread -- so there
     * is no freedom here to give the game its own thread.
     *
     * RENDER THREAD is RenderCore::RenderThread's, reached only through
     * Application::GetRenderer() and ENQUEUE_RENDER_COMMAND -- this class
     * never touches GDI, a GDIContext, or a command queue directly.
     *
     * ONE FRAME OF LAG, NOT A FENCE. Application::Simulate() runs a game's
     * OnTick CONCURRENTLY with the render thread draining the PREVIOUS
     * frame's commands -- that overlap is the point of the split -- but
     * unlike the fenced design this replaces, a game may ENQUEUE_RENDER_COMMAND
     * at any point during OnTick, not only from a single designated place.
     * RenderCore::RenderThread::EndFrame's own frame marker is what makes a
     * command issued mid-tick land in a specific frame rather than racing
     * the boundary -- see RenderCommand.h's header comment for the full
     * mechanism.
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

        /*
         * Frames per second to pace the loop to. ZERO MEANS UNLIMITED: the loop
         * runs as fast as it can, which is what bring-up and throughput
         * measurement want and what a shipped game almost never does.
         *
         * Takes effect from the next frame. The initial value comes from
         * [Display] FrameRateLimit.
         *
         * GAME THREAD ONLY, for the same reason RequestQuit is: the pacing state
         * is plain, and the only thread that reads it is the one that would be
         * writing it.
         */
        void     SetFrameRateLimit(uint32_t InFramesPerSecond);
        uint32_t GetFrameRateLimit() const;

        /*
         * The scene, and the Lua layer bound to it. GAME THREAD ONLY, valid
         * from OnStartup through OnShutdown -- the same window this class
         * already opens and tears down around a game's lifetime. A game reads
         * these to create/query entities (World) or to run a script directly
         * (ScriptEngine::RunString, for a console command say); NimbleEngine.cpp's
         * Initialize()/Shutdown() are what actually create, bind, unbind and
         * destroy both -- see the ordering note on Shutdown() there for why
         * the World must be destroyed before the ScriptEngine is.
         *
         * CORE_CHECKed non-null rather than returning a reference to nothing:
         * calling either before Initialize() has run (or after Shutdown() has)
         * is a programming error in the SAME class as calling GetFrameRateLimit()
         * before Exec() -- there is no sensible fallback value, only a caller
         * that got the lifecycle wrong.
         */
        GameCore::World&         GetWorld();
        GameCore::ScriptEngine&  GetScriptEngine();

        /*
         * The renderer. GAME THREAD ONLY, valid from OnStartup through
         * OnShutdown -- the same lifetime GetWorld()/GetScriptEngine() have,
         * and the same CORE_CHECKed-non-null contract. A game reads this to
         * call RegisterProxyType before its World is populated, or to read
         * RenderStats for a debug overlay; it never needs to call
         * WaitForPreviousFrame()/EndFrame() itself -- Application's own frame
         * loop already does, in NimbleEngine.cpp's Simulate/ExecWithGame.
         */
        RenderCore::Renderer& GetRenderer();

    private:
        bool Initialize();
        void Shutdown();

        /* The real Exec. The two public overloads differ only in whether they
         * have a game to pass. */
        void ExecWithGame(IGame* InGame);

        /* Game-thread half of the frame: IGame::OnTick, then Scene->Tick.
         * Touches neither GDI nor RenderCore directly -- ExecWithGame's own
         * loop is what calls Rendering->WaitForPreviousFrame()/EndFrame()
         * around this. */
        void Simulate(IGame* InGame, const FrameContext& InFrame);

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
         * to call per frame or from the render thread. Handed to
         * Rendering->Initialize(), which is RenderCore's own render-thread
         * startup from here on. */
        EngineCore::WindowHandle NativeWindow;

        /*
         * Owned here, via EngineCore::Memory::Alloc/Dealloc (not new/delete --
         * see NimbleEngine.cpp's Initialize(), which is the ONE place these
         * are ever allocated). Simulate() ticks Scene after IGame::OnTick, on
         * the game thread, before ExecWithGame's Rendering->EndFrame() call --
         * see NimbleEngine.cpp's own comment on why nothing past that call may
         * touch game state.
         */
        GameCore::World*         Scene     = nullptr;
        GameCore::ScriptEngine*  Scripts   = nullptr;
        RenderCore::Renderer*    Rendering = nullptr;

        bool                     bQuitRequested    = false;
        uint64_t                 FrameNumber       = 0;

        /* 0 = unlimited. See SetFrameRateLimit. */
        uint32_t                 FrameRateLimit    = 0;

        /* When the next frame should START, as a steady-clock nanosecond count.
         * A DEADLINE rather than a per-frame duration, so that a frame finishing
         * early or late does not shift every frame after it -- pacing to
         * "now + 16ms" accumulates every overshoot forever. Zero means no
         * deadline is established yet.
         *
         * int64_t rather than a chrono time_point to keep <chrono> out of this
         * header; the .cpp does the conversion. */
        int64_t                  NextFrameDeadlineNs = 0;
    };
};

#endif //NIMBLEENGINE_NIMBLEENGINE_H
