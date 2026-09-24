#ifndef NIMBLEENGINE_GAMESTATE_H
#define NIMBLEENGINE_GAMESTATE_H

#include "Public/Core/CoreArchive.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace NimbleEngine
{
    /*
     * The game's mutable progress -- score, phase, round. Owned by the ILevel,
     * read and written by the IGameMode. The mode is the rules and should be
     * re-runnable against fresh state, so a restart resets this rather than
     * rebuilding the mode.
     *
     * Almost empty on purpose: the engine cannot read a game's state, it only
     * needs somewhere to hang "reset" and "serialize". Not NE_API -- nothing is
     * out-of-line, and the engine only calls through the vtable.
     */
    class GameState : public EngineCore::ISerializable
    {
    public:
        virtual ~GameState() = default;

        /*
         * Back to starting values. Called on every level (re)start, before
         * IGameMode::OnStart. This object belongs to one level; state that must
         * survive a level change does not live here. The default does nothing,
         * which suits a state whose initialisers are its starting values.
         */
        virtual void OnReset() {}

        /*
         * To and from a save, inside this state's own chunk. The default writes
         * nothing, which is correct for a game that does not save or whose
         * progress is all in the World. Application::LoadLevel documents when
         * this runs.
         *
         * To add a field, override GetSerialVersion() to return the next
         * number and append the field behind a check; Ar.GetChunkVersion() is
         * that number on save and the file's on load:
         *
         *     if(Ar.GetChunkVersion() >= 2) { Ar << NewField; }
         */
        void Serialize(EngineCore::Archive& Ar) override { (void)Ar; }
    };
}

#endif //NIMBLEENGINE_GAMESTATE_H
