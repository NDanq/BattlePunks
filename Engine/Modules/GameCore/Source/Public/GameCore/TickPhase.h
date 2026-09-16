#ifndef GAMECORE_TICKPHASE_H
#define GAMECORE_TICKPHASE_H

#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"

/*
 * Where in the frame a system, or a scripted entity, runs.
 *
 * World::Tick walks these buckets IN ORDER, once per frame, and flushes the
 * deferred destroy/remove queue at the END OF EACH PHASE rather than at the end
 * of the frame -- see World.h's "Phases and the deferred queue" note. That is
 * what makes the phase a entity sees meaningful: an entity a PrePhysics system
 * destroys is gone by the time Physics runs, not merely gone by next frame.
 *
 * Four phases, named for what they are relative to rather than numbered, so
 * World.AddSystem("Foo", TickPhase.PostPhysics, ...) on the Lua side reads as
 * intent rather than an arbitrary priority. A fifth phase is one enum line here
 * plus one line in ScriptBindings.cpp's TickPhase table -- nothing else in the
 * module enumerates phases by name.
 */
namespace GameCore
{
    enum ETickPhase : uint8_t
    {
        /* Input response, spawning, AI decisions. The default for a system or a
         * script that does not ask for a phase -- most gameplay wants to run
         * before physics resolves this frame's movement, not after. */
        TP_PrePhysics = 0,

        /* Where a physics module's step belongs. Nothing built into GameCore
         * runs here yet; it exists so that module has a bucket to register into
         * as an ordinary system, rather than needing a new concept. GameCore's
         * own spatial refit (World::RefreshSpatialIndex, see SpatialIndex.h)
         * runs at the END of this phase, after every system registered here, so
         * a physics step's own movement is reflected before PostPhysics queries
         * the tree. */
        TP_Physics,

        /* Reacting to where things ended up: cameras, constraints, triggers,
         * anything that reads this frame's resolved positions. */
        TP_PostPhysics,

        /* Last word before NimbleEngine::Application builds this frame's render
         * commands: UI state, bookkeeping, anything that must see every earlier
         * phase's result. */
        TP_PostUpdate,

        TP_MAX
    };

    /* Never null; "Unknown" for a value outside the enum (e.g. a bad byte read
     * back from a stale save or a corrupt script table). Diagnostic text only --
     * do not parse it back into an ETickPhase. */
    GAMECORE_API const char* TickPhaseName(ETickPhase InPhase);
}

#endif //GAMECORE_TICKPHASE_H
