/**
* VIBECODE CERTIFIED
*/
#ifndef DEMOLAB_LABPLAYER_H
#define DEMOLAB_LABPLAYER_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"

#include "Core/LabHealth.h"
#include "Core/LabTypes.h"

namespace GameCore { class World; }

/*
 * The LabPlayer (TDD §3): the native prefab "native:DemoLab.Player"
 * is the base; characters are its forks (Content Browser > New Native
 * Prefab...) that change the mesh, the anim state and this component.
 */

/* The SteamDummy: a steamcharacter mesh skinned to the CrashDummy's rig (same 74
 * bones, TDD §5.8 option 1), so it plays the CrashDummy's clips and anim state.
 * Imported from BattlePunks (Dev-BattlePunks fad1c7d6); the .glb reimports.
 * The anim state is the engine's CrashDummy one, used unchanged. */
#define LAB_DEFAULT_MESH      "Content/Characters/SteamDummy/SteamDummy.nmmesh"
#define LAB_DEFAULT_ANIMSTATE "Engine/Content/Characters/CrashDummy/CrashDummy.nmanimstate"
/* Its one section's material: vertex colours, and emissive VCR eyes masked by
 * vertex alpha that blink every BlinkInterval s (per entity: Render > Material
 * Parameters, slot 0). */
#define LAB_DEFAULT_MATERIAL  "Content/Materials/Characters/SteamDummy.material.nmgraph"


struct LabCharacterComponent {
    GAMECORE_COMPONENT_BODY()
    char    DisplayName[32] = "Player";
    float   MaxHealth = 100.f;
    float   SpeedScale = 1.0f;          // multiplies the Character Mover's speeds at spawn
    float   MeleeDamageScale = 1.0f;
    uint8_t DefaultWeaponIDs[LabWeapons::SLOTS] = {0, 0, 0};   // registry ids (LabWeapon.cpp); 0 = empty
    uint8_t DefaultThrowable = 0;
    uint8_t DefaultThrowableCount = 0;
    uint8_t Team = LabTeams::PLAYERS;
};

/* Authority: adds whatever a character needs that a fork made before it was added
 * to the base is missing (TDD §3.1), with default values. */
void LabEnsureLayout(GameCore::World& InWorld, GameCore::EntityID InAvatar);

/* Authority: health from the character, speed scale on the mover (once),
 * team, and the default loadout. Spawn and respawn both call it. */
void LabApplyCharacter(GameCore::World& InWorld, GameCore::EntityID InAvatar, bool bInFirstSpawn);

#endif //DEMOLAB_LABPLAYER_H
