#ifndef NIMBLEENGINE_GAMESTATE_H
#define NIMBLEENGINE_GAMESTATE_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace NimbleEngine
{
    /*
     * THE GAME'S MUTABLE PROGRESS -- score, phase, round number, whatever a
     * specific game tracks while it is being played.
     *
     * Owned by the ILevel (Level.h), read and written by the IGameMode. That
     * split is the whole point of this type existing rather than the mode
     * holding the data itself: the mode is the RULES, and rules should be
     * re-runnable against fresh state. Restarting a level means resetting the
     * state, not rebuilding the mode.
     *
     * A BASE CLASS WITH ALMOST NOTHING IN IT, deliberately. The engine never
     * reads a game's state -- it cannot, it has no idea what is in there -- so
     * there is no virtual accessor here for it to call. What the engine needs
     * is exactly one thing: somewhere to hang "put this back to the
     * beginning", so a level restart does not have to go through the game's
     * own bespoke method.
     *
     * Not NE_API: a game subclasses this in its own image and the engine only
     * ever holds a GameState*, calling through the vtable. Nothing here is
     * defined out-of-line, so there is no symbol for a consumer to import --
     * NimbleEngineAPI.h states that rule.
     */
    class GameState
    {
    public:
        virtual ~GameState() = default;

        /*
         * Put this state back to its starting values.
         *
         * Called by the engine when a level is (re)started, BEFORE
         * IGameMode::OnStart, so a mode's OnStart always sees a state that has
         * just been reset. A game that wants state to survive a level change
         * simply does not store it here -- this object belongs to one level.
         *
         * The default does nothing, which is correct for a state that is
         * default-constructed into its starting values anyway.
         */
        virtual void OnReset() {}
    };
}

#endif //NIMBLEENGINE_GAMESTATE_H
