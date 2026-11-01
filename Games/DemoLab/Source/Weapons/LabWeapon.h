#ifndef DEMOLAB_LABWEAPON_H
#define DEMOLAB_LABWEAPON_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

#include "Core/LabTypes.h"

namespace GameCore { class World; }

/*
 * Weapons (TDD §5.1-5.2). A weapon is a fork of the native prefab
 * "native:DemoLab.Weapon" in Content/Prefabs/Weapons/, holding one
 * LabWeaponComponent on its root. It has no Net component: every process
 * spawns its own copy from the replicated inventory (LabCombat.cpp,
 * presentation), and the stats come from the registry below.
 */

#define MAX_CHAR_PATH (96)
#define MAX_BONE_NAME (32)

struct LabWeaponComponent {
    GAMECORE_COMPONENT_BODY()

    /* LabWeapons::EWeaponType / EFireMode / EAnimSet, as uint8_t so the
     * editor's Choice properties take them. */
    uint8_t WeaponType = uint8_t(LabWeapons::EWeaponType::EWT_HitScan);
    uint8_t FireMode   = uint8_t(LabWeapons::EFireMode::EFM_Single);
    uint8_t AnimSet    = uint8_t(LabWeapons::EAnimSet::EAS_OneHanded);
    char    DisplayName[32] = "Weapon";

    // Damage
    float   Damage = 20.f;
    float   HeadshotMultiplier = 2.f;
    float   Range = 5000.f;
    float   Falloff = 0.f;           // 0..1: share of damage lost at full Range
    float   Impulse = 300.f;
    uint8_t bStaggers = 0;

    // Cadence (spread in degrees)
    float   FireInterval = 0.25f;
    uint8_t BurstCount = 3;
    uint8_t Pellets = 1;
    float   SpreadMin = 0.5f;
    float   SpreadMax = 4.f;
    float   SpreadPerShot = 0.6f;
    float   SpreadRecovery = 6.f;
    int32_t MagSize = 8;             // 0: no ammo (melee)
    int32_t ReserveMax = 64;
    float   ReloadSeconds = 1.6f;

    // Melee (seconds from the press; server-timed, not anim-timed)
    float   Windup = 0.15f;
    float   Active = 0.12f;
    float   Recovery = 0.3f;
    float   Reach = 160.f;
    float   ArcDegrees = 90.f;
    uint8_t ComboLength = 1;

    // Projectile and throwable (TDD §5.6)
    char    ProjectilePrefab[MAX_CHAR_PATH] = "";   // a fork of native:DemoLab.Projectile
    float   LaunchSpeed = 2500.f;                   // a projectile weapon's muzzle speed
    float   ThrowSpeedMax = 1600.f;                 // a throwable at full charge (40% at none)

    // Equip: the Anim Socket seats on the character's skeleton
    char    HandBone[MAX_BONE_NAME] = "DEF-hand.R";
    EngineCore::Vector3 HandPos;
    EngineCore::Vector3 HandRot;
    char    HolsterBone[MAX_BONE_NAME] = "DEF-spine";
    EngineCore::Vector3 HolsterPos;
    EngineCore::Vector3 HolsterRot;
    float   EquipSeconds = 0.4f;

    // Presentation
    char    FireSound[MAX_CHAR_PATH] = "";
    char    ImpactFx[MAX_CHAR_PATH] = "";
};

/* The registry ids Demo Lab uses (LabWeapon.cpp). */
#define LAB_WEAPON_WRENCH      (1)
#define LAB_WEAPON_PIPE_PISTOL (2)

namespace LabWeapons {
    struct Entry { uint8_t ID; const char* Prefab; };

    /* nullptr for 0 or an unknown id. Ids are forever: append only. */
    const Entry* Find(uint8_t InID);
    /* 0 if the path is not a registered weapon. */
    uint8_t      FindByPrefab(const char* InPrefab);

    /* The weapon's stats, read once per World from a hidden definition
     * instance, then copied: safe to keep for the frame, not across
     * spawns. nullptr for an unknown id. Every process. */
    const LabWeaponComponent* GetData(GameCore::World& InWorld, uint8_t InID);

    /* Forget the cached stats (a mode's OnLoad, so prefab edits show). */
    void ResetCache();

    inline EWeaponType TypeOf(const LabWeaponComponent& InWeapon) { return EWeaponType(InWeapon.WeaponType); }
    inline EFireMode   ModeOf(const LabWeaponComponent& InWeapon) { return EFireMode(InWeapon.FireMode); }
}

#endif //DEMOLAB_LABWEAPON_H
