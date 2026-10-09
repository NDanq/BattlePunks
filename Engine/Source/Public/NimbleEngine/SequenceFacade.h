#ifndef NIMBLEENGINE_SEQUENCEFACADE_H
#define NIMBLEENGINE_SEQUENCEFACADE_H

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; }

/*
 * Cutscenes for C++ game code, without the Sequencer module's headers. Every
 * level's World plays sequences (GameSession attaches the players' system);
 * the calls below do nothing, returning false or an unset id, for a World
 * that has none.
 *
 * A player is an entity with a Sequence Player component: placed in the
 * level, or made by PlaySequence. The "Sequence" script library is the same
 * thing for Lua and script graphs (Engine/Modules/Sequencer/README.md):
 *
 *     local p = Sequence.Play("Content/Cinematics/Intro.nmseq")
 *
 * Game thread only.
 */
namespace NimbleEngine
{
    /* A new player for InPath (a .nmseq runtime path), playing now and
     * destroyed when it finishes. Unset if it cannot be loaded. */
    NE_API GameCore::EntityID PlaySequence(GameCore::World& InWorld, const char* InPath, bool bInLoop = false);

    NE_API bool StartSequence(GameCore::World& InWorld, GameCore::EntityID InPlayer);
    NE_API bool PauseSequence(GameCore::World& InWorld, GameCore::EntityID InPlayer);
    NE_API bool ResumeSequence(GameCore::World& InWorld, GameCore::EntityID InPlayer);
    NE_API bool StopSequence(GameCore::World& InWorld, GameCore::EntityID InPlayer);
    NE_API bool SkipSequence(GameCore::World& InWorld, GameCore::EntityID InPlayer);
    NE_API bool SeekSequence(GameCore::World& InWorld, GameCore::EntityID InPlayer, double InSeconds);

    /* InEntity plays the sequence's binding InBindingId. */
    NE_API bool BindSequence(GameCore::World& InWorld, GameCore::EntityID InPlayer, const char* InBindingId, GameCore::EntityID InEntity);

    NE_API bool   IsSequencePlaying(const GameCore::World& InWorld, GameCore::EntityID InPlayer);
    NE_API double GetSequenceTime(const GameCore::World& InWorld, GameCore::EntityID InPlayer);
}

#endif //NIMBLEENGINE_SEQUENCEFACADE_H
