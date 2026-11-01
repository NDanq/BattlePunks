#ifndef DEMOLAB_LABAUTOPLAY_H
#define DEMOLAB_LABAUTOPLAY_H

#include "Public/GameCore/EntityID.h"
#include "Public/Math/Vector.h"

#include <string>

namespace GameCore { class World; }
namespace NimbleEngine { struct InputSnapshot; class GameSession; }

/*
 * The DemoLab.AutoPlay test (ctest, headless): with [DemoLab] AutoPlay=1 the
 * game plays itself from the main menu to the end and back -- New Game, then
 * every level through the story mode's avatar input filter, pressing what a
 * player would press, so travel, interaction, holds, cinematics, waves and
 * checkpoints all run as they would -- and falls once on purpose after W1 to
 * check the checkpoint reload. The run ends with a PASS or FAIL line in the
 * log ("[DemoLab] AutoPlay PASS ...") and quits.
 *
 *   DemoLab -headless -DemoLab.AutoPlay=1
 *
 * It steers by entity names in the levels (Tools/Levels/layout.py writes
 * them): "AP_<route>_<n>" waypoints, interactables, ammo crates.
 *
 * The driver is a static: a level load (travel, a checkpoint) builds a new
 * game mode, and the test carries on where it was.
 */
class LabAutoPlay {
public:
    /* The menu mode, each frame: starts a new game once, and passes the run
     * when it comes back after the credits. */
    void TickMenu(NimbleEngine::GameSession& InSession, float InSeconds);

    /* A story level started: its script begins (or the step in hand carries
     * on, after a checkpoint reload). */
    void OnLevelStarted(const std::string& InLevel, bool bInAlarm, bool bInRestored);

    /* Overwrites InOutInput for the local character; call from the filter. */
    void Drive(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOutInput,
               float InSeconds, bool bInCutscene);

    /* The mode tells it the character fell (its filter does not run for the dead). */
    void NoteFall();

    /* "" while running; "PASS" or "FAIL: why" once over. */
    const std::string& GetResult() const { return Result; }
    void Fail(const std::string& InWhy);

    /* Every frame, from any mode: the overall timeout. */
    void TickClock(float InSeconds);

private:
    void Next(const char* InWhat);
    bool Steer(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOut,
               const EngineCore::Vector3& InTarget, float InStop);
    bool Fight(GameCore::World& InWorld, GameCore::EntityID InAvatar, NimbleEngine::InputSnapshot& InOut, float InSeconds);
    void Trace(GameCore::World& InWorld, GameCore::EntityID InAvatar);

    std::string Level;
    bool        bAlarm = false;
    int         Script = -1;        // which level script
    int         StepIndex = 0;
    int         Waypoint = 0;
    float       Elapsed = 0.f;
    float       StepTime = 0.f;
    float       PressClock = 0.f;
    float       FireClock = 0.f;
    float       TraceClock = 0.f;
    float       MenuClock = 0.f;
    /* Steer's way round a body or debris in the path: no headway for a
     * while, then a sidestep, alternating sides. */
    EngineCore::Vector3 StuckAt;
    float       StuckClock = 0.f;
    float       SideClock = 0.f;
    float       SideSign = 1.f;
    bool        bStartedGame = false;
    bool        bFinished = false;  // the credits rolled; the menu passes the run
    bool        bKilled = false;
    bool        bWasDead = false;
    int         Deaths = 0;
    std::string Result;
};

/* The one driver (see above). */
LabAutoPlay& GetLabAutoPlay();

#endif //DEMOLAB_LABAUTOPLAY_H
