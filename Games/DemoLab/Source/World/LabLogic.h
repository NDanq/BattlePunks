#ifndef DEMOLAB_LABLOGIC_H
#define DEMOLAB_LABLOGIC_H

#include "Public/GameCore/ComponentType.h"

#include <cstdint>
#include <string>

namespace GameCore { class World; }

/*
 * Level logic Demo Lab adds to BattlePunks' pieces (LabInteractable.h), all
 * wired by signal name (LabSignals.h) like those:
 *
 *   LabSignalToggle  shows or hides the entity (its Render, and any light on
 *                    it) by a signal: the lights C3 turns on room by room,
 *                    the reactor's red when C5 trips the alarm.
 *   LabTimer         counts Seconds from StartOn, then emits DoneChannel; the
 *                    HUD shows "Label  0:42" while it runs (beacon charge,
 *                    the atrium hold).
 *   LabAllOf         emits Output once every channel in Inputs is true.
 *   LabTravel        travels (Flow/LabTravel.h) when OnSignal fires, once no
 *                    cinematic is playing: the end of each level.
 *
 * Entities whose names start "Calm_" exist only while the travel state's
 * Alarm flag is off, "Alarm_" only while it is on (LabLogic::ApplyStateGroups).
 */

struct LabSignalToggleComponent {
    GAMECORE_COMPONENT_BODY()
    char    Channel[32] = "";
    uint8_t bShowWhenOn = 1;         // shown while the signal is true (else while it is false)
};

struct LabTimerComponent {
    GAMECORE_COMPONENT_BODY()
    char    StartOn[32] = "";
    float   Seconds = 30.f;
    char    DoneChannel[32] = "";
    char    Label[32] = "";
    uint8_t State = 0;               // 0 waiting, 1 running, 2 done
    float   Remaining = 0.f;
};

struct LabAllOfComponent {
    GAMECORE_COMPONENT_BODY()
    char    Inputs[96] = "";         // "A,B,C"
    char    Output[32] = "";
    uint8_t bFired = 0;
};

struct LabTravelComponent {
    GAMECORE_COMPONENT_BODY()
    char    OnSignal[32] = "";
    char    World[32] = "";          // a [World_*] name
    char    Entry[48] = "";          // a Player Start's entity name there
    char    Flags[48] = "";          // the travel state: "Alarm=1"
    char    Title[64] = "";          // the loading card
    uint8_t bFired = 0;
};

namespace LabLogic {
    /* Authority: timers start, AllOf gates fire. */
    void OnSignal(GameCore::World& InWorld, const char* InChannel, bool bInValue);
    /* Authority: timers count down. */
    void Tick(GameCore::World& InWorld, float InSeconds);
    /* Every process: toggles follow their signals. */
    void TickPresentation(GameCore::World& InWorld);

    /* The running timer's HUD line ("Beacon charging  0:42"), "" if none. */
    std::string TimerLine(GameCore::World& InWorld);

    /* Destroys the entities of the state group that is not active: "Calm_*"
     * when bInAlarm, "Alarm_*" otherwise. Returns how many went. */
    int ApplyStateGroups(GameCore::World& InWorld, bool bInAlarm);
}

#endif //DEMOLAB_LABLOGIC_H
