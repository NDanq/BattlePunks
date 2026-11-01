#include "Weapons/LabCombat.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AIFacade.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/Destruction.h"
#include "Public/NimbleEngine/LagCompensation.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/Ragdoll.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/AnimInstance.h"
#include "Public/RenderCore/AnimationSystem.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/ParticleSystemComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "AI/LabArmour.h"
#include "Core/LabHealth.h"
#include "Core/LabPlayer.h"
#include "Weapons/LabProjectile.h"
#include "Weapons/LabWeapon.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "LabVersion.h"

using namespace GameCore;
using namespace NimbleEngine;
using EngineCore::Vector3;

namespace
{
    constexpr float  MAX_ORIGIN_SLOP = 350.f;   // a client's shot origin may sit this far from its boom pivot
    constexpr double HEAD_FROM       = 150.0;   // capsule height above the feet that counts as the head
    constexpr float  PI_F            = 3.14159265f;

    const char* const FX_MUZZLE = "Content/Particles/MuzzleFlash.nmparticle";
    const char* const FX_DUST   = "Content/Particles/Dust.nmparticle";
    const char* const FX_SPARKS = "Content/Particles/Sparks.nmparticle";
    const char* const SND_SHOT  = "Content/Sounds/Weapons/Pistol_Shot.nmsound";
    const char* const SND_HIT   = "Content/Sounds/Robots/Brute_Hit.nmsound";
    const char* const SND_DRY   = "Content/Sounds/Weapons/Pistol_DryFire.nmsound";
    const char* const SND_RELOAD = "Content/Sounds/Weapons/Pistol_Reload.nmsound";
    const char* const SND_SWING = "Content/Sounds/Robots/Brute_Swing_01.nmsound";

    /* A punch when nothing is in hand, or the Melee action with a gun. */
    LabWeaponComponent MakeFists()
    {
        LabWeaponComponent Fists;
        Fists.WeaponType  = uint8_t(LabWeapons::EWeaponType::EWT_Melee);
        Fists.FireMode    = uint8_t(LabWeapons::EFireMode::EFM_Combo);
        Fists.AnimSet     = uint8_t(LabWeapons::EAnimSet::EAS_UnArmed);
        std::snprintf(Fists.DisplayName, sizeof(Fists.DisplayName), "%s", "Fists");
        Fists.Damage      = 15.f;
        Fists.Impulse     = 150.f;
        Fists.MagSize     = 0;
        Fists.Windup      = 0.1f;
        Fists.Active      = 0.1f;
        Fists.Recovery    = 0.25f;
        Fists.Reach       = 140.f;
        Fists.ArcDegrees  = 80.f;
        Fists.ComboLength = 2;
        return Fists;
    }
    const LabWeaponComponent GFists = MakeFists();

    bool IsUp(World& InWorld, const EntityID InAvatar)
    {
        const Entity E = InWorld.GetEntity(InAvatar);
        const LabHealthComponent* Health = E.IsAlive() ? E.GetComponent<LabHealthComponent>() : nullptr;
        return Health && !Health->bDead && !Health->bDown;
    }

    /* The character that owns a hit entity: itself, or the nearest ancestor with health. */
    EntityID HealthOwner(World& InWorld, EntityID InEntity)
    {
        for(int Depth = 0; InEntity.IsSet() && InWorld.IsAlive(InEntity) && Depth < 8; ++Depth)
        {
            const Entity E = InWorld.GetEntity(InEntity);
            if(E.GetComponent<LabHealthComponent>()) { return InEntity; }
            InEntity = E.GetParent();
        }
        return EntityID();
    }

    EntityID FindChild(World& InWorld, const EntityID InParent, const char* InName)
    {
        for(EntityID C = InWorld.GetEntity(InParent).GetFirstChild(); C.IsSet(); C = InWorld.GetEntity(C).GetNextSibling())
        {
            if(std::strcmp(InWorld.GetEntity(C).GetName(), InName) == 0) { return C; }
        }
        return EntityID();
    }

    /* The weapon to use for a melee swing: the equipped melee weapon, else fists. */
    const LabWeaponComponent& MeleeData(World& InWorld, const EntityID InAvatar)
    {
        const LabWeaponComponent* Held = LabCombat::EquippedData(InWorld, InAvatar);
        if(Held && LabWeapons::TypeOf(*Held) == LabWeapons::EWeaponType::EWT_Melee) { return *Held; }
        return GFists;
    }

    // --- Spread -------------------------------------------------------------

    /* A small deterministic PRNG, so a client can draw the same jitter. */
    float Hash01(uint32_t InSeed)
    {
        InSeed ^= InSeed >> 16; InSeed *= 0x7feb352dU;
        InSeed ^= InSeed >> 15; InSeed *= 0x846ca68bU;
        InSeed ^= InSeed >> 16;
        return float(InSeed & 0xFFFFFF) / float(0x1000000);
    }

    Vector3 Jitter(const Vector3& InDir, const float InDegrees, const uint32_t InSeed)
    {
        if(InDegrees <= 0.f) { return InDir; }
        /* Any two axes across the shot. */
        const Vector3 Up = std::fabs(InDir.GetY()) > 0.95 ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
        const Vector3 Right = (InDir ^ Up).GetNormalize();
        const Vector3 Across = (Right ^ InDir).GetNormalize();
        const float Angle = std::sqrt(Hash01(InSeed)) * InDegrees * PI_F / 180.f;
        const float Turn  = Hash01(InSeed * 7919u + 13u) * 2.f * PI_F;
        const Vector3 Out = InDir + Right * double(std::tan(Angle) * std::cos(Turn)) + Across * double(std::tan(Angle) * std::sin(Turn));
        return Out.GetNormalize();
    }

    // --- Effects ------------------------------------------------------------

    /* A transient effect entity: destroyed when Seconds runs out. Local to
     * each process; never saved or replicated. */
    struct LabLifetimeComponent {
        GAMECORE_COMPONENT_BODY()
        float Seconds = 1.f;
    };

    void SpawnFx(World& InWorld, const char* InSystem, const Vector3& InAt, const float InLife)
    {
        if(IsDedicatedServer()) { return; }
        Entity Fx = InWorld.CreateEntity("LabFx");
        TransformComponent Transform;
        Transform.Position = InAt;
        Fx.AddComponent(Transform);
        RenderCore::ParticleSystemComponent Emitter;
        std::snprintf(Emitter.System, sizeof(Emitter.System), "%s", InSystem);
        Fx.AddComponent(Emitter);
        LabLifetimeComponent Life;
        Life.Seconds = InLife;
        Fx.AddComponent(Life);
    }

    void PlayAt(World& InWorld, const char* InSound, const Vector3& InAt, const EntityID InSource)
    {
        if(IsDedicatedServer() || !InSound || !InSound[0]) { return; }
        if(InSource.IsSet() && InSource == GetLocalAvatar(InWorld, 0)) { PlaySound2D(InWorld, InSound, 0.8f); }
        else { PlaySoundAt(InWorld, InSound, InAt, 1.0f); }
    }

    Vector3 MuzzleOf(World& InWorld, const EntityID InAvatar)
    {
        const EntityID Held = LabCombat::HeldInstance(InWorld, InAvatar);
        const EntityID Muzzle = Held.IsSet() ? FindChild(InWorld, Held, "Muzzle") : EntityID();
        if(Muzzle.IsSet()) { return InWorld.GetEntity(Muzzle).GetWorldPose().Position; }
        return InWorld.GetEntity(InAvatar).GetWorldPose().Position + Vector3(0, 140, 0);
    }

    /* The shot's cosmetic half: flash, sound, impact. */
    void ShotFx(World& InWorld, const EntityID InShooter, const Vector3& InImpact, const uint8_t InSurface)
    {
        const LabWeaponComponent* Weapon = LabCombat::EquippedData(InWorld, InShooter);
        const Vector3 Muzzle = MuzzleOf(InWorld, InShooter);
        SpawnFx(InWorld, FX_MUZZLE, Muzzle, 0.6f);
        PlayAt(InWorld, Weapon && Weapon->FireSound[0] ? Weapon->FireSound : SND_SHOT, Muzzle, InShooter);
        /* The impact: grit off the walls, a hollow clank off a robot (variants in turn). */
        static int Variant = 0;
        Variant = (Variant + 1) % 3;
        static const char* const STONE[] = { "Content/Sounds/Impacts/Impact_Stone_01.nmsound", "Content/Sounds/Impacts/Impact_Stone_02.nmsound",
                                             "Content/Sounds/Impacts/Impact_Stone_03.nmsound" };
        static const char* const ROBOT[] = { "Content/Sounds/Impacts/Impact_Robot_01.nmsound", "Content/Sounds/Impacts/Impact_Robot_02.nmsound",
                                             "Content/Sounds/Impacts/Impact_Robot_03.nmsound" };
        if(InSurface == PS_World)
        {
            SpawnFx(InWorld, Weapon && Weapon->ImpactFx[0] ? Weapon->ImpactFx : FX_DUST, InImpact, 1.0f);
            PlayAt(InWorld, STONE[Variant], InImpact, EntityID());
        }
        else if(InSurface == PS_Lab)
        {
            SpawnFx(InWorld, FX_SPARKS, InImpact, 1.0f);
            PlayAt(InWorld, ROBOT[Variant], InImpact, EntityID());
        }
    }

    /* Whether InAvatar's anim state declares InParam. The TDD §5.7 contract
     * names more params than a character may author yet, and setting an
     * unknown name takes one of the component's 12 slots. */
    bool HasAnimParam(World& InWorld, const EntityID InAvatar, const char* InParam)
    {
        const RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(&InWorld);
        const RenderCore::AnimInstance* Instance = Animation ? Animation->FindInstance(InAvatar) : nullptr;
        return Instance && Instance->GetState() && Instance->GetState()->FindParameter(InParam) >= 0;
    }

    void SetAnimBool(World& InWorld, const EntityID InAvatar, RenderCore::AnimationComponent& InAnim, const char* InParam, const bool bInValue)
    {
        if(HasAnimParam(InWorld, InAvatar, InParam) && InAnim.GetBool(InParam) != bInValue) { InAnim.SetBool(InParam, bInValue); }
    }

    void SetAnimFloat(World& InWorld, const EntityID InAvatar, RenderCore::AnimationComponent& InAnim, const char* InParam, const float InValue)
    {
        if(HasAnimParam(InWorld, InAvatar, InParam)) { InAnim.SetFloat(InParam, InValue); }
    }

    void TriggerAnim(World& InWorld, const EntityID InAvatar, const char* InTrigger)
    {
        RenderCore::AnimationComponent* Anim = InWorld.GetEntity(InAvatar).GetComponent<RenderCore::AnimationComponent>();
        if(Anim && HasAnimParam(InWorld, InAvatar, InTrigger)) { Anim->SetTrigger(InTrigger); }
    }

    /* The owner's immediate feedback for an attack it just asked for. */
    void PredictAttack(World& InWorld, const EntityID InAvatar, const LabWeaponComponent& InWeapon,
                       const Vector3& InOrigin, const Vector3& InDir)
    {
        TriggerAnim(InWorld, InAvatar, "Attack");
        if(LabWeapons::TypeOf(InWeapon) == LabWeapons::EWeaponType::EWT_Melee)
        {
            PlayAt(InWorld, SND_SWING, InWorld.GetEntity(InAvatar).GetWorldPose().Position, InAvatar);
            return;
        }
        if(LabWeapons::TypeOf(InWeapon) == LabWeapons::EWeaponType::EWT_Projectile)
        {
            ShotFx(InWorld, InAvatar, InOrigin, PS_None);   // the bolt itself shows where it goes
            return;
        }
        /* Where the shot will land, as far as this process can tell. */
        PhysicsRaycastHit Hit;
        uint8_t Surface = PS_None;
        Vector3 Impact = InOrigin + InDir * double(InWeapon.Range);
        if(PhysicsRaycast(InWorld, InOrigin, InDir, InWeapon.Range, Hit, -1, InAvatar))
        {
            Impact = Hit.Point - InDir * 3.0;
            Surface = HealthOwner(InWorld, Hit.Entity).IsSet() ? PS_Lab : PS_World;
        }
        ShotFx(InWorld, InAvatar, Impact, Surface);
    }
}

GAMECORE_DEFINE_COMPONENT(LabLifetimeComponent, "LabLifetime")

// === Inventory ==============================================================

uint8_t& LabInventoryComponent::WeaponAt(const int InSlot)
{
    return InSlot == 0 ? Weapon0 : (InSlot == 1 ? Weapon1 : Weapon2);
}

uint8_t LabInventoryComponent::WeaponAt(const int InSlot) const
{
    return InSlot == 0 ? Weapon0 : (InSlot == 1 ? Weapon1 : (InSlot == 2 ? Weapon2 : uint8_t(0)));
}

int32_t& LabInventoryComponent::MagAt(const int InSlot)
{
    return InSlot == 0 ? Mag0 : (InSlot == 1 ? Mag1 : Mag2);
}

int32_t& LabInventoryComponent::ReserveAt(const int InSlot)
{
    return InSlot == 0 ? Reserve0 : (InSlot == 1 ? Reserve1 : Reserve2);
}

int32_t LabInventoryComponent::MagAt(const int InSlot) const
{
    return InSlot == 0 ? Mag0 : (InSlot == 1 ? Mag1 : (InSlot == 2 ? Mag2 : 0));
}

int32_t LabInventoryComponent::ReserveAt(const int InSlot) const
{
    return InSlot == 0 ? Reserve0 : (InSlot == 1 ? Reserve1 : (InSlot == 2 ? Reserve2 : 0));
}

uint8_t LabInventoryComponent::EquippedWeapon() const
{
    return Equipped < LabWeapons::SLOTS ? WeaponAt(Equipped) : uint8_t(0);
}

GAMECORE_DEFINE_COMPONENT(LabInventoryComponent, "LabInventoryComponent")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabInventoryComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Weapon0", C.Weapon0);
    Ar.Field("Weapon1", C.Weapon1);
    Ar.Field("Weapon2", C.Weapon2);
    Ar.Field("Mag0", C.Mag0);
    Ar.Field("Mag1", C.Mag1);
    Ar.Field("Mag2", C.Mag2);
    Ar.Field("Reserve0", C.Reserve0);
    Ar.Field("Reserve1", C.Reserve1);
    Ar.Field("Reserve2", C.Reserve2);
    Ar.Field("Throwable", C.Throwable);
    Ar.Field("ThrowCount", C.ThrowCount);
    Ar.Field("Equipped", C.Equipped);
}

#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabInventoryComponent) {
    Rep.Field("Weapon0", &LabInventoryComponent::Weapon0);
    Rep.Field("Weapon1", &LabInventoryComponent::Weapon1);
    Rep.Field("Weapon2", &LabInventoryComponent::Weapon2);
    Rep.Field("Mag0", &LabInventoryComponent::Mag0);
    Rep.Field("Mag1", &LabInventoryComponent::Mag1);
    Rep.Field("Mag2", &LabInventoryComponent::Mag2);
    Rep.Field("Reserve0", &LabInventoryComponent::Reserve0);
    Rep.Field("Reserve1", &LabInventoryComponent::Reserve1);
    Rep.Field("Reserve2", &LabInventoryComponent::Reserve2);
    Rep.Field("Throwable", &LabInventoryComponent::Throwable);
    Rep.Field("ThrowCount", &LabInventoryComponent::ThrowCount);
    Rep.Field("Equipped", &LabInventoryComponent::Equipped);
}
#endif

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabInventoryComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("Weapon0", &LabInventoryComponent::Weapon0);
    Props.Field("Weapon1", &LabInventoryComponent::Weapon1);
    Props.Field("Weapon2", &LabInventoryComponent::Weapon2);
    Props.Field("Mag0", &LabInventoryComponent::Mag0);
    Props.Field("Mag1", &LabInventoryComponent::Mag1);
    Props.Field("Mag2", &LabInventoryComponent::Mag2);
    Props.Field("Reserve0", &LabInventoryComponent::Reserve0);
    Props.Field("Reserve1", &LabInventoryComponent::Reserve1);
    Props.Field("Reserve2", &LabInventoryComponent::Reserve2);
    Props.Field("Equipped", &LabInventoryComponent::Equipped);
}
#endif

// === Combat component =======================================================

void LabCombatComponent::ServerFire(const RpcContext& InContext, const Vector3& InOrigin, const Vector3& InDir, const uint8_t InSeq)
{
    if(InContext.OwningWorld) { LabCombat::Fire(*InContext.OwningWorld, InContext.Entity, &InContext, InOrigin, InDir, InSeq); }
}

void LabCombatComponent::ServerMelee(const RpcContext& InContext)
{
    if(InContext.OwningWorld) { LabCombat::Melee(*InContext.OwningWorld, InContext.Entity, InContext.bRemote ? InContext.ViewTimeMs : 0u); }
}

void LabCombatComponent::ServerReload(const RpcContext& InContext)
{
    if(InContext.OwningWorld) { LabCombat::Reload(*InContext.OwningWorld, InContext.Entity); }
}

void LabCombatComponent::ServerEquip(const RpcContext& InContext, const uint8_t InSlot)
{
    if(InContext.OwningWorld) { LabCombat::Equip(*InContext.OwningWorld, InContext.Entity, InSlot); }
}

void LabCombatComponent::ServerThrow(const RpcContext& InContext, const Vector3& InOrigin, const Vector3& InDir, const float InCharge)
{
    if(InContext.OwningWorld) { LabCombat::Throw(*InContext.OwningWorld, InContext.Entity, InOrigin, InDir, InCharge); }
}

void LabCombatComponent::MulticastBurstFx(const RpcContext& InContext, const Vector3& InAt, const RpcString<96>& InFx,
                                           const RpcString<96>& InSound, const float InRadius)
{
    if(!InContext.OwningWorld) { return; }
    SpawnFx(*InContext.OwningWorld, InFx.Text[0] ? InFx.Text : FX_SPARKS, InAt, 1.6f);
    if(InRadius > 0.f) { SpawnFx(*InContext.OwningWorld, FX_DUST, InAt + Vector3(0, 20, 0), 1.6f); }
    PlayAt(*InContext.OwningWorld, InSound.Text[0] ? InSound.Text : SND_HIT, InAt, EntityID());
}

void LabCombatComponent::MulticastFireFx(const RpcContext& InContext, const Vector3& InImpact, const uint8_t InSurface)
{
    if(!InContext.OwningWorld) { return; }
    World& W = *InContext.OwningWorld;
    /* The shooter's own process drew this when it pressed. */
    const AvatarHarnessComponent* Harness = W.GetEntity(InContext.Entity).GetComponent<AvatarHarnessComponent>();
    if(Harness && Harness->IsLocallyControlled()) { return; }
    ShotFx(W, InContext.Entity, InImpact, InSurface);
}

void LabCombatComponent::ClientHitConfirm(const RpcContext& InContext, const float InDamage, const bool bInKill, const bool bInHead)
{
    if(!InContext.OwningWorld) { return; }
    if(LabCombatComponent* Self = InContext.OwningWorld->GetEntity(InContext.Entity).GetComponent<LabCombatComponent>())
    {
        Self->HitMarker = bInKill ? 0.45f : 0.25f;
    }
    (void)InDamage; (void)bInHead;
}

#define LAB_COMBAT_VER (1)

GAMECORE_DEFINE_COMPONENT(LabCombatComponent, "LabCombatComponent")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabCombatComponent, LAB_COMBAT_VER) {
    Ar.Field("AttackSeq", C.AttackSeq);
    Ar.Field("ComboIndex", C.ComboIndex);
    Ar.Field("ReloadSeq", C.ReloadSeq);
    Ar.Field("HitSeq", C.HitSeq);
}

#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabCombatComponent) {
    Rep.Field("AttackSeq", &LabCombatComponent::AttackSeq);
    Rep.Field("ComboIndex", &LabCombatComponent::ComboIndex);
    Rep.Field("ReloadSeq", &LabCombatComponent::ReloadSeq);
    Rep.Field("HitSeq", &LabCombatComponent::HitSeq);
    Rep.Field("LastHitPart", &LabCombatComponent::LastHitPart);
    Rep.Field("LastHitDir", &LabCombatComponent::LastHitDir).Precision(0.01f);
    Rep.Field("LastHitHeavy", &LabCombatComponent::LastHitHeavy);
    Rep.Flag("bReloading", &LabCombatComponent::bReloading);
    Rep.Field("StaggerFrames", &LabCombatComponent::StaggerFrames);
    Rep.Field("ThrowSeq", &LabCombatComponent::ThrowSeq);
    Rep.Field("Spread", &LabCombatComponent::Spread).Precision(0.05f);
}
#endif

GAMECORE_DEFINE_COMPONENT_RPCS(LabCombatComponent) {
    Rpc.Server<&LabCombatComponent::ServerFire>("ServerFire").RateLimit(20);
    Rpc.Server<&LabCombatComponent::ServerMelee>("ServerMelee").RateLimit(6);
    Rpc.Server<&LabCombatComponent::ServerReload>("ServerReload").RateLimit(4);
    Rpc.Server<&LabCombatComponent::ServerEquip>("ServerEquip").RateLimit(4);
    Rpc.Server<&LabCombatComponent::ServerInteract>("ServerInteract").RateLimit(4);
    Rpc.Server<&LabCombatComponent::ServerThrow>("ServerThrow").RateLimit(4);
    Rpc.Multicast<&LabCombatComponent::MulticastBurstFx>("MulticastBurstFx").Unreliable();
    Rpc.Multicast<&LabCombatComponent::MulticastFireFx>("MulticastFireFx").Unreliable();
    Rpc.Client<&LabCombatComponent::ClientHitConfirm>("ClientHitConfirm").Unreliable();
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabCombatComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("AttackSeq", &LabCombatComponent::AttackSeq, PF_ReadOnly);
    Props.Field("HitSeq", &LabCombatComponent::HitSeq, PF_ReadOnly);
    Props.Field("Spread", &LabCombatComponent::Spread, PF_ReadOnly);
}
#endif

// === Authority actions ======================================================

const LabWeaponComponent* LabCombat::EquippedData(World& InWorld, const EntityID InAvatar)
{
    const Entity E = InWorld.GetEntity(InAvatar);
    const LabInventoryComponent* Inventory = E.IsAlive() ? E.GetComponent<LabInventoryComponent>() : nullptr;
    const uint8_t ID = Inventory ? Inventory->EquippedWeapon() : uint8_t(0);
    return ID ? LabWeapons::GetData(InWorld, ID) : nullptr;
}

void LabCombat::Fire(World& InWorld, const EntityID InAvatar, const RpcContext* InCall,
                      const Vector3& InOrigin, const Vector3& InDir, const uint8_t InSeq)
{
    if(!HasAuthority() || !IsUp(InWorld, InAvatar)) { return; }
    Entity Self = InWorld.GetEntity(InAvatar);
    LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
    LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
    if(!Combat || !Inventory || Inventory->Equipped >= LabWeapons::SLOTS) { return; }

    const LabWeaponComponent* WeaponPtr = LabWeapons::GetData(InWorld, Inventory->EquippedWeapon());
    if(!WeaponPtr) { return; }
    const LabWeaponComponent Weapon = *WeaponPtr;   // a copy: spawns below may move storage
    const uint8_t HeldID = Inventory->EquippedWeapon();

    if(LabWeapons::TypeOf(Weapon) == LabWeapons::EWeaponType::EWT_Melee)
    {
        Melee(InWorld, InAvatar, InCall && InCall->bRemote ? InCall->ViewTimeMs : 0u);
        return;
    }

    /* Validate: what a cheat could send. */
    if(Combat->bReloading || Combat->Cooldown > Weapon.FireInterval * 0.25f) { return; }
    const int Slot = Inventory->Equipped;
    if(Weapon.MagSize > 0 && Inventory->MagAt(Slot) <= 0) { return; }
    if(InDir.GetLength() < 1e-4) { return; }
    const Vector3 Dir = InDir.GetNormalize();

    Vector3 Origin = InOrigin;
    if(const CharacterMoverComponent* Mover = Self.GetComponent<CharacterMoverComponent>())
    {
        const Vector3 Pivot = Self.GetWorldPose().Position + Vector3(0, double(Mover->BoomHeight), 0);
        if((Origin - Pivot).GetLength() > MAX_ORIGIN_SLOP) { Origin = Pivot; }
    }

    if(Weapon.MagSize > 0) { --Inventory->MagAt(Slot); }
    Combat->Cooldown = Weapon.FireInterval;
    ++Combat->AttackSeq;
    if(LabWeapons::ModeOf(Weapon) == LabWeapons::EFireMode::EFM_Burst && Combat->BurstLeft == 0)
    {
        Combat->BurstLeft = uint8_t(std::max(0, int(Weapon.BurstCount) - 1));
    }
    const float Spread = Combat->Spread > Weapon.SpreadMin ? Combat->Spread : Weapon.SpreadMin;
    Combat->Spread = std::min(Weapon.SpreadMax, Spread + Weapon.SpreadPerShot);

    const uint32_t SeedBase = uint32_t(InAvatar.Pack() & 0xFFFFFFFFu) * 7919u + uint32_t(InSeq) * 31u;
    ReportNoise(InWorld, InAvatar, Origin, 2200.f);

    if(LabWeapons::TypeOf(Weapon) == LabWeapons::EWeaponType::EWT_Projectile)
    {
        /* Launched from the muzzle toward where the camera ray lands, so the
         * bolt flies to the crosshair from an over-the-shoulder view. */
        Vector3 Target = Origin + Dir * double(Weapon.Range);
        PhysicsRaycastHit Aimed;
        if(PhysicsRaycast(InWorld, Origin, Dir, Weapon.Range, Aimed, LAB_SHOT_MASK, InAvatar)) { Target = Aimed.Point; }
        Vector3 Start = MuzzleOf(InWorld, InAvatar);
        if((Start - Origin).GetLength() > MAX_ORIGIN_SLOP) { Start = Origin; }
        const Vector3 Toward = Target - Start;
        const Vector3 LaunchDir = Jitter(Toward.GetLength() > 1.0 ? Toward.GetNormalize() : Dir, Spread, SeedBase);
        LabProjectiles::Launch(InWorld, InAvatar, HeldID, Weapon, Start, LaunchDir, Weapon.LaunchSpeed);
        GAMECORE_RPC(LabCombatComponent, MulticastFireFx)(InWorld.GetEntity(InAvatar), Start, uint8_t(PS_None));
        return;
    }

    float   Dealt = 0.f;
    bool    bKill = false, bHead = false;
    Vector3 LastImpact = Origin + Dir * double(Weapon.Range);
    uint8_t LastSurface = PS_None;

    for(int Pellet = 0; Pellet < std::max(1, int(Weapon.Pellets)); ++Pellet)
    {
        const Vector3 ShotDir = Jitter(Dir, Spread, SeedBase + uint32_t(Pellet));
        LagCompensatedHit Hit;
        const bool bAny = (InCall && InCall->bRemote)
            ? LagCompensatedRaycast(InWorld, *InCall, Origin, ShotDir, Weapon.Range, Hit)
            : LagCompensatedRaycastAt(InWorld, 0, InAvatar, Origin, ShotDir, Weapon.Range, Hit);
        if(!bAny) { continue; }

        LastImpact = Hit.Point - ShotDir * 3.0;
        const EntityID Victim = HealthOwner(InWorld, Hit.Entity);
        if(!Victim.IsSet() || Victim == InAvatar)
        {
            LastSurface = PS_World;
            /* Glass cases and samples (Level 2's workshop) shatter when shot. */
            const Entity Struck = InWorld.GetEntity(Hit.Entity);
            NimbleEngine::DestructibleComponent* Breakable = Struck.IsAlive() ? Struck.GetComponent<NimbleEngine::DestructibleComponent>() : nullptr;
            if(Breakable && Breakable->bEnabled && !Breakable->IsBroken())
            {
                Breakable->Break(Hit.Point, ShotDir * double(std::max(Weapon.Impulse, 1.f) * 60.f));
            }
            continue;
        }
        LastSurface = PS_Lab;

        bool bHeadHit = Hit.bCompensated && double(Hit.Height) >= HEAD_FROM;
        if(!Hit.bCompensated)
        {
            if(const RagdollComponent* Ragdoll = InWorld.GetEntity(Victim).GetComponent<RagdollComponent>())
            {
                bHeadHit = Ragdoll->GetPartAt(Hit.Point) == "Head";
            }
        }

        float Amount = Weapon.Damage * (bHeadHit ? Weapon.HeadshotMultiplier : 1.f);
        if(Weapon.Falloff > 0.f && Weapon.Range > 0.f)
        {
            Amount *= 1.f - std::min(1.f, Hit.Distance / Weapon.Range) * Weapon.Falloff;
        }

        LabDamageEvent Event;
        Event.Victim     = Victim;
        Event.Instigator = InAvatar;
        Event.WeaponID   = HeldID;
        Event.Amount     = Amount;
        Event.DamageType = EDamageType::EDT_Bullet;
        Event.Point      = Hit.Point;
        Event.Dir        = ShotDir;
        Event.Impulse    = Weapon.Impulse * 20.f;
        Event.Part       = bHeadHit ? EHitPart::EHP_Head : EHitPart::EHP_Body;
        Event.bStaggers  = Weapon.bStaggers;
        Dealt += LabDamage::Apply(InWorld, Event);

        const Entity VictimEntity = InWorld.GetEntity(Victim);
        const LabHealthComponent* VictimHealth = VictimEntity.IsAlive() ? VictimEntity.GetComponent<LabHealthComponent>() : nullptr;
        bKill = bKill || (VictimHealth && VictimHealth->bDead);
        bHead = bHead || bHeadHit;

        /* Bodies already down take the shot's push. */
        if(VictimHealth && VictimHealth->bDead)
        {
            if(RagdollComponent* Ragdoll = VictimEntity.GetComponent<RagdollComponent>())
            {
                if(Ragdoll->IsActive()) { Ragdoll->AddImpulse(ShotDir * double(Weapon.Impulse * 10.f), Hit.Point); }
            }
        }
    }

    GAMECORE_RPC(LabCombatComponent, MulticastFireFx)(InWorld.GetEntity(InAvatar), LastImpact, LastSurface);
    if(Dealt > 0.f)
    {
        GAMECORE_RPC(LabCombatComponent, ClientHitConfirm)(InWorld.GetEntity(InAvatar), Dealt, bKill, bHead);
    }
}

void LabCombat::Melee(World& InWorld, const EntityID InAvatar, const uint32_t InViewTimeMs)
{
    if(!HasAuthority() || !IsUp(InWorld, InAvatar)) { return; }
    LabCombatComponent* Combat = InWorld.GetEntity(InAvatar).GetComponent<LabCombatComponent>();
    if(!Combat || Combat->bReloading) { return; }
    const LabWeaponComponent& Weapon = MeleeData(InWorld, InAvatar);
    Combat = InWorld.GetEntity(InAvatar).GetComponent<LabCombatComponent>();
    if(!Combat) { return; }

    /* A press while idle starts a swing; one during Recovery chains the combo. */
    if(Combat->MeleePhase == 0)
    {
        Combat->ComboIndex = 1;
    }
    else if(Combat->MeleePhase == 3 && Combat->ComboIndex < std::max<uint8_t>(1, Weapon.ComboLength))
    {
        ++Combat->ComboIndex;
    }
    else
    {
        return;
    }

    Combat->MeleePhase = 1;
    Combat->MeleeTimer = Weapon.Windup;
    Combat->MeleeViewTimeMs = InViewTimeMs;
    for(EntityID& Hit : Combat->MeleeHits) { Hit = EntityID(); }
    ++Combat->AttackSeq;
}

void LabCombat::Reload(World& InWorld, const EntityID InAvatar)
{
    if(!HasAuthority() || !IsUp(InWorld, InAvatar)) { return; }
    Entity Self = InWorld.GetEntity(InAvatar);
    LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
    LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
    if(!Inventory || !Combat || Combat->bReloading || Inventory->Equipped >= LabWeapons::SLOTS) { return; }

    const LabWeaponComponent* Weapon = LabWeapons::GetData(InWorld, Inventory->EquippedWeapon());
    Self = InWorld.GetEntity(InAvatar);
    Inventory = Self.GetComponent<LabInventoryComponent>();
    Combat = Self.GetComponent<LabCombatComponent>();
    if(!Weapon || Weapon->MagSize <= 0) { return; }
    const int Slot = Inventory->Equipped;
    if(Inventory->MagAt(Slot) >= Weapon->MagSize || Inventory->ReserveAt(Slot) <= 0) { return; }

    Combat->bReloading = 1;
    Combat->ReloadTimer = Weapon->ReloadSeconds;
    Combat->BurstLeft = 0;
    ++Combat->ReloadSeq;
}

void LabCombat::Equip(World& InWorld, const EntityID InAvatar, const uint8_t InSlot)
{
    if(!HasAuthority() || !IsUp(InWorld, InAvatar)) { return; }
    Entity Self = InWorld.GetEntity(InAvatar);
    LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
    LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
    if(!Inventory || !Combat) { return; }

    if(InSlot == LabWeapons::UNARMED)
    {
        Inventory->Equipped = LabWeapons::UNARMED;
    }
    else
    {
        if(InSlot >= LabWeapons::SLOTS || Inventory->WeaponAt(InSlot) == 0 || Inventory->Equipped == InSlot) { return; }
        Inventory->Equipped = InSlot;
    }

    /* A switch cancels a reload and a burst, and takes the draw time. */
    Combat->bReloading = 0;
    Combat->ReloadTimer = 0.f;
    Combat->BurstLeft = 0;
    const LabWeaponComponent* Weapon = EquippedData(InWorld, InAvatar);
    const float Draw = Weapon ? Weapon->EquipSeconds : 0.f;
    if(LabCombatComponent* Fresh = InWorld.GetEntity(InAvatar).GetComponent<LabCombatComponent>())
    {
        Fresh->Cooldown = std::max(Fresh->Cooldown, Draw);
    }
}

bool LabCombat::GiveWeapon(World& InWorld, const EntityID InAvatar, const uint8_t InWeaponID, const bool bInEquip)
{
    if(!HasAuthority()) { return false; }
    const LabWeaponComponent* Weapon = LabWeapons::GetData(InWorld, InWeaponID);
    if(!Weapon) { return false; }
    const int32_t Mag = Weapon->MagSize;
    const int32_t Reserve = Weapon->ReserveMax;

    const bool bThrowable = LabWeapons::TypeOf(*Weapon) == LabWeapons::EWeaponType::EWT_Throwable;

    LabInventoryComponent* Inventory = InWorld.GetEntity(InAvatar).GetComponent<LabInventoryComponent>();
    if(!Inventory) { return false; }

    /* A throwable: the pouch holds one kind; a new kind replaces the old. */
    if(bThrowable)
    {
        if(Inventory->Throwable != InWeaponID) { Inventory->Throwable = InWeaponID; Inventory->ThrowCount = 0; }
        Inventory->ThrowCount = uint8_t(std::min<int32_t>(std::max(1, Reserve), Inventory->ThrowCount + std::max(1, Mag)));
        return true;
    }

    /* Already held: ammo only. */
    for(int Slot = 0; Slot < LabWeapons::SLOTS; ++Slot)
    {
        if(Inventory->WeaponAt(Slot) == InWeaponID)
        {
            Inventory->ReserveAt(Slot) = Reserve;
            if(bInEquip) { Inventory->Equipped = uint8_t(Slot); }
            return true;
        }
    }

    int Slot = -1;
    for(int S = 0; S < LabWeapons::SLOTS && Slot < 0; ++S)
    {
        if(Inventory->WeaponAt(S) == 0) { Slot = S; }
    }
    if(Slot < 0) { Slot = Inventory->Equipped < LabWeapons::SLOTS ? Inventory->Equipped : 0; }

    Inventory->WeaponAt(Slot) = InWeaponID;
    Inventory->MagAt(Slot) = Mag;
    Inventory->ReserveAt(Slot) = Reserve;
    if(bInEquip || Inventory->Equipped >= LabWeapons::SLOTS) { Inventory->Equipped = uint8_t(Slot); }
    return true;
}

void LabCombat::GiveAmmo(World& InWorld, const EntityID InAvatar, const uint8_t InWeaponID, const int32_t InRounds)
{
    if(!HasAuthority()) { return; }
    LabInventoryComponent* Inventory = InWorld.GetEntity(InAvatar).GetComponent<LabInventoryComponent>();
    if(!Inventory) { return; }
    for(int Slot = 0; Slot < LabWeapons::SLOTS; ++Slot)
    {
        const uint8_t ID = Inventory->WeaponAt(Slot);
        if(ID == 0 || (InWeaponID != 0 && ID != InWeaponID)) { continue; }
        const LabWeaponComponent* Weapon = LabWeapons::GetData(InWorld, ID);
        Inventory = InWorld.GetEntity(InAvatar).GetComponent<LabInventoryComponent>();
        if(!Weapon || Weapon->MagSize <= 0) { continue; }
        Inventory->ReserveAt(Slot) = std::min(Weapon->ReserveMax, Inventory->ReserveAt(Slot) + InRounds);
    }
}

namespace
{
    /* Where a throw leaves the hand: shoulder height, a little ahead. The
     * owner's preview and the authority's throw use the same point. */
    Vector3 ThrowStart(World& InWorld, const EntityID InAvatar, const Vector3& InAim)
    {
        Vector3 Flat(InAim.GetX(), 0, InAim.GetZ());
        Flat = Flat.GetLength() > 1e-4 ? Flat.GetNormalize() : Vector3(0, 0, 1);
        return InWorld.GetEntity(InAvatar).GetWorldPose().Position + Vector3(0, 150, 0) + Flat * 40.0;
    }
}

void LabCombat::Throw(World& InWorld, const EntityID InAvatar, const Vector3& InOrigin, const Vector3& InDir, const float InCharge)
{
    if(!HasAuthority() || !IsUp(InWorld, InAvatar)) { return; }
    Entity Self = InWorld.GetEntity(InAvatar);
    LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
    LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
    if(!Inventory || !Combat || Inventory->Throwable == 0 || Inventory->ThrowCount == 0 || Combat->Cooldown > 0.1f) { return; }
    if(InDir.GetLength() < 1e-4) { return; }
    (void)InOrigin;   // the throw leaves the hand, whatever the client's camera

    const uint8_t ID = Inventory->Throwable;
    const LabWeaponComponent* Found = LabWeapons::GetData(InWorld, ID);
    if(!Found) { return; }
    const LabWeaponComponent Weapon = *Found;

    Self = InWorld.GetEntity(InAvatar);
    Inventory = Self.GetComponent<LabInventoryComponent>();
    Combat = Self.GetComponent<LabCombatComponent>();
    --Inventory->ThrowCount;
    if(Inventory->ThrowCount == 0) { Inventory->Throwable = 0; }
    Combat->Cooldown = 0.6f;
    ++Combat->ThrowSeq;

    const Vector3 Dir = InDir.GetNormalize();
    const Vector3 Velocity = LabProjectiles::ThrowVelocity(Dir, Weapon, InCharge);
    const double Speed = Velocity.GetLength();
    LabProjectiles::Launch(InWorld, InAvatar, ID, Weapon, ThrowStart(InWorld, InAvatar, Dir), Velocity * (1.0 / Speed), float(Speed));
}

void LabCombat::BurstFx(World& InWorld, const EntityID InInstigator, const Vector3& InAt, const char* InFx, const char* InSound, const float InRadius)
{
    if(!HasAuthority()) { return; }
    if(InInstigator.IsSet() && InWorld.IsAlive(InInstigator) && InWorld.GetEntity(InInstigator).GetComponent<LabCombatComponent>())
    {
        GAMECORE_RPC(LabCombatComponent, MulticastBurstFx)(InWorld.GetEntity(InInstigator), InAt, RpcString<96>(InFx),
                                                            RpcString<96>(InSound), InRadius);
        return;
    }
    SpawnFx(InWorld, InFx && InFx[0] ? InFx : FX_SPARKS, InAt, 1.6f);
    PlayAt(InWorld, InSound && InSound[0] ? InSound : SND_HIT, InAt, EntityID());
}

void LabCombat::ReportNoise(World& InWorld, const EntityID InSource, const Vector3& InAt, const float InRange)
{
    if(!HasAuthority()) { return; }
    /* Loudness 1 is a gunshot (AIFacade.h); scaled by how far it carries. */
    ReportAINoise(InWorld, InAt, std::clamp(InRange / 2200.f, 0.2f, 2.f), InSource);
    EventArgs Args;
    Args.Args[Args.Count++] = EventArg::Make(InSource);
    Args.Args[Args.Count++] = EventArg::Make(double(InRange));
    InWorld.GetEvents().Emit("Lab.Noise", Args);
}

// === Systems ================================================================

namespace
{
    /* The owner's attack: predicted cosmetics here, the real thing on the authority. */
    void LocalAttack(World& InWorld, const EntityID InAvatar, const LabWeaponComponent& InWeapon, const bool bInMeleeAction)
    {
        Entity Self = InWorld.GetEntity(InAvatar);
        LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
        if(!Combat) { return; }

        const bool bMelee = bInMeleeAction || LabWeapons::TypeOf(InWeapon) == LabWeapons::EWeaponType::EWT_Melee;
        if(bMelee)
        {
            const LabWeaponComponent& Swing = bInMeleeAction ? MeleeData(InWorld, InAvatar) : InWeapon;
            Combat = InWorld.GetEntity(InAvatar).GetComponent<LabCombatComponent>();
            Combat->LocalCooldown = Swing.Windup + Swing.Active;
            PredictAttack(InWorld, InAvatar, Swing, Vector3(), Vector3());
            if(HasAuthority()) { LabCombat::Melee(InWorld, InAvatar, 0u); }
            else { GAMECORE_RPC(LabCombatComponent, ServerMelee)(InWorld.GetEntity(InAvatar)); }
            return;
        }

        Vector3 Origin, Dir;
        if(!GetCharacterAim(InWorld, InAvatar, Origin, Dir)) { return; }
        Combat->LocalCooldown = InWeapon.FireInterval;
        const uint8_t Seq = uint8_t(Combat->AttackSeq + 1);
        PredictAttack(InWorld, InAvatar, InWeapon, Origin, Dir);
        if(HasAuthority()) { LabCombat::Fire(InWorld, InAvatar, nullptr, Origin, Dir, Seq); }
        else { GAMECORE_RPC(LabCombatComponent, ServerFire)(InWorld.GetEntity(InAvatar), Origin, Dir, Seq); }
    }

    void LocalThrow(World& InWorld, const EntityID InAvatar, const float InCharge)
    {
        Vector3 Origin, Dir;
        if(!GetCharacterAim(InWorld, InAvatar, Origin, Dir)) { return; }
        TriggerAnim(InWorld, InAvatar, "Attack");
        if(HasAuthority()) { LabCombat::Throw(InWorld, InAvatar, Origin, Dir, InCharge); }
        else { GAMECORE_RPC(LabCombatComponent, ServerThrow)(InWorld.GetEntity(InAvatar), Origin, Dir, InCharge); }
    }

    /* The owner's arc preview while Throw is held: dots along the flight,
     * local to this process, reused frame to frame. */
    constexpr int ARC_DOTS = 24;
    std::map<const World*, std::vector<EntityID>> GArcDots;

    void ShowArc(World& InWorld, const EntityID InAvatar, const bool bInShow, const float InCharge)
    {
        std::vector<EntityID>& Dots = GArcDots[&InWorld];
        Vector3 Points[ARC_DOTS];
        int Count = 0;
        if(bInShow)
        {
            Vector3 Origin, Dir;
            const LabInventoryComponent* Inventory = InWorld.GetEntity(InAvatar).GetComponent<LabInventoryComponent>();
            const LabWeaponComponent* Weapon = Inventory ? LabWeapons::GetData(InWorld, Inventory->Throwable) : nullptr;
            float Gravity = 981.f;
            if(Weapon && GetCharacterAim(InWorld, InAvatar, Origin, Dir))
            {
                const LabWeaponComponent Copy = *Weapon;
                Count = LabProjectiles::PredictArc(InWorld, InAvatar, ThrowStart(InWorld, InAvatar, Dir),
                                                    LabProjectiles::ThrowVelocity(Dir, Copy, InCharge), Gravity,
                                                    1.f / 15.f, Points, ARC_DOTS);
            }
        }
        while(int(Dots.size()) < ARC_DOTS && bInShow)
        {
            Entity Dot = InWorld.CreateEntity("LabArcDot");
            TransformComponent T;
            T.Scale = Vector3(5, 5, 5);
            Dot.AddComponent(T);
            RenderCore::RenderComponent Render;
            std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Sphere.nmmesh");
            Render.bCastShadows = 0;
            Dot.AddComponent(Render);
            Dots.push_back(Dot.GetID());
        }
        for(int I = 0; I < int(Dots.size()); ++I)
        {
            if(!InWorld.IsAlive(Dots[size_t(I)])) { continue; }
            Entity Dot = InWorld.GetEntity(Dots[size_t(I)]);
            if(RenderCore::RenderComponent* Render = Dot.GetComponent<RenderCore::RenderComponent>()) { Render->bVisible = I < Count ? 1 : 0; }
            if(I < Count)
            {
                if(TransformComponent* T = Dot.GetComponent<TransformComponent>()) { T->Position = Points[I]; }
            }
        }
    }

    void AskEquip(World& InWorld, const EntityID InAvatar, const uint8_t InSlot)
    {
        if(HasAuthority()) { LabCombat::Equip(InWorld, InAvatar, InSlot); }
        else { GAMECORE_RPC(LabCombatComponent, ServerEquip)(InWorld.GetEntity(InAvatar), InSlot); }
    }

    void AskReload(World& InWorld, const EntityID InAvatar)
    {
        if(HasAuthority()) { LabCombat::Reload(InWorld, InAvatar); }
        else { GAMECORE_RPC(LabCombatComponent, ServerReload)(InWorld.GetEntity(InAvatar)); }
    }

    /* The next held slot after InFrom, in InStep's direction; InFrom if none. */
    uint8_t CycleSlot(const LabInventoryComponent& InInventory, const int InStep)
    {
        const int From = InInventory.Equipped < LabWeapons::SLOTS ? InInventory.Equipped : (InStep > 0 ? -1 : LabWeapons::SLOTS);
        for(int I = 1; I <= LabWeapons::SLOTS; ++I)
        {
            const int Slot = ((From + InStep * I) % LabWeapons::SLOTS + LabWeapons::SLOTS) % LabWeapons::SLOTS;
            if(InInventory.WeaponAt(Slot) != 0) { return uint8_t(Slot); }
        }
        return InInventory.Equipped;
    }
}

void LabCombat::TickInput(World& InWorld, const float InSeconds)
{
    struct Action { EntityID Avatar; int Kind; uint8_t Slot; float Charge; };   // 0 fire, 1 melee, 2 reload, 3 equip, 4 dry, 5 throw
    std::vector<Action> Actions;
    EntityID Aiming;          // the local character charging a throw, for the arc
    float    AimingCharge = 0.f;

    InWorld.Each<LabCombatComponent>([&](Entity InEntity, LabCombatComponent& InCombat)
    {
        InCombat.LocalCooldown = std::max(0.f, InCombat.LocalCooldown - InSeconds);
        InCombat.HitMarker = std::max(0.f, InCombat.HitMarker - InSeconds);

        const AvatarHarnessComponent* Harness = InEntity.GetComponent<AvatarHarnessComponent>();
        const LabInventoryComponent* Inventory = InEntity.GetComponent<LabInventoryComponent>();
        if(!Harness || !Inventory || !Harness->bEnabled || !Harness->IsLocallyControlled()) { return; }
        const LabHealthComponent* Health = InEntity.GetComponent<LabHealthComponent>();
        if(Health && (Health->bDead || Health->bDown)) { return; }

        const EntityID Avatar = InEntity.GetID();
        if(Harness->WasPressed("Slot1")) { Actions.push_back({ Avatar, 3, 0, 0.f }); }
        if(Harness->WasPressed("Slot2")) { Actions.push_back({ Avatar, 3, 1, 0.f }); }
        if(Harness->WasPressed("Slot3")) { Actions.push_back({ Avatar, 3, 2, 0.f }); }
        if(Harness->WasPressed("NextWeapon")) { Actions.push_back({ Avatar, 3, CycleSlot(*Inventory, 1), 0.f }); }
        if(Harness->WasPressed("PrevWeapon")) { Actions.push_back({ Avatar, 3, CycleSlot(*Inventory, -1), 0.f }); }
        if(Harness->WasPressed("Holster")) { Actions.push_back({ Avatar, 3, LabWeapons::UNARMED, 0.f }); }
        if(Harness->WasPressed("Reload")) { Actions.push_back({ Avatar, 2, 0, 0.f }); }

        /* Throw: hold to charge (a full second), let go to throw. */
        if(Inventory->Throwable != 0 && Inventory->ThrowCount > 0)
        {
            if(Harness->IsDown("Throw"))
            {
                InCombat.bCharging = 1;
                InCombat.ThrowCharge = std::min(1.f, InCombat.ThrowCharge + InSeconds);
                Aiming = Avatar;
                AimingCharge = InCombat.ThrowCharge;
            }
            else if(InCombat.bCharging)
            {
                Actions.push_back({ Avatar, 5, 0, InCombat.ThrowCharge });
                InCombat.bCharging = 0;
                InCombat.ThrowCharge = 0.f;
            }
            if(InCombat.bCharging) { return; }   // no shooting while winding up
        }
        else
        {
            InCombat.bCharging = 0;
            InCombat.ThrowCharge = 0.f;
        }

        if(InCombat.LocalCooldown > 0.f || InCombat.bReloading) { return; }
        if(Harness->WasPressed("Melee")) { Actions.push_back({ Avatar, 1, 0, 0.f }); return; }

        const uint8_t Equipped = Inventory->Equipped;
        if(Equipped >= LabWeapons::SLOTS)
        {
            if(Harness->WasPressed("Fire")) { Actions.push_back({ Avatar, 1, 0, 0.f }); }   // unarmed: Fire punches
            return;
        }
        const LabWeaponComponent* Weapon = LabWeapons::GetData(InWorld, Inventory->WeaponAt(Equipped));
        if(!Weapon) { return; }
        const bool bAuto = LabWeapons::ModeOf(*Weapon) == LabWeapons::EFireMode::EFM_Auto;
        const bool bWant = bAuto ? Harness->IsDown("Fire") : Harness->WasPressed("Fire");
        if(!bWant) { return; }

        LabInventoryComponent* Inv = InEntity.GetComponent<LabInventoryComponent>();
        if(Weapon->MagSize > 0 && Inv && Inv->MagAt(Equipped) <= 0)
        {
            if(Harness->WasPressed("Fire")) { Actions.push_back({ Avatar, 4, 0, 0.f }); }
            return;
        }
        Actions.push_back({ Avatar, 0, 0, 0.f });
    });

    /* Acted on after the walk: an action may spawn (weapon data) and move storage. */
    for(const Action& A : Actions)
    {
        if(!InWorld.IsAlive(A.Avatar)) { continue; }
        switch(A.Kind)
        {
        case 0:
            if(const LabWeaponComponent* Weapon = EquippedData(InWorld, A.Avatar))
            {
                const LabWeaponComponent Copy = *Weapon;
                LocalAttack(InWorld, A.Avatar, Copy, false);
            }
            break;
        case 1:
        {
            const LabWeaponComponent* Weapon = EquippedData(InWorld, A.Avatar);
            const LabWeaponComponent Copy = Weapon ? *Weapon : GFists;
            LocalAttack(InWorld, A.Avatar, Copy, true);
            break;
        }
        case 2: AskReload(InWorld, A.Avatar); break;
        case 3: AskEquip(InWorld, A.Avatar, A.Slot); break;
        case 4:
            PlayAt(InWorld, SND_DRY, InWorld.GetEntity(A.Avatar).GetWorldPose().Position, A.Avatar);
            AskReload(InWorld, A.Avatar);
            break;
        case 5: LocalThrow(InWorld, A.Avatar, A.Charge); break;
        default: break;
        }
    }
    ShowArc(InWorld, Aiming, Aiming.IsSet() && InWorld.IsAlive(Aiming), AimingCharge);
}

namespace
{
    /* Authority: one tick of an Active melee window. */
    void SweepMelee(World& InWorld, const EntityID InAvatar)
    {
        const LabWeaponComponent Weapon = MeleeData(InWorld, InAvatar);
        Entity Self = InWorld.GetEntity(InAvatar);
        Vector3 Origin, Aim;
        if(!GetCharacterAim(InWorld, InAvatar, Origin, Aim)) { return; }

        const Vector3 Feet = Self.GetWorldPose().Position;
        Vector3 Forward(Aim.GetX(), 0, Aim.GetZ());
        if(Forward.GetLength() < 1e-4) { return; }
        Forward = Forward.GetNormalize();
        const double CosHalfArc = std::cos(double(Weapon.ArcDegrees) * 0.5 * double(PI_F) / 180.0);

        const LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
        const uint32_t ViewTime = Combat ? Combat->MeleeViewTimeMs : 0u;
        const uint8_t Combo = Combat ? Combat->ComboIndex : uint8_t(1);
        const LabCharacterComponent* Character = Self.GetComponent<LabCharacterComponent>();
        const float Scale = Character ? Character->MeleeDamageScale : 1.f;
        /* The swinging weapon's id; 0 for fists. */
        const LabWeaponComponent* HeldData = LabCombat::EquippedData(InWorld, InAvatar);
        const LabInventoryComponent* Inventory = InWorld.GetEntity(InAvatar).GetComponent<LabInventoryComponent>();
        const uint8_t WeaponID = HeldData && Inventory && LabWeapons::TypeOf(*HeldData) == LabWeapons::EWeaponType::EWT_Melee
                               ? Inventory->EquippedWeapon() : uint8_t(0);

        struct Candidate { EntityID Victim; Vector3 At; };
        std::vector<Candidate> Hits;
        InWorld.Each<LabHealthComponent>([&](Entity InVictim, LabHealthComponent& InHealth)
        {
            if(InVictim.GetID() == InAvatar || InHealth.bDead) { return; }
            Vector3 At = InVictim.GetWorldPose().Position;
            if(ViewTime) { GetLagCompensatedPosition(InWorld, InVictim.GetID(), ViewTime, At); }
            const Vector3 To = At - Feet;
            if(std::fabs(To.GetY()) > 120.0) { return; }
            Vector3 Flat(To.GetX(), 0, To.GetZ());
            const double Dist = Flat.GetLength();
            if(Dist > double(Weapon.Reach) + 30.0) { return; }
            if(Dist > 1.0 && Flat.GetNormalize().Dot(Forward) < CosHalfArc) { return; }
            Hits.push_back({ InVictim.GetID(), At });
        });

        for(const Candidate& C : Hits)
        {
            LabCombatComponent* Mine = InWorld.GetEntity(InAvatar).GetComponent<LabCombatComponent>();
            if(!Mine) { return; }
            bool bAlready = false;
            int Free = -1;
            for(int I = 0; I < 8; ++I)
            {
                if(Mine->MeleeHits[I] == C.Victim) { bAlready = true; }
                if(!Mine->MeleeHits[I].IsSet() && Free < 0) { Free = I; }
            }
            if(bAlready || Free < 0) { continue; }

            /* Line of sight, chest to chest. */
            const Vector3 From = Feet + Vector3(0, 120, 0);
            const Vector3 To = C.At + Vector3(0, 110, 0);
            const Vector3 Ray = To - From;
            PhysicsRaycastHit Block;
            if(PhysicsRaycast(InWorld, From, Ray, float(Ray.GetLength()), Block, -1, InAvatar)
               && HealthOwner(InWorld, Block.Entity) != C.Victim)
            {
                continue;
            }
            Mine->MeleeHits[Free] = C.Victim;

            /* Backstab: both facing the same way. */
            float Amount = Weapon.Damage * Scale;
            if(const CharacterMoverComponent* VictimMover = InWorld.GetEntity(C.Victim).GetComponent<CharacterMoverComponent>())
            {
                const double Yaw = double(VictimMover->BodyYaw) * double(PI_F) / 180.0;
                const Vector3 VictimForward(std::sin(Yaw), 0, std::cos(Yaw));
                if(VictimForward.Dot(Forward) > 0.5) { Amount *= 2.f; }
            }
            const bool bFinisher = Combo >= std::max<uint8_t>(1, Weapon.ComboLength) && Weapon.ComboLength > 1;

            LabDamageEvent Event;
            Event.Victim     = C.Victim;
            Event.Instigator = InAvatar;
            Event.WeaponID   = WeaponID;
            Event.Amount     = bFinisher ? Amount * 1.5f : Amount;
            Event.DamageType = EDamageType::EDT_Melee;
            Event.Point      = C.At + Vector3(0, 110, 0);
            Event.Dir        = (Forward + Vector3(0, 0.25, 0)).GetNormalize();
            Event.Impulse    = Weapon.Impulse * 20.f;
            Event.bStaggers  = uint8_t(Weapon.bStaggers || bFinisher);
            const float Dealt = LabDamage::Apply(InWorld, Event);
            if(Dealt > 0.f)
            {
                SpawnFx(InWorld, FX_SPARKS, Event.Point, 0.8f);
                PlayAt(InWorld, SND_HIT, Event.Point, EntityID());
                const LabHealthComponent* VictimHealth = InWorld.GetEntity(C.Victim).GetComponent<LabHealthComponent>();
                GAMECORE_RPC(LabCombatComponent, ClientHitConfirm)(InWorld.GetEntity(InAvatar), Dealt,
                                                                    VictimHealth && VictimHealth->bDead, false);
            }
        }
    }
}

void LabCombat::TickServer(World& InWorld, const float InSeconds)
{
    if(!HasAuthority()) { return; }

    std::vector<EntityID> Sweeps, Bursts;
    InWorld.Each<LabCombatComponent>([&](Entity InEntity, LabCombatComponent& InCombat)
    {
        InCombat.Cooldown = std::max(0.f, InCombat.Cooldown - InSeconds);

        const EntityID Avatar = InEntity.GetID();
        LabInventoryComponent* Inventory = InEntity.GetComponent<LabInventoryComponent>();
        const LabHealthComponent* Health = InEntity.GetComponent<LabHealthComponent>();
        if(Health && (Health->bDead || Health->bDown))
        {
            InCombat.MeleePhase = 0; InCombat.bReloading = 0; InCombat.BurstLeft = 0;
            return;
        }

        /* Spread recovers toward the weapon's minimum. */
        const uint8_t Held = Inventory ? Inventory->EquippedWeapon() : uint8_t(0);
        const LabWeaponComponent* Weapon = Held ? LabWeapons::GetData(InWorld, Held) : nullptr;
        if(Weapon) { InCombat.Spread = std::max(Weapon->SpreadMin, InCombat.Spread - Weapon->SpreadRecovery * InSeconds); }
        else { InCombat.Spread = 0.f; }

        if(InCombat.bReloading && Inventory && Weapon)
        {
            InCombat.ReloadTimer -= InSeconds;
            if(InCombat.ReloadTimer <= 0.f)
            {
                const int Slot = Inventory->Equipped;
                const int32_t Want = Weapon->MagSize - Inventory->MagAt(Slot);
                const int32_t Take = std::max(0, std::min(Want, Inventory->ReserveAt(Slot)));
                Inventory->MagAt(Slot) += Take;
                Inventory->ReserveAt(Slot) -= Take;
                InCombat.bReloading = 0;
            }
        }

        if(InCombat.BurstLeft > 0 && InCombat.Cooldown <= 0.f) { Bursts.push_back(Avatar); }

        if(InCombat.MeleePhase != 0)
        {
            const LabWeaponComponent& Swing = MeleeData(InWorld, Avatar);
            InCombat.MeleeTimer -= InSeconds;
            if(InCombat.MeleePhase == 2) { Sweeps.push_back(Avatar); }
            if(InCombat.MeleeTimer <= 0.f)
            {
                if(InCombat.MeleePhase == 1) { InCombat.MeleePhase = 2; InCombat.MeleeTimer = Swing.Active; Sweeps.push_back(Avatar); }
                else if(InCombat.MeleePhase == 2) { InCombat.MeleePhase = 3; InCombat.MeleeTimer = Swing.Recovery; }
                else { InCombat.MeleePhase = 0; InCombat.ComboIndex = 0; }
            }
        }
    });

    for(const EntityID Avatar : Sweeps)
    {
        if(InWorld.IsAlive(Avatar)) { SweepMelee(InWorld, Avatar); }
    }
    for(const EntityID Avatar : Bursts)
    {
        Vector3 Origin, Dir;
        LabCombatComponent* Combat = InWorld.IsAlive(Avatar) ? InWorld.GetEntity(Avatar).GetComponent<LabCombatComponent>() : nullptr;
        if(!Combat || !GetCharacterAim(InWorld, Avatar, Origin, Dir)) { continue; }
        --Combat->BurstLeft;
        Fire(InWorld, Avatar, nullptr, Origin, Dir, uint8_t(Combat->AttackSeq + 1));
    }
}

// === Presentation ===========================================================

EntityID LabCombat::HeldInstance(World& InWorld, const EntityID InAvatar)
{
    const Entity E = InWorld.GetEntity(InAvatar);
    const LabInventoryComponent* Inventory = E.IsAlive() ? E.GetComponent<LabInventoryComponent>() : nullptr;
    if(!Inventory || Inventory->Equipped >= LabWeapons::SLOTS) { return EntityID(); }
    char Name[16];
    std::snprintf(Name, sizeof(Name), "Weapon_%d", int(Inventory->Equipped));
    return FindChild(InWorld, InAvatar, Name);
}

namespace
{
    void DestroyWeaponInstance(World& InWorld, const EntityID InRoot)
    {
        if(!InRoot.IsSet() || !InWorld.IsAlive(InRoot)) { return; }
        const Entity Root = InWorld.GetEntity(InRoot);
        if(Root.GetInstance().IsSet()) { InWorld.DestroyPrefabInstance(Root.GetInstance()); }
        else { InWorld.DestroyEntity(InRoot); }
    }

    void Seat(World& InWorld, const EntityID InInstance, const LabWeaponComponent& InWeapon, const bool bInHand)
    {
        RenderCore::AnimSocketComponent* Socket = InWorld.GetEntity(InInstance).GetComponent<RenderCore::AnimSocketComponent>();
        if(!Socket) { return; }
        const char* Bone = bInHand ? InWeapon.HandBone : InWeapon.HolsterBone;
        Socket->PositionOffset = bInHand ? InWeapon.HandPos : InWeapon.HolsterPos;
        Socket->RotationOffset = bInHand ? InWeapon.HandRot : InWeapon.HolsterRot;
        if(std::strcmp(Socket->Bone, Bone) != 0) { std::snprintf(Socket->Bone, sizeof(Socket->Bone), "%s", Bone); }
    }
}

namespace
{
    Vector3 Lerp3(const Vector3& InA, const Vector3& InB, const double InT) { return InA + (InB - InA) * InT; }

    float Approach(const float InValue, const float InTarget, const float InStep)
    {
        return InValue < InTarget ? std::min(InTarget, InValue + InStep) : std::max(InTarget, InValue - InStep);
    }

    /*
     * The weapon hand, posed by the AimHand IK (Tools/AnimStateTool): the
     * CrashDummy has no aim, recoil, reload or swing clips, so the right hand
     * is placed from replicated state instead, on every process:
     *
     *   aim     raised along the camera ray while scoped, or just after a shot
     *           when the body faces the view; a kick back at each shot
     *   reload  dipped to the belly while bReloading
     *   melee   an arc across the body over the swing's windup, active and
     *           recovery times (a combo alternates sides)
     *   stagger hands up, guarding, while StaggerFrames last
     *   hit     a jolt back at each hit taken
     *
     * Weight 0 leaves the arm as animated. The off hand follows the right
     * one through the OffHand IK when the weapon is two-handed.
     */
    void PoseWeaponHand(World& InWorld, const EntityID InAvatar, LabCombat::Presented& P, const float InSeconds)
    {
        RenderCore::AnimationSystem* Animation = RenderCore::AnimationSystem::Find(&InWorld);
        RenderCore::AnimInstance* Instance = Animation ? Animation->FindInstance(InAvatar) : nullptr;
        Entity Self = InWorld.GetEntity(InAvatar);
        const CharacterMoverComponent* Mover = Self.GetComponent<CharacterMoverComponent>();
        const LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
        const LabHealthComponent* Health = Self.GetComponent<LabHealthComponent>();
        if(!Instance || !Mover || !Combat) { return; }
        if(Health && Health->bDead)
        {
            Instance->SetIKGoal("AimHand", RenderCore::AnimVec3(), 0.f);
            return;
        }

        const LabWeaponComponent* HeldPtr = LabCombat::EquippedData(InWorld, InAvatar);
        const LabWeaponComponent Held = HeldPtr ? *HeldPtr : GFists;
        Self = InWorld.GetEntity(InAvatar);
        Mover = Self.GetComponent<CharacterMoverComponent>();
        Combat = Self.GetComponent<LabCombatComponent>();
        const bool bRanged = HeldPtr && LabWeapons::TypeOf(Held) != LabWeapons::EWeaponType::EWT_Melee;

        /* Counters: a shot kicks; a swing starts its arc; a hit jolts. */
        if(Combat->AttackSeq != P.IKAttackSeq)
        {
            if(P.bSeenInit)
            {
                if(bRanged) { P.Recoil = 1.f; P.FireHold = 0.9f; }
                else
                {
                    const LabWeaponComponent& Swing = HeldPtr ? Held : GFists;
                    P.SwingWindup = Swing.Windup;
                    P.SwingActive = Swing.Active;
                    P.SwingLength = Swing.Windup + Swing.Active + Swing.Recovery;
                    P.SwingT = 0.f;
                    P.SwingSide = (Combat->ComboIndex % 2 == 0) ? -1.f : 1.f;
                }
            }
            P.IKAttackSeq = Combat->AttackSeq;
        }
        if(Combat->ThrowSeq != P.IKThrowSeq)
        {
            if(P.bSeenInit) { P.ThrowT = 0.f; }
            P.IKThrowSeq = Combat->ThrowSeq;
        }
        if(Combat->HitSeq != P.IKHitSeq)
        {
            if(P.bSeenInit) { P.Flinch = 1.f; }
            P.IKHitSeq = Combat->HitSeq;
        }
        P.Recoil   = std::max(0.f, P.Recoil - InSeconds * 8.f);
        P.Flinch   = std::max(0.f, P.Flinch - InSeconds * 5.f);
        P.FireHold = std::max(0.f, P.FireHold - InSeconds);
        if(P.SwingT >= 0.f) { P.SwingT += InSeconds; if(P.SwingT > P.SwingLength) { P.SwingT = -1.f; } }
        if(P.ThrowT >= 0.f) { P.ThrowT += InSeconds; if(P.ThrowT > 0.45f) { P.ThrowT = -1.f; } }

        /* The frame: the camera's ray, the body's facing. */
        constexpr double RAD = 3.14159265358979 / 180.0;
        Vector3 Origin, Dir;
        if(!GetCharacterAim(InWorld, InAvatar, Origin, Dir)) { return; }
        const double Yaw = double(Mover->CameraYaw) * RAD;
        const Vector3 Right(std::cos(Yaw), 0, -std::sin(Yaw));
        const Vector3 Up(0, 1, 0);
        const double BodyYaw = double(Mover->BodyYaw) * RAD;
        const Vector3 Facing(std::sin(BodyYaw), 0, std::cos(BodyYaw));
        const Vector3 FacingRight(std::cos(BodyYaw), 0, -std::sin(BodyYaw));
        const Vector3 Chest = Self.GetWorldPose().Position + Vector3(0, Mover->bCrouched ? 95.0 : 135.0, 0);
        double ViewOff = std::fabs(double(Mover->CameraYaw) - double(Mover->BodyYaw));
        while(ViewOff > 180.0) { ViewOff = std::fabs(ViewOff - 360.0); }

        const bool bAiming = bRanged && !Combat->bReloading
                          && (Mover->Scope > 0.2f || (P.FireHold > 0.f && ViewOff < 50.0));
        P.AimBlend    = Approach(P.AimBlend, bAiming ? 1.f : 0.f, InSeconds * 8.f);
        P.ReloadBlend = Approach(P.ReloadBlend, bRanged && Combat->bReloading ? 1.f : 0.f, InSeconds * 6.f);

        Vector3 Goal = Chest + Dir * 48.0 + Right * 10.0 + Up * 4.0;
        float Weight = P.AimBlend;
        Goal = Goal - Dir * (7.0 * P.Recoil) + Up * (3.0 * P.Recoil);

        if(P.ReloadBlend > 0.f)
        {
            const Vector3 Dip = Chest + Facing * 30.0 - Up * 28.0 + FacingRight * 6.0;
            Goal = Lerp3(Goal, Dip, double(P.ReloadBlend));
            Weight = std::max(Weight, P.ReloadBlend);
        }

        if(P.SwingT >= 0.f && P.SwingLength > 0.f)
        {
            /* Cocked high on the swing side, through the front, down across. */
            const double Side = double(P.SwingSide);
            const Vector3 Cocked = Chest + FacingRight * (32.0 * Side) + Up * 28.0 - Facing * 4.0;
            const Vector3 Front  = Chest + Facing * 58.0 + Up * 6.0;
            const Vector3 Finish = Chest - FacingRight * (30.0 * Side) - Up * 22.0 + Facing * 34.0;
            const float T = P.SwingT;
            float W = 1.f;
            Vector3 Arc;
            if(T < P.SwingWindup)
            {
                const double U = P.SwingWindup > 0.f ? double(T / P.SwingWindup) : 1.0;
                Arc = Cocked;
                W = float(U);
            }
            else if(T < P.SwingWindup + P.SwingActive)
            {
                const double U = P.SwingActive > 0.f ? double((T - P.SwingWindup) / P.SwingActive) : 1.0;
                Arc = Lerp3(Lerp3(Cocked, Front, U), Lerp3(Front, Finish, U), U);   // a quadratic through the front
            }
            else
            {
                const double Rest = double(P.SwingLength - P.SwingWindup - P.SwingActive);
                const double U = Rest > 0.0 ? double(T - P.SwingWindup - P.SwingActive) / Rest : 1.0;
                Arc = Finish;
                W = float(1.0 - U);
            }
            Goal = Arc;
            Weight = std::max(Weight, W);
        }

        /* A throw: wound back high while the owner charges, then over the top. */
        const Vector3 WoundUp = Chest + Up * 30.0 - Facing * 22.0 + FacingRight * 18.0;
        if(Combat->bCharging)
        {
            Goal = WoundUp;
            Weight = 1.f;
        }
        if(P.ThrowT >= 0.f)
        {
            const double U = std::clamp(double(P.ThrowT) / 0.45, 0.0, 1.0);
            const Vector3 Release = Chest + Up * 20.0 + Facing * 50.0;
            const Vector3 Follow  = Chest - Up * 10.0 + Facing * 40.0 - FacingRight * 10.0;
            Goal = U < 0.5 ? Lerp3(WoundUp, Release, U * 2.0) : Lerp3(Release, Follow, (U - 0.5) * 2.0);
            Weight = float(U < 0.8 ? 1.0 : (1.0 - U) * 5.0);
        }

        if(Combat->StaggerFrames > 0)
        {
            Goal = Chest + Up * 32.0 + Facing * 18.0 - FacingRight * 4.0;
            Weight = 1.f;
        }
        if(P.Flinch > 0.f && Weight > 0.f)
        {
            Goal = Goal - Facing * (12.0 * P.Flinch) + Up * (6.0 * P.Flinch);
        }

        Instance->SetIKGoal("AimHand", RenderCore::AnimVec3{ float(Goal.GetX()), float(Goal.GetY()), float(Goal.GetZ()) },
                            std::clamp(Weight, 0.f, 1.f));
    }
}

void LabCombat::TickPresentation(World& InWorld, const float InSeconds, PresentedMap& InOutPresented)
{
    /* Transient effects run out. */
    {
        std::vector<EntityID> Expired;
        InWorld.Each<LabLifetimeComponent>([&](Entity InEntity, LabLifetimeComponent& InLife)
        {
            InLife.Seconds -= InSeconds;
            if(InLife.Seconds <= 0.f) { Expired.push_back(InEntity.GetID()); }
        });
        for(const EntityID E : Expired) { InWorld.DestroyEntity(E); }
    }

    std::vector<EntityID> Avatars;
    InWorld.Each<LabInventoryComponent>([&](Entity InEntity, LabInventoryComponent&) { Avatars.push_back(InEntity.GetID()); });

    for(const EntityID Avatar : Avatars)
    {
        Presented& P = InOutPresented[Avatar.Pack()];

        /* 1. Spawn and destroy: one instance per held slot, a direct child of
         *    the animated root (the socket reads its parent's pose). */
        for(int Slot = 0; Slot < LabWeapons::SLOTS; ++Slot)
        {
            const LabInventoryComponent* Inventory = InWorld.GetEntity(Avatar).GetComponent<LabInventoryComponent>();
            const uint8_t ID = Inventory ? Inventory->WeaponAt(Slot) : uint8_t(0);
            const bool bLive = P.Instance[Slot].IsSet() && InWorld.IsAlive(P.Instance[Slot]);
            if(P.ID[Slot] == ID && (bLive || ID == 0)) { continue; }

            DestroyWeaponInstance(InWorld, P.Instance[Slot]);
            P.Instance[Slot] = EntityID();
            P.ID[Slot] = ID;
            const LabWeapons::Entry* Entry = LabWeapons::Find(ID);
            if(!Entry) { continue; }

            char Name[16];
            std::snprintf(Name, sizeof(Name), "Weapon_%d", Slot);
            PrefabInstance Instance = InWorld.SpawnPrefab(Entry->Prefab, TransformPose(), Name);
            if(!Instance.IsValid()) { continue; }
            Entity Root = Instance.GetRoot();
            Root.SetParent(Avatar, false);
            P.Instance[Slot] = Root.GetID();
        }

        Entity Self = InWorld.GetEntity(Avatar);
        const LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
        const LabHealthComponent* Health = Self.GetComponent<LabHealthComponent>();
        const bool bDead = Health && Health->bDead;

        /* 2. Sockets: the equipped weapon in the hand, the rest holstered.
         *    A dead character's props stay where the ragdoll dropped them. */
        uint8_t AnimSet = uint8_t(LabWeapons::EAnimSet::EAS_UnArmed);
        for(int Slot = 0; Slot < LabWeapons::SLOTS && Inventory && !bDead; ++Slot)
        {
            if(!P.Instance[Slot].IsSet()) { continue; }
            const LabWeaponComponent* Weapon = LabWeapons::GetData(InWorld, P.ID[Slot]);
            if(!Weapon) { continue; }
            const LabWeaponComponent Copy = *Weapon;
            const bool bHand = Inventory->Equipped == Slot;
            if(bHand) { AnimSet = Copy.AnimSet; }
            Seat(InWorld, P.Instance[Slot], Copy, bHand);
            Inventory = InWorld.GetEntity(Avatar).GetComponent<LabInventoryComponent>();
        }

        /* 3. Anim params, from replicated state. Triggers fire on counter
         *    changes; the owner already fired Attack when it pressed. */
        Self = InWorld.GetEntity(Avatar);
        const LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
        Health = Self.GetComponent<LabHealthComponent>();
        const AvatarHarnessComponent* Harness = Self.GetComponent<AvatarHarnessComponent>();
        const bool bOwner = Harness && Harness->IsLocallyControlled();
        /* A character's speed scale, on processes that did not spawn it
         * (the mover's tunables are not replicated). */
        if(!P.bSeenInit && !HasAuthority())
        {
            const LabCharacterComponent* Character = Self.GetComponent<LabCharacterComponent>();
            CharacterMoverComponent* Mover = Self.GetComponent<CharacterMoverComponent>();
            if(Character && Mover && Character->SpeedScale != 1.0f)
            {
                Mover->WalkSpeed   *= Character->SpeedScale;
                Mover->RunSpeed    *= Character->SpeedScale;
                Mover->CrouchSpeed *= Character->SpeedScale;
                Mover->AimSpeed    *= Character->SpeedScale;
            }
        }
        if(RenderCore::AnimationComponent* Anim = Self.GetComponent<RenderCore::AnimationComponent>())
        {
            /* Armed draws; OneHand frees the off arm (a pistol, a wrench);
             * TwoHand puts the off hand on the weapon (the off-hand IK). */
            const LabWeapons::EAnimSet Set = LabWeapons::EAnimSet(AnimSet);
            const bool bTwoHand = Set == LabWeapons::EAnimSet::EAS_TwoHanded || Set == LabWeapons::EAnimSet::EAS_Rifle
                               || Set == LabWeapons::EAnimSet::EAS_Heavy;
            SetAnimBool(InWorld, Avatar, *Anim, "Armed", Set != LabWeapons::EAnimSet::EAS_UnArmed);
            SetAnimBool(InWorld, Avatar, *Anim, "OneHand", Set != LabWeapons::EAnimSet::EAS_UnArmed && !bTwoHand);
            SetAnimBool(InWorld, Avatar, *Anim, "TwoHand", bTwoHand);
            SetAnimFloat(InWorld, Avatar, *Anim, "WeaponSet", float(AnimSet));
            if(Combat)
            {
                SetAnimFloat(InWorld, Avatar, *Anim, "Combo", float(Combat->ComboIndex));
                if(P.bSeenInit)
                {
                    if(Combat->AttackSeq != P.SeenAttackSeq && !bOwner) { TriggerAnim(InWorld, Avatar, "Attack"); }
                    if(Combat->ReloadSeq != P.SeenReloadSeq) { TriggerAnim(InWorld, Avatar, "Reload"); }
                    if(Combat->HitSeq != P.SeenHitSeq)
                    {
                        TriggerAnim(InWorld, Avatar, "Hit");
                        if(Combat->LastHitHeavy) { P.StaggerTimer = 0.6f; }
                    }
                }
                if(Combat->ReloadSeq != P.SeenReloadSeq && P.bSeenInit)
                {
                    PlayAt(InWorld, SND_RELOAD, Self.GetWorldPose().Position, Avatar);
                }
                P.SeenAttackSeq = Combat->AttackSeq;
                P.SeenReloadSeq = Combat->ReloadSeq;
                P.SeenHitSeq    = Combat->HitSeq;
            }
            P.StaggerTimer = std::max(0.f, P.StaggerTimer - InSeconds);
            SetAnimBool(InWorld, Avatar, *Anim, "Stagger", P.StaggerTimer > 0.f);
            SetAnimBool(InWorld, Avatar, *Anim, "Downed", Health && Health->bDown);
        }
        P.bSeenInit = 1;

        /* 4. The weapon hand: the AimHand IK goal, from replicated state. */
        PoseWeaponHand(InWorld, Avatar, P, InSeconds);

        /* 5. The ragdoll follows the replicated bDead: late joiners see the
         *    corpse; a revive stands it back up. */
        if(Health)
        {
            RagdollComponent* Ragdoll = InWorld.GetEntity(Avatar).GetComponent<RagdollComponent>();
            if(Health->bDead && !P.bShownDead)
            {
                if(Ragdoll && !Ragdoll->IsActive()) { Ragdoll->Activate(); }
                P.bShownDead = 1;
            }
            else if(!Health->bDead && P.bShownDead)
            {
                if(Ragdoll) { Ragdoll->Reset(); }
                P.bShownDead = 0;
            }
        }
    }

    /* 6. Forget avatars that are gone; their weapon instances, children,
     *    went with them. */
    for(auto It = InOutPresented.begin(); It != InOutPresented.end();)
    {
        if(!InWorld.IsAlive(EntityID::Unpack(It->first))) { It = InOutPresented.erase(It); }
        else { ++It; }
    }
}

void LabCombat::ClearTransients(World& InWorld, PresentedMap& InOutPresented)
{
    std::vector<EntityID> Doomed;
    InWorld.Each<LabWeaponComponent>([&](Entity InEntity, LabWeaponComponent&)
    {
        const EntityID Parent = InEntity.GetParent();
        if(Parent.IsSet() && InWorld.GetEntity(Parent).GetComponent<LabInventoryComponent>()) { Doomed.push_back(InEntity.GetID()); }
        else if(std::strcmp(InEntity.GetName(), "LabWeaponDefinition") == 0) { Doomed.push_back(InEntity.GetID()); }
    });
    InWorld.Each<LabLifetimeComponent>([&](Entity InEntity, LabLifetimeComponent&) { Doomed.push_back(InEntity.GetID()); });
    InWorld.Each<LabDebrisComponent>([&](Entity InEntity, LabDebrisComponent&) { Doomed.push_back(InEntity.GetID()); });
    InWorld.Each<LabProjectileComponent>([&](Entity InEntity, LabProjectileComponent&) { Doomed.push_back(InEntity.GetID()); });
    /* The arc preview's dots, by name: they carry no component of their own. */
    struct Named { World* W; std::vector<EntityID>* Out; };
    Named Ctx{ &InWorld, &Doomed };
    InWorld.ForEachEntity([](EntityID InEntity, void* InUserData)
    {
        Named& C = *static_cast<Named*>(InUserData);
        if(std::strcmp(C.W->GetEntity(InEntity).GetName(), "LabArcDot") == 0) { C.Out->push_back(InEntity); }
        else if(LabArmour::IsTransient(*C.W, InEntity)) { C.Out->push_back(InEntity); }   // a saved Brute's plates
    }, &Ctx);
    for(const EntityID E : Doomed) { DestroyWeaponInstance(InWorld, E); }
    GArcDots.erase(&InWorld);
    InOutPresented.clear();
}

void LabCombat::ClearPresentation(World& InWorld, PresentedMap& InOutPresented)
{
    for(const EntityID Dot : GArcDots[&InWorld]) { if(InWorld.IsAlive(Dot)) { InWorld.DestroyEntity(Dot); } }
    GArcDots.erase(&InWorld);
    for(auto& Pair : InOutPresented)
    {
        for(const EntityID Instance : Pair.second.Instance) { DestroyWeaponInstance(InWorld, Instance); }
    }
    InOutPresented.clear();
}
