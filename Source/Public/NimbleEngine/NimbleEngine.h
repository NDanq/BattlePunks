#ifndef NIMBLEENGINE_NIMBLEENGINE_H
#define NIMBLEENGINE_NIMBLEENGINE_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreFileProvider.h"
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
     * THE GAME'S RULES -- game mode, game state, win conditions, whatever a
     * specific game means by "the game" -- and the whole of what the engine
     * calls into.
     *
     * Application owns the frame loop unconditionally: pumping events, pacing,
     * the render fence, present, garbage collection and World::Tick are all
     * its business and none of them are reachable from here. What a mode
     * contributes is game LOGIC, at four points in a lifecycle the engine
     * drives.
     *
     * =============================== THREADING ===============================
     *
     * EVERY METHOD HERE RUNS ON THE GAME THREAD, which is also the main thread
     * and the SDL pump thread. OnTick in particular runs CONCURRENTLY with the
     * render thread executing the PREVIOUS frame's commands -- that overlap is
     * the point of the split -- so a mode must not touch RenderCore::RenderContext
     * or a RenderProxy directly; both are render-thread-only types.
     * ========================================================================
     *
     * =============================== LIFECYCLE ===============================
     *
     *   OnLoad    engine up, nothing running yet. The World, ScriptEngine and
     *             Renderer are all valid and EMPTY. Build the mode's entities
     *             and register its systems here.
     *
     *   OnStart   everything loaded, including the startup Lua script, and the
     *             first tick has not run. Read what OnLoad and the script both
     *             produced, and start the game proper.
     *
     *   OnTick    once per frame, for the life of the game.
     *
     *   OnUnload  the loop has ended. Undo what OnLoad did -- above all, unbind
     *             any delegate it bound.
     *
     * The split between OnLoad and OnStart is the startup script: a mode that
     * wants to see entities Lua created must read them in OnStart, because at
     * OnLoad the script has not run yet. A mode with no such dependency can
     * leave OnStart alone entirely.
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
     *      behind whatever frame boundary Application's own EndFrame() call
     *      draws next, so a command issued mid-tick still lands in a specific,
     *      well-defined frame rather than racing ahead of one already
     *      presenting.
     *
     * WHAT A COMMAND MAY CAPTURE. Trivially copyable and trivially
     * destructible only -- see RenderCommand.h's own capture rule. A
     * RenderProxy* or RenderProxyHandle is fine to capture; a pointer into
     * plain game-thread state (an IGameMode member, a local) is not, for the
     * same live-data-race reason FrameContext itself is a value.
     */
    class IGameMode
    {
    public:
        virtual ~IGameMode() = default;

        /*
         * The mode is being loaded: the engine is up -- window created, render
         * thread running and past its readiness check -- and the World,
         * ScriptEngine and Renderer are all valid. Nothing has ticked, and the
         * startup script has NOT run yet.
         *
         * Returning false aborts startup and the engine shuts down cleanly.
         * OnUnload still runs, so a mode that fails halfway through still gets
         * to undo the half it did.
         *
         * InApp outlives the mode for the duration of Exec, so stashing it to
         * reach World/Renderer or call RequestQuit() later is the intended use.
         */
        virtual bool OnLoad(Application& InApp) { (void)InApp; return true; }

        /*
         * The game is starting: everything is loaded, the startup Lua script
         * has run, and the first tick has not. This is where a mode reads
         * script-created state, or flips itself from "built" to "playing".
         *
         * Not called if OnLoad returned false.
         */
        virtual void OnStart() {}

        /* Once per frame. The only method a mode must implement. */
        virtual void OnTick(const FrameContext& InFrame) = 0;

        /*
         * The mode is being unloaded, after the loop ends and before the
         * window and render thread go away. The World and Renderer are still
         * valid here.
         *
         * PAIRS WITH OnLoad HAVING BEEN CALLED AT ALL, not with it having
         * succeeded -- a mode that refused to load still undoes what it did
         * while deciding. Whatever OnLoad bound, this unbinds; see Delegate.h's
         * RELOAD note for why that is not optional.
         */
        virtual void OnUnload() {}
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
        /* The engine with no mode attached: opens the window, runs the frame
         * loop and the thread split, draws the clear. What the repository had
         * before IGameMode existed, and what the engine's own bring-up uses. */
        void Exec();

        /* Runs InMode, which must outlive the call. */
        void Exec(IGameMode& InMode);

        /*
         * Ends the loop after the current frame completes.
         *
         * GAME THREAD ONLY -- from inside OnLoad, OnStart or OnTick. It is a
         * plain bool, not an atomic, because the only writer is the thread that
         * reads it; making it callable from anywhere would be a different and
         * much larger promise about shutdown ordering.
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
         * from OnLoad through OnUnload -- the same window this class
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
         * The renderer. GAME THREAD ONLY, valid from OnLoad through
         * OnUnload -- the same lifetime GetWorld()/GetScriptEngine() have,
         * and the same CORE_CHECKed-non-null contract. A game reads this to
         * call RegisterProxyType before its World is populated, or to read
         * RenderStats for a debug overlay; it never needs to call
         * WaitForPreviousFrame()/EndFrame() itself -- Application's own frame
         * loop already does, in NimbleEngine.cpp's Simulate/RunFrame.
         */
        RenderCore::Renderer& GetRenderer();

    private:
        bool Initialize();
        void Shutdown();

        /* The real Exec, and the owner of the whole lifecycle: engine up, mode
         * loaded and started, loop, mode unloaded, engine down. The two public
         * overloads differ only in whether they have a mode to pass. */
        void ExecWithMode(IGameMode* InMode);

        /*
         * OnLoad, then the startup script, then OnStart -- the ordering
         * IGameMode's own lifecycle comment states, in one place rather than
         * spread through the loop's preamble. Returns false if the mode
         * refused to load, which is a clean shutdown and not an error.
         *
         * Split out from ExecWithMode because it is the part a reader has to
         * get exactly right: the script runs BETWEEN the two mode callbacks,
         * and that is the whole distinction between them.
         */
        bool LoadAndStartMode(IGameMode* InMode);

        /*
         * One iteration: pump, assemble the FrameContext, Simulate, fence,
         * collect, present, end the frame. Returns false when the loop should
         * stop -- window closed, quit requested, or the frame cap hit. Every
         * engine-side step of a frame lives here; a mode contributes only the
         * OnTick that Simulate calls.
         *
         * Does NOT pace: ExecWithMode does that, after this returns, because
         * the timer-resolution scope pacing needs is a .cpp-local type that
         * cannot be named in this header -- and because pacing is the one step
         * that must be skipped on the way out of the loop.
         */
        bool RunFrame(IGameMode* InMode, uint64_t InMaxFrames);

        /* Game-thread half of the frame: IGameMode::OnTick, then Scene->Tick.
         * Touches neither GDI nor RenderCore directly -- RunFrame is what calls
         * Rendering->WaitForPreviousFrame()/EndFrame() around this. */
        void Simulate(IGameMode* InMode, const FrameContext& InFrame);

        /*
         * WHERE ASSETS COME FROM, for every subsystem that reads one --
         * GDI's cooked shaders above all. Installed into EngineCore's
         * locator by Initialize() and cleared by Shutdown(); see the comment
         * at the SetFileProvider call for why this is a member (it must
         * outlive the render thread's own shader loads) rather than a local.
         *
         * A member by value, not a pointer: it owns no resources beyond a
         * base-directory string, so there is nothing to allocate and nothing
         * for a failed Initialize() to leak.
         */
        EngineCore::FilesystemProvider AssetProvider;

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
         * are ever allocated). Simulate() ticks Scene after IGameMode::OnTick, on
         * the game thread, before RunFrame's Rendering->EndFrame() call --
         * see NimbleEngine.cpp's own comment on why nothing past that call may
         * touch game state.
         */
        GameCore::World*         Scene     = nullptr;
        GameCore::ScriptEngine*  Scripts   = nullptr;
        RenderCore::Renderer*    Rendering = nullptr;

        bool                     bQuitRequested    = false;
        uint64_t                 FrameNumber       = 0;

        /* When the previous tick was assembled, as a steady-clock nanosecond
         * count -- what the next frame's DeltaSeconds is measured from. A
         * member rather than a local in the loop because RunFrame is one
         * iteration and the delta spans two; int64_t rather than a chrono
         * time_point for the same reason NextFrameDeadlineNs is one, to keep
         * <chrono> out of this header. Zero until the first frame assembles. */
        int64_t                  PreviousTickNs      = 0;

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
