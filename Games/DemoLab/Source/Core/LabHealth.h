#ifndef DEMOLAB_LABHEALTH_H
#define DEMOLAB_LABHEALTH_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/ComponentRpc.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

#include "Core/LabTypes.h"

/*
 * Health, teams and the damage pipeline (TDD §6). Every change happens on the
 * authority; clients see the replicated fields, and the presentation system
 * (LabCombat.cpp) turns bDead into a ragdoll on every process.
 */

struct LabDamageEvent {
    GameCore::EntityID  Victim;
    GameCore::EntityID  Instigator;
    uint8_t             WeaponID   = 0;
    float               Amount     = 0.f;
    EDamageType         DamageType = EDamageType::EDT_Bullet;
    EngineCore::Vector3 Point;
    EngineCore::Vector3 Dir;
    float               Impulse    = 0.f;
    EHitPart            Part       = EHitPart::EHP_Body;
    uint8_t             bStaggers  = 0;
};

namespace LabDamage {
    /* Authority only. Runs the mode's rules, takes the health, kills or downs
     * the victim, and emits "Lab.Damaged" (victim, instigator, amount, part).
     * Returns the health actually taken (or healed). */
    float Apply(GameCore::World& InWorld, const LabDamageEvent& InEvent);

    /* Authority only: dead now, whatever the health. */
    void Kill(GameCore::World& InWorld, GameCore::EntityID InVictim, const LabDamageEvent& InEvent);

    /* Authority only: back to full health, alive; the ragdoll resets on every
     * process from the replicated bDead. */
    void Revive(GameCore::World& InWorld, GameCore::EntityID InVictim);
}

struct LabHealthComponent {
    GAMECORE_COMPONENT_BODY()
    float   MaxHealth = 100.f;
    float   Health = 100.f;
    uint8_t bDead = 0;
    uint8_t bDown = 0;
    GameCore::EntityID LastInstigator;
    float   DownedTimer = 0.0f;        // server: seconds left before a downed character bleeds out
    float   ReviveProgress = 0.0f;     // 0..1, replicated for the HUD

    /* Every process: the ragdoll, from the killing shot. */
    void MulticastKilled(const GameCore::RpcContext& InContext, const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt);
};

struct LabTeamComponent {
    GAMECORE_COMPONENT_BODY()
    uint8_t Team = LabTeams::NONE;
};

namespace LabTeams {
    uint8_t TeamOf(GameCore::World& InWorld, GameCore::EntityID InEntity);
    /* Team NONE is nobody's enemy; equal teams are friends. */
    bool    AreHostile(uint8_t InA, uint8_t InB);
}

#endif //DEMOLAB_LABHEALTH_H
