#ifndef NIMBLEENGINE_NIMBLEENGINE_H
#define NIMBLEENGINE_NIMBLEENGINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreArchive.h"      // EngineCore::Archive, for RestoreSaveInto below
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
    class GameState;
    class ILevel;

    /* Declared ahead of Application so its friend declarations need not repeat
     * NE_API -- MSVC calls that inconsistent linkage (C4273), as with
     * CoreConfig.h's ConfigRegisterVar. Documented in SaveGame.h. */
    NE_API bool SaveGameToFile  (Application& InApp, const std::string& InPath);
    NE_API bool LoadGameFromFile(Application& InApp, const std::string& InPath);

    /*
     * What a game is told about the frame it is ticking. A value with no
     * pointers into engine state, read on the thread that owns each field.
     * ViewportPixels is why it exists: GameWindow's accessors do not assert the
     * pump thread, so the engine reads the size once and hands over the number.
     *
     * Not NE_API: header-only aggregate (NimbleEngineAPI.h).
     */
    struct FrameContext
    {
        /* Since the previous tick; zero on the first frame, not an invented value. */
        double           DeltaSeconds = 0.0;

        /* Ticks completed before this one, so the first tick sees 0. */
        uint64_t         FrameNumber  = 0;

        /* PHYSICAL pixels. Size a viewport from this, not from logical units. */
        EngineCore::Size ViewportPixels;
    };

    /*
     * THE GAME'S RULES -- the whole of what the engine calls into. Application
     * owns the loop (pump, pacing, fence, present, GC, World::Tick); ILevel owns
     * the entities; a mode supplies logic at the lifecycle points below.
     *
     * THREADING. Every method runs on the game thread (= main = SDL pump).
     * OnTick overlaps the render thread executing the previous frame, so never
     * touch a RenderContext or RenderProxy directly.
     *
     * LIFECYCLE -- Application::LoadLevel has the full sequence:
     *
     *   OnLoad    the level has populated the World. Cache handles, bind
     *             delegates.
     *   OnStart   the startup script has run and the GameState is reset.
     *             Read what Lua created here, not in OnLoad.
     *   OnTick    once per frame.
     *   OnUnload  undo OnLoad -- above all, unbind delegates.
     *
     * DRAWING has no OnRender. Add a PrimitiveComponent (WorldBridge draws it),
     * register a component + RenderProxy pair with RegisterProxyType, or
     * ENQUEUE_RENDER_COMMAND anywhere in OnTick -- one frame of lag, landing in a
     * well-defined frame. Commands capture trivially copyable, trivially
     * destructible values only (RenderCommand.h): a proxy handle yes, a pointer
     * to game-thread state no.
     */
    class IGameMode
    {
    public:
        virtual ~IGameMode() = default;

        /*
         * The World is already populated; nothing has ticked and the startup
         * script has not run. Set up what the rules need -- the level spawns the
         * scene, not the mode. False aborts the load, but OnUnload still runs.
         * InApp outlives the mode; stashing it is intended.
         */
        virtual bool OnLoad(Application& InApp) { (void)InApp; return true; }

        /* Everything loaded, script run, state reset, no tick yet. Not called if
         * OnLoad returned false. */
        virtual void OnStart() {}

        /* Once per frame. The only method a mode must implement. */
        virtual void OnTick(const FrameContext& InFrame) = 0;

        /*
         * A save has just been restored; OnStart has not run. Only on the
         * load-from-save path. Re-derive every cached EntityID here -- restored
         * entities are new entities, so handles from OnLoad are stale.
         */
        virtual void OnPostLoadSave() {}

        /* A save is about to be written. The mode itself is not saved, so flush
         * anything that must survive into the GameState. */
        virtual void OnPreSaveGame() {}

        /*
         * After the loop ends; the World and Renderer are still valid. Pairs
         * with OnLoad having been CALLED, not with it succeeding. Unbinding what
         * OnLoad bound is required -- Delegate.h's RELOAD note.
         */
        virtual void OnUnload() {}
    };

    /*
     * The application: the game thread, and the render thread RenderCore owns
     * on its behalf.
     *
     * The main thread is the game thread and the SDL pump thread; GameWindowSystem
     * CORE_CHECKs that, and macOS requires it, so the game cannot have its own
     * thread. The render thread is reached only through GetRenderer() and
     * ENQUEUE_RENDER_COMMAND.
     *
     * One frame of lag, not a fence: Simulate() runs OnTick while the render
     * thread drains the previous frame, and EndFrame's frame marker is what lets
     * a mid-tick command land in a specific frame (RenderCommand.h).
     */
    class NE_API Application
    {
    public:
        /* No level of its own: window, loop, clear. An empty engine-owned level
         * still exists, so the renderer and scripts have a World to bind. */
        void Exec();

        /* Runs InLevel, which must outlive the call. The primary entry point. */
        void Exec(ILevel& InLevel);

        /* InMode against an empty engine-owned level -- for a mode that spawns
         * its own entities, small games and the smoke tests. */
        void Exec(IGameMode& InMode);

        /*
         * Swap to InLevel, which must outlive the call. Always deferred to the
         * top of the next frame -- destroying a World from inside its own Tick
         * would free the pools being iterated. The outgoing level is fully
         * unloaded first, so the two never coexist. Game thread only.
         */
        void RequestLevel(ILevel& InLevel);

        /* nullptr before the first load and after the last unload. Game thread only. */
        ILevel* GetLevel();

        /* GetLevel()->GetGameState(), or nullptr. */
        GameState* GetGameState();

        /* Ends the loop after this frame. Game thread only -- a plain bool,
         * because its only writer is also its only reader. */
        void RequestQuit();

        /* Pacing target; 0 is unlimited. Next frame on; initial value from
         * [Display] FrameRateLimit. Game thread only, like RequestQuit. */
        void     SetFrameRateLimit(uint32_t InFramesPerSecond);
        uint32_t GetFrameRateLimit() const;

        /*
         * The active level's World and the Lua layer bound to it; game thread
         * only, valid OnLoad through OnUnload. A level swap replaces the World,
         * so re-ask rather than holding the reference. The ScriptEngine outlives
         * every level (one lua_State, rebound per level): registered classes
         * survive a swap, instances do not. CORE_CHECKed rather than null --
         * calling outside that window is a lifecycle bug.
         */
        GameCore::World&         GetWorld();
        GameCore::ScriptEngine&  GetScriptEngine();

        /* Same lifetime and contract as GetWorld(). For RegisterProxyType and
         * RenderStats; the loop already handles the frame fence and EndFrame. */
        RenderCore::Renderer& GetRenderer();

    private:
        /* SaveGame.cpp reads the active level and World and arms
         * PendingSaveBytes. Friends rather than a public setter: LoadLevel's
         * ordering only holds if exactly one caller can arm a restore. */
        friend NE_API bool SaveGameToFile(Application& InApp, const std::string& InPath);
        friend NE_API bool LoadGameFromFile(Application& InApp, const std::string& InPath);

        bool Initialize();
        void Shutdown();

        /* The real Exec: engine up, level loaded, loop, level unloaded, engine
         * down. The level-less overloads pass a .cpp-local empty level. */
        void ExecWithLevel(ILevel* InLevel);

        /*
         * THE LOAD SEQUENCE -- the ordering every other comment points at:
         *
         *   World created, scripts and renderer bound
         *   ILevel::OnLoad            the level populates the World
         *   IGameMode::OnLoad
         *   the startup script        between the mode's two callbacks, which is
         *                             the whole point of having two
         *   GameState::OnReset
         *   [only when restoring a save]
         *       ILevel::OnPrepareForLoadedGame
         *       the World, then the GameState, from PendingSaveBytes
         *       IGameMode::OnPostLoadSave
         *   IGameMode::OnStart
         *
         * The restore comes after OnReset, so fields a save does not cover are
         * defaults rather than stale values, and before OnStart, so a mode starts
         * against restored state.
         *
         * False if the level or mode refused. The level stays loaded either way,
         * so UnloadLevel() still pairs with OnLoad having been called.
         */
        bool LoadLevel(ILevel* InLevel);

        /* The restore step of LoadLevel, split out so the sequence above stays
         * readable. False on a corrupt save; the level stays loaded. */
        bool RestoreSaveInto(EngineCore::Archive& Ar, ILevel& InLevel, GameState* InState);

        /* The reverse, in Shutdown()'s order: mode OnUnload, level OnUnload,
         * unbind scripts, unbind renderer, destroy the World. Null-safe and
         * idempotent -- LoadLevel's failure path runs it too. */
        void UnloadLevel();

        /* Applies a pending RequestLevel() at the top of RunFrame, where no
         * World is ticking. */
        void ApplyPendingLevel();

        /*
         * One frame: pump, FrameContext, Simulate, fence, collect, present, end.
         * False when the loop should stop. Pacing is in ExecWithLevel instead:
         * its timer scope is a .cpp-local type, and it is skipped on the way out.
         */
        bool RunFrame(uint64_t InMaxFrames);

        /* IGameMode::OnTick, then World::Tick. The mode is re-read each frame,
         * since a swap may have replaced it. */
        void Simulate(const FrameContext& InFrame);

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
         * render thread serve every level, rebound as each loads. */
        GameCore::ScriptEngine*  Scripts   = nullptr;
        RenderCore::Renderer*    Rendering = nullptr;

        /* LevelWorld is allocated by LoadLevel, destroyed by UnloadLevel, and is
         * the same object as ILevel::OwnedWorld meanwhile. */
        ILevel*                  ActiveLevel = nullptr;
        GameCore::World*         LevelWorld  = nullptr;

        /* Set by RequestLevel, consumed by the next RunFrame -- the request
         * usually arrives from inside the outgoing World's Tick. */
        ILevel*                  PendingLevel = nullptr;

        /*
         * A save waiting to be restored, armed with PendingLevel by
         * LoadGameFromFile and cleared by the LoadLevel that consumes it, so an
         * ordinary level load never picks up a stale save. A private std::vector
         * on an exported class is C4251 noise, not a hazard.
         */
        std::vector<uint8_t>     PendingSaveBytes;

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
