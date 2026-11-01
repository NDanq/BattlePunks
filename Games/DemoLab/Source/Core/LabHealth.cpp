#include "Core/LabHealth.h"

#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/EventBus.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/GameMode.h"
#include "Public/NimbleEngine/LagCompensation.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/Ragdoll.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "Weapons/LabCombat.h"
#include "GameModes/LabGameModeBase.h"

#include <algorithm>

#include "LabVersion.h"

using namespace GameCore;
using namespace NimbleEngine;

// === Damage =================================================================

float LabDamage::Apply(World& InWorld, const LabDamageEvent& InEvent)
{
    if(!HasAuthority()) { return 0.f; }

    Entity Victim = InWorld.GetEntity(InEvent.Victim);
    LabHealthComponent* Health = Victim.IsAlive() ? Victim.GetComponent<LabHealthComponent>() : nullptr;
    if(!Health || Health->bDead) { return 0.f; }

    const bool bHeal = InEvent.DamageType == EDamageType::EDT_Heal;
    LabGameModeBase* Mode = FindGameMode<LabGameModeBase>(InWorld);

    /* Team rule: friends only hurt each other with friendly fire on. */
    if(!bHeal && InEvent.Instigator.IsSet() && InEvent.Instigator != InEvent.Victim)
    {
        const uint8_t VictimTeam = LabTeams::TeamOf(InWorld, InEvent.Victim);
        const uint8_t ShooterTeam = LabTeams::TeamOf(InWorld, InEvent.Instigator);
        if(VictimTeam != LabTeams::NONE && !LabTeams::AreHostile(VictimTeam, ShooterTeam) && !(Mode && Mode->FriendlyFire()))
        {
            return 0.f;
        }
    }

    float Amount = InEvent.Amount;
    if(Mode) { Amount = Mode->ModifyDamage(InEvent, Amount); }
    if(Amount <= 0.f) { return 0.f; }

    /* The mode's hook may have spawned or destroyed; fetch the component again. */
    Health = InWorld.GetEntity(InEvent.Victim).GetComponent<LabHealthComponent>();
    if(!Health) { return 0.f; }

    const float OldHealth = Health->Health;
    if(bHeal)
    {
        if(Health->bDown) { return 0.f; }
        Health->Health = std::min(Health->MaxHealth, Health->Health + Amount);
        return Health->Health - OldHealth;
    }

    Health->Health = std::max(0.f, OldHealth - Amount);
    Health->LastInstigator = InEvent.Instigator;
    const float Taken = OldHealth - Health->Health;

    if(LabCombatComponent* Combat = InWorld.GetEntity(InEvent.Victim).GetComponent<LabCombatComponent>())
    {
        ++Combat->HitSeq;
        Combat->LastHitDir   = InEvent.Dir;
        Combat->LastHitPart  = uint8_t(InEvent.Part);
        Combat->LastHitHeavy = InEvent.bStaggers;
        /* A heavy hit stops the character (LabMove): about half a second of input. */
        if(InEvent.bStaggers) { Combat->StaggerFrames = 30; }
    }

    EventArgs Args;
    Args.Args[Args.Count++] = EventArg::Make(InEvent.Victim);
    Args.Args[Args.Count++] = EventArg::Make(InEvent.Instigator);
    Args.Args[Args.Count++] = EventArg::Make(double(Taken));
    Args.Args[Args.Count++] = EventArg::Make(InEvent.Part == EHitPart::EHP_Head ? "Head" : "Body");
    InWorld.GetEvents().Emit("Lab.Damaged", Args);

    Health = InWorld.GetEntity(InEvent.Victim).GetComponent<LabHealthComponent>();
    if(Health && Health->Health <= 0.f && !Health->bDead)
    {
        if(Mode && Mode->UsesDowned() && !Health->bDown)
        {
            Health->bDown = 1;
            Health->DownedTimer = 20.f;
            Health->ReviveProgress = 0.f;
            Mode->OnLabDowned(InEvent.Victim);
        }
        else
        {
            Kill(InWorld, InEvent.Victim, InEvent);
        }
    }
    return Taken;
}

void LabDamage::Kill(World& InWorld, EntityID InVictim, const LabDamageEvent& InEvent)
{
    if(!HasAuthority()) { return; }

    Entity Victim = InWorld.GetEntity(InVictim);
    if(!Victim.IsAlive()) { return; }

    LabHealthComponent* Health = Victim.GetComponent<LabHealthComponent>();
    if(!Health || Health->bDead) { return; }

    Health->bDead  = 1;
    Health->bDown  = 0;
    Health->Health = 0.f;
    Health->LastInstigator = InEvent.Instigator;

    if(AvatarHarnessComponent* Harness = Victim.GetComponent<AvatarHarnessComponent>()) { Harness->bEnabled = 0; }
    if(LagCompensatedComponent* Capsule = Victim.GetComponent<LagCompensatedComponent>()) { Capsule->bEnabled = 0; }

    EngineCore::Vector3 Dir = InEvent.Dir;
    if(Dir.GetLength() < 1e-4) { Dir = EngineCore::Vector3(0, 0, 1); }
    GAMECORE_RPC(LabHealthComponent, MulticastKilled)(Victim, Dir.GetNormalize() * double(InEvent.Impulse), InEvent.Point);

    EventArgs Args;
    Args.Args[Args.Count++] = EventArg::Make(InVictim);
    Args.Args[Args.Count++] = EventArg::Make(InEvent.Instigator);
    Args.Args[Args.Count++] = EventArg::Make(double(InEvent.WeaponID));
    InWorld.GetEvents().Emit("Lab.Killed", Args);

    if(LabGameModeBase* Mode = FindGameMode<LabGameModeBase>(InWorld))
    {
        Mode->OnLabKilled(InVictim, InEvent.Instigator, InEvent.WeaponID);
    }
}

void LabDamage::Revive(World& InWorld, EntityID InVictim)
{
    if(!HasAuthority()) { return; }

    Entity Victim = InWorld.GetEntity(InVictim);
    LabHealthComponent* Health = Victim.IsAlive() ? Victim.GetComponent<LabHealthComponent>() : nullptr;
    if(!Health) { return; }

    Health->bDead = 0;
    Health->bDown = 0;
    Health->Health = Health->MaxHealth;
    Health->DownedTimer = 0.f;
    Health->ReviveProgress = 0.f;
    Health->LastInstigator = EntityID();

    if(AvatarHarnessComponent* Harness = Victim.GetComponent<AvatarHarnessComponent>()) { Harness->bEnabled = 1; }
    if(LagCompensatedComponent* Capsule = Victim.GetComponent<LagCompensatedComponent>()) { Capsule->bEnabled = 1; }
}

// === Health =================================================================

void LabHealthComponent::MulticastKilled(const RpcContext& InContext, const EngineCore::Vector3& InImpulse, const EngineCore::Vector3& InAt)
{
    if(!InContext.OwningWorld) { return; }
    if(RagdollComponent* Ragdoll = InContext.OwningWorld->GetEntity(InContext.Entity).GetComponent<RagdollComponent>())
    {
        if(!Ragdoll->IsActive()) { Ragdoll->ActivateWithImpulse(InImpulse, InAt); }
    }
}

GAMECORE_DEFINE_COMPONENT(LabHealthComponent, "LabHealthComponent")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabHealthComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("MaxHealth", C.MaxHealth);
    Ar.Field("Health", C.Health);
    float DownedCoolOff = 0.f;
    Ar.Field("DownedCoolOff", DownedCoolOff);
    Ar.Flag("IsDead", C.bDead);
    Ar.Flag("IsDown", C.bDown);

    Ar.Entity("LastInstigator", C.LastInstigator);
    Ar.Field("DownedTimer", C.DownedTimer);
    Ar.Field("ReviveProgress", C.ReviveProgress);
}

#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabHealthComponent) {
    Rep.Field("MaxHealth", &LabHealthComponent::MaxHealth);
    Rep.Field("Health", &LabHealthComponent::Health).Precision(0.1f);
    Rep.Flag("bDead", &LabHealthComponent::bDead);
    Rep.Flag("bDown", &LabHealthComponent::bDown);
    Rep.Entity("LastInstigator", &LabHealthComponent::LastInstigator);
    Rep.Field("ReviveProgress", &LabHealthComponent::ReviveProgress).Precision(0.01f);
}
#endif

GAMECORE_DEFINE_COMPONENT_RPCS(LabHealthComponent) {
    Rpc.Multicast<&LabHealthComponent::MulticastKilled>("MulticastKilled");   // reliable: the ragdoll
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabHealthComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("MaxHealth", &LabHealthComponent::MaxHealth);
    Props.Field("Health", &LabHealthComponent::Health);
    Props.Flag("Dead", &LabHealthComponent::bDead, PF_ReadOnly);
    Props.Flag("Down", &LabHealthComponent::bDown, PF_ReadOnly);
}
#endif

// === Team ===================================================================

GAMECORE_DEFINE_COMPONENT(LabTeamComponent, "LabTeamComponent")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabTeamComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Team", C.Team);
}

#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabTeamComponent) {
    Rep.Field("Team", &LabTeamComponent::Team);
}
#endif

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabTeamComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Team", &LabTeamComponent::Team);
}
#endif

uint8_t LabTeams::TeamOf(World& InWorld, EntityID InEntity)
{
    const Entity E = InWorld.GetEntity(InEntity);
    const LabTeamComponent* Team = E.IsAlive() ? E.GetComponent<LabTeamComponent>() : nullptr;
    return Team ? Team->Team : NONE;
}

bool LabTeams::AreHostile(const uint8_t InA, const uint8_t InB)
{
    if(InA == NONE || InB == NONE) { return false; }
    return InA != InB;
}
