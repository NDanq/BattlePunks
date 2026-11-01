#ifndef DEMOLAB_LABUI_H
#define DEMOLAB_LABUI_H

#include <string>

namespace NimbleEngine { class GameSession; }

/*
 * Demo Lab's menus (the documents in Content/UI) and the C++ behind their buttons:
 * the native actions "Lab.*" (UI_DEFINE_ACTIONS in LabUI.cpp).
 *
 *   MainMenu.nmui       Continue, New Game, Chapter Select, Settings, Credits, Quit
 *   ChapterSelect.nmui  the flow table's four rows (Flow/LabFlow.h)
 *   Settings.nmui       Graphics, Display, Audio, Controls, Accessibility: config vars
 *   PauseMenu.nmui      Resume, Restart checkpoint, Settings, Main menu, Quit
 *   Credits.nmui        the credits (Credits.lua holds the text: Docs/Credits.md's)
 *   TechOverlay.nmui    F1: the engine features on screen, live "stat unit" numbers
 *   Loading.nmui        the loading card (Flow/LabTravel.h)
 *
 * Settings write config vars through one action, Lab.SetVar('A.B|C.D', $value):
 * every path given gets the value (bool, int or float, as the var is
 * declared), and Lab.RefreshSettings copies them into the "Set.<path>" keys
 * the controls bind to. Closing the page saves User.ini.
 */
namespace LabUI {
    constexpr const char* MAIN_MENU      = "Content/UI/MainMenu.nmui";
    constexpr const char* CHAPTER_SELECT = "Content/UI/ChapterSelect.nmui";
    constexpr const char* SETTINGS       = "Content/UI/Settings.nmui";
    constexpr const char* PAUSE_MENU     = "Content/UI/PauseMenu.nmui";
    constexpr const char* CREDITS        = "Content/UI/Credits.nmui";
    constexpr const char* TECH_OVERLAY   = "Content/UI/TechOverlay.nmui";

    /* Escape in a level: the menu opens, the game stops, the cursor comes back. */
    void OpenPauseMenu(NimbleEngine::GameSession& InSession);
    void ClosePauseMenu(NimbleEngine::GameSession& InSession);
    bool IsPauseMenuOpen(NimbleEngine::GameSession& InSession);

    /* F1. */
    void ToggleTechOverlay(NimbleEngine::GameSession& InSession);
    /* Each frame while it is open: the level's features and stat unit. */
    void UpdateTechOverlay(NimbleEngine::GameSession& InSession, const std::string& InLevel, bool bInAlarm);
    /* DemoLab.PerfLog > 0: every that many seconds, the frame and each GPU span (stat frame) to the
     * log -- for timing a level in the launcher, where the stat panels cannot be read back. */
    void LogPerf(NimbleEngine::GameSession& InSession, const std::string& InLevel, float InSeconds);

    /* Each frame: [DemoLab] Subtitles off hides the cinematic overlay's lines. */
    void ApplySubtitleSetting(NimbleEngine::GameSession& InSession);

    /* Copies every setting into the store ("Set.*"). */
    void RefreshSettings(NimbleEngine::GameSession& InSession);

    /* [DemoLab] LookSensitivity and InvertY, for the look axes. */
    float LookScale();
    bool  InvertLook();

    /* [DemoLab] FoliageDensity (0.25-1.5): L1's Foliage_* spawners' spacing
     * is divided by its square root (LabGameModeBase::ApplyFoliageDensity). */
    float FoliageDensity();
}

#endif //DEMOLAB_LABUI_H
