#ifndef SEQUENCER_SEQUENCEPLAYERCOMPONENT_H
#define SEQUENCER_SEQUENCEPLAYERCOMPONENT_H

#include <cstddef>
#include <cstdint>

#include "Public/GameCore/ComponentType.h"
#include "Public/Sequencer/SequencerAPI.h"

namespace Sequencer
{
    enum : size_t { SEQUENCE_PATH_MAX = 128 };   // including the NUL

    /* SequencePlayerComponent::BlendEase: the curve of a blend in or out.
     * Smooth is a smoothstep, as camera cuts blend; the rest are
     * UIAnimation.h's eases. */
    enum ESequenceBlendEase : uint8_t
    {
        SBE_Smooth = 0,
        SBE_Linear,
        SBE_QuadIn,
        SBE_QuadOut,
        SBE_QuadInOut,
        SBE_CubicIn,
        SBE_CubicOut,
        SBE_CubicInOut,
        SBE_Count
    };

    /* InT (clamped to 0..1) through the curve InEase (an ESequenceBlendEase;
     * Smooth for an unknown value). */
    SEQUENCER_API float SequenceBlendEase(uint8_t InEase, float InT);

    /* SequencePlayerComponent::ReleaseControl: when a Cinematic player hands
     * the players their input back and takes the letterbox away. */
    enum ESequenceControlRelease : uint8_t
    {
        SCR_AfterBlend = 0,   // when the blend out has finished (with no Blend Out: as the sequence ends)
        SCR_BlendStart,       // as the sequence ends, so the player moves while the view eases in
        SCR_BeforeEnd,        // ReleaseBeforeEnd seconds of the sequence before its end
        SCR_Count
    };

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

        /* HANDING THE VIEW OVER (README.md, "Blending in and out"). Seconds
         * the view eases from the view it took (the gameplay camera, tracked
         * as it moves) into the first shot, and from the last shot back to
         * the view it gives back when the sequence finishes. A skip blends
         * out over half of BlendOut. 0 (the default) cuts. */
        uint8_t BlendEase      = 0;      // ESequenceBlendEase, both ways
        uint8_t ReleaseControl = 0;      // ESequenceControlRelease: when a Cinematic player lets go of input and letterbox
        float   BlendIn        = 0.0f;
        float   BlendOut       = 0.0f;
        /* With ReleaseControl SCR_BeforeEnd: this many seconds of the
         * sequence before its end. */
        float   ReleaseBeforeEnd = 0.0f;

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
