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

/* The SteamDummy: a steampunk mesh skinned to the CrashDummy's rig (same 74
 * bones), so it plays the CrashDummy's clips. Its anim state is the engine's
 * CrashDummy one but for the OffHand IK, weighted by TwoHand rather than
 * Armed: a pistol or a wrench is held in one hand, the off arm left as
 * animated, instead of reaching across the body for it (LabCombat sets
 * TwoHand from the weapon's AnimSet). Imported from BattlePunks (Dev-BattlePunks fad1c7d6);
 * SteamDummy_PBR is the same skin and skeleton with the art pass's UV atlas
 * (Tools/Art). Every character's look is its material in slot 0 -- the
 * player's, the Watchman's (Clear Coat, red eyes) or the Brute's
 * (Anisotropic) -- each keeping the CRT eyes' EyeColor, EyeIntensity and
 * BlinkInterval parameters. */
#define LAB_DEFAULT_MESH      "Content/Characters/SteamDummy/SteamDummy_PBR.nmmesh"
#define LAB_DEFAULT_ANIMSTATE "Content/Characters/SteamDummy/SteamDummy.nmanimstate"
#define LAB_DEFAULT_MATERIAL  "Content/Materials/Characters/SteamDummy_Player.material.nmgraph"
#define LAB_WATCHMAN_MATERIAL "Content/Materials/Characters/SteamDummy_Watchman.material.nmgraph"
#define LAB_BRUTE_MATERIAL    "Content/Materials/Characters/SteamDummy_Brute.material.nmgraph"


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
