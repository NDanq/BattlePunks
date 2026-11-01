#ifndef DEMOLAB_LABTYPES_H
#define DEMOLAB_LABTYPES_H

#include <cstdint>

/* Physics layers (bits). Level geometry is LAYER_WORLD; ragdoll hit boxes sit
 * on LAYER_BODIES, so the game's line-of-sight rays (the bots' and the
 * AutoPlay driver's, WORLD_MASK) see walls but not the bodies they look at.
 * Shots see both. (The Character Mover ignores its own bodies by itself.) */
#define LAB_LAYER_WORLD  (1)
#define LAB_LAYER_BODIES (2)
#define LAB_WORLD_MASK   (LAB_LAYER_WORLD)
#define LAB_SHOT_MASK    (-1)

/*
 * The enums shared by every DemoLab system (TDD §2). Components store
 * them as uint8_t, because the editor's Choice properties and replication take
 * plain integers; cast at the use.
 */

namespace LabWeapons {
    enum class EFireMode : uint8_t {
        EFM_Single = 0x00,
        EFM_Burst,
        EFM_Auto,
        EFM_Combo,       // melee: presses chain up to ComboLength swings
        EFM_MAX
    };

    enum class EWeaponType : uint8_t {
        EWT_Melee = 0x00,
        EWT_HitScan,
        EWT_Projectile,  // not in the MVP: fires as hitscan
        EWT_Throwable,   // not in the MVP
        EWT_MAX
    };

    /* = the WeaponSet anim param (TDD §5.7). */
    enum class EAnimSet : uint8_t {
        EAS_UnArmed = 0x00,
        EAS_OneHanded,
        EAS_TwoHanded,
        EAS_Rifle,
        EAS_Heavy,
        EAS_Throwable,
        EAS_MAX
    };

    /* Inventory slots; an id of 0 is an empty slot. */
    constexpr int     SLOTS    = 3;
    constexpr uint8_t UNARMED  = 0xFF;
    constexpr uint8_t NO_EQUIP = 0xFF;
}

enum class EDamageType : uint8_t {
    EDT_Bullet = 0,
    EDT_Melee,
    EDT_Explosion,
    EDT_Heal,
    EDT_Fall
};

/* Hit parts reported by a damage event. */
enum class EHitPart : uint8_t {
    EHP_Body = 0,
    EHP_Head
};

/* LabTeamComponent::Team (TDD §6.1). The same number goes to the AI stimulus. */
namespace LabTeams {
    constexpr uint8_t NONE    = 0;     // nobody's enemy (props)
    constexpr uint8_t PLAYERS = 1;
    constexpr uint8_t ENEMIES = 2;
    constexpr uint8_t FFA_BASE = 100;  // arena FFA: 100 + PlayerIndex
}

#endif //DEMOLAB_LABTYPES_H
