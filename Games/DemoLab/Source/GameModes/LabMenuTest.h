#ifndef DEMOLAB_LABMENUTEST_H
#define DEMOLAB_LABMENUTEST_H

#include "Public/GameCore/EntityID.h"

#include <string>

namespace GameCore { class World; }
namespace NimbleEngine { struct InputSnapshot; class GameSession; }

/*
 * The menu tests (ctest, headless and silent): with [DemoLab] MenuTest set,
 * the game drives its own menus through their documents -- each step clicks a
 * named button, moves a slider or ticks a check box in the open .nmui, so the
 * button's OnClick or the control's OnChanged runs exactly as a player's
 * click would -- and presses F1 and Escape through the player's input map.
 *
 *   MenuTest=1  DemoLab.MenuTest
 *     Settings  every page's kind of control changed: the values apply (the
 *               config vars, the quality tier, the look scale), and closing
 *               the page writes them to the user file
 *     Chapters  each of Chapter Select's four rows starts its level, in its
 *               state (Calm or Alarm, armed or not), at its entry, with a
 *               checkpoint
 *     F1        the tech overlay opens, names the level, shows stat unit
 *               numbers that change frame to frame, and closes again
 *     Pause     Escape opens it and pauses; Resume; Restart checkpoint
 *               reloads the level's checkpoint; Main menu goes back
 *
 *   MenuTest=2  DemoLab.SettingsPersist: a new process reads the user file
 *               the first left and finds every value it set; then deletes it.
 *
 * Both write to Saved/DemoLabTest/User.ini beside the executable, never the
 * player's own settings file. The run ends with "[DemoLab] MenuTest PASS" or
 * "[DemoLab] MenuTest FAIL: why" in the log, and quits.
 *
 * The driver is a static: each level load builds a new game mode, and the
 * test carries on where it was.
 */
class LabMenuTest {
public:
    bool IsActive() const;

    /* The menu mode: it has started (each visit), and each frame. */
    void OnMenuStarted(NimbleEngine::GameSession& InSession);
    void TickMenu(NimbleEngine::GameSession& InSession, float InSeconds);

    /* The story mode: a level started (or a checkpoint came back), and each
     * frame after the mode's own tick (bInCutscene: one is playing). */
    void OnLevelStarted(NimbleEngine::GameSession& InSession, const std::string& InLevel, bool bInAlarm, bool bInRestored);
    void TickStory(NimbleEngine::GameSession& InSession, const std::string& InLevel, bool bInCutscene, float InSeconds);

    /* Overwrites the local character's input: nothing held, and F1 or
     * Escape pressed when the test asks. Call from the story's filter. */
    void Drive(NimbleEngine::InputSnapshot& InOutInput);

    const std::string& GetResult() const { return Result; }

private:
    void Fail(const std::string& InWhy);
    void Pass();
    void Log(const std::string& InLine) const;
    void TickClock(NimbleEngine::GameSession& InSession, float InSeconds);
    void TickSettings(NimbleEngine::GameSession& InSession);
    void TickPersist(NimbleEngine::GameSession& InSession);
    void TickLevel(NimbleEngine::GameSession& InSession, const std::string& InLevel, bool bInCutscene, float InSeconds);
    bool CheckChapter(NimbleEngine::GameSession& InSession, const std::string& InLevel, bool bInAlarm);

    int         Phase = 0;            // the step in hand (LabMenuTest.cpp)
    int         Chapter = 0;          // the Chapter Select row being checked
    float       Clock = 0.f;          // seconds in the step in hand
    float       Elapsed = 0.f;
    float       QuitIn = 1.5f;
    bool        bPressOverlay = false;
    bool        bPressPause = false;
    bool        bLevelStarted = false;
    bool        bRestoredSeen = false;
    std::string Stats;                // the overlay's numbers, as first read
    std::string Result;
};

LabMenuTest& GetLabMenuTest();

#endif //DEMOLAB_LABMENUTEST_H
