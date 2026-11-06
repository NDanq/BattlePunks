#ifndef NIMBLEENGINE_GAMESTATE_H
#define NIMBLEENGINE_GAMESTATE_H

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Public/Core/CoreArchive.h"
#include "Public/NimbleEngine/NimbleEngineAPI.h"

namespace NimbleEngine
{
    /*
     * Phase names the engine suggests. A phase is any short name: a game adds
     * its own ("Serving", "RoundCleared") and the engine treats them all alike.
     */
    namespace GamePhases
    {
        constexpr const char* WaitingToStart = "WaitingToStart";
        constexpr const char* InProgress     = "InProgress";
        constexpr const char* GameOver       = "GameOver";
    }

    enum : size_t { GAME_PHASE_NAME_MAX = 32 };   // including the NUL

    /*
     * The game's mutable progress -- score, phase, round. Owned by the ILevel,
     * read and written by the IGameMode. The mode is the rules and should be
     * re-runnable against fresh state, so a restart resets this rather than
     * rebuilding the mode.
     *
     * THE PHASE is the one thing the engine itself keeps here: a name, how long
     * it has lasted and a round counter. Scripts read it through the "Game"
     * library; only GameSession::SetPhase changes it, so the mode's rules
     * (IGameMode::CanEnterPhase, OnPhaseChanged) always see a change. The
     * engine resets and saves it (its own chunk); a subclass's OnReset and
     * Serialize need not touch it.
     *
     * Everything else is the game's. Not NE_API -- nothing is out-of-line, and
     * the engine only calls through the vtable.
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

        /* The phase a load starts in, entered without asking the mode. */
        virtual const char* GetInitialPhase() const { return GamePhases::WaitingToStart; }

        /*
         * To and from a save, inside this state's own chunk. Default: nothing.
         *
         * To add a field, bump GetSerialVersion() and guard it (the chunk
         * version is the current one on save, the file's on load):
         *
         *     if(Ar.GetChunkVersion() >= 2) { Ar << NewField; }
         */
        void Serialize(EngineCore::Archive& Ar) override { (void)Ar; }

        // === The phase ==========================================================

        /* Never null; "" before the first load. */
        const char* GetPhase() const { return Phase; }
        bool        IsPhase(const char* InPhase) const { return InPhase && std::strcmp(Phase, InPhase) == 0; }

        /* Since the phase was entered, advanced by GameSession::Tick. */
        double      GetPhaseSeconds() const { return PhaseSeconds; }

        /* 0 after a reset; the game counts rounds as it likes. */
        int32_t     GetRound() const { return Round; }
        void        SetRound(const int32_t InRound) { Round = InRound; }

    private:
        friend class GameSession;

        void WritePhase(const char* InPhase)
        {
            std::snprintf(Phase, sizeof(Phase), "%s", InPhase ? InPhase : "");
            PhaseSeconds = 0.0;
        }

        char    Phase[GAME_PHASE_NAME_MAX] = {};
        double  PhaseSeconds = 0.0;
        int32_t Round = 0;
    };
}

#endif //NIMBLEENGINE_GAMESTATE_H
