#ifndef LIGHTLAB_LIGHTLABPHYSICS_H
#define LIGHTLAB_LIGHTLABPHYSICS_H

#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; }
namespace NimbleEngine { class ILevel; }

/*
 * PhysicsLab: LightLab's physics demo (LightLabPhysics.cpp), a level of its
 * own -- Content/Levels/PhysicsLab.nmworld, or [Game] StartLevel=PhysicsLab,
 * or open that world in the editor and Play. Every body in it is a GameCore
 * Rigid Body simulated by the engine's JoltPhysics module, which LightLab's
 * CMakeLists.txt opts in to.
 */
namespace LightLabPhysics
{
    /* The level, owned by this file; LightLabGameModule registers it. */
    NimbleEngine::ILevel& GetLevel();
}

/* LightLabGame.cpp's free flight, which every LightLab level shares: the
 * Player's Input component steers its Transform. Start registers
 * "LightLab.Fly" in TP_PrePhysics, starting from the Player's placed
 * rotation; InContext must outlive the level's World. */
struct LightLabFlyContext
{
    GameCore::EntityID Player;
    double             Yaw   = 0.0;
    double             Pitch = 0.0;
};

void LightLabStartFly(GameCore::World& InWorld, GameCore::EntityID InPlayer, LightLabFlyContext& InContext);

#endif //LIGHTLAB_LIGHTLABPHYSICS_H
