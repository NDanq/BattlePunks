#ifndef SANDBOX_BRICKBREAKER_H
#define SANDBOX_BRICKBREAKER_H

namespace NimbleEngine
{
    class ILevel;
}

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
