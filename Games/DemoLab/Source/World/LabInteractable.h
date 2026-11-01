#ifndef DEMOLAB_LABINTERACTABLE_H
#define DEMOLAB_LABINTERACTABLE_H

#include "Public/GameCore/ComponentType.h"
#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

#include <cstdint>
#include <string>

namespace GameCore { class World; }

/*
 * The story's level pieces (TDD §7.5), all wired by signal name
 * (LabSignals.h) and placed in the editor:
 *
 *   LabInteractable  press Interact within Radius: gives a weapon, emits or
 *                     toggles a signal, starts a cutscene. A child named
 *                     "Handle" swings with a toggle (a lever), or turns while
 *                     a hold (HoldSeconds > 0: a crank) is being held.
 *   LabGate          slides open along +Y while OpenOn is true (false, with
 *                     Invert: a shutter). Give it a Kinematic Rigid Body so
 *                     it blocks while shut.
 *   LabTrigger       a box a player walks into: emits a signal, starts a
 *                     cutscene, sets the checkpoint.
 *   LabObjective     sets the HUD objective when a signal fires.
 *
 * Gameplay changes happen on the authority; the replicated flags (bUsed,
 * bOn, bOpen) drive what every process shows.
 */

struct LabInteractableComponent {
    GAMECORE_COMPONENT_BODY()
    char    Prompt[48] = "Use";
    float   Radius = 170.f;
    uint8_t WeaponID = 0;            // a pickup: this registry id goes in the inventory, equipped
    int32_t Ammo = 0;                // extra reserve rounds for every held weapon (an ammo crate)
    char    Channel[32] = "";        // emitted true on use; a toggle emits its new state
    uint8_t bToggle = 0;
    char    Sequence[64] = "";       // a Sequence Player entity's name (a cinematic), or a .nmseq path
    char    EnableOn[32] = "";       // usable only once this signal is true; "" always
    uint8_t bOnce = 1;
    uint8_t bHideWhenUsed = 0;
    float   HoldSeconds = 0.f;       // > 0: Interact must be held this long (a crank)

    // This run: how far a hold has got, seconds.
    float   HoldProgress = 0.f;

    // Replicated state.
    uint8_t bUsed = 0;
    uint8_t bOn = 0;
};

struct LabGateComponent {
    GAMECORE_COMPONENT_BODY()
    char    OpenOn[32] = "";
    float   OpenHeight = 320.f;      // how far it rises
    float   Speed = 220.f;           // units per second
    uint8_t bInvert = 0;             // open while OpenOn is false: a shutter that slams on a signal
    uint8_t bOpen = 0;               // replicated

    // This run, every process.
    EngineCore::Vector3 ClosedPosition;
    float   Offset = 0.f;
    uint8_t bHasClosed = 0;
};

struct LabTriggerComponent {
    GAMECORE_COMPONENT_BODY()
    EngineCore::Vector3 Extent{150.0, 200.0, 150.0};   // half size, world axes, centred on the entity
    char    Channel[32] = "";
    char    Sequence[64] = "";
    char    EnableOn[32] = "";
    uint8_t bOnce = 1;
    uint8_t bCheckpoint = 0;         // a player entering sets the respawn point here

    // Authority, this run.
    uint8_t bFired = 0;
    uint8_t bOccupied = 0;
};

struct LabObjectiveComponent {
    GAMECORE_COMPONENT_BODY()
    char    OnSignal[32] = "";       // "" : from the level's start
    char    Text[96] = "";
    uint8_t bShown = 0;
};

namespace LabWorld {
    /* Authority: InUser uses InTarget, if it may. */
    void Interact(GameCore::World& InWorld, GameCore::EntityID InUser, GameCore::EntityID InTarget);

    /* Every process: the interactable InAvatar would use now (the nearest
     * in range, favouring the one looked at); unset if none. */
    GameCore::EntityID FindInteractable(GameCore::World& InWorld, GameCore::EntityID InAvatar);
    bool IsUsable(GameCore::World& InWorld, const LabInteractableComponent& InThing);

    /* Starts a placed Sequence Player by entity name, or plays a .nmseq path. */
    bool PlaySequence(GameCore::World& InWorld, const char* InNameOrPath);

    /* Authority: gates follow their signal. */
    void OnSignal(GameCore::World& InWorld, const char* InChannel, bool bInValue);

    /* Systems. */
    void TickInteractInput(GameCore::World& InWorld, float InSeconds);   // every process: locally controlled avatars press Interact
    void TickTriggers(GameCore::World& InWorld, float InSeconds);        // authority
    void TickPresentation(GameCore::World& InWorld, float InSeconds);    // every process: gates slide, levers swing, used pickups hide

    /* The HUD prompt for the local player, "" for none. */
    std::string PromptFor(GameCore::World& InWorld, GameCore::EntityID InAvatar);
    /* The hold under way for the local player, 0..1; < 0 when the prompt is not a hold. */
    float HoldFraction(GameCore::World& InWorld, GameCore::EntityID InAvatar);
}

#endif //DEMOLAB_LABINTERACTABLE_H
