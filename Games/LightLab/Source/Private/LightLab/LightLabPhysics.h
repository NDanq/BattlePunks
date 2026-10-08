#ifndef LIGHTLAB_LIGHTLABPHYSICS_H
#define LIGHTLAB_LIGHTLABPHYSICS_H

#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; }

/*
 * PhysicsLab: LightLab's physics demo (LightLabPhysics.cpp), the mode of its
 * own world -- [World_PhysicsLab], Content/Levels/PhysicsLab.nmworld: [Game]
 * StartLevel=PhysicsLab, or open that world in the editor and Play. Every
 * body in it is a GameCore Rigid Body simulated by the engine's JoltPhysics
 * module, which LightLab's [Modules] opts in to.
 */
namespace LightLabPhysics
{
    /* As "PhysicsLab" (WorldList.h); from LightLabGameModule. */
    void RegisterGameMode();
    void UnregisterGameMode();
}

/* LightLabGame.cpp's free flight, which every LightLab level shares: the
 * Player's Input component steers its Transform. Start registers
 * "LightLab.Fly" in TP_PrePhysics, starting from the Player's placed
 * rotation; InContext must outlive the World. */
struct LightLabFlyContext
{
    GameCore::EntityID Player;
    double             Yaw   = 0.0;
    double             Pitch = 0.0;
    bool               bEnabled = true;   // off: the Player is left alone (PhysicsLab's driving)
};

void LightLabStartFly(GameCore::World& InWorld, GameCore::EntityID InPlayer, LightLabFlyContext& InContext);

#endif //LIGHTLAB_LIGHTLABPHYSICS_H
