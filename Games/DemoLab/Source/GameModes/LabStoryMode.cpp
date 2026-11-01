#include "GameModes/LabGameModeBase.h"

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/AudioFacade.h"
#include "Public/NimbleEngine/Avatar.h"
#include "Public/NimbleEngine/CharacterMover.h"
#include "Public/NimbleEngine/GameState.h"
#include "Public/NimbleEngine/Input.h"
#include "Public/NimbleEngine/Level.h"
#include "Public/NimbleEngine/SaveGame.h"
#include "Public/NimbleEngine/SequenceFacade.h"
#include "Public/UI/UISystem.h"

#include "AI/LabBots.h"
#include "Flow/LabFlow.h"
#include "Flow/LabTravel.h"
#include "GameModes/LabAutoPlay.h"
#include "GameModes/LabInputDriver.h"
#include "UI/LabUI.h"
#include "Weapons/LabWeapon.h"
#include "World/LabEncounter.h"
#include "World/LabInteractable.h"
#include "World/LabLogic.h"
#include "World/LabSignals.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace GameCore;
using namespace NimbleEngine;

/* The test hook: the game plays itself (GameModes/LabAutoPlay.h). */
NIMBLE_GAME_CONFIG_BOOL(GLabAutoPlay, "DemoLab", "AutoPlay", false, nullptr);
/* The story's difficulty: the share of enemy damage the player takes. */
NIMBLE_GAME_CONFIG_FLOAT(GLabStoryDamageScale, "DemoLab", "StoryDamageScale", 0.25f, nullptr);
/* Health back per second, once the player has not been hurt for RegenDelay s. */
NIMBLE_GAME_CONFIG_FLOAT(GLabRegenRate, "DemoLab", "RegenRate", 8.0f, nullptr);
NIMBLE_GAME_CONFIG_FLOAT(GLabRegenDelay, "DemoLab", "RegenDelay", 5.0f, nullptr);
/* Seconds the credits roll before the menu comes back. */
NIMBLE_GAME_CONFIG_FLOAT(GLabCreditsSeconds, "DemoLab", "CreditsSeconds", 14.0f, nullptr);

bool LabIsAutoPlay() { return GLabAutoPlay.GetBool(); }

/*
 * Watching one cinematic (Tools/Cinematics/README.md): -DemoLab.Cinematic=C3
 * plays C3 in its level -- travelling to it, setting the signals the story
 * would have set by then, standing the player on its "CineMark_C3" -- and
 * quits a few seconds after it ends. With -record=<file.mp4>, its video.
 */
NIMBLE_GAME_CONFIG_STRING(GLabCinematic, "DemoLab", "Cinematic", "", nullptr);

namespace
{
    struct LabCinematicTest
    {
        const char* Id;
        const char* Level;
        bool        bAlarm;
        const char* Entry;      // where a travel to the level arrives
        const char* Player;     // the Sequence Player that plays it
        const char* Before;     // comma-separated signals set first; ">Name" plays and skips that player
    };

    const LabCinematicTest LAB_CINEMATICS[] = {
        { "C1", "L1_Foothills",  false, nullptr,             "C1_Arrival",          "" },
        { "C2", "L1_Foothills",  false, nullptr,             "C2_BlastDoor",        "L1.AtGate,L1.GateOpen,L1.Plaza" },
        { "C3", "L2_Laboratory", false, nullptr,             "Calm_C3_Breaker",     "" },
        { "C4", "L2_Laboratory", false, nullptr,             "Calm_C4_Descent",
          ">Calm_C3_Breaker,L2.Keycard,L2.ArchiveOpen,L2.TurbineHall,L2.Valve1,L2.Valve2,L2.ValvesDone" },
        { "C5", "L3_Core",       false, nullptr,             "C5_Theft",            "" },
        { "C6", "L2_Laboratory", true,  "Entry_FreightLift", "Alarm_C6_Extraction", "L2A.W3Done,L2A.StragglersDone,L2A.HoldDone" },
    };

    bool GLabCinematicPlayed = false;   // it has played: a level travelled to after it only quits

    const LabCinematicTest* FindCinematicTest()
    {
        const char* Id = GLabCinematic.GetString();
        if(!Id || !Id[0]) { return nullptr; }
        for(const LabCinematicTest& Test : LAB_CINEMATICS)
        {
            if(std::strcmp(Test.Id, Id) == 0) { return &Test; }
        }
        return nullptr;
    }
}

/*
 * What a checkpoint keeps beyond the World: the objective, the latched
 * signals (LabSignals.h), where the character stands back up, and the travel
 * state (Flow/LabTravel.h) -- so Continue lands in the Alarm state it left.
 * The World save covers everything on entities: health, inventory, gates,
 * encounters, timers, used interactables.
 */
class LabStoryState : public GameState
{
public:
    void OnReset() override
    {
        Objective.clear();
        Signals.clear();
        Travel.clear();
        bHaveCheckpoint = false;
        CheckpointX = CheckpointY = CheckpointZ = CheckpointYaw = 0.0;
    }

    /* A new field goes at the end, behind Ar.GetChunkVersion() >= 2, with this bumped. */
    uint16_t GetSerialVersion() const override { return 1; }

    void Serialize(EngineCore::Archive& Ar) override
    {
        Ar << Objective;
        uint32_t Count = uint32_t(Signals.size());
        Ar << Count;
        if(Ar.IsLoading()) { Signals.resize(Count); }
        for(std::pair<std::string, bool>& Signal : Signals) { Ar << Signal.first << Signal.second; }
        Ar << bHaveCheckpoint << CheckpointX << CheckpointY << CheckpointZ << CheckpointYaw;
        Ar << Travel;
    }

    std::string Objective;
    std::vector<std::pair<std::string, bool>> Signals;
    std::string Travel;
    bool   bHaveCheckpoint = false;
    double CheckpointX = 0.0, CheckpointY = 0.0, CheckpointZ = 0.0, CheckpointYaw = 0.0;
};

/*
 * The story (Docs/Design.md): every level of the flow table runs this mode.
 * The level drives itself with signals (LabSignals.h), interactables,
 * encounters, timers and cinematics; this mode keeps the objective and the
 * banner, applies the travel state (the entry Player Start, the Calm/Alarm
 * groups), travels when a LabTravel's signal fires, saves checkpoints and
 * reloads the last one when the character falls.
 *
 * Checkpoints (SaveGameToFile, LabFlow::CHECKPOINT_PATH) are saved
 *   - as each level starts (once its opening cinematic is over),
 *   - on any signal named "Checkpoint.*" (a cinematic's last Event key),
 *   - at a LabTrigger with Checkpoint on, and
 *   - when an encounter is cleared.
 *
 * Events it listens for (from C++, Lua, or Sequencer Event keys):
 *
 *   Objective.Set     arg0 = the text          (or a LabObjective on a signal)
 *   Story.Banner      arg0 = the text, arg1 = seconds (default 4)
 *   Lab.Checkpoint    a LabTrigger with Checkpoint on (sent by the trigger)
 *   Encounter.Start   arg0 = an encounter's Id (handled by the base)
 *
 * and the signal "Game.Complete": the credits roll, then the main menu.
 */
class LabStoryMode : public LabGameModeBase {
public:
    bool OnLoad(GameSession& InSession) override
    {
        if(!LabGameModeBase::OnLoad(InSession)) { return false; }
        LevelName = InSession.GetLevel() && InSession.GetLevel()->GetName() ? InSession.GetLevel()->GetName() : "";

        /* The travel state the level came in with (Flow/LabTravel.h); the
         * engine has already placed the avatar's entry. */
        LabTravel::Arrive(InSession, LevelName.c_str());
        bAlarm = LabTravel::GetFlag("Alarm");
        const int Dropped = LabLogic::ApplyStateGroups(InSession.GetWorld(), bAlarm);
        CORE_LOG("[DemoLab] " + LevelName + (bAlarm ? " (Alarm)" : "") + ": " + std::to_string(Dropped) + " state-group entities dropped")

        Subscribe<&LabStoryMode::OnObjectiveSet>("Objective.Set");
        Subscribe<&LabStoryMode::OnBanner>("Story.Banner");
        Subscribe<&LabStoryMode::OnCheckpoint>("Lab.Checkpoint");
        Subscribe<&LabStoryMode::OnInteracted>("Lab.Interacted");
        Subscribe<&LabStoryMode::OnEncounterDone>("Encounter.Done");
        Subscribe<&LabStoryMode::OnDamaged>("Lab.Damaged");
        Subscribe<&LabStoryMode::OnPlacePlayer>("Lab.PlacePlayer");
        return true;
    }

    void OnStart() override
    {
        LabGameModeBase::OnStart();
        if(bAlarm) { LabSignals::Emit(GetWorld(), "State.Alarm", true); }
        GetLabAutoPlay().OnLevelStarted(LevelName, bAlarm, bRestored);
        StartCinematicTest();
        if(bRestored)
        {
            HudStory = LabHUD::Story();
            HudStory.Objective = State.Objective;
            ShowBanner("Checkpoint", 2.f);
            return;
        }
        HudStory = LabHUD::Story();
        ShowObjectivesFor("");
        bSaveRequested = true;   // the level's start is a checkpoint
    }

    void OnUnload() override
    {
        if(HasSession() && !IsDedicatedServer())
        {
            UI::UISystem& UI = GetSession().GetUI();
            for(const char* Doc : { LabUI::PAUSE_MENU, LabUI::SETTINGS, LabUI::CREDITS })
            {
                UI.Close(std::string(Doc));
            }
            GetSession().SetPaused(false);
        }
        LabGameModeBase::OnUnload();
    }

    GameState* GetGameState() override { return &State; }

    void OnPreSaveGame() override
    {
        State.Objective = HudStory.Objective;
        State.Signals = LabSignals::Snapshot(GetWorld());
        State.Travel = LabTravel::Save();
        State.bHaveCheckpoint = bHaveCheckpoint;
        State.CheckpointX = Checkpoint.Position.GetX();
        State.CheckpointY = Checkpoint.Position.GetY();
        State.CheckpointZ = Checkpoint.Position.GetZ();
        State.CheckpointYaw = Checkpoint.Rotation.GetY();
    }

    /* Before a checkpoint is restored: an empty World, or the scene doubles. */
    void OnPrepareForLoadedGame() override
    {
        World& W = GetWorld();
        Presented.clear();
        std::vector<EntityID> Doomed;
        W.ForEachEntity([](EntityID InEntity, void* InUserData) { static_cast<std::vector<EntityID>*>(InUserData)->push_back(InEntity); }, &Doomed);
        for(const EntityID E : Doomed) { W.DestroyEntity(E); }
    }

    void OnPostLoadSave() override
    {
        World& W = GetWorld();
        LabSignals::Restore(W, State.Signals);
        LabTravel::Restore(State.Travel);
        bAlarm = LabTravel::GetFlag("Alarm");
        bHaveCheckpoint = State.bHaveCheckpoint;
        Checkpoint.Position = EngineCore::Vector3(State.CheckpointX, State.CheckpointY, State.CheckpointZ);
        Checkpoint.Rotation = EngineCore::Vector3(0, State.CheckpointYaw, 0);
        LabCombat::ClearTransients(W, Presented);
        LabEncounters::RestartInterrupted(W);

        /* The character stands at the checkpoint, whole -- however the save found it. */
        const EntityID Local = FindLocalPlayer(W, 0).IsSet() ? GetAvatar(W, FindLocalPlayer(W, 0)) : EntityID();
        if(Local.IsSet() && W.IsAlive(Local)) { Respawn(Local, bHaveCheckpoint ? &Checkpoint : nullptr); }
        bRestored = true;
        RespawnIn = 0.f;
        ReloadIn = 0.f;
        CORE_LOG("[DemoLab] checkpoint restored")
    }

    void OnTick(const FrameContext& InFrame) override
    {
        const float Dt = float(InFrame.DeltaSeconds);
        LabAutoPlay& Auto = GetLabAutoPlay();
        if(GLabAutoPlay.GetBool())
        {
            Auto.TickClock(Dt);
            if(!Auto.GetResult().empty() && Auto.GetResult() != "PASS")
            {
                QuitIn -= Dt;
                if(QuitIn <= 0.f && QuitIn > -1000.f) { QuitIn = -2000.f; GetSession().RequestQuit(); }
            }
        }
        if(BannerFor > 0.f)
        {
            BannerFor -= Dt;
            if(BannerFor <= 0.f) { HudStory.Banner.clear(); }
        }
        if(!IsDedicatedServer())
        {
            LabUI::UpdateTechOverlay(GetSession(), LevelName, bAlarm);
            LabUI::LogPerf(GetSession(), LevelName + (bAlarm ? " (Alarm)" : ""), Dt);
            LabUI::ApplySubtitleSetting(GetSession());
            UpdateLocalInput();
        }
        HudStory.bIntel = bAlarm || LabSignals::Get(GetWorld(), "L3.Intel");

        /* A checkpoint asked for: saved here, between frames, not from a system. */
        if(HasAuthority() && bSaveRequested && !IsSequencePlaying() && !bTravelling)
        {
            bSaveRequested = false;
            std::error_code Ignored;
            std::filesystem::create_directories(std::filesystem::path(LabFlow::CHECKPOINT_PATH).parent_path(), Ignored);
            if(SaveGameToFile(GetSession(), LabFlow::CHECKPOINT_PATH)) { CORE_LOG("[DemoLab] checkpoint saved") }
        }
        if(HasAuthority() && ReloadIn > 0.f)
        {
            ReloadIn -= Dt;
            if(ReloadIn <= 0.f && !LoadGameFromFile(GetSession(), LabFlow::CHECKPOINT_PATH))
            {
                RespawnIn = 0.01f;   // no checkpoint after all: stand up here
            }
        }
        if(HasAuthority() && RespawnIn > 0.f)
        {
            RespawnIn -= Dt;
            if(RespawnIn <= 0.f && GetWorld().IsAlive(Fallen))
            {
                Respawn(Fallen, bHaveCheckpoint ? &Checkpoint : nullptr);
                ShowBanner("Back on your feet", 2.f);
            }
        }
        TickRegen(Dt);
        TickCinematicTest(Dt);
        TickTravel(Dt);
        LabGameModeBase::OnTick(InFrame);
    }

    bool WantsMouseCapture() const override { return !GLabAutoPlay.GetBool(); }

    void OnPausePressed() override
    {
        if(GLabAutoPlay.GetBool() || bTravelling || bCredits) { return; }
        LabUI::OpenPauseMenu(GetSession());
    }

    /* Nobody is hurt while a cutscene plays: the player cannot act. */
    float ModifyDamage(const LabDamageEvent& InEvent, const float InAmount) override
    {
        if(IsSequencePlaying() || bCredits) { return 0.f; }
        /* The difficulty: what enemies do to the player. */
        const Entity Victim = GetWorld().GetEntity(InEvent.Victim);
        const bool bPlayer = Victim.GetComponent<AvatarHarnessComponent>() && !Victim.GetComponent<LabBotComponent>();
        const bool bFromEnemy = InEvent.Instigator.IsSet() && InEvent.Instigator != InEvent.Victim;
        return bPlayer && bFromEnemy ? InAmount * std::max(0.f, GLabStoryDamageScale.GetFloat()) : InAmount;
    }

    /* Back from a checkpoint, the level's opening cinematic has been seen:
     * it stops as it starts. */
    void OnCutsceneStarted(const EntityID InPlayer, const char* InPath) override
    {
        /* Watching another cinematic: this one (the level's opening) is passed. */
        const LabCinematicTest* Test = FindCinematicTest();
        if(Test && InPath && !std::strstr(InPath, (std::string("/") + Test->Id + "_").c_str()) && GetWorld().IsAlive(InPlayer))
        {
            SkipSequence(GetWorld(), InPlayer);
            return;
        }
        if(Test) { bCineStarted = true; GLabCinematicPlayed = true; }
        const bool bOpening = InPath && (std::strstr(InPath, "C1_") || std::strstr(InPath, "Opening"));
        if(bRestored && bOpening && GetWorld().IsAlive(InPlayer))
        {
            SkipSequence(GetWorld(), InPlayer);   // to its end: played, so it does not start again
        }
    }

    /* Straight to death: no downed state in the story. */
    bool UsesDowned() const override { return false; }

    void OnLabKilled(const EntityID InVictim, EntityID, const uint8_t InWeaponID) override
    {
        const Entity Victim = GetWorld().GetEntity(InVictim);
        if(!Victim.GetComponent<AvatarHarnessComponent>() || Victim.GetComponent<LabBotComponent>())
        {
            CORE_LOG(std::string("[DemoLab] downed ") + LabName(GetWorld(), InVictim) + " with weapon " + std::to_string(int(InWeaponID)))
            PlaySoundAt(GetWorld(), "Content/Sounds/Robots/Robot_Death.nmsound", Victim.GetWorldPose().Position);
            return;
        }
        Fallen = InVictim;
        GetLabAutoPlay().NoteFall();
        ShowBanner("Down! Back to the checkpoint...", 3.f);
        CORE_LOG("[DemoLab] the player fell")
        if(LabFlow::HasCheckpoint()) { ReloadIn = 2.5f; }
        else { RespawnIn = 3.f; }
    }

protected:
    void OnAvatarSpawned(const EntityID InPlayer, const EntityID InAvatar, const uint32_t InOrdinal) override
    {
        LabGameModeBase::OnAvatarSpawned(InPlayer, InAvatar, InOrdinal);
        /* Travelling armed (the freight lift back up, Chapter Select's row 4): the sidearm. */
        if(LabTravel::GetFlag("Armed")) { LabCombat::GiveWeapon(GetWorld(), InAvatar, LAB_WEAPON_PIPE_PISTOL, true); }
        if(!bHaveCheckpoint)
        {
            Checkpoint = GetWorld().GetEntity(InAvatar).GetWorldPose();
            Checkpoint.Scale = EngineCore::Vector3(1, 1, 1);
            if(const CharacterMoverComponent* Mover = GetWorld().GetEntity(InAvatar).GetComponent<CharacterMoverComponent>())
            {
                Checkpoint.Rotation = EngineCore::Vector3(0, double(Mover->BodyYaw), 0);
            }
            bHaveCheckpoint = true;
        }
    }

    void OnSignal(const char* InChannel, const bool bInValue) override
    {
        if(!bInValue) { return; }
        if(std::strcmp(InChannel, "Game.Complete") == 0) { StartCredits(); return; }
        if(std::strncmp(InChannel, "Checkpoint.", 11) == 0) { CheckpointHere(); }

        /* The end of a level: a LabTravel listening for this signal. */
        GetWorld().Each<LabTravelComponent>([&](Entity, LabTravelComponent& InTravel)
        {
            if(InTravel.bFired || std::strcmp(InTravel.OnSignal, InChannel) != 0) { return; }
            InTravel.bFired = 1;
            PendingTravel = InTravel;
            bTravelPending = true;
        });
        ShowObjectivesFor(InChannel);
    }

private:
    /* The player's input: the AutoPlay test's, or the devices' with the
     * Controls settings applied. A cutscene's lock is the engine's. */
    void FilterPlayerInput(World& InWorld, const EntityID InAvatar, InputSnapshot& InOutInput) override
    {
        if(InAvatar != GetLocalAvatar(InWorld, 0)) { return; }
        if(GLabAutoPlay.GetBool())
        {
            GetLabAutoPlay().Drive(InWorld, InAvatar, InOutInput, GetFrameSeconds(), IsSequencePlaying());
            return;
        }
        const int LookX = FindInputAxis(LabDrive::MAP, "LookX"), LookY = FindInputAxis(LabDrive::MAP, "LookY");
        const float Scale = LabUI::LookScale();
        if(LookX >= 0 && LookX < int(INPUT_AXES_MAX)) { InOutInput.Axes[LookX] *= Scale; }
        if(LookY >= 0 && LookY < int(INPUT_AXES_MAX)) { InOutInput.Axes[LookY] *= LabUI::InvertLook() ? -Scale : Scale; }
    }

    /* F1 and the like: the local player's harness, read on the game thread. */
    void UpdateLocalInput()
    {
        World& W = GetWorld();
        const EntityID Local = GetLocalAvatar(W, 0);
        const AvatarHarnessComponent* Harness = Local.IsSet() ? W.GetEntity(Local).GetComponent<AvatarHarnessComponent>() : nullptr;
        if(Harness && Harness->WasPressed("TechOverlay")) { LabUI::ToggleTechOverlay(GetSession()); }
    }

    /* -DemoLab.Cinematic: to the cinematic's level and state, or its stage set. */
    void StartCinematicTest()
    {
        const LabCinematicTest* Test = FindCinematicTest();
        if(!Test) { return; }
        if(GLabCinematicPlayed) { GetSession().RequestQuit(); return; }
        if(LevelName != Test->Level || bAlarm != Test->bAlarm)
        {
            CORE_LOG(std::string("[DemoLab] cinematic test ") + Test->Id + ": travelling to " + Test->Level)
            bTravelling = true;
            LabTravel::Travel(GetSession(), Test->Level, Test->Entry, Test->bAlarm ? "Alarm=1" : "", "");
            return;
        }
        CineClock = 1.5f;   // the level settles (and an opening cutscene is passed) first
    }

    void TickCinematicTest(const float InSeconds)
    {
        const LabCinematicTest* Test = FindCinematicTest();
        if(!Test || bTravelling) { return; }
        World& W = GetWorld();
        if(CineClock > 0.f)
        {
            CineClock -= InSeconds;
            if(CineClock > 0.f) { return; }
            CineClock = -1.f;
            std::string Before = Test->Before;
            while(!Before.empty())
            {
                const size_t Comma = Before.find(',');
                const std::string Item = Before.substr(0, Comma);
                Before = Comma == std::string::npos ? std::string() : Before.substr(Comma + 1);
                if(Item.empty()) { continue; }
                if(Item[0] == '>')
                {
                    const EntityID Earlier = W.FindEntityByName(Item.c_str() + 1);
                    if(Earlier.IsSet()) { StartSequence(W, Earlier); SkipSequence(W, Earlier); }
                }
                else { LabSignals::Emit(W, Item.c_str(), true); }
            }
            if(std::strcmp(Test->Id, "C1") != 0)   // C1 opens its level by itself
            {
                PlacePlayer((std::string("CineMark_") + Test->Id).c_str());
                LabWorld::PlaySequence(W, Test->Player);
            }
            CORE_LOG(std::string("[DemoLab] cinematic test ") + Test->Id + ": playing " + Test->Player)
            return;
        }
        if(CineClock < 0.f && bCineStarted && !IsSequencePlaying())
        {
            CineQuitIn -= InSeconds;
            if(CineQuitIn <= 0.f && CineQuitIn > -1000.f) { CineQuitIn = -2000.f; GetSession().RequestQuit(); }
        }
    }

    /* Stands the local player on a marker entity, facing its yaw (a cutscene's Event key
     * "Lab.PlacePlayer", arg0 the marker's name): where the hand-over finds them. */
    void PlacePlayer(const char* InMarker)
    {
        World& W = GetWorld();
        const EntityID Mark = W.FindEntityByName(InMarker);
        const EntityID Local = GetLocalAvatar(W, 0);
        if(!Mark.IsSet() || !Local.IsSet() || !HasAuthority()) { return; }
        const TransformPose Pose = W.GetEntity(Mark).GetWorldPose();
        TeleportCharacter(W, Local, Pose.Position, float(Pose.Rotation.GetY()));
    }

    void OnPlacePlayer(const EventArgs& InArgs)
    {
        if(InArgs.Count >= 1 && InArgs.Args[0].Type == EventArg::ET_String) { PlacePlayer(InArgs.Args[0].String); }
    }

    /* The player heals slowly out of a fight. */
    void TickRegen(const float InSeconds)
    {
        SinceHurt += InSeconds;
        HurtSoundClock = std::max(0.f, HurtSoundClock - InSeconds);
        if(!HasAuthority() || SinceHurt < GLabRegenDelay.GetFloat() || IsSequencePlaying()) { return; }
        const EntityID Local = GetLocalAvatar(GetWorld(), 0);
        const LabHealthComponent* Health = Local.IsSet() ? GetWorld().GetEntity(Local).GetComponent<LabHealthComponent>() : nullptr;
        if(!Health || Health->bDead || Health->Health >= Health->MaxHealth) { return; }
        LabDamageEvent Heal;
        Heal.Victim = Local;
        Heal.Amount = GLabRegenRate.GetFloat() * InSeconds;
        Heal.DamageType = EDamageType::EDT_Heal;
        LabDamage::Apply(GetWorld(), Heal);
    }

    void OnDamaged(const EventArgs& InArgs)
    {
        if(InArgs.Count < 1 || InArgs.Args[0].Type != EventArg::ET_Entity) { return; }
        const EntityID Victim = EntityID::Unpack(InArgs.Args[0].Entity);
        if(Victim == GetLocalAvatar(GetWorld(), 0)) { SinceHurt = 0.f; return; }
        /* A robot hurt: its squeal (Brute or Watchman), at most every half second. */
        const Entity Robot = GetWorld().GetEntity(Victim);
        const LabBotComponent* Bot = Robot.IsAlive() ? Robot.GetComponent<LabBotComponent>() : nullptr;
        const LabHealthComponent* Health = Robot.IsAlive() ? Robot.GetComponent<LabHealthComponent>() : nullptr;
        if(!Bot || !Health || Health->bDead || HurtSoundClock > 0.f) { return; }
        HurtSoundClock = 0.5f;
        PlaySoundAt(GetWorld(), Bot->Archetype == LabBots::BA_Brute ? "Content/Sounds/Robots/Brute_Hurt.nmsound"
                                                                     : "Content/Sounds/Robots/Watchman_Hurt.nmsound",
                    Robot.GetWorldPose().Position);
    }

    void TickTravel(const float InSeconds)
    {
        if(bCredits)
        {
            CreditsFor -= InSeconds;
            if(CreditsFor <= 0.f && !bTravelling)
            {
                bTravelling = true;
                LabFlow::ToMainMenu(GetSession());
            }
            return;
        }
        if(!bTravelPending || bTravelling || IsSequencePlaying() || !HasAuthority()) { return; }
        bTravelPending = false;
        bTravelling = true;
        LabTravel::Travel(GetSession(), PendingTravel.World, PendingTravel.Entry, PendingTravel.Flags, PendingTravel.Title);
    }

    void StartCredits()
    {
        if(bCredits) { return; }
        bCredits = true;
        CreditsFor = GLabAutoPlay.GetBool() ? 3.f : std::max(1.f, GLabCreditsSeconds.GetFloat());
        HudStory.Objective.clear();
        CORE_LOG("[DemoLab] extraction complete; the credits roll")
        /* A finished story's checkpoint is not a place to continue from. */
        std::error_code Ignored;
        std::filesystem::remove(LabFlow::CHECKPOINT_PATH, Ignored);
        if(!IsDedicatedServer())
        {
            GetSession().GetUI().SetValue("Credits.Rolling", UI::UIValue::Bool(true));
            GetSession().GetUI().Open(LabUI::CREDITS);
        }
    }

    void ShowObjectivesFor(const char* InChannel)
    {
        std::string Text;
        GetWorld().Each<LabObjectiveComponent>([&](Entity, LabObjectiveComponent& InObjective)
        {
            if(InObjective.bShown || std::strcmp(InObjective.OnSignal, InChannel) != 0) { return; }
            InObjective.bShown = 1;
            Text = InObjective.Text;
        });
        if(!Text.empty()) { SetObjective(Text); }
    }

    void SetObjective(const std::string& InText)
    {
        if(InText == HudStory.Objective) { return; }
        HudStory.Objective = InText;
        CORE_LOG("[DemoLab] objective: " + InText)
    }

    void ShowBanner(const std::string& InText, const float InSeconds)
    {
        HudStory.Banner = InText;
        BannerFor = InSeconds;
    }

    void OnObjectiveSet(const EventArgs& InArgs)
    {
        if(InArgs.Count >= 1 && InArgs.Args[0].Type == EventArg::ET_String) { SetObjective(InArgs.Args[0].String); }
    }

    void OnBanner(const EventArgs& InArgs)
    {
        if(InArgs.Count < 1 || InArgs.Args[0].Type != EventArg::ET_String) { return; }
        const float Seconds = InArgs.Count >= 2 && InArgs.Args[1].Type == EventArg::ET_Number ? float(InArgs.Args[1].Number) : 4.f;
        ShowBanner(InArgs.Args[0].String, Seconds);
    }

    void OnCheckpoint(const EventArgs& InArgs)
    {
        if(InArgs.Count < 1 || InArgs.Args[0].Type != EventArg::ET_Entity) { return; }
        const EntityID Trigger = EntityID::Unpack(InArgs.Args[0].Entity);
        if(!GetWorld().IsAlive(Trigger)) { return; }
        Checkpoint = GetWorld().GetEntity(Trigger).GetWorldPose();
        Checkpoint.Scale = EngineCore::Vector3(1, 1, 1);
        bHaveCheckpoint = true;
        bSaveRequested = true;
        ShowBanner("Checkpoint", 2.f);
    }

    /* Where the character stands now, saved. */
    void CheckpointHere()
    {
        const EntityID Local = GetLocalAvatar(GetWorld(), 0);
        if(Local.IsSet())
        {
            Checkpoint = GetWorld().GetEntity(Local).GetWorldPose();
            Checkpoint.Scale = EngineCore::Vector3(1, 1, 1);
            if(const CharacterMoverComponent* Mover = GetWorld().GetEntity(Local).GetComponent<CharacterMoverComponent>())
            {
                Checkpoint.Rotation = EngineCore::Vector3(0, double(Mover->BodyYaw), 0);
            }
            bHaveCheckpoint = true;
        }
        bSaveRequested = true;
    }

    /* A cleared fight is a checkpoint too. */
    void OnEncounterDone(const EventArgs&)
    {
        CheckpointHere();
        ShowBanner("Area clear", 2.f);
    }

    void OnInteracted(const EventArgs& InArgs)
    {
        if(InArgs.Count < 1 || InArgs.Args[0].Type != EventArg::ET_Entity) { return; }
        const Entity Thing = GetWorld().GetEntity(EntityID::Unpack(InArgs.Args[0].Entity));
        const LabInteractableComponent* Data = Thing.IsAlive() ? Thing.GetComponent<LabInteractableComponent>() : nullptr;
        if(!Data) { return; }
        if(Data->WeaponID)
        {
            const LabWeaponComponent* Weapon = LabWeapons::GetData(GetWorld(), Data->WeaponID);
            ShowBanner(std::string("Picked up the ") + (Weapon ? Weapon->DisplayName : Thing.GetName()), 2.5f);
        }
        else if(Data->Ammo > 0) { ShowBanner("Ammunition", 1.5f); }
    }

    std::string   LevelName;
    bool          bAlarm = false;

    TransformPose Checkpoint;
    bool          bHaveCheckpoint = false;
    EntityID      Fallen;
    float         RespawnIn = 0.f;
    float         BannerFor = 0.f;

    LabStoryState State;
    bool          bSaveRequested = false;
    bool          bRestored = false;
    float         ReloadIn = 0.f;

    LabTravelComponent PendingTravel;
    bool          bTravelPending = false;
    bool          bTravelling = false;
    bool          bCredits = false;
    float         CreditsFor = 0.f;

    float         QuitIn = 3.f;
    float         CineClock = 0.f;     // -DemoLab.Cinematic: until it plays (> 0), played (< 0)
    float         CineQuitIn = 3.f;
    bool          bCineStarted = false;
    float         SinceHurt = 100.f;
    float         HurtSoundClock = 0.f;
};

NIMBLE_REGISTER_GAME_MODE(LabStoryMode, "LabStory")
