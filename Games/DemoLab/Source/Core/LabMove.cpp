#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/Input.h"

#include "Core/LabHealth.h"
#include "Weapons/LabCombat.h"

using namespace GameCore;
using namespace NimbleEngine;

/*
 * LabMove (TDD §9.1 rule 5, Implementation Guide §7.3): the engine's
 * CharacterMove with the character's penalties on top. A penalty lives in the
 * simulation, not the input filter, because the server never runs a remote
 * client's filter -- and it reads only replicated state, so a predicting
 * client replays it exactly:
 *
 *   LabCombat.StaggerFrames > 0   no moving, jumping or running; one frame used up per step
 *   LabHealth.bDown               a crawl: 30% of the move, no jump or run
 *
 * The character's harness names it (Simulation "LabMove", LabPlayer.cpp).
 */
NIMBLE_DEFINE_AVATAR_SIMULATION(LabMove, 0.1f)
{
    Entity Avatar = InWorld.GetEntity(InAvatar);
    const AvatarHarnessComponent* Harness = Avatar.GetComponent<AvatarHarnessComponent>();
    const CharacterMoverComponent* Mover = Avatar.GetComponent<CharacterMoverComponent>();
    LabCombatComponent* Combat = Avatar.GetComponent<LabCombatComponent>();
    const LabHealthComponent* Health = Avatar.GetComponent<LabHealthComponent>();
    if(!Harness || !Mover)
    {
        StepCharacter(InWorld, InAvatar, InFrame);
        return;
    }

    const bool bStaggered = Combat && Combat->StaggerFrames > 0;
    const bool bDowned = Health && Health->bDown;
    if(!bStaggered && !bDowned)
    {
        StepCharacter(InWorld, InAvatar, InFrame);
        return;
    }

    AvatarInputFrame Frame = InFrame;
    const float MoveScale = bStaggered ? 0.f : 0.3f;
    for(const char* Axis : { Mover->MoveForward, Mover->MoveRight })
    {
        const int Index = FindInputAxis(Harness->InputMap, Axis);
        if(Index >= 0 && Index < int(INPUT_AXES_MAX)) { Frame.Axes[Index] *= MoveScale; }
    }
    for(const char* Action : { Mover->JumpAction, Mover->RunAction })
    {
        const int Index = FindInputAction(Harness->InputMap, Action);
        if(Index < 0) { continue; }
        Frame.Down     &= ~(1u << Index);
        Frame.Pressed  &= ~(1u << Index);
        Frame.Released &= ~(1u << Index);
    }
    if(bStaggered) { --Combat->StaggerFrames; }

    StepCharacter(InWorld, InAvatar, Frame);
}
