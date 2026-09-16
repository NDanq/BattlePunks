#ifndef GAMECORE_TICKPHASE_H
#define GAMECORE_TICKPHASE_H

#include <cstdint>

#include "Public/GameCore/GameCoreAPI.h"

/*
 * Where in the frame a system, or a scripted entity, runs. World::Tick walks
 * these buckets in order and flushes the deferred destroy/remove queue at
 * the end of each phase, not the end of the frame -- see DESIGN.md and
 * World.h's phases section.
 */
namespace GameCore
{
    enum ETickPhase : uint8_t
    {
        /* Input response, spawning, AI decisions. The default when a system
         * or script does not ask for a phase. */
        TP_PrePhysics = 0,

        /* Where a physics module's step belongs (nothing built-in runs here
         * yet). World's own spatial refit runs at the END of this phase, so
         * a physics step's movement is reflected before PostPhysics queries
         * the tree -- see SpatialIndex.h. */
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
