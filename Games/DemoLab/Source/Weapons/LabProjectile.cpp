#include "Weapons/LabProjectile.h"
#include "LabVersion.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/LagCompensation.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/RenderCore/Components/RenderComponent.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "Core/LabHealth.h"
#include "Weapons/LabCombat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace GameCore;
using namespace NimbleEngine;
using EngineCore::Vector3;

// === Component ==============================================================

GAMECORE_DEFINE_COMPONENT(LabProjectileComponent, "LabProjectile")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabProjectileComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Gravity", C.Gravity);
    Ar.Field("Drag", C.Drag);
    Ar.Field("Radius", C.Radius);
    Ar.Field("Fuse", C.Fuse);
    Ar.Field("Bounces", C.Bounces);
    Ar.Field("Restitution", C.Restitution);
    Ar.Field("Life", C.Life);
    Ar.Field("SpinRate", C.SpinRate);
    Ar.Field("BurstFx", C.BurstFx);
    Ar.Field("BurstSound", C.BurstSound);
    Ar.Entity("Instigator", C.Instigator);
    Ar.Field("WeaponID", C.WeaponID);
    Ar.Field("Damage", C.Damage);
    Ar.Field("Impulse", C.Impulse);
    Ar.Flag("Staggers", C.bStaggers);
    Ar.Field("Velocity", C.Velocity);
    Ar.Field("Age", C.Age);
    Ar.Flag("Resting", C.bResting);
}

#if WITH_NETCORE
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabProjectileComponent) {
    Rep.Field("Velocity", &LabProjectileComponent::Velocity).Precision(0.1f);
    Rep.Field("Gravity", &LabProjectileComponent::Gravity, RFF_InitialOnly);
    Rep.Field("Drag", &LabProjectileComponent::Drag, RFF_InitialOnly);
    Rep.Field("SpinRate", &LabProjectileComponent::SpinRate, RFF_InitialOnly);
    Rep.Entity("Instigator", &LabProjectileComponent::Instigator, RFF_InitialOnly);
    Rep.Flag("Resting", &LabProjectileComponent::bResting);
}
#endif

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabProjectileComponent) {
    Props.TypeCategory("DemoLab");
    Props.Category("Flight");
    Props.Field("Gravity", &LabProjectileComponent::Gravity);
    Props.Field("Drag", &LabProjectileComponent::Drag);
    Props.Field("Life", &LabProjectileComponent::Life);
    Props.Field("SpinRate", &LabProjectileComponent::SpinRate);
    Props.Category("Impact");
    Props.Field("Radius", &LabProjectileComponent::Radius);
    Props.Field("Fuse", &LabProjectileComponent::Fuse);
    Props.Field("Bounces", &LabProjectileComponent::Bounces);
    Props.Field("Restitution", &LabProjectileComponent::Restitution);
    Props.Category("Presentation");
    Props.Field("BurstFx", &LabProjectileComponent::BurstFx, PF_AssetParticleSystem);
    Props.Field("BurstSound", &LabProjectileComponent::BurstSound, PF_AssetSound);
}
#endif

/* The base every projectile forks: a networked, low-priority body that
 * draws a small sphere until a fork gives it its own mesh. */
static void BuildLabProjectile(PrefabBuilder& Builder)
{
    PrefabBuilder::Node Root = Builder.Root("Projectile");
    Root.Add<TransformComponent>();

    NetComponent Net;
    Net.Priority = 0;   // effects-grade: everything else goes first
    Root.Add(Net);

    RenderCore::RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", "Engine/Content/Meshes/Sphere.nmmesh");
    Root.Add(Render);

    Root.Add<LabProjectileComponent>();
}

GAMECORE_DEFINE_NATIVE_PREFAB(LabProjectile, "DemoLab.Projectile", &BuildLabProjectile)

// === Flight =================================================================

namespace
{
    constexpr double PI_D = 3.14159265358979;

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

    void Hurt(World& InWorld, const LabProjectileComponent& InShot, const EntityID InVictim, const float InAmount,
              const Vector3& InAt, const Vector3& InDir, const EDamageType InType)
    {
        LabDamageEvent Event;
        Event.Victim     = InVictim;
        Event.Instigator = InShot.Instigator;
        Event.WeaponID   = InShot.WeaponID;
        Event.Amount     = InAmount;
        Event.DamageType = InType;
        Event.Point      = InAt;
        Event.Dir        = InDir.GetLength() > 1e-4 ? InDir.GetNormalize() : Vector3(0, 1, 0);
        Event.Impulse    = InShot.Impulse * 20.f;
        Event.bStaggers  = InShot.bStaggers;
        const float Dealt = LabDamage::Apply(InWorld, Event);
        if(Dealt > 0.f && InWorld.IsAlive(InShot.Instigator))
        {
            const LabHealthComponent* Health = InWorld.GetEntity(InVictim).GetComponent<LabHealthComponent>();
            GAMECORE_RPC(LabCombatComponent, ClientHitConfirm)(InWorld.GetEntity(InShot.Instigator), Dealt,
                                                                Health && Health->bDead, false);
        }
    }

    /* The burst: the direct victim, then everything within Radius that the
     * blast can see, by distance. */
    void Burst(World& InWorld, const EntityID InProjectile, const Vector3& InAt, const EntityID InDirect)
    {
        const LabProjectileComponent* Found = InWorld.GetEntity(InProjectile).GetComponent<LabProjectileComponent>();
        if(!Found) { return; }
        const LabProjectileComponent Shot = *Found;

        if(Shot.Radius <= 0.f)
        {
            if(InDirect.IsSet()) { Hurt(InWorld, Shot, InDirect, Shot.Damage, InAt, Shot.Velocity, EDamageType::EDT_Bullet); }
        }
        else
        {
            struct Victim { EntityID Id; Vector3 Chest; double Distance; };
            std::vector<Victim> Victims;
            InWorld.Each<LabHealthComponent>([&](Entity InEntity, LabHealthComponent& InHealth)
            {
                if(InHealth.bDead) { return; }
                const Vector3 Chest = InEntity.GetWorldPose().Position + Vector3(0, 100, 0);
                const double D = (Chest - InAt).GetLength();
                if(D <= double(Shot.Radius) + 40.0) { Victims.push_back({ InEntity.GetID(), Chest, D }); }
            });
            for(const Victim& V : Victims)
            {
                /* Line of sight from the blast: anything else in between shields. */
                const Vector3 Ray = V.Chest - InAt;
                PhysicsRaycastHit Block;
                if(V.Distance > 1.0 && PhysicsRaycast(InWorld, InAt, Ray, float(V.Distance), Block, LAB_WORLD_MASK)
                   && HealthOwner(InWorld, Block.Entity) != V.Id && Block.Distance < float(V.Distance) - 20.f)
                {
                    continue;
                }
                const double Falloff = std::clamp(1.0 - V.Distance / double(Shot.Radius), 0.15, 1.0);
                const float Amount = V.Id == InDirect ? Shot.Damage : float(double(Shot.Damage) * Falloff);
                Hurt(InWorld, Shot, V.Id, Amount, V.Chest, Ray, EDamageType::EDT_Explosion);
            }
        }

        LabCombat::BurstFx(InWorld, Shot.Instigator, InAt, Shot.BurstFx, Shot.BurstSound, Shot.Radius);
        LabCombat::ReportNoise(InWorld, Shot.Instigator, InAt, Shot.Radius > 0.f ? 2500.f : 900.f);
        InWorld.DestroyEntity(InProjectile);
    }
}

EntityID LabProjectiles::Launch(World& InWorld, const EntityID InInstigator, const uint8_t InWeaponID,
                                 const LabWeaponComponent& InWeapon, const Vector3& InOrigin, const Vector3& InDir, const float InSpeed)
{
    if(!HasAuthority() || !InWeapon.ProjectilePrefab[0]) { return EntityID(); }
    const Vector3 Dir = InDir.GetLength() > 1e-4 ? InDir.GetNormalize() : Vector3(0, 0, 1);

    TransformPose Pose;
    Pose.Position = InOrigin;
    Pose.Rotation = Vector3(-std::asin(std::clamp(double(Dir.GetY()), -1.0, 1.0)) * 180.0 / PI_D,
                            std::atan2(Dir.GetX(), Dir.GetZ()) * 180.0 / PI_D, 0.0);
    PrefabInstance Spawned = InWorld.SpawnPrefab(InWeapon.ProjectilePrefab, Pose, "LabProjectile");
    if(!Spawned.IsValid())
    {
        CORE_LOG_WARN(std::string("[DemoLab] cannot spawn projectile ") + InWeapon.ProjectilePrefab)
        return EntityID();
    }
    LabProjectileComponent* Shot = Spawned.GetRoot().GetComponent<LabProjectileComponent>();
    if(!Shot)
    {
        CORE_LOG_WARN(std::string("[DemoLab] ") + InWeapon.ProjectilePrefab + " has no LabProjectile on its root")
        Spawned.GetRoot().Destroy();
        return EntityID();
    }
    Shot->Instigator = InInstigator;
    Shot->WeaponID   = InWeaponID;
    Shot->Damage     = InWeapon.Damage;
    Shot->Impulse    = InWeapon.Impulse;
    Shot->bStaggers  = InWeapon.bStaggers;
    Shot->Velocity   = Dir * double(InSpeed);
    return Spawned.GetRoot().GetID();
}

void LabProjectiles::TickServer(World& InWorld, const float InSeconds)
{
    if(!HasAuthority() || InSeconds <= 0.f) { return; }

    struct PendingBurst { EntityID Projectile; Vector3 At; EntityID Direct; };
    std::vector<PendingBurst> Bursts;
    std::vector<EntityID> Expired;

    InWorld.Each<LabProjectileComponent>([&](Entity InEntity, LabProjectileComponent& InShot)
    {
        TransformComponent* Transform = InEntity.GetComponent<TransformComponent>();
        if(!Transform) { return; }

        InShot.Age += InSeconds;
        if(InShot.Age >= InShot.Life || (InShot.Fuse >= 0.f && InShot.Age >= InShot.Fuse))
        {
            if(InShot.Radius > 0.f || InShot.Fuse >= 0.f) { Bursts.push_back({ InEntity.GetID(), Transform->Position, EntityID() }); }
            else { Expired.push_back(InEntity.GetID()); }
            return;
        }
        if(InShot.bResting) { return; }

        /* Integrate, then trace the step: the world, and every character where it
         * stands now (no rewind: the server simulates projectiles). */
        InShot.Velocity.SetY(InShot.Velocity.GetY() - double(InShot.Gravity * InSeconds));
        if(InShot.Drag > 0.f) { InShot.Velocity = InShot.Velocity * double(std::max(0.f, 1.f - InShot.Drag * InSeconds)); }
        const Vector3 Step = InShot.Velocity * double(InSeconds);
        const double Length = Step.GetLength();
        if(Length < 1e-6) { return; }

        LagCompensatedHit Hit;
        if(!LagCompensatedRaycastAt(InWorld, 0, InShot.Instigator, Transform->Position, Step, float(Length), Hit, LAB_SHOT_MASK))
        {
            Transform->Position = Transform->Position + Step;
            if(InShot.SpinRate != 0.f) { Transform->Rotation.SetX(Transform->Rotation.GetX() + double(InShot.SpinRate * InSeconds)); }
            return;
        }

        const EntityID Victim = HealthOwner(InWorld, Hit.Entity);
        const Vector3 Dir = Step * (1.0 / Length);
        if(Victim.IsSet() && Victim != InShot.Instigator)
        {
            Bursts.push_back({ InEntity.GetID(), Hit.Point - Dir * 2.0, Victim });
            return;
        }

        /* The world: bounce while it has bounces, then burst -- or, with a
         * fuse, come to rest and wait for it. */
        if(InShot.Bounces > 0)
        {
            --InShot.Bounces;
            PhysicsRaycastHit Surface;
            Vector3 Normal(0, 1, 0);
            if(PhysicsRaycast(InWorld, Transform->Position, Step, float(Length) + 5.f, Surface, LAB_WORLD_MASK, InShot.Instigator)
               && Surface.Normal.GetLength() > 0.5)
            {
                Normal = Surface.Normal.GetNormalize();
            }
            const Vector3 V = InShot.Velocity;
            const Vector3 Reflected = V - Normal * (2.0 * V.Dot(Normal));
            InShot.Velocity = Reflected * double(InShot.Restitution);
            Transform->Position = Hit.Point + Normal * 3.0;
            return;
        }
        if(InShot.Fuse >= 0.f)
        {
            Transform->Position = Hit.Point - Dir * 3.0;
            InShot.Velocity = Vector3();
            InShot.bResting = 1;
            return;
        }
        Bursts.push_back({ InEntity.GetID(), Hit.Point - Dir * 3.0, EntityID() });
    });

    for(const EntityID E : Expired) { InWorld.DestroyEntity(E); }
    for(const PendingBurst& B : Bursts)
    {
        if(InWorld.IsAlive(B.Projectile)) { Burst(InWorld, B.Projectile, B.At, B.Direct); }
    }
}

void LabProjectiles::TickClient(World& InWorld, const float InSeconds)
{
    if(HasAuthority()) { return; }
    InWorld.Each<LabProjectileComponent>([&](Entity InEntity, LabProjectileComponent& InShot)
    {
        TransformComponent* Transform = InEntity.GetComponent<TransformComponent>();
        if(!Transform || InShot.bResting) { return; }
        InShot.Velocity.SetY(InShot.Velocity.GetY() - double(InShot.Gravity * InSeconds));
        Transform->Position = Transform->Position + InShot.Velocity * double(InSeconds);
        if(InShot.SpinRate != 0.f) { Transform->Rotation.SetX(Transform->Rotation.GetX() + double(InShot.SpinRate * InSeconds)); }
    });
}

int LabProjectiles::PredictArc(World& InWorld, const EntityID InIgnore, const Vector3& InOrigin, const Vector3& InVelocity,
                                const float InGravity, const float InStep, Vector3* OutPoints, const int InMax)
{
    Vector3 At = InOrigin;
    Vector3 V = InVelocity;
    int Count = 0;
    for(int I = 0; I < InMax; ++I)
    {
        V.SetY(V.GetY() - double(InGravity * InStep));
        const Vector3 Step = V * double(InStep);
        PhysicsRaycastHit Hit;
        if(PhysicsRaycast(InWorld, At, Step, float(Step.GetLength()), Hit, LAB_WORLD_MASK, InIgnore))
        {
            OutPoints[Count++] = Hit.Point;
            break;
        }
        At = At + Step;
        OutPoints[Count++] = At;
    }
    return Count;
}

Vector3 LabProjectiles::ThrowVelocity(const Vector3& InAim, const LabWeaponComponent& InWeapon, const float InCharge)
{
    /* Pitched up 8°: about the right side axis, toward +Y. */
    Vector3 Aim = InAim.GetLength() > 1e-4 ? InAim.GetNormalize() : Vector3(0, 0, 1);
    const double Flat = std::sqrt(Aim.GetX() * Aim.GetX() + Aim.GetZ() * Aim.GetZ());
    const double Pitch = std::atan2(Aim.GetY(), Flat) + 8.0 * PI_D / 180.0;
    const double Yaw = std::atan2(Aim.GetX(), Aim.GetZ());
    const Vector3 Dir(std::sin(Yaw) * std::cos(Pitch), std::sin(Pitch), std::cos(Yaw) * std::cos(Pitch));
    const float Speed = InWeapon.ThrowSpeedMax * (0.4f + 0.6f * std::clamp(InCharge, 0.f, 1.f));
    return Dir * double(Speed);
}
