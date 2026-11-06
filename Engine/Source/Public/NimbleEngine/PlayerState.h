#ifndef NIMBLEENGINE_PLAYERSTATE_H
#define NIMBLEENGINE_PLAYERSTATE_H

#include <cstdint>

#include "Public/NimbleEngine/NimbleEngineAPI.h"
#include "Public/GameCore/ComponentType.h"

namespace GameCore { class World; }

namespace NimbleEngine
{
    enum : size_t { PLAYER_NAME_MAX = 32 };   // including the NUL

    /*
     * "PlayerState": one player's standing, on the entity that is that player
     * (a paddle, a character). A component, so it is saved with the World,
     * spawned by prefabs and seen by scripts like any other.
     *
     * Score and Lives are read-only to scripts: they change through
     * GameSession::AddScore / AddLives (the "Game" library's AddScore and
     * AddLives), which tell the mode (IGameMode::OnScoreChanged,
     * OnLivesChanged) and emit ScoreChanged / LivesChanged. C++ may write the
     * fields directly to set up a player; that tells nobody.
     *
     * Adding or removing one tells the mode (IGameMode::OnPlayerAdded,
     * OnPlayerRemoved). GameSession::GetPlayer(Index) finds one by PlayerIndex.
     */
    struct NE_API PlayerStateComponent
    {
        GAMECORE_COMPONENT_BODY()

        char    PlayerName[PLAYER_NAME_MAX] = "Player";
        uint8_t PlayerIndex = 0;   // which player; 0 is the first, and the keyboard's
        int32_t Score       = 0;
        int32_t Lives       = 0;   // 0 for a game without lives
        uint8_t bActive     = 1;   // still in the game; the mode decides what that means
    };

    /* The lowest PlayerIndex no player in InWorld has: a new player's. */
    NE_API uint8_t FindFreePlayerIndex(GameCore::World& InWorld);
}

#endif //NIMBLEENGINE_PLAYERSTATE_H
