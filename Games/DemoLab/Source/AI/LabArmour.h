#ifndef DEMOLAB_LABARMOUR_H
#define DEMOLAB_LABARMOUR_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

#include <cstdint>

namespace GameCore { class World; }
struct LabDamageEvent;

/*
 * The Brute's riveted armour (Docs/Design.md, Cast): eight plates
 * (Content/Characters/SteamDummy/Armour, Tools/Art/assets.json) worn on its
 * bones -- chest, back, pauldrons, thigh guards, greaves. Each is a child
 * entity "LabPlate_<Name>" with a Render and an Anim Socket on its bone.
 *
 * A hit that lands on a plate (the hit point within the plate's reach of its
 * centre) is blunted: the Brute takes ARMOURED_SHARE of it and the plate
 * takes the whole of it. A plate at 0 breaks off -- it leaves its socket and
 * falls as a physics body (a Dynamic Rigid Body on LAB_LAYER_DEBRIS, which
 * collides with the level only), with a clang and sparks, and is gone after a
 * few seconds. Where it was, the Brute takes full damage. A Brute that falls
 * sheds every plate it still has. Head shots are never blunted.
 *
 * Authority only (the story is single player): plates and debris are not
 * replicated, and never saved -- a checkpoint drops them (ClearTransients).
 */

constexpr int LAB_ARMOUR_PLATES = 8;

/* On an armoured Brute. Not saved (a restored Brute is unarmoured). */
struct LabArmourComponent {
    GAMECORE_COMPONENT_BODY()
    GameCore::EntityID Plates[LAB_ARMOUR_PLATES];    // unset: broken off (or never worn)
    float   PlateHealth[LAB_ARMOUR_PLATES] = {};
    uint8_t Broken = 0;                              // plates broken off so far
};

/* A broken-off plate, falling: destroyed when Seconds runs out. */
struct LabDebrisComponent {
    GAMECORE_COMPONENT_BODY()
    float Seconds = 8.f;
};

namespace LabArmour {
    /* Share of a hit an intact plate lets through. */
    constexpr float ARMOURED_SHARE = 0.3f;
    /* A plate's own health, in hit points of the hits it takes. */
    constexpr float PLATE_HEALTH = 40.f;

    /* Authority: the eight plates on InBrute's bones, and its LabArmour. */
    void Equip(GameCore::World& InWorld, GameCore::EntityID InBrute);

    /* Authority, from LabDamage::Apply: the share of InAmount that gets
     * through InEvent.Victim's armour (all of it for the unarmoured, a head
     * shot, a heal, a fall); chips and breaks the plate struck. */
    float Absorb(GameCore::World& InWorld, const LabDamageEvent& InEvent, float InAmount);

    /* Authority: every plate InBrute still wears breaks off (it fell). */
    void ShedAll(GameCore::World& InWorld, GameCore::EntityID InBrute, const EngineCore::Vector3& InPush);

    /* Intact plates on InBrute; 0 for anyone unarmoured. */
    int PlatesLeft(GameCore::World& InWorld, GameCore::EntityID InBrute);

    /* Each frame: falling plates expire. */
    void Tick(GameCore::World& InWorld, float InSeconds);

    /* Whether InEntity is a plate or a piece of debris (a checkpoint load drops them). */
    bool IsTransient(GameCore::World& InWorld, GameCore::EntityID InEntity);
}

#endif //DEMOLAB_LABARMOUR_H
