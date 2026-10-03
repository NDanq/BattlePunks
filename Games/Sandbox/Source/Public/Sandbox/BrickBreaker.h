#ifndef SANDBOX_BRICKBREAKER_H
#define SANDBOX_BRICKBREAKER_H

#include <cstdint>

#include "Public/GameCore/ComponentType.h"

namespace NimbleEngine
{
    class ILevel;
}

/*
 * "Brick": how many more hits a brick takes. BrickBreakerGameMode sets it by
 * row when it builds a wall -- 1 on the bottom row, one more for each row up --
 * and Scripts/Brick.script.nmgraph counts it down, breaking the brick at 0.
 * Saved with the level (Content/Levels/BrickBreaker.nmworld), and editable there.
 */
struct SandboxBrickComponent
{
    GAMECORE_COMPONENT_BODY()

    int32_t Hits = 1;
};

/*
 * BrickBreaker: the game framework's example (Engine/Source/GAMEFRAMEWORK.md),
 * a level of SandboxGame and its start level. BrickBreaker.cpp has the rules;
 * SandboxGame.cpp's module registers the level.
 */
namespace BrickBreaker
{
    /* The level, owned by BrickBreaker.cpp; lives as long as the module. */
    NimbleEngine::ILevel& GetLevel();
}

#endif //SANDBOX_BRICKBREAKER_H
