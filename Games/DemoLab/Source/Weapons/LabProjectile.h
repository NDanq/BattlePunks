#ifndef DEMOLAB_LABPROJECTILE_H
#define DEMOLAB_LABPROJECTILE_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

#include "Weapons/LabWeapon.h"

namespace GameCore { class World; }

/*
 * Projectiles and throwables (TDD §5.6). A projectile is a fork of
 * "native:DemoLab.Projectile" (Content/Prefabs/Projectiles/): a networked
 * prefab the authority spawns and steps. Clients see its replicated
 * Transform and carry it on between updates with the replicated Velocity.
 *
 * The prefab authors the flight (gravity, bounces, fuse, radius, effects);
 * the weapon that fires it fills in the damage at spawn.
 */
struct LabProjectileComponent {
    GAMECORE_COMPONENT_BODY()

    // Authored on the prefab.
    float   Gravity = 0.f;           // units/s² down
    float   Drag = 0.f;              // 1/s
    float   Radius = 0.f;            // splash; 0: the direct hit only
    float   Fuse = -1.f;             // seconds to burst; -1: on impact (after its bounces)
    uint8_t Bounces = 0;             // impacts it bounces off before an impact bursts it
    float   Restitution = 0.4f;
    float   Life = 5.f;              // gone (bursting if it has a Radius) after this
    float   SpinRate = 0.f;          // degrees/s it tumbles, for the look
    char    BurstFx[MAX_CHAR_PATH] = "";
    char    BurstSound[MAX_CHAR_PATH] = "";

    // Set at spawn by the weapon.
    GameCore::EntityID Instigator;
    uint8_t WeaponID = 0;
    float   Damage = 0.f;
    float   Impulse = 0.f;
    uint8_t bStaggers = 0;
    EngineCore::Vector3 Velocity;    // replicated: clients extrapolate with it

    // Authority, this run.
    float   Age = 0.f;
    uint8_t bResting = 0;
};

namespace LabProjectiles {
    /* Authority: spawns InWeapon's projectile at InOrigin flying InDir at InSpeed. */
    GameCore::EntityID Launch(GameCore::World& InWorld, GameCore::EntityID InInstigator, uint8_t InWeaponID,
                              const LabWeaponComponent& InWeapon, const EngineCore::Vector3& InOrigin,
                              const EngineCore::Vector3& InDir, float InSpeed);

    /* Authority: flight, impacts, fuses, bursts. */
    void TickServer(GameCore::World& InWorld, float InSeconds);
    /* Every other process: carries projectiles on between updates. */
    void TickClient(GameCore::World& InWorld, float InSeconds);

    /* The arc a throw would fly, for the owner's preview: up to InMax points,
     * InStep seconds apart, stopping at the first hit. */
    int  PredictArc(GameCore::World& InWorld, GameCore::EntityID InIgnore, const EngineCore::Vector3& InOrigin,
                    const EngineCore::Vector3& InVelocity, float InGravity, float InStep, EngineCore::Vector3* OutPoints, int InMax);

    /* The launch velocity of a throw: aim pitched up 8°, 40..100% of the max speed by charge. */
    EngineCore::Vector3 ThrowVelocity(const EngineCore::Vector3& InAim, const LabWeaponComponent& InWeapon, float InCharge);
}

#endif //DEMOLAB_LABPROJECTILE_H
