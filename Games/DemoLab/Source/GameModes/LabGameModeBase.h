#ifndef DEMOLAB_LABGAMEMODEBASE_H
#define DEMOLAB_LABGAMEMODEBASE_H

#include "Public/GameCore/EventBus.h"
#include "Public/NimbleEngine/NimbleEngine.h"
#include "Public/NimbleEngine/PlayerStart.h"
#include "Public/NimbleEngine/WorldList.h"

#include "Core/LabHealth.h"
#include "UI/LabHUD.h"
#include "Weapons/LabCombat.h"

#include <string>
#include <vector>

/*
 * The rules every DemoLab mode shares (TDD §7.1): avatars spawn as
 * characters, and the combat, interaction and presentation systems run. Derived
 * modes (LabSandboxMode, LabStoryMode) set the rules below and add their
 * own flow.
 *
 *   Lab.Combat               PrePhysics   every process (locally controlled characters)
 *   Lab.Interact             PrePhysics   every process (locally controlled characters)
 *   Lab.CombatServer         PrePhysics   authority: cooldowns, reloads, melee windows
 *   Lab.Projectiles          PrePhysics   authority steps them; other processes carry them on
 *   Lab.World                PostPhysics  authority: triggers, encounters
 *   Lab.WeaponPresentation   PostPhysics  every process, before the anim sockets update
 *   Lab.WorldPresentation    PostPhysics  every process: gates, levers, pickups
 *
 * It also opens the HUD (UI/LabHUD.h) on every process with a screen and
 * hides it while a cutscene plays, and captures the mouse for mouse-look:
 * Pause (Escape) frees it, a click takes it back.
 *
 * It owns the World's one avatar input filter: a bot's avatar is driven by
 * LabBots::Drive, a player's goes through FilterPlayerInput.
 */
class LabGameModeBase : public NimbleEngine::NetGameMode {
public:
    using Super = NimbleEngine::NetGameMode;

    bool OnLoad(NimbleEngine::GameSession& InSession) override;
    void OnStart() override;
    void OnTick(const NimbleEngine::FrameContext& InFrame) override;
    void OnUnload() override;

    // Rules a derived mode sets. Called by LabDamage on the authority.
    virtual bool  UsesDowned() const { return false; }
    virtual bool  FriendlyFire() const { return false; }
    virtual float ModifyDamage(const LabDamageEvent& InEvent, float InAmount) { (void)InEvent; return InAmount; }
    virtual void  OnLabDowned(GameCore::EntityID InVictim) { (void)InVictim; }
    virtual void  OnLabKilled(GameCore::EntityID InVictim, GameCore::EntityID InKiller, uint8_t InWeaponID)
    {
        (void)InVictim; (void)InKiller; (void)InWeaponID;
    }

    /* Every process: a line for the kill feed (it fades after a few seconds). */
    void PushFeed(const std::string& InLine);

protected:
    const char* GetAvatarPrefab(GameCore::EntityID InPlayer) override;
    void OnAvatarSpawned(GameCore::EntityID InPlayer, GameCore::EntityID InAvatar, uint32_t InOrdinal) override;

    /* A derived mode's per-signal hook, after gates and encounters heard it. */
    virtual void OnSignal(const char* InChannel, bool bInValue) { (void)InChannel; (void)bInValue; }

    /* Authority: alive again where it stands, or at InAt. Keeps the inventory. */
    void Respawn(GameCore::EntityID InAvatar, const GameCore::TransformPose* InAt);

    /* A player's (not a bot's) input, after the devices: the test hooks drive
     * the local character here. The base leaves it as read. */
    virtual void FilterPlayerInput(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOutInput)
    {
        (void)InWorld; (void)InAvatar; (void)InOutInput;
    }

    /* Seconds the last frame took, for the input filter (which gets none). */
    float GetFrameSeconds() const { return FrameSeconds; }

    /* Whether the mode takes the mouse for mouse-look at start; the AutoPlay
     * test says no, so a run never grabs the desktop's pointer. */
    virtual bool WantsMouseCapture() const { return true; }

    /* The local player pressed Pause (Escape): the base frees the mouse; the
     * story opens its pause menu. */
    virtual void OnPausePressed() { GetSession().SetMouseCaptured(false); }

    /* [DemoLab] SelfBot: the local player's character plays by itself -- a
     * headless client in a network test. On the authority it gets a brain
     * (SelfBotTree); on a client, where there is no AI, it chases the
     * nearest enemy it can see. */
    virtual bool IsSelfBot() const;
    virtual const char* SelfBotTree() const { return "Content/AI/Watchman.nmbt"; }

    /* True while nobody may act (an arena countdown): every character's input,
     * bots' too, is cleared. */
    virtual bool IsInputHeld() const { return false; }

    /* A cutscene began (InPath, its .nmseq), on its Sequence Player. */
    virtual void OnCutsceneStarted(GameCore::EntityID InPlayer, const char* InPath) { (void)InPlayer; (void)InPath; }

    /* A cutscene is playing (SequenceStarted .. Finished/Stopped). */
    bool IsSequencePlaying() const { return !PlayingSequences.empty(); }

    LabCombat::PresentedMap Presented;
    LabHUD::Story           HudStory;      // what the HUD shows beyond the character: a derived mode's to fill

private:
    void TickCombatInput(GameCore::World& InWorld, float InSeconds);
    void TickInteractInput(GameCore::World& InWorld, float InSeconds);
    void TickCombatServer(GameCore::World& InWorld, float InSeconds);
    void TickWorld(GameCore::World& InWorld, float InSeconds);
    void TickWeaponPresentation(GameCore::World& InWorld, float InSeconds);
    void TickWorldPresentation(GameCore::World& InWorld, float InSeconds);
    void TickProjectiles(GameCore::World& InWorld, float InSeconds);

    void OnSignalEvent(const GameCore::EventArgs& InArgs);
    void OnEncounterStartEvent(const GameCore::EventArgs& InArgs);
    void OnSequenceStarted(const GameCore::EventArgs& InArgs);
    void OnSequenceEnded(const GameCore::EventArgs& InArgs);
    void OnDamagedEvent(const GameCore::EventArgs& InArgs);
    void OnKilledEvent(const GameCore::EventArgs& InArgs);
    void UpdateSelfBot();
    void FilterInput(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOutInput);
    void UpdateMouseCapture();

    std::vector<uint64_t> PlayingSequences;   // packed Sequence Player entities
    float                 FrameSeconds = 1.f / 60.f;
    struct FeedLine { std::string Text; float Seconds; };
    std::vector<FeedLine> FeedLines;
};

/* A character's name for the feed: its DisplayName, else its entity name. */
const char* LabName(GameCore::World& InWorld, GameCore::EntityID InCharacter);

#endif //DEMOLAB_LABGAMEMODEBASE_H
