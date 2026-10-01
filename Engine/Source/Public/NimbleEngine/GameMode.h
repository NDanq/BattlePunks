#ifndef NIMBLEENGINE_GAMEMODE_H
#define NIMBLEENGINE_GAMEMODE_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/Core/CoreTypes.h"

namespace NimbleEngine
{
    class GameSession;

    /*
     * What a game is told about the frame it is ticking. A value with no
     * pointers into engine state, read on the thread that owns each field.
     * ViewportPixels is why it exists: the host reads its surface size once and
     * hands over the number, whatever that surface is (an SDL window, an
     * editor viewport).
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
     * THE GAME'S RULES -- the whole of what the engine calls into. A host owns
     * the loop (pump, pacing, fence, present, GC); the GameSession owns the
     * level's lifecycle and World::Tick; ILevel owns the entities; a mode
     * supplies logic at the lifecycle points below.
     *
     * HOST-AGNOSTIC. A mode sees a GameSession, never the program running it --
     * the same mode runs in the game executable (Application) and, later,
     * inside the editor. Host services it may need (the frame-rate limit) are
     * on GameSession::GetHost().
     *
     * THREADING. Every method runs on the game thread (= main = the event pump).
     * OnTick overlaps the render thread executing the previous frame, so never
     * touch a RenderContext or RenderProxy directly.
     *
     * LIFECYCLE -- GameSession::LoadLevel has the full sequence:
     *
     *   OnLoad    the level has populated the World. Cache handles, bind
     *             delegates.
     *   OnStart   the startup script has run and the GameState is reset.
     *             Read what Lua created here, not in OnLoad.
     *   OnTick    once per frame.
     *   OnUnload  undo OnLoad -- above all, unbind delegates.
     *
     * DRAWING has no OnRender. Add a RenderComponent (WorldBridge draws it),
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
         * InSession outlives the mode; stashing it is intended.
         */
        virtual bool OnLoad(GameSession& InSession) { (void)InSession; return true; }

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
         * After the last tick; the World and Renderer are still valid. Pairs
         * with OnLoad having been CALLED, not with it succeeding. Unbinding what
         * OnLoad bound is required -- Delegate.h's RELOAD note.
         */
        virtual void OnUnload() {}
    };
}

#endif //NIMBLEENGINE_GAMEMODE_H
