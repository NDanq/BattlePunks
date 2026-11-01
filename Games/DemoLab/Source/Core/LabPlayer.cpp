/**
* VIBECODE CERTIFIED
*/
#include "Core/LabPlayer.h"

#include "Public/GameCore/ComponentReplication.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/NativePrefab.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/LagCompensation.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/Ragdoll.h"
#include "Public/RenderCore/Components/AnimationComponent.h"
#include "Public/RenderCore/Components/CameraComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "Weapons/LabCombat.h"

#include <cstdio>

#include "LabVersion.h"

using namespace GameCore;
using namespace NimbleEngine;

// === LabCharacterComponent =================================================


GAMECORE_DEFINE_COMPONENT(LabCharacterComponent, "LabCharacterComponent")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabCharacterComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("MaxHealth", C.MaxHealth);
    Ar.Field("SpeedScale", C.SpeedScale);

    Ar.Field("DisplayName", C.DisplayName);
    Ar.Field("MeleeDamageScale", C.MeleeDamageScale);
    Ar.Field("DefaultWeapon0", C.DefaultWeaponIDs[0]);
    Ar.Field("DefaultWeapon1", C.DefaultWeaponIDs[1]);
    Ar.Field("DefaultWeapon2", C.DefaultWeaponIDs[2]);
    Ar.Field("DefaultThrowable", C.DefaultThrowable);
    Ar.Field("DefaultThrowableCount", C.DefaultThrowableCount);
    Ar.Field("Team", C.Team);
}

#if WITH_NETCORE
/* Never changes after spawn: sent once. */
GAMECORE_DEFINE_COMPONENT_REPLICATION(LabCharacterComponent) {
    Rep.Field("DisplayName", &LabCharacterComponent::DisplayName, RFF_InitialOnly);
    Rep.Field("MaxHealth", &LabCharacterComponent::MaxHealth, RFF_InitialOnly);
    Rep.Field("SpeedScale", &LabCharacterComponent::SpeedScale, RFF_InitialOnly);
    Rep.Field("MeleeDamageScale", &LabCharacterComponent::MeleeDamageScale, RFF_InitialOnly);
    Rep.Field("Team", &LabCharacterComponent::Team, RFF_InitialOnly);
}
#endif

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabCharacterComponent) {
    Props.TypeCategory("DemoLab");
    Props.Field("DisplayName", &LabCharacterComponent::DisplayName);
    Props.Field("MaxHealth", &LabCharacterComponent::MaxHealth);
    Props.Field("SpeedScale", &LabCharacterComponent::SpeedScale);
    Props.Field("MeleeDamageScale", &LabCharacterComponent::MeleeDamageScale);
    Props.Category("Loadout");
    Props.Array("DefaultWeapons", &LabCharacterComponent::DefaultWeaponIDs);
    Props.Field("DefaultThrowable", &LabCharacterComponent::DefaultThrowable);
    Props.Field("DefaultThrowableCount", &LabCharacterComponent::DefaultThrowableCount);
    Props.Field("Team", &LabCharacterComponent::Team);
}
#endif

// === Layout =================================================================

void LabEnsureLayout(World& InWorld, const EntityID InAvatar)
{
    Entity Avatar = InWorld.GetEntity(InAvatar);
    if(!Avatar.IsAlive()) { return; }

    if(!Avatar.GetComponent<LabCharacterComponent>()) { Avatar.AddComponent<LabCharacterComponent>(); }
    if(!Avatar.GetComponent<LabHealthComponent>()) { Avatar.AddComponent<LabHealthComponent>(); }
    if(!Avatar.GetComponent<LabTeamComponent>()) { Avatar.AddComponent<LabTeamComponent>(); }
    if(!Avatar.GetComponent<LabInventoryComponent>()) { Avatar.AddComponent<LabInventoryComponent>(); }
    if(!Avatar.GetComponent<LabCombatComponent>()) { Avatar.AddComponent<LabCombatComponent>(); }
    if(!Avatar.GetComponent<LagCompensatedComponent>())
    {
        LagCompensatedComponent Capsule;
        Capsule.Radius = 35.f;
        Capsule.Top = 180.f;
        Avatar.AddComponent(Capsule);
    }
}

void LabApplyCharacter(World& InWorld, const EntityID InAvatar, const bool bInFirstSpawn)
{
    if(!HasAuthority()) { return; }
    Entity Avatar = InWorld.GetEntity(InAvatar);
    const LabCharacterComponent* Found = Avatar.IsAlive() ? Avatar.GetComponent<LabCharacterComponent>() : nullptr;
    if(!Found) { return; }
    const LabCharacterComponent Character = *Found;   // spawns below may move storage

    if(LabHealthComponent* Health = Avatar.GetComponent<LabHealthComponent>())
    {
        Health->MaxHealth = Character.MaxHealth;
        Health->Health = Character.MaxHealth;
    }
    if(LabTeamComponent* Team = Avatar.GetComponent<LabTeamComponent>()) { Team->Team = Character.Team; }

    /* The tunables are not replicated: scaled here at spawn on the server,
     * and on first sight by the presentation system on every other process. */
    if(bInFirstSpawn && Character.SpeedScale != 1.0f)
    {
        if(CharacterMoverComponent* Mover = Avatar.GetComponent<CharacterMoverComponent>())
        {
            Mover->WalkSpeed   *= Character.SpeedScale;
            Mover->RunSpeed    *= Character.SpeedScale;
            Mover->CrouchSpeed *= Character.SpeedScale;
            Mover->AimSpeed    *= Character.SpeedScale;
        }
    }

    if(LabInventoryComponent* Inventory = InWorld.GetEntity(InAvatar).GetComponent<LabInventoryComponent>())
    {
        for(int Slot = 0; Slot < LabWeapons::SLOTS; ++Slot)
        {
            Inventory->WeaponAt(Slot)  = 0;
            Inventory->MagAt(Slot)     = 0;
            Inventory->ReserveAt(Slot) = 0;
        }
        Inventory->Equipped     = LabWeapons::UNARMED;
        Inventory->PendingEquip = LabWeapons::NO_EQUIP;
        Inventory->Throwable  = Character.DefaultThrowable;
        Inventory->ThrowCount = Character.DefaultThrowableCount;
    }
    for(const uint8_t ID : Character.DefaultWeaponIDs)
    {
        if(ID) { LabCombat::GiveWeapon(InWorld, InAvatar, ID); }
    }
}

// === Native prefab ==========================================================

/* The base character. Render and Animation sit on the root: the mover writes its
 * anim params on the avatar root entity, and weapon sockets read the root's
 * pose. */
static void BuildLabPlayer(PrefabBuilder& Builder)
{
    PrefabBuilder::Node Root = Builder.Root("LabPlayer");
    Root.Add<TransformComponent>();

    NetComponent Net;
    Net.bPredicted = 1;
    Root.Add(Net);

    AvatarHarnessComponent Harness;
    std::snprintf(Harness.InputMap, sizeof(Harness.InputMap), "%s", "DemoLabPlayer");
    std::snprintf(Harness.Simulation, sizeof(Harness.Simulation), "%s", "LabMove");   // CharacterMove + stagger and downed (LabMove.cpp)
    Root.Add(Harness);

    CharacterMoverComponent Mover;
    Mover.bFaceView = 0;   // faces where it walks, and the view while aiming
    Mover.Mask = LAB_LAYER_WORLD;
    Root.Add(Mover);

    RenderCore::RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", LAB_DEFAULT_MESH);
    std::snprintf(Render.Materials[0], sizeof(Render.Materials[0]), "%s", LAB_DEFAULT_MATERIAL);
    Root.Add(Render);

    RenderCore::AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%s", LAB_DEFAULT_ANIMSTATE);
    Root.Add(Animation);

    RagdollComponent Ragdoll;
    Ragdoll.bHitBoxes = 1;
    Ragdoll.bDropProps = 0;   // weapons are per-process instances; they go with the body
    Ragdoll.Layer = LAB_LAYER_BODIES;
    Root.Add(Ragdoll);

    LagCompensatedComponent Capsule;
    Capsule.Radius = 35.f;
    Capsule.Top = 180.f;
    Root.Add(Capsule);

    Root.Add<LabCharacterComponent>();
    Root.Add<LabHealthComponent>();
    LabTeamComponent Team;
    Team.Team = LabTeams::PLAYERS;
    Root.Add(Team);
    Root.Add<LabInventoryComponent>();
    Root.Add<LabCombatComponent>();

    PrefabBuilder::Node Boom = Root.Child("CameraBoom");
    Boom.Add<TransformComponent>();
    PrefabBuilder::Node Eye = Boom.Child("Camera");
    Eye.Add<TransformComponent>();
    RenderCore::CameraComponent Camera;
    Camera.bActive = 0;   // the view target; never the level's first active
    Camera.Projection = RenderCore::CP_Perspective;
    Camera.FovY = 62.0f;  // the mover drives it from FovNormal / FovScope
    Camera.ZNear = 5.0f;
    Camera.ZFar = 40000.0f;
    Eye.Add(Camera);
}

GAMECORE_DEFINE_NATIVE_PREFAB(LabPlayer, "DemoLab.Player", &BuildLabPlayer)

/* A target or story enemy: the same body, no harness, no camera. Team 2. */
static void BuildLabDummy(PrefabBuilder& Builder)
{
    PrefabBuilder::Node Root = Builder.Root("LabDummy");
    Root.Add<TransformComponent>();

    NetComponent Net;
    Root.Add(Net);

    RenderCore::RenderComponent Render;
    std::snprintf(Render.Mesh, sizeof(Render.Mesh), "%s", LAB_DEFAULT_MESH);
    std::snprintf(Render.Materials[0], sizeof(Render.Materials[0]), "%s", LAB_DEFAULT_MATERIAL);
    Root.Add(Render);

    RenderCore::AnimationComponent Animation;
    std::snprintf(Animation.Animation, sizeof(Animation.Animation), "%s", LAB_DEFAULT_ANIMSTATE);
    Root.Add(Animation);

    RagdollComponent Ragdoll;
    Ragdoll.bHitBoxes = 1;
    Ragdoll.bDropProps = 0;
    Ragdoll.Layer = LAB_LAYER_BODIES;
    Root.Add(Ragdoll);

    LagCompensatedComponent Capsule;
    Capsule.Radius = 35.f;
    Capsule.Top = 180.f;
    Root.Add(Capsule);

    LabCharacterComponent Character;
    std::snprintf(Character.DisplayName, sizeof(Character.DisplayName), "%s", "Target Dummy");
    Character.MaxHealth = 60.f;
    Character.Team = LabTeams::ENEMIES;
    Root.Add(Character);

    LabHealthComponent Health;
    Health.MaxHealth = 60.f;
    Health.Health = 60.f;
    Root.Add(Health);

    LabTeamComponent Team;
    Team.Team = LabTeams::ENEMIES;
    Root.Add(Team);
    Root.Add<LabCombatComponent>();
}

GAMECORE_DEFINE_NATIVE_PREFAB(LabDummy, "DemoLab.Dummy", &BuildLabDummy)
