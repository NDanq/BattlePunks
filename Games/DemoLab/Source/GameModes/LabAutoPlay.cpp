#include "GameModes/LabAutoPlay.h"

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/GameSession.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PhysicsFacade.h"

#include "Core/LabHealth.h"
#include "Core/LabTypes.h"
#include "Flow/LabFlow.h"
#include "GameModes/LabInputDriver.h"
#include "Weapons/LabCombat.h"
#include "Weapons/LabWeapon.h"
#include "World/LabEncounter.h"
#include "World/LabInteractable.h"
#include "World/LabSignals.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace GameCore;
using namespace NimbleEngine;
using EngineCore::Vector3;

LabAutoPlay& GetLabAutoPlay()
{
    static LabAutoPlay Driver;
    return Driver;
}

// === The scripts ============================================================

/*
 * One script per level (and state), a list of steps. Names are the levels'
 * (Tools/Levels/layout.py): a route walks "AP_<Name>_0", "_1", ... in turn;
 * Use, Hold and Refill walk to the named interactable and press Interact.
 */
#define LAB_STEP(Kind, Name, Signal, Limit) { LabAutoPlayStepKind::Kind, Name, Signal, Limit }

namespace LabAutoPlayStepKind
{
    constexpr int Wait = 0, Route = 1, Use = 2, Hold = 3, Fight = 4, DieOnce = 5, Refill = 6, WaitSignal = 7, WaitTravel = 8;
}

namespace
{
    struct RawStep { int Kind; const char* Name; const char* Signal; float Limit; };

    const RawStep L1_STEPS[] = {
        LAB_STEP(Wait,       "",               "",               3.f),
        LAB_STEP(Route,      "L1_Road1",       "",               150.f),   // the switchbacks: ~175 m
        LAB_STEP(Hold,       "Crank_Gatehouse", "L1.GateOpen",   20.f),
        LAB_STEP(Route,      "L1_Road2",       "",               120.f),
        LAB_STEP(WaitSignal, "",               "L1.Done",        30.f),
        LAB_STEP(WaitTravel, "",               "",               30.f),
    };
    const RawStep L2_STEPS[] = {
        LAB_STEP(Wait,       "",               "",               2.f),
        LAB_STEP(Route,      "L2_Breaker",     "",               40.f),
        LAB_STEP(Use,        "Breaker",        "L2.Powered",     40.f),
        LAB_STEP(Route,      "L2_Workshop",    "",               60.f),
        LAB_STEP(Use,        "Keycard",        "L2.Keycard",     20.f),
        LAB_STEP(Route,      "L2_Reader",      "",               30.f),
        LAB_STEP(Use,        "KeycardReader",  "L2.ArchiveOpen", 20.f),
        LAB_STEP(Route,      "L2_Valve1",      "",               60.f),
        LAB_STEP(Use,        "Valve1",         "L2.Valve1",      20.f),
        LAB_STEP(Route,      "L2_Valve2",      "",               30.f),
        LAB_STEP(Use,        "Valve2",         "L2.Valve2",      20.f),
        LAB_STEP(Route,      "L2_Valve3",      "",               30.f),
        LAB_STEP(Use,        "Valve3",         "L2.ValvesDone",  20.f),
        LAB_STEP(Route,      "L2_Elevator",    "",               60.f),
        LAB_STEP(Use,        "ElevatorPanel",  "L2.Elevator",    20.f),
        LAB_STEP(WaitTravel, "",               "",               40.f),
    };
    const RawStep L3_STEPS[] = {
        LAB_STEP(Wait,       "",               "",               2.f),
        LAB_STEP(Route,      "L3_Vault",       "",               60.f),
        LAB_STEP(Use,        "Intel",          "L3.Intel",       20.f),
        LAB_STEP(WaitSignal, "",               "Checkpoint.Theft", 40.f),
        LAB_STEP(Route,      "L3_Locker",      "",               30.f),
        LAB_STEP(Use,        "Locker",         "L3.Armed",       20.f),
        LAB_STEP(Fight,      "L3_Fight1",      "L3.W1Done",      240.f),
        LAB_STEP(DieOnce,    "",               "L3.W1Done",      30.f),
        LAB_STEP(Refill,     "AmmoCrate_Ring", "",               20.f),
        LAB_STEP(Route,      "L3_Tunnel",      "",               60.f),
        LAB_STEP(Use,        "Beacon_Start",   "L3.BeaconCharging", 20.f),
        LAB_STEP(Fight,      "L3_Fight2",      "L3.BeaconReady", 240.f),
        LAB_STEP(Use,        "Beacon_Fire",    "L3.BeaconFired", 20.f),
        LAB_STEP(Route,      "L3_Freight",     "",               40.f),
        LAB_STEP(Use,        "FreightPanel",   "L3.FreightLift", 20.f),
        LAB_STEP(WaitTravel, "",               "",               40.f),
    };
    const RawStep L2A_STEPS[] = {
        LAB_STEP(Wait,       "",               "",               2.f),
        LAB_STEP(Fight,      "L2A_Fight3",     "L2A.W3Done",     240.f),
        LAB_STEP(Refill,     "AmmoCrate_Turbine", "",            20.f),
        LAB_STEP(Route,      "L2A_Archive",    "",               60.f),
        LAB_STEP(Fight,      "L2A_Fight4",     "L2A.StragglersDone", 180.f),
        LAB_STEP(Refill,     "AmmoCrate_Workshop", "",           20.f),
        LAB_STEP(Route,      "L2A_Atrium",     "",               60.f),
        LAB_STEP(Fight,      "L2A_Fight5",     "L2A.HoldDone",   240.f),
        LAB_STEP(Route,      "L2A_Gantry",     "",               60.f),
        LAB_STEP(Use,        "RoofHatch",      "Game.Complete",  60.f),
        LAB_STEP(WaitTravel, "",               "",               60.f),
    };

    struct LevelScript { const char* Level; bool bAlarm; const RawStep* Steps; int Count; };
    const LevelScript SCRIPTS[] = {
        { "L1_Foothills",  false, L1_STEPS,  int(sizeof(L1_STEPS) / sizeof(L1_STEPS[0])) },
        { "L2_Laboratory", false, L2_STEPS,  int(sizeof(L2_STEPS) / sizeof(L2_STEPS[0])) },
        { "L3_Core",       false, L3_STEPS,  int(sizeof(L3_STEPS) / sizeof(L3_STEPS[0])) },
        { "L2_Laboratory", true,  L2A_STEPS, int(sizeof(L2A_STEPS) / sizeof(L2A_STEPS[0])) },
    };

    constexpr float TIMEOUT_SECONDS = 1200.f;
    constexpr double RAD = 3.14159265358979 / 180.0;

    /* By name, or with the state group's prefix (Level 2's "Calm_" / "Alarm_"). */
    EntityID FindNamed(World& InWorld, const std::string& InName)
    {
        for(const char* Prefix : { "", "Calm_", "Alarm_" })
        {
            const EntityID E = InWorld.FindEntityByName((Prefix + InName).c_str());
            if(E.IsSet()) { return E; }
        }
        return EntityID();
    }

    Vector3 PositionOf(World& InWorld, const char* InName)
    {
        const EntityID E = FindNamed(InWorld, InName);
        return E.IsSet() ? InWorld.GetEntity(E).GetWorldPose().Position : Vector3();
    }

    bool Exists(World& InWorld, const std::string& InName) { return FindNamed(InWorld, InName).IsSet(); }

    std::string WaypointName(const char* InRoute, const int InIndex)
    {
        return std::string("AP_") + InRoute + "_" + std::to_string(InIndex);
    }

    /* The nearest living encounter member, or unset. */
    EntityID NearestEnemy(World& InWorld, const Vector3& InFrom)
    {
        EntityID Best;
        double BestDistance = 1e30;
        InWorld.Each<LabEncounterMemberComponent>([&](Entity InEntity, LabEncounterMemberComponent&)
        {
            const LabHealthComponent* Health = InEntity.GetComponent<LabHealthComponent>();
            if(!Health || Health->bDead) { return; }
            const double D = (InEntity.GetWorldPose().Position - InFrom).GetLength();
            if(D < BestDistance) { BestDistance = D; Best = InEntity.GetID(); }
        });
        return Best;
    }

    /* The nearest entity whose name starts with InPrefix. */
    EntityID NearestNamed(World& InWorld, const char* InPrefix, const Vector3& InFrom)
    {
        EntityID Best;
        double BestDistance = 1e30;
        const size_t Length = std::strlen(InPrefix);
        InWorld.Each<LabInteractableComponent>([&](Entity InEntity, LabInteractableComponent&)
        {
            const char* Name = InEntity.GetName();
            if(std::strncmp(Name, "Alarm_", 6) == 0) { Name += 6; }
            if(std::strncmp(Name, InPrefix, Length) != 0) { return; }
            const double D = (InEntity.GetWorldPose().Position - InFrom).GetLength();
            if(D < BestDistance) { BestDistance = D; Best = InEntity.GetID(); }
        });
        return Best;
    }

    const RawStep& RawOf(const int InScript, const int InStep)
    {
        static const RawStep None = { LabAutoPlayStepKind::WaitTravel, "", "", 1e9f };
        if(InScript < 0 || InStep >= SCRIPTS[InScript].Count) { return None; }
        return SCRIPTS[InScript].Steps[InStep];
    }
}

// === Driver =================================================================

void LabAutoPlay::Fail(const std::string& InWhy)
{
    if(!Result.empty()) { return; }
    Result = "FAIL: " + InWhy;
    CORE_LOG_ERROR("[DemoLab] AutoPlay " + Result)
}

void LabAutoPlay::TickClock(const float InSeconds)
{
    if(!Result.empty()) { return; }
    Elapsed += InSeconds;
    if(Elapsed > TIMEOUT_SECONDS) { Fail("timed out in " + Level + " at step " + std::to_string(StepIndex)); }
}

void LabAutoPlay::Next(const char* InWhat)
{
    char Line[200];
    std::snprintf(Line, sizeof(Line), "[DemoLab] AutoPlay %.1fs %s step %d: %s", double(Elapsed), Level.c_str(), StepIndex, InWhat);
    CORE_LOG(Line)
    ++StepIndex;
    StepTime = 0.f;
    Waypoint = 0;
}

void LabAutoPlay::TickMenu(GameSession& InSession, const float InSeconds)
{
    if(!Result.empty()) { return; }
    MenuClock += InSeconds;
    if(bFinished)
    {
        if(MenuClock > 1.f)
        {
            Result = "PASS";
            char Line[128];
            std::snprintf(Line, sizeof(Line), "[DemoLab] AutoPlay PASS in %.1fs (falls: %d)", double(Elapsed), Deaths);
            CORE_LOG(Line)
        }
        return;
    }
    if(!bStartedGame && MenuClock > 1.5f)
    {
        bStartedGame = true;
        CORE_LOG("[DemoLab] AutoPlay: New Game from the main menu")
        LabFlow::NewGame(InSession);
    }
    else if(bStartedGame && MenuClock > 30.f)
    {
        Fail("New Game never left the menu");
    }
}

void LabAutoPlay::OnLevelStarted(const std::string& InLevel, const bool bInAlarm, const bool bInRestored)
{
    int Found = -1;
    for(int I = 0; I < int(sizeof(SCRIPTS) / sizeof(SCRIPTS[0])); ++I)
    {
        if(InLevel == SCRIPTS[I].Level && bInAlarm == SCRIPTS[I].bAlarm) { Found = I; }
    }
    MenuClock = 0.f;
    if(bInRestored && Found == Script)
    {
        /* Back from a checkpoint: from the last route walked before the step
         * in hand (the checkpoint may stand behind it), unless the fall was
         * the test's own. */
        if(RawOf(Script, StepIndex).Kind != LabAutoPlayStepKind::DieOnce)
        {
            for(int I = StepIndex; I >= 0; --I)
            {
                if(RawOf(Script, I).Kind == LabAutoPlayStepKind::Route) { StepIndex = I; break; }
            }
        }
        StepTime = 0.f;
        Waypoint = 0;
        CORE_LOG("[DemoLab] AutoPlay: back at the checkpoint in " + InLevel + ", step " + std::to_string(StepIndex))
        return;
    }
    Level = InLevel;
    bAlarm = bInAlarm;
    Script = Found;
    StepIndex = 0;
    StepTime = 0.f;
    Waypoint = 0;
    CORE_LOG("[DemoLab] AutoPlay: " + InLevel + (bInAlarm ? " (Alarm)" : "") + (Found < 0 ? ": no script" : ""))
}

void LabAutoPlay::NoteFall()
{
    if(bWasDead) { return; }
    bWasDead = true;
    ++Deaths;
    CORE_LOG("[DemoLab] AutoPlay: the player fell (" + std::to_string(Deaths) + ")")
}

bool LabAutoPlay::Steer(World& InWorld, const EntityID InAvatar, InputSnapshot& InOut, const Vector3& InTarget, const float InStop)
{
    return LabDrive::WalkTo(InWorld, InAvatar, InOut, InTarget, InStop);
}

void LabAutoPlay::Trace(World& InWorld, const EntityID InAvatar)
{
    const Entity Self = InWorld.GetEntity(InAvatar);
    const CharacterMoverComponent* Mover = Self.GetComponent<CharacterMoverComponent>();
    const Vector3 P = Self.GetWorldPose().Position;
    const LabHealthComponent* Health = Self.GetComponent<LabHealthComponent>();
    char Line[220];
    std::snprintf(Line, sizeof(Line), "[DemoLab] AutoPlay trace %.1fs %s step %d wp %d at (%.0f, %.0f, %.0f) yaw %.0f grounded %d hp %.0f",
                  double(Elapsed), Level.c_str(), StepIndex, Waypoint, P.GetX(), P.GetY(), P.GetZ(),
                  Mover ? double(Mover->CameraYaw) : 0.0, Mover ? int(Mover->bGrounded) : -1, Health ? double(Health->Health) : -1.0);
    CORE_LOG(Line)
}

bool LabAutoPlay::Fight(World& InWorld, const EntityID InAvatar, InputSnapshot& InOut, const float InSeconds)
{
    (void)InSeconds;
    const Entity Self = InWorld.GetEntity(InAvatar);
    const Vector3 Feet = Self.GetWorldPose().Position;
    const LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
    const LabCombatComponent* Combat = Self.GetComponent<LabCombatComponent>();
    if(!Inventory) { return false; }

    /* The pistol in hand. */
    int PistolSlot = -1;
    for(int Slot = 0; Slot < LabWeapons::SLOTS; ++Slot) { if(Inventory->WeaponAt(Slot) == LAB_WEAPON_PIPE_PISTOL) { PistolSlot = Slot; } }
    if(PistolSlot < 0) { Fail("no sidearm for a fight"); return false; }
    if(Inventory->Equipped != PistolSlot)
    {
        static const char* const Slots[] = { "Slot1", "Slot2", "Slot3" };
        if(PressClock <= 0.f) { LabDrive::Press(InOut, Slots[PistolSlot]); PressClock = 0.5f; }
        return false;
    }
    const int32_t Mag = Inventory->MagAt(PistolSlot);
    const int32_t Reserve = Inventory->ReserveAt(PistolSlot);

    /* Dry: the nearest ammo crate. */
    if(Mag <= 0 && Reserve <= 0 && !(Combat && Combat->bReloading))
    {
        const EntityID Crate = NearestNamed(InWorld, "AmmoCrate", Feet);
        if(!Crate.IsSet()) { Fail("out of ammunition, and no crate"); return false; }
        if(LabDrive::WalkTo(InWorld, InAvatar, InOut, InWorld.GetEntity(Crate).GetWorldPose().Position, 110.f) && PressClock <= 0.f)
        {
            LabDrive::Press(InOut, "Interact");
            PressClock = 0.5f;
        }
        return false;
    }

    const EntityID Target = NearestEnemy(InWorld, Feet);
    if(!Target.IsSet())
    {
        /* Nobody up yet: to the fight's anchor (its route's first point). */
        const RawStep& Raw = RawOf(Script, StepIndex);
        const std::string Anchor = WaypointName(Raw.Name, 0);
        if(Exists(InWorld, Anchor)) { LabDrive::WalkTo(InWorld, InAvatar, InOut, PositionOf(InWorld, Anchor.c_str()), 80.f); }
        if(Mag < 8 && Reserve > 0 && Combat && !Combat->bReloading && PressClock <= 0.f) { LabDrive::Press(InOut, "Reload"); PressClock = 0.6f; }
        return false;
    }

    const Vector3 Chest = LabDrive::ChestOf(InWorld, Target);
    Vector3 Origin, Dir;
    bool bClear = false;
    if(GetCharacterAim(InWorld, InAvatar, Origin, Dir))
    {
        const Vector3 Ray = Chest - Origin;
        PhysicsRaycastHit Hit;
        bClear = !PhysicsRaycast(InWorld, Origin, Ray, float(Ray.GetLength()) - 40.f, Hit, LAB_WORLD_MASK, InAvatar);
    }
    const double Distance = (InWorld.GetEntity(Target).GetWorldPose().Position - Feet).GetLength();
    if(!bClear || Distance > 3500.0)
    {
        /* Out of sight: close in. */
        LabDrive::WalkTo(InWorld, InAvatar, InOut, InWorld.GetEntity(Target).GetWorldPose().Position, 250.f);
        return false;
    }
    if(Distance < 300.0) { LabDrive::SetAxis(InOut, "MoveForward", -1.f); }   // a Brute up close: back off
    LabDrive::Press(InOut, "Aim", true);
    const float Off = LabDrive::AimAt(InWorld, InAvatar, InOut, Chest, 6.f);
    if(Mag <= 0)
    {
        if(Combat && !Combat->bReloading && PressClock <= 0.f) { LabDrive::Press(InOut, "Reload"); PressClock = 0.5f; }
        return false;
    }
    if(Off < 2.5f && FireClock <= 0.f)
    {
        LabDrive::Press(InOut, "Fire");
        FireClock = 0.3f;
    }
    return false;
}

void LabAutoPlay::Drive(World& InWorld, const EntityID InAvatar, InputSnapshot& InOut, const float InSeconds, const bool bInCutscene)
{
    InOut = InputSnapshot();
    if(!Result.empty()) { return; }
    StepTime += InSeconds;
    PressClock = std::max(0.f, PressClock - InSeconds);
    FireClock = std::max(0.f, FireClock - InSeconds);
    TraceClock -= InSeconds;
    if(TraceClock <= 0.f) { TraceClock = 5.f; Trace(InWorld, InAvatar); }
    if(LabSignals::Get(InWorld, "Game.Complete")) { bFinished = true; }
    if(bInCutscene) { StepTime = std::min(StepTime, 0.5f); return; }

    const Entity Self = InWorld.GetEntity(InAvatar);
    const LabHealthComponent* Health = Self.GetComponent<LabHealthComponent>();
    if(Health && Health->bDead) { return; }
    if(bWasDead)
    {
        bWasDead = false;
        StepTime = 0.f;
        Waypoint = 0;
        if(Deaths > 4) { Fail("fell too often"); return; }
    }

    if(Script < 0) { return; }
    const RawStep& Raw = RawOf(Script, StepIndex);
    if(StepTime > Raw.Limit)
    {
        Fail(Level + " step " + std::to_string(StepIndex) + " (" + Raw.Name + Raw.Signal + ") took too long");
        return;
    }
    const bool bSignal = Raw.Signal[0] && LabSignals::Get(InWorld, Raw.Signal);

    switch(Raw.Kind)
    {
    case LabAutoPlayStepKind::Wait:
        if(StepTime >= Raw.Limit - 0.05f) { Next("ready"); }
        break;

    case LabAutoPlayStepKind::Route:
    {
        const std::string Point = WaypointName(Raw.Name, Waypoint);
        if(!Exists(InWorld, Point))
        {
            if(Waypoint == 0) { Fail("no route " + Point); break; }
            Next((std::string("route ") + Raw.Name + " walked").c_str());
            break;
        }
        if(Steer(InWorld, InAvatar, InOut, PositionOf(InWorld, Point.c_str()), 70.f)) { ++Waypoint; }
        break;
    }

    case LabAutoPlayStepKind::Use:
    case LabAutoPlayStepKind::Hold:
    {
        if(bSignal) { Next((std::string("used ") + Raw.Name).c_str()); break; }
        const EntityID Thing = FindNamed(InWorld, Raw.Name);
        if(!Thing.IsSet()) { Fail(std::string("no entity ") + Raw.Name); break; }
        if(!Steer(InWorld, InAvatar, InOut, InWorld.GetEntity(Thing).GetWorldPose().Position, 100.f)) { break; }
        if(Raw.Kind == LabAutoPlayStepKind::Hold) { LabDrive::Press(InOut, "Interact", true); }
        else if(PressClock <= 0.f) { LabDrive::Press(InOut, "Interact"); PressClock = 0.6f; }
        break;
    }

    case LabAutoPlayStepKind::Fight:
        if(bSignal) { Next((std::string("fight over: ") + Raw.Signal).c_str()); break; }
        Fight(InWorld, InAvatar, InOut, InSeconds);
        break;

    case LabAutoPlayStepKind::DieOnce:
    {
        /* After the fight's checkpoint is written, fall once: the reload must
         * bring the player back whole, the fight won, the sidearm held. */
        if(!bKilled)
        {
            if(StepTime > 2.f && HasAuthority())
            {
                LabDamageEvent Fall;
                Fall.Victim = InAvatar;
                Fall.Amount = 1000.f;
                Fall.DamageType = EDamageType::EDT_Fall;
                Fall.Dir = Vector3(0, 0, 1);
                Fall.Point = Self.GetWorldPose().Position + Vector3(0, 100, 0);
                bKilled = true;
                CORE_LOG("[DemoLab] AutoPlay: falling once to test the checkpoint")
                LabDamage::Apply(InWorld, Fall);
            }
            break;
        }
        if(StepTime < 1.f) { break; }
        const LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
        bool bPistol = false;
        for(int Slot = 0; Inventory && Slot < LabWeapons::SLOTS; ++Slot) { bPistol |= Inventory->WeaponAt(Slot) == LAB_WEAPON_PIPE_PISTOL; }
        if(!bSignal) { Fail(std::string("the checkpoint lost ") + Raw.Signal); break; }
        if(!bPistol) { Fail("the checkpoint lost the sidearm"); break; }
        if(Health && Health->Health < Health->MaxHealth) { Fail("the player came back hurt"); break; }
        Next("back from the checkpoint whole, the fight won, the sidearm held");
        break;
    }

    case LabAutoPlayStepKind::Refill:
    {
        const LabInventoryComponent* Inventory = Self.GetComponent<LabInventoryComponent>();
        int32_t Reserve = 0;
        for(int Slot = 0; Inventory && Slot < LabWeapons::SLOTS; ++Slot)
        {
            if(Inventory->WeaponAt(Slot) == LAB_WEAPON_PIPE_PISTOL) { Reserve = Inventory->ReserveAt(Slot); }
        }
        if(Reserve >= 40 || StepTime > Raw.Limit - 1.f) { Next("ammunition topped up"); break; }
        const EntityID Crate = FindNamed(InWorld, Raw.Name);
        if(!Crate.IsSet()) { Next("no crate here"); break; }
        if(Steer(InWorld, InAvatar, InOut, InWorld.GetEntity(Crate).GetWorldPose().Position, 100.f) && PressClock <= 0.f)
        {
            LabDrive::Press(InOut, "Interact");
            PressClock = 0.6f;
        }
        break;
    }

    case LabAutoPlayStepKind::WaitSignal:
        if(bSignal) { Next((std::string("heard ") + Raw.Signal).c_str()); }
        break;

    case LabAutoPlayStepKind::WaitTravel:
    default:
        break;
    }
}
