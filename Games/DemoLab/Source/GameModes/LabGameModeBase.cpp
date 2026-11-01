#include "GameModes/LabGameModeBase.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AIFacade.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/GameConsole.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/PlayerState.h"

#include "AI/LabBots.h"
#include "GameModes/LabInputDriver.h"
#include "Core/LabPlayer.h"
#include "Weapons/LabProjectile.h"
#include "Weapons/LabWeapon.h"
#include "World/LabEncounter.h"
#include "World/LabInteractable.h"
#include "World/LabLogic.h"
#include "World/LabSignals.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace GameCore;
using namespace NimbleEngine;

/* The character a player gets: the base, or a fork of native:DemoLab.Player. */
NIMBLE_GAME_CONFIG_STRING(GLabCharacter, "DemoLab", "Character", "native:DemoLab.Player", nullptr);
/* The local player's character is bot-driven (LabGameModeBase::IsSelfBot). */
NIMBLE_GAME_CONFIG_BOOL(GLabSelfBot, "DemoLab", "SelfBot", false, nullptr);

bool LabGameModeBase::IsSelfBot() const { return GLabSelfBot.GetBool(); }

bool LabGameModeBase::OnLoad(GameSession& InSession)
{
    if(!Super::OnLoad(InSession)) { return false; }

    LabWeapons::ResetCache();
    LabSignals::Reset(InSession.GetWorld());

    AddSystem<&LabGameModeBase::TickCombatInput>("Lab.Combat", TP_PrePhysics);
    AddSystem<&LabGameModeBase::TickInteractInput>("Lab.Interact", TP_PrePhysics);
    AddSystem<&LabGameModeBase::TickCombatServer>("Lab.CombatServer", TP_PrePhysics);
    AddSystem<&LabGameModeBase::TickProjectiles>("Lab.Projectiles", TP_PrePhysics);
    AddSystem<&LabGameModeBase::TickWorld>("Lab.World", TP_PostPhysics);
    AddSystem<&LabGameModeBase::TickWeaponPresentation>("Lab.WeaponPresentation", TP_PostPhysics);
    AddSystem<&LabGameModeBase::TickWorldPresentation>("Lab.WorldPresentation", TP_PostPhysics);

    Subscribe<&LabGameModeBase::OnSignalEvent>(LabSignals::EVENT);
    Subscribe<&LabGameModeBase::OnEncounterStartEvent>("Encounter.Start");
    Subscribe<&LabGameModeBase::OnDamagedEvent>("Lab.Damaged");
    Subscribe<&LabGameModeBase::OnKilledEvent>("Lab.Killed");
    SetAvatarInputFilter<&LabGameModeBase::FilterInput>();
    Subscribe<&LabGameModeBase::OnSequenceStarted>("SequenceStarted");
    Subscribe<&LabGameModeBase::OnSequenceEnded>("SequenceFinished");
    Subscribe<&LabGameModeBase::OnSequenceEnded>("SequenceStopped");
    PlayingSequences.clear();
    return true;
}

void LabGameModeBase::OnStart()
{
    Super::OnStart();
    if(IsDedicatedServer()) { return; }
    LabHUD::Open(GetSession());
    if(WantsMouseCapture()) { GetSession().SetMouseCaptured(true); }
}

void LabGameModeBase::OnTick(const FrameContext& InFrame)
{
    FrameSeconds = InFrame.DeltaSeconds;
    LabSignals::Flush(GetWorld());
    LabBots::TickStimuli(GetWorld());
    UpdateSelfBot();
    if(IsDedicatedServer()) { return; }

    for(size_t I = 0; I < FeedLines.size();)
    {
        FeedLines[I].Seconds -= FrameSeconds;
        if(FeedLines[I].Seconds <= 0.f) { FeedLines.erase(FeedLines.begin() + long(I)); } else { ++I; }
    }
    HudStory.Feed.clear();
    for(const FeedLine& Line : FeedLines) { HudStory.Feed.push_back(Line.Text); }
    UpdateMouseCapture();
    HudStory.bCinematic = IsSequencePlaying();
    LabHUD::Update(GetSession(), HudStory);
}

void LabGameModeBase::OnUnload()
{
    if(HasSession())
    {
        LabCombat::ClearPresentation(GetWorld(), Presented);
        LabSignals::Reset(GetWorld());
        if(!IsDedicatedServer())
        {
            LabHUD::Close(GetSession());
            GetSession().SetMouseCaptured(false);
        }
    }
    Presented.clear();
    PlayingSequences.clear();
}

const char* LabGameModeBase::GetAvatarPrefab(EntityID)
{
    return GLabCharacter.GetString();
}

void LabGameModeBase::OnAvatarSpawned(EntityID, const EntityID InAvatar, uint32_t)
{
    World& W = GetWorld();
    LabEnsureLayout(W, InAvatar);
    LabApplyCharacter(W, InAvatar, true);
    /* Seen by enemy bots (LabBots::TickStimuli keeps it current). */
    SetAIStimulus(W, InAvatar, LabTeams::TeamOf(W, InAvatar), true);
}

void LabGameModeBase::Respawn(const EntityID InAvatar, const TransformPose* InAt)
{
    World& W = GetWorld();
    if(!HasAuthority() || !W.IsAlive(InAvatar)) { return; }
    LabDamage::Revive(W, InAvatar);
    if(InAt) { TeleportCharacter(W, InAvatar, InAt->Position, float(InAt->Rotation.GetY())); }
    if(LabCombatComponent* Combat = W.GetEntity(InAvatar).GetComponent<LabCombatComponent>())
    {
        Combat->bReloading = 0;
        Combat->MeleePhase = 0;
        Combat->Cooldown = 0.5f;
    }
}

/* Escape frees the mouse (to reach the editor, or a menu later); a click
 * takes it back. */
void LabGameModeBase::UpdateMouseCapture()
{
    if(!WantsMouseCapture()) { return; }
    World& W = GetWorld();
    const EntityID Local = GetLocalAvatar(W, 0);
    const AvatarHarnessComponent* Harness = Local.IsSet() ? W.GetEntity(Local).GetComponent<AvatarHarnessComponent>() : nullptr;
    if(!Harness) { return; }
    if(Harness->WasPressed("Pause")) { OnPausePressed(); }
    else if(!GetSession().IsMouseCaptured() && (Harness->WasPressed("Fire") || Harness->WasPressed("Aim")))
    {
        GetSession().SetMouseCaptured(true);
    }
}

const char* LabName(World& InWorld, const EntityID InCharacter)
{
    if(!InCharacter.IsSet() || !InWorld.IsAlive(InCharacter)) { return "the world"; }
    if(const LabCharacterComponent* Character = InWorld.GetEntity(InCharacter).GetComponent<LabCharacterComponent>()) { return Character->DisplayName; }
    return InWorld.GetEntity(InCharacter).GetName();
}

// === Systems ================================================================

void LabGameModeBase::TickCombatInput(World& InWorld, const float InSeconds)
{
    LabCombat::TickInput(InWorld, InSeconds);
}

void LabGameModeBase::TickInteractInput(World& InWorld, const float InSeconds)
{
    LabWorld::TickInteractInput(InWorld, InSeconds);
}

void LabGameModeBase::TickCombatServer(World& InWorld, const float InSeconds)
{
    LabCombat::TickServer(InWorld, InSeconds);
}

void LabGameModeBase::TickProjectiles(World& InWorld, const float InSeconds)
{
    if(HasAuthority()) { LabProjectiles::TickServer(InWorld, InSeconds); }
    else { LabProjectiles::TickClient(InWorld, InSeconds); }
}

void LabGameModeBase::TickWorld(World& InWorld, const float InSeconds)
{
    LabSignals::Flush(InWorld);
    LabWorld::TickTriggers(InWorld, InSeconds);
    LabEncounters::Tick(InWorld, InSeconds);
    LabLogic::Tick(InWorld, InSeconds);
}

void LabGameModeBase::TickWeaponPresentation(World& InWorld, const float InSeconds)
{
    LabCombat::TickPresentation(InWorld, InSeconds, Presented);
}

void LabGameModeBase::TickWorldPresentation(World& InWorld, const float InSeconds)
{
    LabWorld::TickPresentation(InWorld, InSeconds);
    LabLogic::TickPresentation(InWorld);
}

// === Events =================================================================

void LabGameModeBase::OnSignalEvent(const EventArgs& InArgs)
{
    const char* Channel = nullptr;
    bool bValue = true;
    if(!LabSignals::Read(InArgs, Channel, bValue)) { return; }

    /* Copied: a listener below may emit, and the bus may reuse the args. */
    const std::string Name = Channel;
    World& W = GetWorld();
    LabSignals::Latch(W, Name.c_str(), bValue);
    CORE_LOG("[DemoLab] signal " + Name + (bValue ? " on" : " off"))
    LabSignals::BeginHandling(W);
    LabWorld::OnSignal(W, Name.c_str(), bValue);
    LabEncounters::OnSignal(W, Name.c_str(), bValue);
    LabLogic::OnSignal(W, Name.c_str(), bValue);
    OnSignal(Name.c_str(), bValue);
    LabSignals::EndHandling(W);
}

void LabGameModeBase::OnEncounterStartEvent(const EventArgs& InArgs)
{
    if(InArgs.Count >= 1 && InArgs.Args[0].Type == EventArg::ET_String)
    {
        const std::string Id = InArgs.Args[0].String;
        LabEncounters::StartById(GetWorld(), Id.c_str());
    }
}

void LabGameModeBase::FilterInput(World& InWorld, const EntityID InAvatar, InputSnapshot& InOutInput)
{
    if(InWorld.GetEntity(InAvatar).GetComponent<LabBotComponent>())
    {
        /* A cutscene holds the bots as the engine holds the players. */
        if(IsSequencePlaying()) { InOutInput = InputSnapshot(); return; }
        LabBots::Drive(InWorld, InAvatar, InOutInput, FrameSeconds);
    }
    else { FilterPlayerInput(InWorld, InAvatar, InOutInput); }

    /* Held: the look still turns, nothing else. */
    if(IsInputHeld())
    {
        const int LookX = FindInputAxis(LabDrive::MAP, "LookX"), LookY = FindInputAxis(LabDrive::MAP, "LookY");
        InputSnapshot Held;
        if(LookX >= 0) { Held.Axes[LookX] = InOutInput.Axes[LookX]; }
        if(LookY >= 0) { Held.Axes[LookY] = InOutInput.Axes[LookY]; }
        InOutInput = Held;
    }
}

void LabGameModeBase::UpdateSelfBot()
{
    if(!IsSelfBot()) { return; }
    World& W = GetWorld();
    const EntityID Local = GetLocalAvatar(W, 0);
    if(!Local.IsSet() || W.GetEntity(Local).GetComponent<LabBotComponent>()) { return; }
    LabBotComponent Bot;
    Bot.BotPlayer = GetPossessor(W, Local);
    Bot.Seed = uint32_t(Local.Pack() * 2246822519u) | 1u;
    W.GetEntity(Local).AddComponent(Bot);
    if(HasAuthority() && IsAIAvailable())
    {
        AIBotSetup Setup;
        Setup.Tree = SelfBotTree();
        Setup.Team = LabTeams::TeamOf(W, Local);
        Setup.bDriveTransform = false;
        Setup.MeshYaw = 180.f;
        Setup.SightRange = 4500.f;
        Setup.FovHalfAngle = 80.f;
        Setup.TargetHeight = 115.f;
        MakeAIBot(W, Local, Setup);
    }
    CORE_LOG("[DemoLab] the local character plays by itself")
}

void LabGameModeBase::PushFeed(const std::string& InLine)
{
    FeedLines.push_back({ InLine, 6.f });
    if(FeedLines.size() > 4) { FeedLines.erase(FeedLines.begin()); }
}

void LabGameModeBase::OnKilledEvent(const EventArgs& InArgs)
{
    if(!HasAuthority() || InArgs.Count < 3 || InArgs.Args[0].Type != EventArg::ET_Entity) { return; }
    World& W = GetWorld();
    const EntityID Victim = EntityID::Unpack(InArgs.Args[0].Entity);
    const EntityID Killer = InArgs.Args[1].Type == EventArg::ET_Entity ? EntityID::Unpack(InArgs.Args[1].Entity) : EntityID();
    const uint8_t WeaponID = InArgs.Args[2].Type == EventArg::ET_Number ? uint8_t(InArgs.Args[2].Number) : uint8_t(0);

    const std::string VictimName = LabName(W, Victim);
    std::string Line;
    if(!Killer.IsSet() || Killer == Victim) { Line = VictimName + " went down"; }
    else
    {
        const std::string KillerName = LabName(W, Killer);
        const LabWeaponComponent* Weapon = WeaponID ? LabWeapons::GetData(W, WeaponID) : nullptr;
        Line = KillerName + "  [" + (Weapon ? Weapon->DisplayName : "Fists") + "]  " + VictimName;
    }
    CORE_LOG("[DemoLab] feed: " + Line)

    if(!IsDedicatedServer()) { PushFeed(Line); }
}

void LabGameModeBase::OnDamagedEvent(const EventArgs& InArgs)
{
    if(InArgs.Count < 2 || InArgs.Args[0].Type != EventArg::ET_Entity || InArgs.Args[1].Type != EventArg::ET_Entity) { return; }
    LabBots::OnDamaged(GetWorld(), EntityID::Unpack(InArgs.Args[0].Entity), EntityID::Unpack(InArgs.Args[1].Entity));
}

void LabGameModeBase::OnSequenceStarted(const EventArgs& InArgs)
{
    if(InArgs.Count < 1 || InArgs.Args[0].Type != EventArg::ET_Entity) { return; }
    for(const uint64_t Playing : PlayingSequences) { if(Playing == InArgs.Args[0].Entity) { return; } }
    PlayingSequences.push_back(InArgs.Args[0].Entity);
    OnCutsceneStarted(EntityID::Unpack(InArgs.Args[0].Entity), InArgs.Count > 1 && InArgs.Args[1].Type == EventArg::ET_String ? InArgs.Args[1].String : "");
    CORE_LOG(std::string("[DemoLab] cutscene started: ") + (InArgs.Count > 1 && InArgs.Args[1].Type == EventArg::ET_String ? InArgs.Args[1].String : "?"))
}

void LabGameModeBase::OnSequenceEnded(const EventArgs& InArgs)
{
    if(InArgs.Count < 1 || InArgs.Args[0].Type != EventArg::ET_Entity) { return; }
    for(size_t I = 0; I < PlayingSequences.size(); ++I)
    {
        if(PlayingSequences[I] == InArgs.Args[0].Entity)
        {
            PlayingSequences.erase(PlayingSequences.begin() + long(I));
            CORE_LOG(std::string("[DemoLab] cutscene ended: ") + (InArgs.Count > 1 && InArgs.Args[1].Type == EventArg::ET_String ? InArgs.Args[1].String : "?"))
            return;
        }
    }
}

// === Console ================================================================

/* lab_damage <amount>: hurts the local character (the authority's world). */
NIMBLE_CONSOLE_COMMAND(lab_damage, "<amount>", "Damage the local player's character (negative heals)")
{
    World& W = Console.Session.GetWorld();
    const EntityID Avatar = GetLocalAvatar(W, 0);
    if(!Avatar.IsSet()) { Console.PrintError("lab_damage: no local character"); return; }
    const float Amount = Console.Args.empty() ? 25.f : float(std::atof(Console.Args[0].c_str()));
    LabDamageEvent Event;
    Event.Victim     = Avatar;
    Event.Amount     = Amount < 0.f ? -Amount : Amount;
    Event.DamageType = Amount < 0.f ? EDamageType::EDT_Heal : EDamageType::EDT_Fall;
    Event.Point      = W.GetEntity(Avatar).GetWorldPose().Position + EngineCore::Vector3(0, 120, 0);
    Event.Dir        = EngineCore::Vector3(0, 0, 1);
    Event.Impulse    = 4000.f;
    const float Taken = LabDamage::Apply(W, Event);
    Console.Print("lab_damage: " + std::to_string(Taken));
}

/* lab_give <weapon id>: puts a registry weapon in the local character's hands. */
NIMBLE_CONSOLE_COMMAND(lab_give, "<weapon id>", "Give the local player's character a weapon (1 Wrench, 2 Pipe Pistol, 3 Scrap Rifle)")
{
    World& W = Console.Session.GetWorld();
    const EntityID Avatar = GetLocalAvatar(W, 0);
    if(!Avatar.IsSet() || Console.Args.empty()) { Console.PrintError("lab_give: no local character, or no id"); return; }
    const int ID = std::atoi(Console.Args[0].c_str());
    Console.Print(LabCombat::GiveWeapon(W, Avatar, uint8_t(ID), true) ? "lab_give: given" : "lab_give: unknown weapon");
}

/* lab_signal <channel> [0|1]: emits a story signal, as a lever would. */
NIMBLE_CONSOLE_COMMAND(lab_signal, "<channel> [0|1]", "Emit a DemoLab signal")
{
    if(Console.Args.empty()) { Console.PrintError("lab_signal: no channel"); return; }
    const bool bValue = Console.Args.size() < 2 || Console.Args[1] != "0";
    LabSignals::Emit(Console.Session.GetWorld(), Console.Args[0].c_str(), bValue);
}
