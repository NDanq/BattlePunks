#include "Weapons/LabWeapon.h"
#include "LabVersion.h"

#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/Core/CoreLog.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/RenderCore/Components/AnimSocketComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include <cstdio>
#include <cstring>
#include <string>

using namespace GameCore;
using namespace NimbleEngine;

// === Registry ===============================================================

namespace
{
    /* Ids are forever: append only, never reuse one. */
    const LabWeapons::Entry GWeapons[] = {
        { 1, "Content/Prefabs/Weapons/Wrench.nmprefab" },
        { 2, "Content/Prefabs/Weapons/PipePistol.nmprefab" },
    };

    struct CachedWeapon
    {
        LabWeaponComponent Data;
        uint8_t             bLoaded = 0;
        uint8_t             bMissing = 0;
    };
    CachedWeapon GCache[256];
}

const LabWeapons::Entry* LabWeapons::Find(const uint8_t InID)
{
    if(InID == 0) { return nullptr; }
    for(const Entry& E : GWeapons)
    {
        if(E.ID == InID) { return &E; }
    }
    return nullptr;
}

uint8_t LabWeapons::FindByPrefab(const char* InPrefab)
{
    if(!InPrefab) { return 0; }
    for(const Entry& E : GWeapons)
    {
        if(std::strcmp(E.Prefab, InPrefab) == 0) { return E.ID; }
    }
    return 0;
}

const LabWeaponComponent* LabWeapons::GetData(World& InWorld, const uint8_t InID)
{
    CachedWeapon& Cached = GCache[InID];
    if(Cached.bLoaded) { return &Cached.Data; }
    if(Cached.bMissing) { return nullptr; }

    const Entry* Weapon = Find(InID);
    if(!Weapon) { Cached.bMissing = 1; return nullptr; }

    /* The stats live in the prefab; PrefabAsset does not expose component
     * data, so spawn a definition instance once, copy its root component and
     * throw the instance away. It never draws: hidden at once, destroyed at
     * the end of the tick. */
    PrefabInstance Definition = InWorld.SpawnPrefab(Weapon->Prefab, TransformPose(), "LabWeaponDefinition");
    if(!Definition.IsValid())
    {
        CORE_LOG_WARN("[DemoLab] weapon " + std::to_string(int(InID)) + ": cannot spawn " + Weapon->Prefab)
        Cached.bMissing = 1;
        return nullptr;
    }

    const Entity Root = Definition.GetRoot();
    if(RenderCore::RenderComponent* Render = Root.GetComponent<RenderCore::RenderComponent>()) { Render->bVisible = 0; }
    const LabWeaponComponent* Data = Root.GetComponent<LabWeaponComponent>();
    if(Data)
    {
        Cached.Data = *Data;
        Cached.bLoaded = 1;
    }
    else
    {
        CORE_LOG_WARN(std::string("[DemoLab] ") + Weapon->Prefab + " has no LabWeaponComponent on its root")
        Cached.bMissing = 1;
    }
    InWorld.DestroyPrefabInstance(Definition.GetID());
    return Cached.bLoaded ? &Cached.Data : nullptr;
}

void LabWeapons::ResetCache()
{
    for(CachedWeapon& Cached : GCache)
    {
        Cached.bLoaded = 0;
        Cached.bMissing = 0;
    }
}

// === Component ==============================================================
GAMECORE_DEFINE_COMPONENT(LabWeaponComponent, "LabWeaponComponent")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabWeaponComponent, LabVersion::GetCurrentVersion() ) {
    Ar.Field("WeaponType", C.WeaponType);
    Ar.Field("FireMode", C.FireMode);
    Ar.Field("AnimSet", C.AnimSet);
    Ar.Field("DisplayName", C.DisplayName);

    Ar.Field("Damage", C.Damage);
    Ar.Field("HeadshotMultiplier", C.HeadshotMultiplier);
    Ar.Field("Range", C.Range);
    Ar.Field("Falloff", C.Falloff);
    Ar.Field("Impulse", C.Impulse);
    Ar.Flag("Staggers", C.bStaggers);

    Ar.Field("FireInterval", C.FireInterval);
    Ar.Field("BurstCount", C.BurstCount);
    Ar.Field("Pellets", C.Pellets);
    Ar.Field("SpreadMin", C.SpreadMin);
    Ar.Field("SpreadMax", C.SpreadMax);
    Ar.Field("SpreadPerShot", C.SpreadPerShot);
    Ar.Field("SpreadRecovery", C.SpreadRecovery);
    Ar.Field("MagSize", C.MagSize);
    Ar.Field("ReserveMax", C.ReserveMax);
    Ar.Field("ReloadSeconds", C.ReloadSeconds);

    Ar.Field("Windup", C.Windup);
    Ar.Field("Active", C.Active);
    Ar.Field("Recovery", C.Recovery);
    Ar.Field("Reach", C.Reach);
    Ar.Field("ArcDegrees", C.ArcDegrees);
    Ar.Field("ComboLength", C.ComboLength);

    Ar.Field("HandBone", C.HandBone);
    Ar.Field("HandPos", C.HandPos);
    Ar.Field("HandRot", C.HandRot);
    Ar.Field("HolsterBone", C.HolsterBone);
    Ar.Field("HolsterPos", C.HolsterPos);
    Ar.Field("HolsterRot", C.HolsterRot);
    Ar.Field("EquipSeconds", C.EquipSeconds);

    Ar.Field("ProjectilePrefab", C.ProjectilePrefab);
    Ar.Field("LaunchSpeed", C.LaunchSpeed);
    Ar.Field("ThrowSpeedMax", C.ThrowSpeedMax);

    Ar.Field("FireSound", C.FireSound);
    Ar.Field("ImpactFx", C.ImpactFx);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabWeaponComponent) {
    Props.TypeCategory("DemoLab");
    Props.Choice("WeaponType", &LabWeaponComponent::WeaponType, "Melee|HitScan|Projectile|Throwable");
    Props.Choice("FireMode", &LabWeaponComponent::FireMode, "Single|Burst|Auto|Combo");
    Props.Choice("AnimSet", &LabWeaponComponent::AnimSet, "Unarmed|OneHanded|TwoHanded|Rifle|Heavy|Throwable");
    Props.Field("DisplayName", &LabWeaponComponent::DisplayName);

    Props.Category("Damage");
    Props.Field("Damage", &LabWeaponComponent::Damage);
    Props.Field("HeadshotMultiplier", &LabWeaponComponent::HeadshotMultiplier);
    Props.Field("Range", &LabWeaponComponent::Range);
    Props.Field("Falloff", &LabWeaponComponent::Falloff);
    Props.Field("Impulse", &LabWeaponComponent::Impulse);
    Props.Flag("Staggers", &LabWeaponComponent::bStaggers);

    Props.Category("Cadence");
    Props.Field("FireInterval", &LabWeaponComponent::FireInterval);
    Props.Field("BurstCount", &LabWeaponComponent::BurstCount);
    Props.Field("Pellets", &LabWeaponComponent::Pellets);
    Props.Field("SpreadMin", &LabWeaponComponent::SpreadMin);
    Props.Field("SpreadMax", &LabWeaponComponent::SpreadMax);
    Props.Field("SpreadPerShot", &LabWeaponComponent::SpreadPerShot);
    Props.Field("SpreadRecovery", &LabWeaponComponent::SpreadRecovery);
    Props.Field("MagSize", &LabWeaponComponent::MagSize);
    Props.Field("ReserveMax", &LabWeaponComponent::ReserveMax);
    Props.Field("ReloadSeconds", &LabWeaponComponent::ReloadSeconds);

    Props.Category("Melee");
    Props.Field("Windup", &LabWeaponComponent::Windup);
    Props.Field("Active", &LabWeaponComponent::Active);
    Props.Field("Recovery", &LabWeaponComponent::Recovery);
    Props.Field("Reach", &LabWeaponComponent::Reach);
    Props.Field("ArcDegrees", &LabWeaponComponent::ArcDegrees);
    Props.Field("ComboLength", &LabWeaponComponent::ComboLength);

    Props.Category("Equip");
    Props.Field("HandBone", &LabWeaponComponent::HandBone);
    Props.Field("HandPos", &LabWeaponComponent::HandPos);
    Props.Field("HandRot", &LabWeaponComponent::HandRot);
    Props.Field("HolsterBone", &LabWeaponComponent::HolsterBone);
    Props.Field("HolsterPos", &LabWeaponComponent::HolsterPos);
    Props.Field("HolsterRot", &LabWeaponComponent::HolsterRot);
    Props.Field("EquipSeconds", &LabWeaponComponent::EquipSeconds);

    Props.Category("Projectile");
    Props.Field("ProjectilePrefab", &LabWeaponComponent::ProjectilePrefab, PF_AssetPrefab);
    Props.Field("LaunchSpeed", &LabWeaponComponent::LaunchSpeed);
    Props.Field("ThrowSpeedMax", &LabWeaponComponent::ThrowSpeedMax);

    Props.Category("Presentation");
    Props.Field("FireSound", &LabWeaponComponent::FireSound, PF_AssetSound);
    Props.Field("ImpactFx", &LabWeaponComponent::ImpactFx, PF_AssetParticleSystem);
}
#endif

// === Native prefab ==========================================================

/* The base every weapon forks (Content Browser > New Native Prefab...). The
 * mesh goes on the root's Render or on a child; the Anim Socket's bone and
 * offsets are written each frame from the data above. */
static void BuildLabWeapon(PrefabBuilder& Builder)
{
    PrefabBuilder::Node Root = Builder.Root("Weapon");
    Root.Add<TransformComponent>();
    Root.Add<RenderCore::RenderComponent>();

    RenderCore::AnimSocketComponent Socket;
    std::snprintf(Socket.Bone, sizeof(Socket.Bone), "%s", "DEF-spine");
    Root.Add(Socket);

    Root.Add<LabWeaponComponent>();

    PrefabBuilder::Node Muzzle = Root.Child("Muzzle");
    Muzzle.Add<TransformComponent>();
}

GAMECORE_DEFINE_NATIVE_PREFAB(LabWeapon, "DemoLab.Weapon", &BuildLabWeapon)
