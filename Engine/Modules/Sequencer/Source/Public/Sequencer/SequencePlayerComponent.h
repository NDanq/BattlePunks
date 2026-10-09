#ifndef SEQUENCER_SEQUENCEPLAYERCOMPONENT_H
#define SEQUENCER_SEQUENCEPLAYERCOMPONENT_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/Sequencer/SequencerAPI.h"

namespace Sequencer
{
    enum : size_t { SEQUENCE_PATH_MAX = 128 };   // including the NUL

    /*
     * "Sequence Player". Plays a sequence (.nmseq) in its entity's World:
     * by itself as the level starts (AutoPlay), or when told -- from Lua
     * (Sequence.Start(player), or Sequence.Play(path), which makes a player),
     * or C++ (SequenceWorld). The playhead and what it has changed live with
     * the world's SequenceWorld, not here: components are POD.
     *
     * The entity is only the player's handle; where it is does not matter.
     */
    struct SEQUENCER_API SequencePlayerComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    Sequence[SEQUENCE_PATH_MAX] = {};   // .nmseq, a runtime path
        float   PlayRate  = 1.0f;                   // 1 is real time; 0 holds; never backwards
        uint8_t bAutoPlay = 1;                      // start with the level
        uint8_t bLoop     = 0;                      // from the end back to the start, forever

        /* When it stops or finishes, put back every value it changed. Off,
         * the last frame stays, which is what a cutscene that hands over to
         * gameplay wants. */
        uint8_t bRestoreState = 0;

        /* When it finishes (not when stopped), destroy this entity. What
         * Sequence.Play's players do. */
        uint8_t bDestroyWhenFinished = 0;

        /* While it plays: letterboxed, and the players' input held back
         * (GameSession). Skippable: the Back key (Escape, gamepad East)
         * skips it, as Sequence.Skip does. */
        uint8_t bCinematic  = 0;
        uint8_t bSkippable  = 1;

        // === Engine-written: never saved ====================================
        /* The server's playback, replicated: a client's copy follows it
         * rather than playing on its own (SequenceWorld.h, "Networking"). */
        uint8_t NetState   = 0;      // ESequencePlayState
        uint8_t Reserved0  = 0;
        int32_t NetSerial  = 0;      // bumped by each start, seek and skip
        float   NetSeconds = 0.0f;   // where the playhead was at the last change
    };
}

#endif //SEQUENCER_SEQUENCEPLAYERCOMPONENT_H
