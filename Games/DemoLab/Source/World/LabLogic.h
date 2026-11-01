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
 *                    it) by a signal, and plays or stops its Audio Source:
 *                    the lights C3 turns on room by room, the reactor's red
 *                    and the klaxon and combat music when C5 trips the alarm.
 *   LabTimer         counts Seconds from StartOn, then emits DoneChannel; the
 *                    HUD shows "Label  0:42" while it runs (beacon charge,
 *                    the atrium hold).
 *   LabAllOf         emits Output once every channel in Inputs is true.
 *   LabTravel        travels (Flow/LabTravel.h) when OnSignal fires, once no
 *                    cinematic is playing: the end of each level.
 *   LabSpinner       turns its entity about one local axis while a signal is
 *                    on (or always), easing up to speed and back down: the
 *                    orrery's rings, the turbines once pressurised, the alarm
 *                    beacons. Cosmetic, every process.
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

struct LabSpinnerComponent {
    GAMECORE_COMPONENT_BODY()
    char    Channel[32] = "";        // spins while this signal is true; "" spins always
    uint8_t Axis = 1;                // the Transform rotation it turns: 0 X (pitch), 1 Y (yaw), 2 Z (roll)
    float   Speed = 90.f;            // degrees per second at full speed (negative turns the other way)
    float   SpinUp = 1.f;            // seconds from rest to full speed, and back
    float   Current = 0.f;           // this run: the speed now
};

namespace LabLogic {
    /* Authority: timers start, AllOf gates fire. */
    void OnSignal(GameCore::World& InWorld, const char* InChannel, bool bInValue);
    /* Authority: timers count down. */
    void Tick(GameCore::World& InWorld, float InSeconds);
    /* Every process: toggles follow their signals. */
    void TickPresentation(GameCore::World& InWorld);
    /* Every process: spinners turn. */
    void TickSpinners(GameCore::World& InWorld, float InSeconds);

    /* The running timer's HUD line ("Beacon charging  0:42"), "" if none. */
    std::string TimerLine(GameCore::World& InWorld);

    /* Destroys the entities of the state group that is not active: "Calm_*"
     * when bInAlarm, "Alarm_*" otherwise. Returns how many went. */
    int ApplyStateGroups(GameCore::World& InWorld, bool bInAlarm);
}

#endif //DEMOLAB_LABLOGIC_H
