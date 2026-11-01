#ifndef DEMOLAB_LABENCOUNTER_H
#define DEMOLAB_LABENCOUNTER_H

#include "Public/GameCore/ComponentType.h"

#include <cstdint>

namespace GameCore { class World; }

/*
 * An encounter (TDD §7.5): a fight that starts on a signal (or a Sequencer
 * Event key "Encounter.Start" with arg0 = Id, or at once when StartOn is ""),
 * spawns its waves at the children named "Spawn_*" (bots, or target dummies),
 * and emits DoneChannel once the last wave is dead. Entities already in the
 * level join it with a LabEncounterMember naming its Id.
 */

struct LabEncounterComponent {
    GAMECORE_COMPONENT_BODY()
    char    Id[32] = "";
    char    StartOn[32] = "";
    char    DoneChannel[32] = "";
    char    SpawnPrefab[96] = "native:DemoLab.Dummy";
    /* Waves of bots (Source/AI/LabBots.h): "Watchman:4|Watchman:3,Brute:1" --
     * archetype:count, a comma between kinds, a bar between waves. Empty:
     * one wave of SpawnPrefab (target dummies), one at each spawn point. */
    char    Waves[96] = "";
    float   WaveDelay = 2.f;          // seconds between a cleared wave and the next
    char    LockChannel[32] = "";     // true while the fight is on (gates shut), false after
    char    StopOn[32] = "";          // ends the fight early: the living members fall (the atrium hold's end)
    uint8_t SpawnTeam = 2;
    float   EnemyHealth = 60.f;

    // Replicated, for the HUD.
    uint8_t State = 0;                // 0 waiting, 1 fighting, 2 done
    uint8_t Alive = 0;
    uint8_t Total = 0;
    uint8_t Wave = 0;                 // the wave being fought (0 first)
    uint8_t WaveCount = 1;

    // Authority, this run.
    float   WaveTimer = -1.f;         // counting down to the next wave; < 0: none pending
};

struct LabEncounterMemberComponent {
    GAMECORE_COMPONENT_BODY()
    char Encounter[32] = "";
};

namespace LabEncounters {
    enum : uint8_t { STATE_WAITING = 0, STATE_FIGHTING = 1, STATE_DONE = 2 };

    /* Authority: starts the encounter with this Id (or every one listening
     * on InChannel); already started ones are left alone. */
    void StartById(GameCore::World& InWorld, const char* InId);
    void OnSignal(GameCore::World& InWorld, const char* InChannel, bool bInValue);

    /* Authority: counts the living, finishes the cleared. */
    void Tick(GameCore::World& InWorld, float InSeconds);

    /* The fighting encounter's "Alive / Total", for the HUD; false if none. */
    bool GetActive(GameCore::World& InWorld, int& OutAlive, int& OutTotal);
    /* And its wave, 1-based, of how many; false if none. */
    bool GetWave(GameCore::World& InWorld, int& OutWave, int& OutWaves);

    /* Authority: an encounter caught mid-fight by a checkpoint load goes back
     * to waiting, its members gone, and starts over. */
    void RestartInterrupted(GameCore::World& InWorld);
}

#endif //DEMOLAB_LABENCOUNTER_H
