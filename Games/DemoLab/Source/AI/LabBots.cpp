#include "AI/LabBots.h"
#include "LabVersion.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/Prefab.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AIFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PhysicsFacade.h"
#include "Public/NimbleEngine/PlayerState.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/RenderCore/Components/RenderComponent.h"

#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif

#include "Core/LabHealth.h"
#include "Core/LabPlayer.h"
#include "GameModes/LabInputDriver.h"
#include "Weapons/LabCombat.h"
#include "Weapons/LabWeapon.h"

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

GAMECORE_DEFINE_COMPONENT(LabBotComponent, "LabBot")

GAMECORE_DEFINE_COMPONENT_SERIALIZER(LabBotComponent, LabVersion::GetCurrentVersion()) {
    Ar.Field("Archetype", C.Archetype);
    Ar.Field("AimSkill", C.AimSkill);
    Ar.Field("ReactionSeconds", C.ReactionSeconds);
    Ar.Field("PreferredRange", C.PreferredRange);
    Ar.Entity("BotPlayer", C.BotPlayer);
}

#if WITH_EDITOR
GAMECORE_DEFINE_COMPONENT_PROPERTIES(LabBotComponent) {
    Props.TypeCategory("DemoLab");
    Props.Choice("Archetype", &LabBotComponent::Archetype, "Brute|Watchman");
    Props.Field("AimSkill", &LabBotComponent::AimSkill);
    Props.Field("ReactionSeconds", &LabBotComponent::ReactionSeconds);
    Props.Field("PreferredRange", &LabBotComponent::PreferredRange);
}
#endif

// === Players ================================================================

EntityID LabBots::CreateBotPlayer(World& InWorld, const bool bInScoreboard, const char* InName)
{
    if(!HasAuthority()) { return EntityID(); }

    /* The lowest free index from BOT_PLAYER_BASE up. */
    bool bTaken[256] = {};
    InWorld.Each<PossessorComponent>([&](Entity, PossessorComponent& InPossessor) { bTaken[InPossessor.LocalPlayer] = true; });
    int Index = -1;
    for(int I = BOT_PLAYER_BASE; I < 256 && Index < 0; ++I) { if(!bTaken[I]) { Index = I; } }
    if(Index < 0) { return EntityID(); }

    char Name[PLAYER_NAME_MAX];
    if(InName && InName[0]) { std::snprintf(Name, sizeof(Name), "%s", InName); }
    else { std::snprintf(Name, sizeof(Name), "Bot %d", Index - BOT_PLAYER_BASE + 1); }
    Entity Player = InWorld.CreateEntity(Name);
    Player.AddComponent<NetComponent>();
    PossessorComponent Possessor;
    Possessor.LocalPlayer = uint8_t(Index);
    Player.AddComponent(Possessor);
    if(bInScoreboard)
    {
        PlayerStateComponent State;
        std::snprintf(State.PlayerName, sizeof(State.PlayerName), "%s", Name);
        State.PlayerIndex = FindFreePlayerIndex(InWorld);
        Player.AddComponent(State);
    }
    return Player.GetID();
}

void LabBots::ReleaseBotPlayer(World& InWorld, const EntityID InPlayer)
{
    if(!HasAuthority() || !InWorld.IsAlive(InPlayer)) { return; }
    Unpossess(InWorld, InPlayer);
    InWorld.DestroyEntity(InPlayer);
}

namespace
{
    /* The archetype's loadout and brain tuning. */
    void Outfit(const uint8_t InArchetype, uint8_t OutWeapons[3], LabBotComponent& InOutBot)
    {
        switch(InArchetype)
        {
        case LabBots::BA_Brute:
            OutWeapons[0] = 1;            // the wrench
            InOutBot.PreferredRange = 90.f;
            InOutBot.AimSkill = 0.5f;
            InOutBot.ReactionSeconds = 0.5f;
            break;
        default:
            OutWeapons[0] = 2;            // the pipe pistol
            InOutBot.PreferredRange = 700.f;
            InOutBot.AimSkill = 0.45f;
            InOutBot.ReactionSeconds = 0.6f;
            break;
        }
    }
}

uint8_t LabBots::ArchetypeByName(const char* InName)
{
    return InName && std::strcmp(InName, "Brute") == 0 ? BA_Brute : BA_Watchman;
}

const char* LabBots::TreeFor(const uint8_t InArchetype)
{
    return InArchetype == BA_Brute ? "Content/AI/Brute.nmbt" : "Content/AI/Watchman.nmbt";
}

EntityID LabBots::SpawnBot(World& InWorld, const Spawn& InSpawn)
{
    if(!HasAuthority() || !IsAIAvailable()) { return EntityID(); }

    PrefabInstance Instance = InWorld.SpawnPrefab(InSpawn.Prefab, InSpawn.At, InSpawn.Name ? InSpawn.Name : "LabBot");
    if(!Instance.IsValid())
    {
        CORE_LOG_WARN(std::string("[DemoLab] bot: cannot spawn ") + InSpawn.Prefab)
        return EntityID();
    }
    const EntityID Avatar = Instance.GetRoot().GetID();
    LabEnsureLayout(InWorld, Avatar);

    /* Its team and health before the loadout; the character's own team is
     * a player's, so it is overridden here. */
    if(LabCharacterComponent* Character = InWorld.GetEntity(Avatar).GetComponent<LabCharacterComponent>())
    {
        Character->Team = InSpawn.Team;
        if(InSpawn.MaxHealth > 0.f) { Character->MaxHealth = InSpawn.MaxHealth; }
        /* Its name in the kill feed (a scoreboard bot's player name wins). */
        static const char* const Kinds[] = { "Brute", "Watchman" };
        std::snprintf(Character->DisplayName, sizeof(Character->DisplayName), "%s",
                      InSpawn.Name && InSpawn.Name[0] ? InSpawn.Name : Kinds[InSpawn.Archetype % 2]);
        /* The Brute: bigger and slower; it hits harder with the same wrench. */
        if(InSpawn.Archetype == BA_Brute)
        {
            Character->SpeedScale = 0.75f;
            Character->MeleeDamageScale = 1.5f;
        }
        for(uint8_t& ID : Character->DefaultWeaponIDs) { ID = 0; }
    }
    LabApplyCharacter(InWorld, Avatar, true);

    /* The look: red CRT eyes (the SteamDummy material's EyeColor, slot 0), and
     * the Brute scaled up about its feet. */
    if(RenderCore::RenderComponent* Render = InWorld.GetEntity(Avatar).GetComponent<RenderCore::RenderComponent>())
    {
        Render->SetMaterialParameter(0, "EyeColor", 1.0f, 0.06f, 0.03f, 0.0f);
    }
    if(InSpawn.Archetype == BA_Brute)
    {
        if(TransformComponent* Transform = InWorld.GetEntity(Avatar).GetComponent<TransformComponent>())
        {
            Transform->Scale = EngineCore::Vector3(1.25, 1.25, 1.25);
        }
    }

    const EntityID Player = CreateBotPlayer(InWorld, InSpawn.bScoreboard, InSpawn.Name);
    if(!Player.IsSet() || !Possess(InWorld, Player, Avatar))
    {
        CORE_LOG_WARN("[DemoLab] bot: no player to possess it")
        if(Player.IsSet()) { InWorld.DestroyEntity(Player); }
        Instance.GetRoot().Destroy();
        return EntityID();
    }

    LabBotComponent Bot;
    Bot.Archetype = InSpawn.Archetype;
    Bot.BotPlayer = Player;
    Bot.Seed = uint32_t(Avatar.Pack() * 2654435761u) | 1u;
    uint8_t Weapons[3] = { InSpawn.Weapons[0], InSpawn.Weapons[1], InSpawn.Weapons[2] };
    uint8_t Kit[3] = { 0, 0, 0 };
    Outfit(InSpawn.Archetype, Kit, Bot);
    if(!Weapons[0] && !Weapons[1] && !Weapons[2]) { std::copy(Kit, Kit + 3, Weapons); }
    InWorld.GetEntity(Avatar).AddComponent(Bot);

    for(const uint8_t ID : Weapons) { if(ID) { LabCombat::GiveWeapon(InWorld, Avatar, ID); } }

    AIBotSetup Setup;
    Setup.Tree = InSpawn.Tree;
    Setup.Team = InSpawn.Team;
    Setup.bDriveTransform = false;   // the Character Mover moves it; the agent only plans
    Setup.MeshYaw = 180.f;
    Setup.MaxSpeed = 400.f;
    Setup.SightRange = 4000.f;
    Setup.FovHalfAngle = 80.f;
    Setup.TargetHeight = 115.f;
    if(!MakeAIBot(InWorld, Avatar, Setup))
    {
        CORE_LOG_WARN("[DemoLab] bot: no AI here (a client?)")
    }
    return Avatar;
}

// === The driver =============================================================

namespace
{
    float Random01(uint32_t& InOutSeed)
    {
        InOutSeed = InOutSeed * 1664525u + 1013904223u;
        return float((InOutSeed >> 8) & 0xFFFFFF) / float(0x1000000);
    }

    bool IsUp(World& InWorld, const EntityID InLab)
    {
        const Entity E = InWorld.GetEntity(InLab);
        const LabHealthComponent* Health = E.IsAlive() ? E.GetComponent<LabHealthComponent>() : nullptr;
        return Health && !Health->bDead && !Health->bDown;
    }

    /* Without a brain: the nearest living enemy within 4000 that a ray from
     * the eyes reaches. */
    EntityID NearestEnemyInSight(World& InWorld, const EntityID InSelf, const Vector3& InFeet)
    {
        const uint8_t Team = LabTeams::TeamOf(InWorld, InSelf);
        struct Candidate { EntityID Id; double Distance; };
        std::vector<Candidate> Candidates;
        InWorld.Each<LabHealthComponent>([&](Entity InEntity, LabHealthComponent& InHealth)
        {
            if(InEntity.GetID() == InSelf || InHealth.bDead || InHealth.bDown) { return; }
            if(!LabTeams::AreHostile(Team, LabTeams::TeamOf(InWorld, InEntity.GetID()))) { return; }
            const double D = (InEntity.GetWorldPose().Position - InFeet).GetLength();
            if(D < 4000.0) { Candidates.push_back({ InEntity.GetID(), D }); }
        });
        std::sort(Candidates.begin(), Candidates.end(), [](const Candidate& A, const Candidate& B) { return A.Distance < B.Distance; });
        const Vector3 Eye = InFeet + Vector3(0, 160, 0);
        for(const Candidate& C : Candidates)
        {
            const Vector3 Ray = LabDrive::ChestOf(InWorld, C.Id) - Eye;
            PhysicsRaycastHit Hit;
            if(!PhysicsRaycast(InWorld, Eye, Ray, float(Ray.GetLength()), Hit, LAB_WORLD_MASK, InSelf)) { return C.Id; }
        }
        return EntityID();
    }
}

void LabBots::Drive(World& InWorld, const EntityID InAvatar, InputSnapshot& InOut, const float InSeconds)
{
    InOut = InputSnapshot();
    const float Dt = std::clamp(InSeconds, 0.f, 0.1f);
    if(!IsUp(InWorld, InAvatar)) { return; }

    Entity Self = InWorld.GetEntity(InAvatar);
    const CharacterMoverComponent* Mover = Self.GetComponent<CharacterMoverComponent>();
    const AvatarHarnessComponent* Harness = Self.GetComponent<AvatarHarnessComponent>();
    LabBotComponent* Bot = Self.GetComponent<LabBotComponent>();
    if(!Mover || !Harness || !Bot) { return; }

    /* What the brain sees -- or, with no brain here (a client: AI runs on
     * the authority), the nearest enemy in plain sight. */
    const Vector3 Feet = Self.GetWorldPose().Position;
    const bool bBrain = HasAuthority() && IsAIAvailable();
    EntityID Target = bBrain ? GetAIKeyEntity(InWorld, InAvatar, "TargetActor") : NearestEnemyInSight(InWorld, InAvatar, Feet);
    const bool bSees = Target.IsSet() && IsUp(InWorld, Target)
                    && (!bBrain || GetAIKeyNumber(InWorld, InAvatar, "TargetVisible") > 0.5);
    const Vector3 Chest = bSees ? LabDrive::ChestOf(InWorld, Target) : Vector3();
    const double Distance = bSees ? (InWorld.GetEntity(Target).GetWorldPose().Position - Feet).GetLength() : 1e9;

    const LabWeaponComponent* Held = LabCombat::EquippedData(InWorld, InAvatar);
    const bool bMelee = !Held || LabWeapons::TypeOf(*Held) == LabWeapons::EWeaponType::EWT_Melee;
    const float FireInterval = Held ? Held->FireInterval : 0.35f;
    const float Reach = Held && bMelee ? Held->Reach : 140.f;
    const int32_t MagSize = Held ? Held->MagSize : 0;
    Self = InWorld.GetEntity(InAvatar);
    Bot = Self.GetComponent<LabBotComponent>();
    Mover = Self.GetComponent<CharacterMoverComponent>();
    const LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
    const LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
    const int32_t Mag = Inventory && Inventory->Equipped < LabWeapons::SLOTS ? Inventory->MagAt(Inventory->Equipped) : 0;

    /* A new target: a beat before it reacts. */
    if(Target != Bot->LastTarget) { Bot->LastTarget = Target; Bot->Reaction = Bot->ReactionSeconds; }
    if(bSees) { Bot->Reaction = std::max(0.f, Bot->Reaction - Dt); }
    else { Bot->Reaction = Bot->ReactionSeconds; }
    const bool bEngage = bSees && Bot->Reaction <= 0.f;

    /* Feet: wherever the brain's agent wants to go. While engaging it keeps
     * its own view (the aim turns it), and strafes rather than turning. */
    SteerInputMap Map;
    Map.MoveForward = FindInputAxis(Harness->InputMap, Mover->MoveForward);
    Map.MoveRight   = FindInputAxis(Harness->InputMap, Mover->MoveRight);
    Map.LookX       = bEngage ? -1 : FindInputAxis(Harness->InputMap, Mover->LookX);
    const int Run   = FindInputAction(Harness->InputMap, Mover->RunAction);
    Map.RunBits     = Run >= 0 ? 1u << Run : 0u;
    Map.WalkSpeed   = Mover->WalkSpeed;
    Map.RunSpeed    = Mover->RunSpeed * 0.8f;
    Map.TurnDegreesPerSecond = Mover->TurnRate * 0.5f;
    Map.FaceMoveTolerance = bEngage ? 180.f : 50.f;
    if(bBrain) { SteerInput(InWorld, InAvatar, Map, Mover->CameraYaw, Dt, InOut); }
    else if(Target.IsSet() && !bEngage)
    {
        LabDrive::WalkTo(InWorld, InAvatar, InOut, InWorld.GetEntity(Target).GetWorldPose().Position, Bot->PreferredRange);
    }
    else if(!Target.IsSet() && !bEngage)
    {
        /* Nobody in sight: wander toward the middle, then on around it. */
        Bot->ErrorClock -= Dt;
        LabDrive::WalkTo(InWorld, InAvatar, InOut, Vector3(std::sin(double(Bot->Seed % 628) * 0.01) * 900.0, 0,
                                                            std::cos(double(Bot->Seed % 628) * 0.01) * 900.0), 150.f);
        if(Bot->ErrorClock < -4.f) { Random01(Bot->Seed); Bot->ErrorClock = 0.f; }
    }

    /* Hands. */
    Bot->FireClock = std::max(0.f, Bot->FireClock - Dt);
    Bot->ThrowClock = std::max(0.f, Bot->ThrowClock - Dt);
    if(bEngage)
    {
        /* Aim error, renewed twice a second: worse skill, wider. */
        Bot->ErrorClock -= Dt;
        if(Bot->ErrorClock <= 0.f)
        {
            const float Spread = (1.f - std::clamp(Bot->AimSkill, 0.f, 1.f)) * 7.f;
            Bot->ErrorYaw = (Random01(Bot->Seed) * 2.f - 1.f) * Spread;
            Bot->ErrorPitch = (Random01(Bot->Seed) * 2.f - 1.f) * Spread * 0.5f;
            Bot->ErrorClock = 0.5f;
        }
        const Vector3 To = Chest - Feet;
        const Vector3 Side = Vector3(To.GetZ(), 0, -To.GetX()).GetLength() > 1e-3 ? Vector3(To.GetZ(), 0, -To.GetX()).GetNormalize() : Vector3(1, 0, 0);
        const Vector3 Aimed = Chest + Side * (double(Bot->ErrorYaw) * Distance * 0.0175) + Vector3(0, double(Bot->ErrorPitch) * Distance * 0.0175, 0);
        const float Off = LabDrive::AimAt(InWorld, InAvatar, InOut, Aimed, 4.f);

        if(bMelee)
        {
            if(Distance < double(Reach) + 60.0 && Off < 25.f && Bot->FireClock <= 0.f)
            {
                LabDrive::Press(InOut, "Fire");
                Bot->FireClock = 0.32f;
            }
        }
        else
        {
            if(Distance > 450.0) { LabDrive::Press(InOut, "Aim", true); }
            /* A bomber lobs one now and then, from range. */
            if(Inventory && Inventory->ThrowCount > 0 && Bot->ThrowClock <= 0.f && Distance > 600.0 && Distance < 1700.0)
            {
                Bot->ThrowHold += Dt;
                LabDrive::AimAt(InWorld, InAvatar, InOut, Chest + Vector3(0, Distance * 0.35, 0), 6.f);
                if(Bot->ThrowHold < 0.7f) { LabDrive::Press(InOut, "Throw", true); }
                else { Bot->ThrowHold = 0.f; Bot->ThrowClock = 6.f; }
                return;
            }
            const bool bAuto = Held && LabWeapons::ModeOf(*Held) == LabWeapons::EFireMode::EFM_Auto;
            if(Mag > 0 && Off < 3.0f + (1.f - Bot->AimSkill) * 4.f)
            {
                if(bAuto) { LabDrive::Press(InOut, "Fire", true); if(Bot->FireClock <= 0.f) { LabDrive::Press(InOut, "Fire"); Bot->FireClock = 0.6f; } }
                else if(Bot->FireClock <= 0.f) { LabDrive::Press(InOut, "Fire"); Bot->FireClock = std::max(FireInterval + 0.15f, 0.9f); }   // security, not snipers: about a shot a second
            }
        }
    }
    if(!bMelee && MagSize > 0 && Mag == 0 && Combat && !Combat->bReloading) { LabDrive::Press(InOut, "Reload"); }

    /* What the brain should know, five times a second. */
    Bot->KeyClock -= Dt;
    if(Bot->KeyClock <= 0.f)
    {
        Bot->KeyClock = 0.2f;
        const LabHealthComponent* Health = Self.GetComponent<LabHealthComponent>();
        char Text[16];
        std::snprintf(Text, sizeof(Text), "%.2f", Health && Health->MaxHealth > 0.f ? double(Health->Health / Health->MaxHealth) : 1.0);
        SetAIKey(InWorld, InAvatar, "Health", Text);
        std::snprintf(Text, sizeof(Text), "%.2f", MagSize > 0 ? double(Mag) / double(MagSize) : 1.0);
        SetAIKey(InWorld, InAvatar, "Ammo", Text);
    }
}

void LabBots::TickStimuli(World& InWorld)
{
    if(!HasAuthority() || !IsAIAvailable()) { return; }
    struct Seen { EntityID Lab; uint8_t Team; bool bUp; };
    std::vector<Seen> Labs;
    InWorld.Each<LabHealthComponent>([&](Entity InEntity, LabHealthComponent& InHealth)
    {
        const LabTeamComponent* Team = InEntity.GetComponent<LabTeamComponent>();
        if(!Team || Team->Team == LabTeams::NONE) { return; }
        Labs.push_back({ InEntity.GetID(), Team->Team, !InHealth.bDead && !InHealth.bDown });
    });
    /* After the walk: a stimulus may be added, which moves the pool. */
    for(const Seen& P : Labs) { SetAIStimulus(InWorld, P.Lab, P.Team, P.bUp); }
}

void LabBots::OnDamaged(World& InWorld, const EntityID InVictim, const EntityID InInstigator)
{
    if(!HasAuthority() || !InInstigator.IsSet() || InVictim == InInstigator) { return; }
    if(!InWorld.IsAlive(InVictim) || !InWorld.GetEntity(InVictim).GetComponent<LabBotComponent>()) { return; }
    if(!LabTeams::AreHostile(LabTeams::TeamOf(InWorld, InVictim), LabTeams::TeamOf(InWorld, InInstigator))) { return; }
    SetAIKeyEntity(InWorld, InVictim, "TargetActor", InInstigator);
}
